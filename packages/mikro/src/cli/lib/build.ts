import {existsSync} from 'node:fs'
import {stripTypeScriptTypes} from 'node:module'
import * as pathlib from 'node:path'
import {fileURLToPath} from 'node:url'

import {nodeFileTrace, resolve as traceResolve} from '@mikrojs/analyze-imports'
import {mkdir, readdir, readFile, rm, stat, unlink, writeFile} from 'fs/promises'
import {
  concat,
  defer,
  EMPTY,
  filter,
  from,
  ignoreElements,
  mergeMap,
  type Observable,
  of,
  throwError,
} from 'rxjs'

import type {
  LogLevel,
  MikroEnv,
  MikroJSConfig,
  Minifier,
  MinifyLevel,
} from '../../_exports/index.js'
import {isBuiltinModule} from '../../constants.js'
import {loadMikroConfig} from './loadMikroConfig.js'
import {minifyJs} from './minify.js'
import {parseSize} from './parseSize.js'

/** Console methods to mark as pure (side-effect-free) for each log level.
 * The minifier drops pure calls whose return value is unused, eliminating
 * both the call and its argument expressions. */
const LOG_LEVEL_PURE_FUNCS: Record<LogLevel, string[]> = {
  debug: [],
  info: ['console.debug'],
  warn: ['console.debug', 'console.log', 'console.info'],
  error: ['console.debug', 'console.log', 'console.info', 'console.warn'],
  none: ['console.debug', 'console.log', 'console.info', 'console.warn', 'console.error'],
}

// Lazy-load @mikrojs/native so commands that never compile bytecode or JSON
// (clean, console, env, list, erase, etc.) don't pay the addon load cost and,
// more importantly, don't silently hang at CLI startup if the .node file is
// stale or corrupt. Cached after first successful resolve so repeated calls
// during a single build run don't re-enter the loader.
type MikrojsNative = typeof import('@mikrojs/native')
let nativePromise: Promise<MikrojsNative> | null = null
function loadNative(): Promise<MikrojsNative> {
  if (!nativePromise) {
    nativePromise = import('@mikrojs/native').catch((err: unknown) => {
      // Reset so the next call can try again (e.g. after a rebuild).
      nativePromise = null
      const detail = err instanceof Error ? err.message : String(err)
      throw new Error(
        `Failed to load @mikrojs/native (required for bytecode / JSON compilation). ` +
          `Try rebuilding the native addon: pnpm -F @mikrojs/native build:native\n\n` +
          `Underlying error: ${detail}`,
      )
    })
  }
  return nativePromise
}

/** esbuild plugin that marks mikrojs firmware builtins as external so the
 * on-device loader resolves them against the baked-in bytecode table instead
 * of esbuild trying to inline their source. Pure-JS `@mikrojs/*` packages
 * (no ./cmake export) fall through to normal resolution and get bundled. */
function mikrojsExternalsPlugin(): import('esbuild').Plugin {
  return {
    name: 'mikrojs-externals',
    setup(build) {
      build.onResolve({filter: /^(mikro$|mikro\/|native:)/}, (args) => ({
        path: args.path,
        external: true,
      }))
      // Any bare package specifier (scoped `@scope/...` or unscoped `name/...`)
      // that resolves to a firmware builtin package is externalized so it binds
      // to the firmware builtin instead of being bundled. Not limited to
      // @mikrojs/* — third-party driver/board packages work the same way.
      build.onResolve({filter: /^@?[^./]/}, (args) => {
        if (isFirmwareBuiltin(args.path)) {
          return {path: args.path, external: true}
        }
        return null
      })
    },
  }
}

/** Check if a package is a firmware builtin (has native code), regardless of npm
 * scope. Firmware builtin packages export ./cmake which provides ESP-IDF
 * component paths. */
const firmwareBuiltinCache = new Map<string, boolean>()
function isFirmwareBuiltin(id: string): boolean {
  // Extract package name from import specifier (e.g. "@mikrojs/some-board/some-variant" -> "@mikrojs/some-board")
  const parts = id.split('/')
  const pkgName =
    parts.length >= 2 && parts[0]!.startsWith('@') ? `${parts[0]}/${parts[1]}` : parts[0]!
  if (firmwareBuiltinCache.has(pkgName)) return firmwareBuiltinCache.get(pkgName)!
  try {
    // For packages without an exports map, import.meta.resolve returns a URL
    // without checking that the file exists, so verify on disk.
    const resolved = import.meta.resolve(`${pkgName}/cmake`)
    const isBuiltin = existsSync(fileURLToPath(resolved))
    firmwareBuiltinCache.set(pkgName, isBuiltin)
    return isBuiltin
  } catch {
    firmwareBuiltinCache.set(pkgName, false)
    return false
  }
}

