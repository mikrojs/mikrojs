# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:

- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:

- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:

- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:

- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:

```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

## Project Overview

mikrojs is an experimental JavaScript runtime for microcontrollers, powered by QuickJS-NG. It targets ESP32 via ESP-IDF and provides JS APIs for filesystem (LittleFS), GPIO, WiFi, HTTP, and timers. The core runtime is a standalone C++ library (`packages/@mikrojs/native/`) with zero ESP-IDF dependencies, enabling host-side testing. The codebase has two sides: C/C++ firmware (C++23, C11) and a Node.js/TypeScript tooling workspace at the root. Inspired by [txiki.js](https://github.com/saghul/txiki.js).

## Repository Layout

The project is a pnpm workspace (pnpm 10.30.1, Node >= 24). Key areas:

- **`packages/@mikrojs/quickjs/`** — QuickJS-NG engine wrapper package
  - `deps/quickjs/` — QuickJS-NG source (git submodule)
  - `quickjs.cmake` — Shared CMake module (dual-mode: CMake target + variable exports for ESP-IDF)
  - `postinstall.js` — Builds `qjsc` bytecode compiler at install time
  - `index.js` — Exports `cmakePath`, `includePath`, `qjscPath`
- **`packages/@mikrojs/native/`** — Standalone C++ library + Node-API addon
  - `include/mikrojs/` — Public headers (`mikrojs.h`, `platform.h`, `private.h`, etc.)
  - `src/` — Portable source files (runtime, modules, timers, REPL, etc.)
  - `runtime/` — TypeScript runtime modules (bundled to bytecode during CMake build)
  - `addon/` — Node-API addon (binding.cpp, runtime_wrap, platform_node)
  - `scripts/` — Build scripts for bytecode generation (esbuild bundling + qjsc compilation)
  - `test/` — Host-side tests (run via CMake/ctest)
  - `cmake.js` — Exports `cmakePath`, `includePath`, `srcPath`, `scriptsPath`, `runtimePath`, `bytecodeCmakePath`
- **`packages/@mikrojs/firmware/`** — ESP-IDF firmware package (`@mikrojs/firmware`)
  - `components/mikrojs/` — ESP-IDF adapter (compiles standalone lib + ESP-specific modules)
- **`esp32/`** — Thin consumer of `@mikrojs/firmware`; contains `CMakeLists.txt`, `package.json`, `.envrc`, `.gitignore`, `main/main.cpp`, and `test/`
- **`packages/mikrojs/`** — CLI tool (`mikro`/`mikrojs` commands) built with Ink/React for terminal UI
- **`packages/@mikrojs/`** — Shared packages: `analyze-imports`, `eslint-plugin`, `esptool`. Board and driver packages can be added here as workspace members.
- **`scripts/`** — Workspace package (`@repo/scripts`) for repo tooling (agent detection, etc.)
- **`packages/create-mikrojs/`** — Project scaffolding tool (`npm create mikrojs`)
- **`examples/`** — Example projects (`blank`, `blinky`, `neopixel`, `pwm-led`, `schema`, `sntp`, `uart`, `wifi-fetch`, `wifi-access-point`, and more)

Git submodule: `packages/@mikrojs/quickjs/deps/quickjs` (QuickJS-NG). Clone with `--recurse-submodules` or run `git submodule update --init --recursive`.

ESP-IDF (>= 6.1) is a prerequisite for firmware builds. Install via [EIM](https://docs.espressif.com/projects/idf-im-ui/en/latest/): `eim install -i v6.1 -t all -n true`. The `idf.py` wrapper is provided by the monorepo-internal `@repo/idf.py` package (added to PATH via direnv in `esp32/`) and runs through `eim run` (no manual activation needed).

## Build & Development Commands

### Standalone library (host)

```sh
pnpm run build:lib               # CMake build of libmikrojs.a (includes bytecode generation)
pnpm run test:lib                # Run host-side C++ tests (ctest)
pnpm run coverage:lib            # Instrumented build + tests + gcovr report; enforces the
                                 # line/branch ratchet floors in packages/@mikrojs/native/gcovr.cfg
pnpm run coverage:lib:open       # Same, then opens the HTML details report in the browser
pnpm run clean:lib               # Remove CMake build directory
```

Or directly:

```sh
cd packages/@mikrojs/native
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

### Node.js / TypeScript side

