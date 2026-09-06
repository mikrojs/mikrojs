---
title: Creating Drivers
description: Build a native driver package for a sensor or peripheral
---

# Creating Drivers

Drivers come in two flavors:

1. **Native drivers** have a C/C++ module compiled into firmware as an ESP-IDF component. They export `./cmake` and use `MIK_REGISTER_MODULE` / `MIK_REGISTER_BUILTIN`. Use this when you need direct hardware access (QSPI, DMA, custom peripherals).

2. **Pure JS drivers** are regular npm packages that use existing APIs like `mikro/spi` or `mikro/i2c`. No native code, no `cmake.js`, no ESP-IDF component. They're bundled and deployed with the user's app. Use this when existing APIs are sufficient.

This walkthrough covers a **native driver**, published as a single package `@my-scope/bme280` (substitute your own npm scope and chip). For pure JS drivers, create a regular npm package that imports from `mikro/spi`, `mikro/i2c`, or the other core APIs, and export a factory function.

## Package structure

```
@my-scope/bme280/
  package.json
  cmake.js
  bme280/                        # ESP-IDF component
    CMakeLists.txt
    idf_component.yml            # only if using managed components (see below)
    src/
      mik_bme280.cpp             # Native module implementation
  runtime/
    sensor/
      sensor.ts                  # JS wrapper (public API)
      types.ts                   # TypeScript types
    internal.d.ts                # Type declarations for native:@my-scope/bme280/sensor
  tsconfig.json
```

::: warning Component directory naming
The component directory name (`bme280`) becomes the ESP-IDF component name. Other components reference it with `REQUIRES bme280`. Choose a name and stick with it.
:::

## Step 1: package.json

```json
{
  "name": "@my-scope/bme280",
  "version": "0.0.1",
  "type": "module",
  "exports": {
    "./sensor": "./runtime/sensor/sensor.ts",
    "./cmake": "./cmake.js"
  },
  "dependencies": {
    "@mikrojs/native": "^0.14.0",
    "@mikrojs/quickjs": "^0.14.0"
  }
}
```

The `./cmake` export is how the build system discovers your component. The `./sensor` export is what app code imports; its specifier matches the builtin name registered in Step 4.

::: warning Dependencies are required
`@mikrojs/native` and `@mikrojs/quickjs` must be declared as dependencies of the driver package: the component's `CMakeLists.txt` resolves them with Node from the package directory (Step 5), so they have to be reachable from there.

Use a published version range as shown. `workspace:*` only resolves inside the mikro monorepo.
:::

## Step 2: cmake.js

```js
import {dirname, join} from 'node:path'
import {fileURLToPath} from 'node:url'

const __dirname = dirname(fileURLToPath(import.meta.url))

export const componentPath = join(__dirname, 'bme280')
```

**How discovery works:** the firmware's `project.cmake` runs `discover.js`, which walks the firmware project's `package.json` dependencies, loads each dependency's `./cmake` export, and adds every `componentPath` to `EXTRA_COMPONENT_DIRS`. Adding a driver to a firmware build means adding it to the firmware project's `package.json` dependencies. Nothing else.

## Step 3: Native module (mik_bme280.cpp)

The native module registers itself with the runtime via macros. Here is a minimal example that reads temperature over I2C:

```cpp
#include <mikrojs/mikrojs.h>
#include <mikrojs/platform.h>

#include "driver/i2c_master.h"

// Forward declaration
static JSValue js_bme280_read(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv);

// Module init callback: called on module evaluation to set export values
static int mik__bme280_module_init(JSContext* ctx, JSModuleDef* m) {
    JSValue exports = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, exports, "read",
        JS_NewCFunction(ctx, js_bme280_read, "read", 1));
    return JS_SetModuleExport(ctx, m, "default", exports);
}

// Module constructor: lazily called on first import; declares exports
static JSModuleDef* mik__bme280_init(JSContext* ctx) {
    JSModuleDef* m = JS_NewCModule(ctx, "native:@my-scope/bme280/sensor", mik__bme280_module_init);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "default");
    return m;
}

// Self-register: the runtime discovers this at link time
MIK_REGISTER_MODULE(bme280, "native:@my-scope/bme280/sensor", mik__bme280_init, nullptr, nullptr)

static JSValue js_bme280_read(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    // ... I2C reads, return { temperature, humidity, pressure }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "temperature", JS_NewFloat64(ctx, 22.5));
    JS_SetPropertyStr(ctx, obj, "humidity", JS_NewFloat64(ctx, 45.0));
    JS_SetPropertyStr(ctx, obj, "pressure", JS_NewFloat64(ctx, 1013.25));
    return obj;
}
```