/** Top-level directory of a normalized relative entry path: `app/debug/test.ts`
 * → `app`, `main.ts` → `.`. Deploy promotes only the tree's top-level app dir
 * and the firmware searches for package.json at most one directory deep, so the
 * deploy tree root must be the entry's top-level dir, not its immediate parent. */
export function entryRootDir(entry: string): string {
  const idx = entry.indexOf(pathlib.sep)
  return idx === -1 ? '.' : entry.slice(0, idx)
}

export type BuildEvent =
  | {type: 'phase'; phase: string}
  | {type: 'file'; path: string; size: number}
  | {type: 'done'}
  /** What this build actually resolved to, once mikro.config.ts has been read.
   *  Emitted before any work, so a caller can report the settings that applied
   *  rather than re-deriving them from its own flags and missing the config. */
  | {
      type: 'settings'
      minify: boolean
      minifier: Minifier
      minifyLevel: MinifyLevel
      logLevel: LogLevel
      bundle: boolean
    }

function phase(name: string): Observable<BuildEvent> {
  return of({type: 'phase' as const, phase: name})
}

async function collectOutputFiles(buildDir: string): Promise<BuildEvent[]> {
  const entries = await readdir(buildDir, {recursive: true})
  const events: BuildEvent[] = []
  for (const entry of entries) {
    const full = pathlib.join(buildDir, entry)
    const s = await stat(full)
    if (s.isFile()) {
      events.push({type: 'file', path: '/' + entry, size: s.size})
    }
  }
  return events
}

const SKIP = Promise.resolve([])

export async function trace(entries: string[]) {
  const {fileList, warnings, sourcePathMap} = await nodeFileTrace(entries, {
    analysis: {evaluatePureExpressions: true},
    conditions: ['import'],
    assetExtensions: ['.txt'],
    ts: true,
    resolve: (id, parent, job) => {
      if (isBuiltinModule(id)) {
        return SKIP
      }
      // Skip firmware builtin packages (any scope) — they have native code and
      // a ./cmake export, and resolve to the firmware builtin. Pure JS packages
      // are traced and bundled normally.
      if (isFirmwareBuiltin(id)) {
        return SKIP
      }
      return traceResolve(id, parent, job)
    },
  })
  return {
    fileList: [...fileList],
    warnings: [...warnings],
    sourcePathMap,
  }
}

export function loadConfig(entry: string, env?: MikroEnv): Promise<MikroJSConfig | null> {
  return loadMikroConfig(pathlib.dirname(pathlib.resolve(entry)), env)
}

/** Default directory for file logging when the user opts in via
 * `logFile: true` or omits `logFile.dir`. Mirrored in the CLI's
 * `logs pull` command so it can locate the same file the firmware
 * writes to. */
const DEFAULT_LOG_DIR = '/appfs/logs'