```sh
pnpm install                    # Install all workspace dependencies (builds qjsc via postinstall)
pnpm dev                        # Watch mode: recompile TS, rebuild native addon, serve docs
pnpm build                      # Build everything (C++ + TypeScript + docs)
pnpm build:cpp                  # Build C++ only (libmikrojs.a + native addon)
pnpm build:ts                   # Build TypeScript packages only
pnpm build:docs                 # Build documentation site
pnpm typecheck                  # Type-check all packages (tsc --noEmit)
pnpm lint                       # Run eslint (with @mikrojs/eslint-plugin)
pnpm fmt                        # Format with oxfmt
pnpm fmt:check                  # Check formatting
pnpm precommit                  # Runs lint + fmt:check
pnpm check                      # Full check: build:cpp + build:ts + typecheck + lint + fmt:check + knip + tests
pnpm knip                       # Check for unused code/exports
pnpm vitest                     # Run JS/TS tests
```

### ESP-IDF / C++ side

Requires ESP-IDF >= 6.1 via [EIM](https://docs.espressif.com/projects/idf-im-ui/en/latest/). One-time setup:

```sh
eim install -i v6.1 -t all -n true
```

The `idf.py` wrapper (from the monorepo-internal `@repo/idf.py` package, added to PATH via direnv) runs `idf.py` through `eim run`. From `esp32/`:

```sh
direnv allow
idf.py set-target esp32c6  # or esp32, esp32s3, etc.
idf.py build flash monitor
pnpm test                   # on-device test suite
```

If tests error, try `cd test && idf.py fullclean` first.

**Important:** `sdkconfig.defaults` is only applied when `sdkconfig` does not exist. To apply changes from `sdkconfig.defaults`, delete `sdkconfig` and re-run `idf.py set-target <target>` (the target must be re-set since it's stored in `sdkconfig`).

Tests use doctest (Catch2-style `TEST_CASE` macros) and run on-device via serial monitor. Test files live in `packages/@mikrojs/firmware/components/mikrojs/test/` with tags: `[runtime]`, `[modules]`, `[timers]`, `[fs]`, `[gpio]`, `[pwm]`, `[wifi]`, `[http]`, `[stdio]`, `[repl]`, `[abort]`. You can build tests for a specific component with `idf.py -T xxxxx build`.

## Architecture

### Standalone Library (`packages/@mikrojs/native/`)

The core runtime is a platform-independent C++ library. It abstracts platform-specific operations via `MIKPlatform` (defined in `include/mikrojs/platform.h`):

```c
typedef struct MIKPlatform {
    int64_t (*get_boot_us)(void);       /* High-res timer, resets on deep sleep */
    int64_t (*get_rtc_us)(void);        /* RTC timer, survives deep sleep */
    uint32_t (*random)(void);
    void (*restart)(void);
    void (*yield)(void);
    size_t (*get_free_system_mem)(void);
    size_t (*get_min_free_system_mem)(void); /* All-time low watermark */
    size_t (*get_total_system_mem)(void);
    bool (*get_fs_info)(const char* label, size_t* total, size_t* used);
    void (*log)(int level, const char* tag, const char* fmt, ...);
    int (*stdout_write)(const void* buf, size_t len);
    int (*stderr_write)(const void* buf, size_t len);
    int (*stdin_read)(void* buf, size_t len);
    const char* (*get_device_id)(void);     /* Unique device ID, or NULL */
} MIKPlatform;
```

A default POSIX implementation (`src/platform_posix.cpp`) is provided for desktop builds. The ESP32 adapter provides an ESP-IDF implementation (`packages/@mikrojs/firmware/components/mikrojs/platform_esp32.cpp`).

### QuickJS Package (`packages/@mikrojs/quickjs/`)

Wraps the QuickJS-NG engine source as a workspace package. Provides:

- `quickjs.cmake` — Shared CMake module with include guard. For normal CMake consumers, creates a `quickjs` static library target. For ESP-IDF, exports `QUICKJS_SOURCES`, `QUICKJS_INCLUDE_DIR`, `QUICKJS_COMPILE_OPTIONS` variables.
- `qjsc` — Bytecode compiler, built at `pnpm install` time via `postinstall.js`. Path exported as `QJSC_EXECUTABLE` in CMake and `qjscPath` in JS.

### ESP-IDF Component (`packages/@mikrojs/firmware/components/mikrojs/`)

A thin adapter that:

- Resolves `@mikrojs/quickjs` and `@mikrojs/native` paths in CMake via `node resolve.js <query>` (see `packages/@mikrojs/firmware/resolve.js`)
- Compiles QuickJS and mikrojs sources directly (ESP-IDF requires `idf_component_register(SRCS ...)`)
- Runs its own bytecode generation (esbuild bundle + qjsc compile) during the build
- Provides `platform_esp32.cpp` (ESP-IDF platform implementation)
- Contains ESP-specific modules: GPIO (`mik_pin.cpp`), WiFi (`mik_wifi.cpp`), HTTP (`mik_http.cpp`), serial I/O (`mik_serial_io.cpp`), deploy protocol (`mik_deploy.cpp`), config protocol (`mik_config.cpp`)
- ESP-specific public API in `include/mikrojs_esp32.h`
- Backward-compatible wrapper headers in `include/` (forward to `mikrojs/` headers)
- `MIK_Main()` entry point: the default firmware bootstrap (NVS, LittleFS, JS runtime, REPL, deploy/config protocols)

### Custom Firmware Projects

External consumers can build custom firmware without cloning the monorepo by depending on published npm packages. A custom firmware project needs:

```
my-firmware/
├── package.json        # depends on @mikrojs/firmware + optional board/driver pkgs
├── CMakeLists.txt      # includes project.cmake from @mikrojs/firmware
└── main/
    ├── CMakeLists.txt
    └── main.cpp        # calls MIK_Main() or custom logic
```

The `@mikrojs/firmware` package provides `project.cmake` which handles ESP-IDF version validation, component discovery (scans dependencies for board/driver packages), and sdkconfig/partition defaults. Native board and driver packages are discovered automatically from `package.json` dependencies via their `cmake.js` exports.

Driver and board packages come in two flavors:

- **Native**: Have C/C++ code, export `cmake.js`, compile into firmware via `MIK_REGISTER_MODULE`/`MIK_REGISTER_BUILTIN`. Use this for QSPI displays, custom peripherals, anything needing direct hardware access.
- **Pure JS**: Regular npm packages using `mikrojs/spi`, `mikrojs/i2c`, etc. No native code, no `cmake.js`. Bundled and deployed with the user's app.

The `esp32/` directory in this repo is itself a thin consumer of `@mikrojs/firmware`, dogfooding the same workflow.

Platform-specific modules self-register via `MIK_REGISTER_MODULE()` at file scope and are lazily initialized on first import. No manual init calls in `main.cpp` are needed:

```c
// In mik_pin.cpp:
MIK_REGISTER_MODULE(pin, "native:pin", mik__pin_init, nullptr, nullptr)

// In mik_http.cpp (with loop consumer):
MIK_REGISTER_MODULE(http, "native:http", mik__http_init, mik__http_consume, mik__http_destroy)
```

### Runtime Lifecycle

`MIKRuntime` (defined in `private.h`) wraps a QuickJS `JSRuntime`/`JSContext` and owns a timer registry (`MIKTimerRegistry*`), registered loop consumers, and built-in JS objects. Created via `MIK_NewRuntime()` or `MIK_NewRuntimeOptions()` (with `mem_limit` and `stack_size`), driven by `MIK_Loop()`, freed via `MIK_FreeRuntime()`.

Environment variables are set via `MIK_SetEnvVar()` before calling `MIK_RebuildEnv()`. On ESP32, `MIK_LoadEnvFromNVS()` reads NVS and calls `MIK_SetEnvVar()` for each entry.

### Module System

Custom module loader (`modules.cpp`) supports:

- **ES6 modules** — standard JS modules loaded from filesystem
- **JSON imports** — `.json` files auto-wrapped in ``export default JSON.parse(`...`)``
- **Pre-compiled bytecode** — `.bjs` files compiled on host via `qjsc -b`
- **Built-in modules** — `mikrojs/` prefix, loaded from bytecode table in `builtins.cpp`

Module normalization resolves relative paths (`.` and `..`). Non-relative names pass through unchanged.

`import.meta` properties: `url`, `main`, `dirname`, `basename`, `path`, `env`.

### Adding Built-in Modules

Built-ins are registered in `builtins.cpp` via a `mik_builtin_t` table (name + pre-compiled bytecode data). The loader matches `mikrojs/` prefixed names and deserializes bytecode via `JS_ReadObject`.

### Adding Properties to Native Modules

When adding a new property to a `native:*` module (e.g. `native:sys`), two places must be updated:

1. The `_api_init` function (e.g. `mik__sys_api_init` in `mik_sys.cpp`) which sets the property on the namespace object via `JS_SetPropertyStr`
2. The corresponding exports array in `mikrojs.cpp` (e.g. `sys_exports[]`) which registers it as a module export via `JS_AddModuleExport`

Missing step 2 means the property exists on the internal namespace but `import {prop} from 'native:xxx'` resolves to `undefined`.

### Bytecode Generation Pipeline

Runtime module bytecode headers are generated during each CMake build (both standalone lib and ESP-IDF):

1. **Bundle**: `scripts/bundle-runtime.js` uses esbuild to bundle each TypeScript runtime module (`packages/@mikrojs/native/runtime/*.ts`) into a single JS file, marking `native:*` and `mikrojs/*` imports as external.
2. **Compile**: `scripts/compile-bytecode.sh` runs `qjsc` (from `@mikrojs/quickjs` postinstall) on each bundled JS file to produce a C header with a bytecode array.
3. **Include**: `builtins.cpp` includes the generated headers from `${CMAKE_CURRENT_BINARY_DIR}/gen/`.

The `qjsc` executable is built from the same QuickJS source as the runtime, guaranteeing bytecode version compatibility.

### Dependency Graph

```
mikrojs (CLI) → @mikrojs/native → @mikrojs/quickjs
@mikrojs/firmware → @mikrojs/native + @mikrojs/quickjs
```

### Error Handling

- `mik_throw_errno(ctx, err)` — throws a JS error from an errno value
- `mik_new_error(ctx, err)` — creates (but does not throw) a JS error from errno

### Logging and Propagating Errors (JS/TS)

The console prints an error value in full (name, message, extra fields, stack, cause chain), so always hand it the error object. This applies to runtime code, examples, docs, and skills alike; consumers copy what they see.

- Log: `console.error('WiFi connect failed:', result.error)`. Never `console.error('failed: %s', err.name)`, `.message`, `String(err)`, or a template string; each drops the rest of the error.
- Return: propagate the typed error as-is (`if (!r.ok) return r`), or add context with `err(new Error('download failed', {cause: r.error}))`. Never flatten to `{message: r.error.name}`.
- Panic: `result.orPanic('wifi required')` keeps the error as the cause. Never ``panic(`failed: ${result.error.name}`)``.
- Caught exception to Result: carry the thrown value as `cause`, not `e.message`.

### Feature Philosophy

Every feature starts at [minus 100 points](https://learn.microsoft.com/en-us/archive/blogs/ericgu/minus-100-points). Don't add APIs, globals, or abstractions speculatively. A feature must justify itself with a concrete use case before it gets added, regardless of how small or easy it would be to implement. On a microcontroller this is doubly important: every global costs RAM per runtime instance, every module increases binary size, and every API is a maintenance commitment on a platform where debugging is hard.

When considering WinterTC or other standard APIs, only implement what has a demonstrated need. "It's in the spec" or "it's trivial" are not sufficient justification.

### Naming Conventions (C/C++)

- Public C API: `MIK_PascalCase` (e.g. `MIK_NewRuntime`)
- Internal functions: `mik__snake_case` with double underscore (e.g. `mik__timers_init`)
- Internal macros: `MIK__CAPS` with double underscore
- Public macros: `MIK_CAPS`
- Internal structs/enums: `MIKPascalCase` (e.g. `MIKWifiStatus`, `MIKPwmState`)
- Native JS class names: `PascalCase` (e.g. `Spi`, `I2c`, `Wifi`, `Pwm`)

### Naming Conventions (TypeScript / Public API)

- **Acronyms as words**: Treat acronyms as regular words with only the first letter capitalized. This avoids `XMLHttpRequest`-style inconsistencies. Examples: `Spi`, `SpiOptions`, `I2c`, `Pwm`, `Wifi`, `WifiStatus`, `Sntp`, `SntpOptions`. Not `SPI`, `I2C`, `PWM`, `WiFi`.
- **Classes/interfaces**: `PascalCase` (e.g. `Spi`, `NeoPixel`, `WifiConnectionInfo`)
- **Error types**: `PascalCaseError` (e.g. `SpiError`, `WifiError`, `PwmError`)
- **Singleton instances**: `camelCase` (e.g. `wifi`, `sntp`)

### Export Conventions

All public modules use **named exports only** (no default exports). This keeps import syntax consistent across the entire API surface:

```ts
import {wifi} from 'mikro/wifi'
import {Spi} from 'mikro/spi'
import {pinMode, digitalWrite} from 'mikro/pin'
import {sntp} from 'mikro/sntp'
```

## Pre-commit Hooks

Lefthook runs different pre-commit checks depending on whether the committer is a human or an AI agent (detected via `std-env`, see `scripts/is-agent.js`).

- **Human developers**: fast staged-file `lint` + `fmt:check`
- **AI agents**: staged-file `lint` + `fmt:check`, plus full `build:cpp`, `build:ts`, `knip`, `test:js`, `test:lib`

## Code Style

### C/C++

Google base style, 4-space indent, 100-char line limit, left-aligned pointers (`int* ptr`), sorted includes, attached braces (K&R), no short if-statements on single line, indent case labels, namespace indentation.

### TypeScript

Linting via ESLint (with `@mikrojs/eslint-plugin`), formatting via `oxfmt`. Run `pnpm lint` and `pnpm fmt:check` to verify.