Key points:

- `MIK_REGISTER_MODULE` uses a constructor attribute to add the module to a linked list at startup. No manual registration step needed.
- The two trailing `nullptr` arguments are optional event-loop hooks. The first is a _consume_ callback that the runtime calls on every event loop iteration once the module is imported; use it to drain completion queues from interrupt handlers or background tasks into JS callbacks (see `mik_http.cpp` for an example). The second is a _destroy_ callback called on runtime shutdown to release anything the module allocated. Pass `nullptr` for hooks you don't need.
- The native module name must be package-qualified: `native:<your-package-name>/<module>` (here `native:@my-scope/bme280/sensor`). It is literally `native:` plus the builtin specifier registered in Step 4. The bare `native:mikro/*` namespace is reserved for the core runtime, so a driver reaching a core peripheral imports, for example, `native:mikro/i2c`. This rule is enforced at build time; an unqualified name fails to compile. The public API goes through the TypeScript wrapper.

## Step 4: Bytecode builtin registration

The TypeScript wrapper is compiled to bytecode and registered as a builtin. Add the registration in the same file or a separate `.cpp`:

```cpp
#include <mikrojs/mikrojs.h>

// Generated by mikrojs_generate_bytecode() during build. Names follow
// <SYMBOL_PREFIX>_<module>: header gen/<prefix>_<module>.h, symbols
// <prefix>_<module>_bytecode and <prefix>_<module>_bytecode_size.
#include "gen/bme280_sensor.h"

MIK_REGISTER_BUILTIN(bme280, "@my-scope/bme280/sensor",
                     bme280_sensor_bytecode, bme280_sensor_bytecode_size)
```

When user code does `import {readSensor} from '@my-scope/bme280/sensor'`, the loader finds this builtin by name.

::: warning Import specifiers are exact
A builtin is resolved only by the exact registered name. There is no package-root or index resolution for builtins: `import ... from '@my-scope/bme280'` fails with a resolution error. The import must be the full registered name, here `@my-scope/bme280/sensor`.
:::

## Step 5: CMakeLists.txt

```cmake
# Resolve the CMake helpers from this package's dependencies. quickjs.cmake
# sets QJSC_EXECUTABLE (required by the bytecode pipeline; it is scoped per
# component, so every driver component includes it itself). The bytecode
# helpers live in @mikrojs/native's mikrojs_bytecode.cmake (bytecodeCmakePath).
set(_PKG_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
execute_process(
    COMMAND node -e "import {bytecodeCmakePath} from '@mikrojs/native/cmake'; import {cmakePath} from '@mikrojs/quickjs'; process.stdout.write(JSON.stringify({bytecodeCmake: bytecodeCmakePath, quickjsCmake: cmakePath}))"
    WORKING_DIRECTORY ${_PKG_DIR}
    OUTPUT_VARIABLE _PATHS_JSON
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _resolve_result
)
if(NOT _resolve_result EQUAL 0)
    message(FATAL_ERROR "Failed to resolve @mikrojs/native and @mikrojs/quickjs. Run 'pnpm install' first.")
endif()
string(JSON _QUICKJS_CMAKE GET "${_PATHS_JSON}" "quickjsCmake")
string(JSON _BYTECODE_CMAKE GET "${_PATHS_JSON}" "bytecodeCmake")
include("${_QUICKJS_CMAKE}")

idf_component_register(
    SRCS "src/mik_bme280.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver mikrojs
)

# Declare this component's package namespace. MIK_REGISTER_MODULE /
# MIK_REGISTER_BUILTIN enforce at build time that names are qualified with it
# (native:@my-scope/bme280/<module> and @my-scope/bme280/<module>),
# so a package can't claim or shadow another's native: name.
target_compile_definitions(${COMPONENT_LIB} PRIVATE "MIK_PACKAGE_NAME=\"@my-scope/bme280\"")

include("${_BYTECODE_CMAKE}")

# Generate bytecode from TypeScript runtime modules
mikrojs_generate_bytecode(
    RUNTIME_DIR "${_PKG_DIR}/runtime"
    MODULES sensor
    MODULE_PREFIX "@my-scope/bme280"
    SYMBOL_PREFIX "bme280"
    TARGET gen_sensor_bytecode
    WORKING_DIRECTORY "${_PKG_DIR}"
)
add_dependencies(${COMPONENT_LIB} gen_sensor_bytecode)
target_include_directories(${COMPONENT_LIB} PRIVATE "${gen_sensor_bytecode_INCLUDE_DIR}")

# Force the linker to keep the self-registered module and builtin. The
# arguments are the registration ids: the first argument passed to
# MIK_REGISTER_MODULE / MIK_REGISTER_BUILTIN in Step 3 and 4.
mikrojs_force_include_modules(bme280)
mikrojs_force_include_builtins(bme280)
```