// Strip host-only sections, normalize K/M-suffixed sizes, and flatten the
// `wifi: {country, hostname}`, `logFile: {...}` and `watchdog: {...}` groups
// into dotted-key form so the device-side JSON parser (mik_app_config.cpp)
// can read them without a nested-object pass.
function serializeRuntimeConfig(config: MikroJSConfig): Record<string, unknown> {
  // `env` is resolved away at load time; drop it defensively so the override
  // map can never leak into the device JSON. `otaConfigSchema` is host-only
  // (the config schema ships in the manifest, not here).
  const {
    sim: _sim,
    build: _build,
    env: _env,
    otaConfigSchema: _otaConfigSchema,
    wifi,
    logFile,
    fsReadMax,
    onPanic,
    watchdog,
    ...rest
  } = config
  const out: Record<string, unknown> = {...rest}
  if (fsReadMax !== undefined) out.fsReadMax = parseSize(fsReadMax)
  if (wifi?.country) out['wifi.country'] = wifi.country
  if (wifi?.hostname) out['wifi.hostname'] = wifi.hostname
  if (onPanic !== undefined) {
    out['onPanic.mode'] = onPanic.mode
    if (onPanic.delay !== undefined) out['onPanic.delay'] = onPanic.delay
    if (onPanic.mode === 'deepSleep') out['onPanic.duration'] = onPanic.duration
  }
  if (watchdog !== undefined) {
    // The device reads numbers only; `false` becomes the 0 it treats as disabled.
    if (watchdog.blocking !== undefined) {
      out['watchdog.blocking'] = watchdog.blocking === false ? 0 : watchdog.blocking
    }
    if (watchdog.feed !== undefined) out['watchdog.feed'] = watchdog.feed
    if (watchdog.awake !== undefined) out['watchdog.awake'] = watchdog.awake
  }
  if (logFile !== undefined) {
    const opts = logFile === true ? {} : logFile
    out['logFile.dir'] = opts.dir ?? DEFAULT_LOG_DIR
    if (opts.maxSize !== undefined) out['logFile.maxSize'] = parseSize(opts.maxSize)
    if (opts.flush !== undefined) out['logFile.flush'] = opts.flush
  }
  return out
}

