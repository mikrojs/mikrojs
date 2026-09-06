import * as pathlib from 'node:path'

import {
  catchError,
  concat,
  defer,
  EMPTY,
  ignoreElements,
  map,
  merge,
  type Observable,
  of,
  shareReplay,
  startWith,
  Subject,
  switchMap,
  tap,
  throttle,
  throwError,
  timer,
} from 'rxjs'
import {exhaustMapWithTrailing} from 'rxjs-exhaustmap-with-trailing'

import type {LogLevel, Minifier, MinifyLevel} from '../../../_exports/index.js'
import {build} from '../build.js'
import {writeDevManifest} from '../configSchema.js'
import {collectFiles, loadEnvFiles, validateNvsKeys} from '../deploy.js'
import {formatDeployEvent} from '../deployProgress.js'
import {FirmwareIncompatibleError} from '../firmwareCompat.js'
import {getMikroDir, resolveProjectRoot} from '../projectRoot.js'
import {getPredeployCommands, runHooks} from '../runHooks.js'
import {type DeployEvent, DeviceTimeoutError, type ReplSession} from '../session.js'
import {createWatcher} from '../watcher.js'
import type {ReplHandle} from './replStateMachine.js'

// ── Types ──────────────────────────────────────────────────

export type DevStatus =
  | {type: 'building'}
  | {type: 'rebuilding'}
  | {type: 'checking'; command: string}
  | {type: 'deploying'; event: DeployEvent}
  | {type: 'watching'}
  | {type: 'error'; message: string}

export interface DevSessionState {
  status: DevStatus
}

export interface DevSessionHandle {
  state$: Observable<DevSessionState>
  close(): void
}

interface Trigger {
  force: boolean
  isInitial: boolean
}

// ── Factory ────────────────────────────────────────────────

