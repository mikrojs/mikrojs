import {
  mkdirSync,
  mkdtempSync,
  readdirSync,
  readFileSync,
  realpathSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs'
import {tmpdir} from 'node:os'
import * as pathlib from 'node:path'

import {lastValueFrom} from 'rxjs'
import {afterEach, beforeEach, describe, expect, it} from 'vitest'

import {build, entryRootDir} from '../build.js'

function listFiles(dir: string): string[] {
  return (readdirSync(dir, {recursive: true}) as string[])
    .filter((p) => statSync(pathlib.join(dir, p)).isFile())
    .sort()
}

async function runBuild(entry: string, buildDir: string) {
  await lastValueFrom(build(entry, buildDir, {minify: false, bytecode: false}))
}

describe('entryRootDir', () => {
  it('returns the top-level directory of a nested entry', () => {
    expect(entryRootDir('app/main.ts')).to.equal('app')
    expect(entryRootDir('app/debug/test.ts')).to.equal('app')
  })

  it('returns "." for an entry at the project root', () => {
    expect(entryRootDir('main.ts')).to.equal('.')
  })
})

describe('build', () => {
  let originalCwd: string
  let tempDir: string

  beforeEach(() => {
    originalCwd = process.cwd()
    // realpath so absolute-entry paths compare cleanly against process.cwd()
    // (macOS tmpdir is a symlink; cwd always reports the real path)
    tempDir = realpathSync(mkdtempSync(pathlib.join(tmpdir(), 'build-')))
    writeFileSync(
      pathlib.join(tempDir, 'package.json'),
      JSON.stringify({name: 'fixture', version: '0.0.0', type: 'module'}),
    )
    writeFileSync(pathlib.join(tempDir, 'mikro.config.ts'), 'export default {}\n')
    mkdirSync(pathlib.join(tempDir, 'app', 'debug'), {recursive: true})
    writeFileSync(pathlib.join(tempDir, 'app', 'main.ts'), 'export const a = 1\n')
    writeFileSync(pathlib.join(tempDir, 'app', 'debug', 'test.ts'), 'export const b = 2\n')
    process.chdir(tempDir)
  })

  afterEach(() => {
    process.chdir(originalCwd)
    rmSync(tempDir, {recursive: true, force: true})
  })

  it('places package.json and mikro.config.json in the entry top-level dir', async () => {
    const buildDir = pathlib.join(tempDir, 'out')
    await runBuild('app/main.ts', buildDir)

    expect(listFiles(buildDir)).to.deep.equal([
      'app/main.js',
      'app/mikro.config.json',
      'app/package.json',
    ])
    const pkg = JSON.parse(readFileSync(pathlib.join(buildDir, 'app', 'package.json'), 'utf-8'))
    expect(pkg.main).to.equal('./main.js')
  })

  it('keeps package.json and mikro.config.json at the top-level dir for nested entries', async () => {
    const buildDir = pathlib.join(tempDir, 'out')
    await runBuild('app/debug/test.ts', buildDir)

    expect(listFiles(buildDir)).to.deep.equal([
      'app/debug/test.js',
      'app/mikro.config.json',
      'app/package.json',
    ])
    const pkg = JSON.parse(readFileSync(pathlib.join(buildDir, 'app', 'package.json'), 'utf-8'))
    expect(pkg.main).to.equal('./debug/test.js')
  })

  it('builds the same tree for absolute entry paths', async () => {
    const buildDir = pathlib.join(tempDir, 'out-abs')
    await runBuild(pathlib.join(tempDir, 'app', 'debug', 'test.ts'), buildDir)

    expect(listFiles(buildDir)).to.deep.equal([
      'app/debug/test.js',
      'app/mikro.config.json',
      'app/package.json',
    ])
  })

  // The schema ships in the manifest (app/mikro.app.json); leaking the ota
  // group here would put a copy of it in device RAM on every boot.
  it('strips the host-only ota group from the deployed runtime config', async () => {
    writeFileSync(
      pathlib.join(tempDir, 'mikro.config.ts'),
      `export default {wifi: {country: 'NO'}, otaConfigSchema: {kind: 'object', shape: {}}}\n`,
    )
    const buildDir = pathlib.join(tempDir, 'out-ota')
    await runBuild('app/main.ts', buildDir)

    const runtime = JSON.parse(
      readFileSync(pathlib.join(buildDir, 'app', 'mikro.config.json'), 'utf-8'),
    )
    expect(runtime.ota).toBeUndefined()
    expect(runtime['wifi.country']).toBe('NO')
  })

  it('builds the same tree for ./-prefixed and bare entry paths', async () => {
    const bareDir = pathlib.join(tempDir, 'out-bare')
    const dotDir = pathlib.join(tempDir, 'out-dot')
    await runBuild('app/debug/test.ts', bareDir)
    await runBuild('./app/debug/test.ts', dotDir)

    expect(listFiles(dotDir)).to.deep.equal(listFiles(bareDir))
  })

  it('reports an unresolvable import by its message, without an Error prefix', async () => {
    writeFileSync(pathlib.join(tempDir, 'app', 'main.ts'), "import 'lalala'\n")
    const buildDir = pathlib.join(tempDir, 'out')
    await expect(runBuild('app/main.ts', buildDir)).rejects.toThrow(
      /^Failed to resolve dependency "lalala"/,
    )
  })
})