export function build(
  entry: string,
  buildDir: string,
  options: {
    minify: boolean
    bytecode: boolean
    minifier?: Minifier
    minifyLevel?: MinifyLevel
    logLevel?: LogLevel
    rootDir?: string
    bundle?: boolean
    /** Config environment to resolve from mikro.config.ts ('development' or
     * 'production'). Defaults to 'production'. */
    env?: MikroEnv
  },
): Observable<BuildEvent> {
  // Resolve the entry to a cwd-relative path so absolute paths (drag-and-drop,
  // tab completion) and `./`-prefixed paths build the same tree: the rootDir
  // prefix checks below compare against traced file paths, which are
  // cwd-relative with no `./` prefix.
  entry = pathlib.relative(process.cwd(), pathlib.resolve(entry))
  // rootDir is the directory that becomes the root of the deploy tree.
  // Files outside it are co-located into it (so node_modules/ is findable by
  // the on-device module resolver). Files inside it keep their relative
  // position, which matters for bytecode: the baked-in module name is the
  // file's buildDir-relative path, and must match the on-device deploy path.
  // Defaults to the entry's top-level dir. Test runner overrides with the
  // user's app dir so that tests deeper than entry's parent still co-locate
  // correctly.
  const rootDir = options.rootDir ?? entryRootDir(entry)
  return defer(() => loadConfig(entry, options.env)).pipe(
    mergeMap((config) => {
      const shouldBundle = options.bundle ?? config?.build?.bundle ?? false
      const minifier = options.minifier ?? config?.build?.minifier ?? 'esbuild'
      const minifyLevel = options.minifyLevel ?? config?.build?.minifyLevel ?? 'default'
      const logLevel = options.logLevel ?? config?.build?.logLevel ?? 'debug'
      const pureFuncs = LOG_LEVEL_PURE_FUNCS[logLevel]
      const entryJs = entry.replace(/\.ts$/, '.js')
      const entryOutputPath =
        rootDir === '.' || entryJs.startsWith(rootDir + '/')
          ? entryJs
          : pathlib.join(rootDir, entryJs)

      const writeFilesUnbundled = defer(() => trace([entry])).pipe(
        mergeMap(({warnings, fileList, sourcePathMap}) => {
          if (warnings?.length > 0) {
            return throwError(() => new Error([...warnings].map((w) => w.message).join('\n')))
          }
          return from(fileList).pipe(
            mergeMap(async (file) => {
              // Use sourcePathMap to read from the real path on disk when the
              // output path is a virtual path (e.g. pnpm transitive dependencies)
              const sourcePath = sourcePathMap.get(file) ?? file
              return transform(file, await readFile(sourcePath), {
                minify: options.minify,
                minifier,
                minifyLevel,
                pureFuncs,
              })
            }),
            mergeMap(({path: filePath, contents}) => {
              // Place files outside rootDir (e.g. node_modules/ when rootDir
              // is a subdirectory) inside rootDir so the on-device module
              // resolver can find them when walking up from the entry.
              const outputPath =
                rootDir === '.' || filePath.startsWith(rootDir + '/')
                  ? filePath
                  : pathlib.join(rootDir, filePath)
              return output(buildDir, outputPath, contents)
            }),
          )
        }),
      )

      const writeFilesBundled = defer(async () => {
        const esbuild = await import('esbuild')
        const useEsbuildMinify = options.minify && minifier === 'esbuild'
        // Virtual outdir: write: false means esbuild never touches disk, but
        // splitting: true still needs an outdir to compute chunk paths.
        // Pairing it with outbase = dirname(entry) makes the entry chunk
        // land at `<outdir>/<entry-basename>.js` with shared chunks as
        // siblings, so relative() gives us clean, prefix-free paths.
        const virtualOutdir = pathlib.resolve(process.cwd(), '__mikro_bundle_out__')
        const entryDir = pathlib.dirname(entry)
        const result = await esbuild.build({
          entryPoints: [entry],
          bundle: true,
          splitting: true,
          outdir: virtualOutdir,
          outbase: entryDir,
          write: false,
          minify: useEsbuildMinify,
          treeShaking: true,
          target: 'es2024',
          platform: 'neutral',
          format: 'esm',
          legalComments: 'none',
          logLevel: 'silent',
          plugins: [mikrojsExternalsPlugin()],
          ...(pureFuncs.length > 0 ? {pure: pureFuncs} : undefined),
        })
        if (result.errors.length > 0) {
          throw new Error(result.errors.map((e) => e.text).join('\n'))
        }
        if (!result.outputFiles || result.outputFiles.length === 0) {
          throw new Error('esbuild produced no output')
        }
        const outputPrefix = pathlib.dirname(entryOutputPath)
        for (const outFile of result.outputFiles) {
          let code = outFile.text
          if (options.minify && minifier !== 'esbuild') {
            code = await minifyJs(code, minifier, minifyLevel, pureFuncs)
          }
          const rel = pathlib.relative(virtualOutdir, outFile.path)
          const outputPath =
            outputPrefix === '.' || outputPrefix === '' ? rel : pathlib.join(outputPrefix, rel)
          await output(buildDir, outputPath, code)
        }
      })

      const writeFiles = shouldBundle ? writeFilesBundled : writeFilesUnbundled

      const writeConfig = defer(() => {
        if (config === null) return EMPTY
        return output(
          buildDir,
          pathlib.join(rootDir, 'mikro.config.json'),
          JSON.stringify(serializeRuntimeConfig(config)),
        )
      })

      // Write package.json into the app directory with name, version, type, and main
      const writePackageJson = defer(async () => {
        const pkgPath = pathlib.join(process.cwd(), 'package.json')
        const pkg = JSON.parse(await readFile(pkgPath, 'utf-8'))
        // Mirror writeFiles' co-locate logic: files outside rootDir are
        // placed under it (e.g. `test/x.test.ts` with rootDir=`app` lands at
        // `app/test/x.test.js`). The package.json main must therefore be the
        // file's path relative to rootDir *after* that placement, not a
        // naive pathlib.relative which would produce "../test/..." for
        // cross-parent entries and point outside /app on device.
        const mainPath =
          rootDir === '.' || entryJs.startsWith(rootDir + '/')
            ? pathlib.relative(rootDir, entryJs)
            : entryJs
        return output(
          buildDir,
          pathlib.join(rootDir, 'package.json'),
          JSON.stringify({
            name: pkg.name,
            version: pkg.version,
            type: pkg.type,
            main: `./${mainPath}`,
          }),
        )
      })

      // Locate the project's static/ directory by walking up from the
      // entry's parent until we find one. This handles both the
      // `mikro deploy` case (entry = app/main.ts → static at app/static/)
      // and the `mikro test` case (entry = app/**/x.test.ts → still
      // resolves the same app/static/ by walking up). The search stops
      // at the cwd so a user running from the wrong directory doesn't
      // accidentally pick up an unrelated static dir.
      const staticDir = (() => {
        let dir = pathlib.dirname(entry)
        while (dir !== '.' && dir !== '' && dir !== '/') {
          const candidate = pathlib.join(dir, 'static')
          if (existsSync(candidate)) return candidate
          const parent = pathlib.dirname(dir)
          if (parent === dir) break
          dir = parent
        }
        const rootCandidate = 'static'
        if (existsSync(rootCandidate)) return rootCandidate
        return null
      })()
      // On-device output path mirrors the source layout so read sites
      // like `readFile('/app/static/…')` resolve the same deployed path
      // regardless of which entry the build was run with.
      const staticOutputDir = staticDir ?? pathlib.join(rootDir, 'static')
      let staticFiles: string[] = []
      const writeStatic = defer(async () => {
        if (!staticDir) return
        try {
          const s = await stat(staticDir)
          if (s.isDirectory()) {
            staticFiles = await copyStaticDir(staticDir, buildDir, staticOutputDir)
          }
        } catch {
          // No static directory — nothing to copy
        }
      })

      return concat(
        of<BuildEvent>({
          type: 'settings',
          minify: options.minify,
          minifier,
          minifyLevel,
          logLevel,
          bundle: shouldBundle,
        }),
        phase(shouldBundle ? 'Bundling' : 'Tracing imports'),
        defer(() => rm(buildDir, {force: true, recursive: true})).pipe(ignoreElements()),
        writeFiles.pipe(ignoreElements()),
        writePackageJson.pipe(ignoreElements()),
        writeConfig.pipe(ignoreElements()),
        writeStatic.pipe(ignoreElements()),
        options.bytecode
          ? concat(
              phase('Compiling bytecode'),
              defer(() => {
                const keepPlain = new Set([
                  pathlib.resolve(buildDir, rootDir, 'package.json'),
                  pathlib.resolve(buildDir, rootDir, 'mikro.config.json'),
                  ...staticFiles,
                ])
                return compileJson(buildDir, keepPlain)
              }).pipe(ignoreElements()),
              defer(() => {
                const skip = new Set(staticFiles)
                return compileBytecode(buildDir, skip)
              }).pipe(ignoreElements()),
            )
          : EMPTY,
        defer(() => collectOutputFiles(buildDir)).pipe(mergeMap((events) => from(events))),
        of({type: 'done' as const}),
      )
    }),
  )
}