export function createDevSession(options: {
  session: ReplSession
  entry: string
  forceDeploy: boolean
  minify: boolean
  bytecode: boolean
  watch: boolean
  /** Skip package.json `mikro.predeploy` hooks. */
  noHooks?: boolean
  /** REPL handle for the Ink-mode UI. Omit in agent mode. When present, the
   *  dev session drives the status line, disables the prompt during deploy,
   *  and subscribes to `repl.deploys$` for Ctrl+S triggers. */
  repl?: ReplHandle
  /** Extra deploy trigger source (e.g. an agent-mode stdin 'deploy' command).
   *  Merged with `repl?.deploys$` if both are present. */
  externalDeploys$?: Observable<{force: boolean}>
  minifier?: Minifier
  minifyLevel?: MinifyLevel
  logLevel?: LogLevel
  envFile?: string
  noAutoEnv?: boolean
  /** Drives `MIKRO_ENV` and the `.env.<mode>` file picked up by
   *  `loadEnvFiles`. `mikro dev` passes `'development'` (default);
   *  `mikro sim dev` passes `'simulator'`. */
  mode?: string
}): DevSessionHandle {
  const {
    session,
    repl,
    externalDeploys$,
    entry,
    forceDeploy,
    minify,
    bytecode,
    watch,
    noHooks,
    minifier,
    minifyLevel,
    logLevel,
    envFile,
    noAutoEnv,
    mode = 'development',
  } = options
  const buildDir = pathlib.join(getMikroDir(), 'build')
  const watchDir = process.cwd()
  const projectRoot = resolveProjectRoot()

  const watcher = watch ? createWatcher(watchDir) : null

  // Pre-deploy hooks gate each deploy. A trigger fires `hookStage$`, which
  // runs `mikro.predeploy` commands from package.json; on success it feeds
  // `deployNow$`, which is serialized by `exhaustMapWithTrailing` into the
  // real build+upload. `switchMap` on the hook stream means a fresh save
  // during a running `tsc`/`eslint` aborts the in-flight hook and restarts.
  // Build+upload itself is not cancelable — aborting a partial flash would
  // leave the device in an inconsistent state.
  const deployNow$ = new Subject<Trigger>()

  function hookStage$(trigger: Trigger): Observable<DevSessionState> {
    // Re-read package.json per trigger so edits to `mikro.predeploy` are
    // picked up without restarting the dev session.
    let commands: readonly string[]
    try {
      commands = noHooks ? [] : getPredeployCommands(projectRoot)
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      repl?.setError(message)
      return of({status: {type: 'error', message}} satisfies DevSessionState)
    }

    const advanceToDeploy$ = defer(() => {
      deployNow$.next(trigger)
      return EMPTY as Observable<DevSessionState>
    })

    const firstCommand = commands[0]
    if (firstCommand === undefined) return advanceToDeploy$

    const decoder = new TextDecoder()
    // Accumulate hook output so a failure can pin it into the footer
    // alongside the summary line. Captured per-run via defer so every
    // subscription gets a fresh buffer.
    return defer(() => {
      let output = ''
      return concat(
        of({status: {type: 'checking', command: firstCommand}} satisfies DevSessionState),
        runHooks(commands, projectRoot).pipe(
          tap((event) => {
            if (event.type === 'start') {
              repl?.setDeployStatus(`Checking: ${event.command}`)
            } else if (event.type === 'stdout' || event.type === 'stderr') {
              output += decoder.decode(event.chunk, {stream: true})
              // In agent/no-repl mode, stream chunks to stderr so
              // output is still visible live.
              if (!repl) process.stderr.write(event.chunk)
            }
          }),
          ignoreElements(),
        ),
        advanceToDeploy$,
      ).pipe(
        tap({subscribe: () => repl?.setDisabled(true)}),
        catchError((err: unknown) => {
          const message = err instanceof Error ? err.message : String(err)
          const trimmed = output.trimEnd()
          const pinned = trimmed.length > 0 ? `${trimmed}\n\n${message}` : message
          repl?.setFooterError(pinned)
          repl?.setDisabled(false)
          return of({status: {type: 'error', message}} satisfies DevSessionState)
        }),
      )
    })
  }

  function buildAndDeploy$(trigger: Trigger): Observable<DevSessionState> {
    const buildStatus: DevStatus = trigger.isInitial ? {type: 'building'} : {type: 'rebuilding'}

    return concat(
      // Build phase
      of({status: buildStatus} satisfies DevSessionState),
      // Dev-watch is always the development config env, regardless of which
      // `.env.<mode>` file `mode` selects.
      build(entry, buildDir, {
        minify,
        bytecode,
        minifier,
        minifyLevel,
        logLevel,
        env: 'development',
      }).pipe(
        ignoreElements(),
        // Name the phase: the device was already restarted above, so without
        // this the boot log reads as if the deploy went through.
        catchError((err: unknown) => {
          const detail = err instanceof Error ? err.message : String(err)
          return throwError(() => new Error(`Build failed:\n${detail}`))
        }),
      ),

      // Deploy phase
      defer(async () => {
        // The manifest (for ota.config()'s defaults) rides the file sync.
        await writeDevManifest({projectRoot, buildDir})
        const files = await collectFiles(buildDir)
        const envVars = [
          {key: 'MIKRO_ENV', value: mode, secret: false},
          ...(await loadEnvFiles({
            cwd: projectRoot,
            mode,
            envFile,
            noAutoEnv,
          })),
        ]
        validateNvsKeys(envVars)
        return {files, envVars}
      }).pipe(
        switchMap(({files, envVars}) =>
          session.deploy({files, envVars, force: forceDeploy || trigger.force, restart: true}).pipe(
            // Throttle only `checking` events so fast checksum scans don't flicker;
            // other event types pass through instantly via EMPTY.
            throttle((event) => (event.type === 'checking' ? timer(80) : EMPTY), {
              leading: true,
              trailing: true,
            }),
            tap((event) => {
              if (!repl) return
              const msg =
                event.type === 'checking'
                  ? `Checking [${event.index + 1}/${event.total}] ${event.file}`
                  : formatDeployEvent(event)
              if (msg !== null) repl.setDeployStatus(msg)
            }),
            map((event): DevSessionState => ({status: {type: 'deploying', event}})),
          ),
        ),
      ),

      // Done
      of({status: {type: 'watching'}} satisfies DevSessionState),
    ).pipe(
      tap({
        subscribe: () => repl?.setDisabled(true),
        complete: () => repl?.setDisabled(false),
      }),
      catchError((err: unknown) => {
        const message = err instanceof Error ? err.message : String(err)
        // The REPL handshake driver already surfaces firmware-incompat
        // errors; suppress here to avoid printing the same message twice.
        if (!(err instanceof FirmwareIncompatibleError)) {
          // Device timeouts already surface in the REPL footer (with the
          // troubleshooting link inlined); suppress the scrollback dup so
          // the same line doesn't appear twice. Other deploy failures
          // (build errors, hook stderr, MSG_ERR responses) still log.
          repl?.setError(message, {suppressLogEntry: err instanceof DeviceTimeoutError})
        }
        repl?.setDisabled(false)
        return of({status: {type: 'error', message}} satisfies DevSessionState)
      }),
    )
  }

  // External triggers: file changes (if watching) and explicit deploys
  // (Ctrl+S via repl, or agent-mode stdin command).
  const fileTriggers$: Observable<Trigger> = watcher
    ? watcher.changes$.pipe(map(() => ({force: false, isInitial: false})))
    : EMPTY
  const deployTriggers$ =
    repl && externalDeploys$
      ? merge(repl.deploys$, externalDeploys$)
      : (repl?.deploys$ ?? externalDeploys$ ?? EMPTY)
  const explicitTriggers$ = deployTriggers$.pipe(
    map((t): Trigger => ({force: t.force, isInitial: false})),
  )

  // Initial trigger injected at subscription time. Concat ensures it fires
  // before any external trigger is processed. We also issue a one-shot
  // `session.restart()` here so the device boots fresh and emits MSG_READY
  // — the REPL's `ready` transition would otherwise ride on the deploy
  // path's awaitReady$, leaving the session stuck in 'connecting' (with
  // troubleshooting UI) whenever a hook failure short-circuits the deploy.
  let hasKickedReady = false
  const initialTrigger$: Observable<Trigger> = defer(() => {
    if (!hasKickedReady) {
      hasKickedReady = true
      session.restart()
    }
    return of<Trigger>({force: forceDeploy, isInitial: true})
  })
  const triggers$ = concat(initialTrigger$, merge(fileTriggers$, explicitTriggers$))

  // Hook phase is cancelable via switchMap: a new save mid-`tsc` aborts the
  // in-flight hook run and restarts with the latest files. On success, the
  // trigger is pushed into deployNow$ for the (non-cancelable) build+upload.
  const hookPhase$ = triggers$.pipe(switchMap((trigger) => hookStage$(trigger)))

  // Build+upload phase: exhaust-with-trailing so rapid triggers during a
  // running deploy coalesce into a single trailing rerun.
  const deployPhase$ = deployNow$.pipe(
    exhaustMapWithTrailing((trigger) => buildAndDeploy$(trigger)),
  )

  // Order matters in the merge: deployPhase$ subscribes to deployNow$ before
  // hookPhase$ starts emitting, so a synchronous `deployNow$.next(...)` from
  // a no-hooks initial trigger isn't dropped.
  const state$ = merge(deployPhase$, hookPhase$).pipe(
    startWith({status: {type: 'building'}} satisfies DevSessionState),
    shareReplay({bufferSize: 1, refCount: true}),
  )

  return {
    state$,
    close() {
      watcher?.close()
      deployNow$.complete()
    },
  }
}