`mikrojs_generate_bytecode` runs the full pipeline: esbuild bundles each TypeScript module (`<module>/<module>.ts` under `RUNTIME_DIR`), then `qjsc` compiles it to a C header containing the bytecode array.

::: warning idf_component.yml is optional
Only add an `idf_component.yml` if the component depends on packages from the IDF Component Registry. If it has no managed dependencies, ship no manifest at all. In particular, a `dependencies:` key with no entries under it (for example, only comments) parses as null and fails the build with "Invalid field 'dependencies': Input should be a valid dictionary".
:::

## Step 6: TypeScript wrapper

`runtime/sensor/sensor.ts`:

```ts
import type {Result} from 'mikro/result'
import {ok, err} from 'mikro/result'
import type {Reading} from './types.js'

// Import the native module (compiled into firmware)
import native from 'native:@my-scope/bme280/sensor'

export const SensorError = {
  ReadFailed: (cause: unknown) => ({name: 'ReadFailed', cause}) as const,
}
export type SensorError = ReturnType<(typeof SensorError)[keyof typeof SensorError]>

export function readSensor(bus: number, address?: number): Result<Reading, SensorError> {
  // eslint-disable-next-line @mikrojs/no-try-catch -- native boundary: C code throws
  try {
    return ok(native.read(bus, address ?? 0x76))
  } catch (e) {
    return err(SensorError.ReadFailed(e))
  }
}
```

The native module throws, so the wrapper is where that becomes a `Result`. Return a tagged union keyed on `name` rather than a bare `Error`, so callers can switch on the variant. Keep the thrown value as `cause` instead of copying its `message` into a string: the console prints the cause with its own message, fields and stack, so nothing is lost when a caller logs the error (see [Logging errors](/error-handling#logging-errors)). See [Defining errors](/error-handling#defining-errors) for the pattern and [`no-try-catch`](/eslint-rules#no-try-catch) for why the boundary needs the disable comment.

`runtime/sensor/types.ts`:

```ts
export interface Reading {
  temperature: number
  humidity: number
  pressure: number
}
```

## Step 7: internal.d.ts

Type declarations for the native module so TypeScript can check imports:

```ts
declare module 'native:@my-scope/bme280/sensor' {
  interface Native {
    read(
      bus: number,
      address: number,
    ): {
      temperature: number
      humidity: number
      pressure: number
    }
  }
  const native: Native
  export default native
}
```

## Step 8: tsconfig.json

```json
{
  "extends": "../../../tsconfig.base.json",
  "compilerOptions": {
    "rootDir": "runtime",
    "outDir": "dist"
  },
  "include": ["runtime"]
}
```

## Testing

1. Add the driver to your firmware project's `package.json` (during development a `file:` path or `workspace:*` also works):

   ```json
   {
     "dependencies": {
       "@my-scope/bme280": "^0.0.1"
     }
   }
   ```

2. Run `pnpm install`, then build and flash:

   ```sh
   cd esp32
   rm sdkconfig && idf.py set-target esp32c6
   idf.py build flash monitor
   ```

3. Test from the REPL:

   ```js
   import {readSensor} from '@my-scope/bme280/sensor'
   const result = readSensor(0)
   console.log(result)
   ```

## Important notes

- **Firmware builtin imports are external**: esbuild marks `mikro/*`, and any package that exports `./cmake` (your driver included), as external during bundling. They resolve at runtime in the firmware rather than being bundled into the app.
- **Declare `mikro` as an optional peer**: if a published driver declares a `mikro` peerDependency (useful for types during development), mark it optional; drivers don't import `mikro` at runtime. A required peer also breaks preview installs: semver ranges never match prerelease versions, so with `autoInstallPeers` enabled a stable copy of `mikro` and its `@mikrojs/*` dependencies gets installed alongside the preview.

  ```json
  "peerDependenciesMeta": {
    "mikro": {"optional": true}
  }
  ```

- **NVS namespaces**: if your driver stores state in NVS, use your own namespace. Namespace names starting with `mik.` are reserved for the runtime.
- **C vs C++ source files**: Compile vendor C libraries (like sensor SDKs) as `.c` files. Mixing them into `.cpp` files can cause issues with `-Werror` due to C++ strictness.
- **DMA buffers**: If your peripheral uses DMA (SPI displays, for example), allocate buffers with `heap_caps_malloc(size, MALLOC_CAP_DMA)`. DMA requires internal SRAM; PSRAM will not work.