/**
 * Build multiple test files into one unbundled deploy tree. Writes a
 * synthesized package.json carrying a `tests` array that the firmware
 * supervisor uses as its sole "test mode" signal. Each path runs in
 * its own fresh runtime through one transport session.
 *
 * Always unbundled (bundling would defeat per-file isolation and
 * inflates deploy size via duplicated shared deps). No `main` field:
 * the supervisor drives iteration from `tests`, and leaving `main` out
 * makes the intent obvious to anyone reading the deployed bundle.
 */
export function buildTests(
  entries: string[],
  buildDir: string,
  options: {
    minify: boolean
    bytecode: boolean
    minifier?: Minifier
    minifyLevel?: MinifyLevel
    logLevel?: LogLevel
    rootDir: string
    /** Config environment to resolve from mikro.config.ts. Tests always build
     * in 'development'. */
    env?: MikroEnv
  },
): Observable<BuildEvent> {
  if (entries.length === 0) {
    return throwError(() => new Error('buildTests: no entries'))
  }
  // Same resolution as build(): prefix checks require cwd-relative paths.
  entries = entries.map((e) => pathlib.relative(process.cwd(), pathlib.resolve(e)))
  const rootDir = options.rootDir
  // Load config from the first entry's directory; assume all tests share a project.
  return defer(() => loadConfig(entries[0]!, options.env)).pipe(
    mergeMap((config) => {
      const minifier = options.minifier ?? config?.build?.minifier ?? 'esbuild'
      const minifyLevel = options.minifyLevel ?? config?.build?.minifyLevel ?? 'default'
      const logLevel = options.logLevel ?? config?.build?.logLevel ?? 'debug'
      const pureFuncs = LOG_LEVEL_PURE_FUNCS[logLevel]

      // Compute the on-device path for each entry, mirroring writeFiles'
      // co-locate logic: files outside rootDir land under it.
      const toOutputPath = (src: string): string => {
        const js = src.replace(/\.ts$/, '.js')
        return rootDir === '.' || js.startsWith(rootDir + '/') ? js : pathlib.join(rootDir, js)
      }
      const entryOutputs = entries.map(toOutputPath)

      const writeFiles = defer(() => trace(entries)).pipe(
        mergeMap(({warnings, fileList, sourcePathMap}) => {
          if (warnings?.length > 0) {
            return throwError(() => new Error([...warnings].map((w) => w.message).join('\n')))
          }
          return from(fileList).pipe(
            mergeMap(async (file) => {
              const sourcePath = sourcePathMap.get(file) ?? file
              return transform(file, await readFile(sourcePath), {
                minify: options.minify,
                minifier,
                minifyLevel,
                pureFuncs,
              })
            }),
            mergeMap(({path: filePath, contents}) => {
              const outputPath =
                rootDir === '.' || filePath.startsWith(rootDir + '/')
                  ? filePath
                  : pathlib.join(rootDir, filePath)
              return output(buildDir, outputPath, contents)
            }),
          )
        }),
      )

      const writePackageJson = defer(async () => {
        const pkgPath = pathlib.join(process.cwd(), 'package.json')
        const pkg = JSON.parse(await readFile(pkgPath, 'utf-8'))
        // Paths in the manifest are relative to rootDir, so the firmware
        // resolves them against /appfs/<rootDir>/ via MIK_LoadTests.
        const relToRoot = (out: string): string =>
          rootDir === '.' || out.startsWith(rootDir + '/') ? pathlib.relative(rootDir, out) : out
        const tests = entryOutputs.map((o) => `./${relToRoot(o)}`)
        return output(
          buildDir,
          pathlib.join(rootDir, 'package.json'),
          JSON.stringify({
            name: pkg.name,
            version: pkg.version,
            type: pkg.type,
            tests,
          }),
        )
      })

      const writeConfig = defer(() => {
        if (config === null) return EMPTY
        return output(
          buildDir,
          pathlib.join(rootDir, 'mikro.config.json'),
          JSON.stringify(serializeRuntimeConfig(config)),
        )
      })

      // Locate static/ by walking up from the first entry (same heuristic
      // as build()). All tests in one project share one static dir.
      const staticDir = (() => {
        let dir = pathlib.dirname(entries[0]!)
        while (dir !== '.' && dir !== '' && dir !== '/') {
          const candidate = pathlib.join(dir, 'static')
          if (existsSync(candidate)) return candidate
          const parent = pathlib.dirname(dir)
          if (parent === dir) break
          dir = parent
        }
        const rootCandidate = 'static'
        if (existsSync(rootCandidate)) return rootCandidate
        return null
      })()
      const staticOutputDir = staticDir ?? pathlib.join(rootDir, 'static')
      let staticFiles: string[] = []
      const writeStatic = defer(async () => {
        if (!staticDir) return
        try {
          const s = await stat(staticDir)
          if (s.isDirectory()) {
            staticFiles = await copyStaticDir(staticDir, buildDir, staticOutputDir)
          }
        } catch {
          // No static directory — nothing to copy
        }
      })

      return concat(
        phase('Tracing imports'),
        defer(() => rm(buildDir, {force: true, recursive: true})).pipe(ignoreElements()),
        writeFiles.pipe(ignoreElements()),
        writePackageJson.pipe(ignoreElements()),
        writeConfig.pipe(ignoreElements()),
        writeStatic.pipe(ignoreElements()),
        options.bytecode
          ? concat(
              phase('Compiling bytecode'),
              defer(() => {
                const keepPlain = new Set([
                  pathlib.resolve(buildDir, rootDir, 'package.json'),
                  pathlib.resolve(buildDir, rootDir, 'mikro.config.json'),
                  ...staticFiles,
                ])
                return compileJson(buildDir, keepPlain)
              }).pipe(ignoreElements()),
              defer(() => {
                const skip = new Set(staticFiles)
                return compileBytecode(buildDir, skip)
              }).pipe(ignoreElements()),
            )
          : EMPTY,
        defer(() => collectOutputFiles(buildDir)).pipe(mergeMap((events) => from(events))),
        of({type: 'done' as const}),
      )
    }),
  )
}

function extractImports(source: string): string[] {
  const imports = new Set<string>()
  for (const match of source.matchAll(/from\s*["']([^"']+)["']/g)) {
    imports.add(match[1]!)
  }
  for (const match of source.matchAll(/import\s*["']([^"']+)["']/g)) {
    imports.add(match[1]!)
  }
  return [...imports]
}

async function findFilesByExt(dir: string, ext: string): Promise<string[]> {
  const entries = await readdir(dir, {withFileTypes: true})
  const files: string[] = []
  for (const entry of entries) {
    const fullPath = pathlib.join(dir, entry.name)
    if (entry.isDirectory()) {
      files.push(...(await findFilesByExt(fullPath, ext)))
    } else if (entry.name.endsWith(ext)) {
      files.push(fullPath)
    }
  }
  return files
}

function compileBytecode(buildDir: string, skip?: Set<string>) {
  return defer(() => findFilesByExt(buildDir, '.js')).pipe(
    mergeMap((files) => from(files)),
    filter((file) => !skip?.has(pathlib.resolve(file))),
    mergeMap(async (file) => {
      const {compileBytecode: nativeCompileBytecode} = await loadNative()
      const source = await readFile(file, 'utf-8')
      const imports = extractImports(source)
      const relPath = pathlib.relative(buildDir, file)
      const moduleName = `/${relPath}`
      const moduleDir = pathlib.dirname(moduleName)
      const externals = imports.map((imp) => {
        if (imp.startsWith('.')) {
          return pathlib.join(moduleDir, imp)
        }
        return imp
      })
      const outFile = file.replace(/\.js$/, '.bjs')
      const bytecode = nativeCompileBytecode(source, moduleName, externals)
      await writeFile(outFile, bytecode)
      await unlink(file)
    }, 4),
  )
}

function compileJson(buildDir: string, keepPlain: Set<string>) {
  return defer(() => findFilesByExt(buildDir, '.json')).pipe(
    mergeMap((files) => from(files)),
    filter((file) => !keepPlain.has(pathlib.resolve(file))),
    mergeMap(async (file) => {
      const {jsonToBjson} = await loadNative()
      const json = await readFile(file, 'utf-8')
      const bjson = jsonToBjson(json)
      const outFile = file.replace(/\.json$/, '.bjson')
      await writeFile(outFile, bjson)
      await unlink(file)
    }, 4),
  )
}

async function copyStaticDir(
  srcDir: string,
  buildDir: string,
  outputBase: string,
): Promise<string[]> {
  const copied: string[] = []
  const entries = await readdir(srcDir, {withFileTypes: true})
  for (const entry of entries) {
    const srcPath = pathlib.join(srcDir, entry.name)
    const outRel = pathlib.join(outputBase, entry.name)
    if (entry.isDirectory()) {
      copied.push(...(await copyStaticDir(srcPath, buildDir, outRel)))
    } else {
      const data = await readFile(srcPath)
      await output(buildDir, outRel, data)
      copied.push(pathlib.resolve(buildDir, outRel))
    }
  }
  return copied
}

async function transform(
  path: string,
  contents: string | Buffer,
  options: {minify: boolean; minifier: Minifier; minifyLevel: MinifyLevel; pureFuncs?: string[]},
) {
  const parsedPath = pathlib.parse(path)
  if (parsedPath.ext === '.ts') {
    let code = stripTypeScriptTypes(contents.toString(), {mode: 'strip'})
    if (options.minify) {
      code = await minifyJs(code, options.minifier, options.minifyLevel, options.pureFuncs)
    }
    return {
      path: pathlib.format({
        ...parsedPath,
        base: pathlib.basename(parsedPath.base, parsedPath.ext) + '.js',
      }),
      contents: code,
    }
  }
  if (parsedPath.ext === '.js' || parsedPath.ext === '.mjs') {
    if (options.minify) {
      return {
        path,
        contents: await minifyJs(
          contents.toString(),
          options.minifier,
          options.minifyLevel,
          options.pureFuncs,
        ),
      }
    }
    return {path, contents}
  }
  if (parsedPath.base === 'package.json') {
    return {
      path,
      contents: JSON.stringify(trimPackageJson(JSON.parse(contents.toString()))),
    }
  }
  return {path, contents}
}

async function output(outputPath: string, filePath: string, contents: Buffer | string) {
  const dest = pathlib.join(outputPath, filePath)
  const dirname = pathlib.dirname(dest)
  await mkdir(dirname, {recursive: true})
  return writeFile(dest, contents)
}

type PackageJson = {
  name: string
  version: string
  exports?: Exports
  publishConfig?: {
    exports?: Exports
  }
}

type ExportConditions = {
  [condition: string]: Exports
}

type Exports = null | string | Array<string | ExportConditions> | ExportConditions

function trimPackageJson(pkgJson: PackageJson) {
  return {
    name: pkgJson.name,
    version: pkgJson.version,
    type: 'module',
    exports: pkgJson.publishConfig?.exports || pkgJson.exports,
  }
}
