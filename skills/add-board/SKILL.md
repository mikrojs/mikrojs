---
name: add-board
description: Scaffold a mikrojs board package for a development board. Use this skill whenever the user wants to add support for a specific ESP32 development board, create a board definition with pin maps, configure sdkconfig defaults for a board, or set up pre-configured driver re-exports. Also trigger when the user mentions a specific board by name (e.g. "LilyGo T-Display", "Waveshare", "Seeed XIAO"), wants to define a board package, or asks how to make mikrojs work with their board. Even partial requests like "I have this ESP32-S3 board with a display" should trigger this skill.
---

# Create a mikrojs Board Package

Generate all files for a new board package that provides pin maps, sdkconfig defaults, and pre-configured driver re-exports for a specific development board.

When this skill triggers, gather the required information from the user, then generate all files listed below. The repo's `docs/develop/creating-boards.md` describes the architecture (native vs pure-JS boards, pin maps, sdkconfig). If the user has a demo/example code or schematic for the board, read it to extract pin mappings and hardware details. After generating files, guide the user through building and testing.

## What you need from the user

1. **Board name** (e.g. `xiao-esp32c6`)
2. **Vendor/package name** (e.g. `@mikrojs/some-vendor` for a vendor with multiple boards, or `@mikrojs/my-board` for a single board)
3. **Chip** (e.g. `esp32s3`, `esp32c6`)
4. **Pin map** (GPIO assignments for the board's peripherals)
5. **What drivers the board needs** (e.g. display driver, sensor driver)
6. **Any special sdkconfig** (PSRAM, flash size, CPU frequency)

## Package structure to generate

A board package can contain multiple boards. Each board gets its own directory:

```
packages/@mikrojs/{vendor}/
  package.json                            # mikrojs.boards manifest + exports
  cmake.js                                # exports componentPaths array
  tsconfig.json
  boards/
    {board-name}/                          # one directory per board
      CMakeLists.txt                      # ESP-IDF component
      builtins.cpp                        # MIK_REGISTER_BUILTIN
      sdkconfig.defaults                  # board-specific ESP-IDF config
      {board-name}.ts                     # runtime: re-exports drivers with pin config
```

## Files to generate

### 1. package.json

```json
{
  "name": "@mikrojs/{vendor}",
  "version": "0.0.0",
  "private": true,
  "description": "{vendor} board definitions for mikrojs",
  "license": "MIT",
  "type": "module",
  "exports": {
    "./{board-name}": "./boards/{board-name}/{board-name}.ts",
    "./cmake": "./cmake.js"
  },
  "engines": {
    "mikro": ">=0.0.0"
  },
  "dependencies": {
    "@mikrojs/driver-{driver}": "workspace:*"
  },
  "mikro": {
    "boards": {
      "./{board-name}": {
        "chip": "{chip}",
        "runtime": "./boards/{board-name}/{board-name}.ts",
        "sdkconfig": "./boards/{board-name}/sdkconfig.defaults",
        "description": "{description}"
      }
    }
  }
}
```

Key points:

- The `mikrojs.boards` keys are subpath exports (prefixed with `./`), tying the board declaration to the package's export map.
- `runtime` points to the TypeScript source that gets compiled to firmware bytecode.
- `sdkconfig` points to the board-specific ESP-IDF config defaults.
- `engines.mikrojs` declares the minimum firmware version.
- Only driver packages go in `dependencies`. Build-time deps (`@mikrojs/native`, `@mikrojs/quickjs`) are NOT needed.

### 2. cmake.js

```js
import {readFileSync} from 'node:fs'
import {dirname, join} from 'node:path'
import {fileURLToPath} from 'node:url'

const __dirname = dirname(fileURLToPath(import.meta.url))
const pkg = JSON.parse(readFileSync(join(__dirname, 'package.json'), 'utf8'))

export const componentPaths = Object.keys(pkg.mikrojs?.boards ?? {}).map((subpath) => {
  const name = subpath.startsWith('./') ? subpath.slice(2) : subpath
  return join(__dirname, 'boards', name)
})
```

This dynamically discovers board component paths from the `mikrojs.boards` manifest. Adding a new board to the manifest automatically includes it in builds.

### 3. tsconfig.json

```json
{
  "extends": "../../../tsconfig.json",
  "include": ["./boards/**/*.ts"],
  "compilerOptions": {
    "types": [],
    "noEmit": true,
    "moduleDetection": "auto",
    "customConditions": ["development"]
  }
}
```

### 4. Per-board CMakeLists.txt

```cmake
set(_BOARD_PKG_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
execute_process(
    COMMAND node -e "import {bytecodeCmakePath} from '@mikrojs/native/cmake'; import {cmakePath} from '@mikrojs/quickjs'; process.stdout.write(JSON.stringify({bytecodeCmake: bytecodeCmakePath, quickjsCmake: cmakePath, runtime: '${CMAKE_CURRENT_LIST_DIR}'}))"
    WORKING_DIRECTORY ${_BOARD_PKG_DIR}
    OUTPUT_VARIABLE _BOARD_JSON
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(JSON _BYTECODE_CMAKE GET "${_BOARD_JSON}" "bytecodeCmake")
string(JSON _QUICKJS_CMAKE GET "${_BOARD_JSON}" "quickjsCmake")
string(JSON _RUNTIME_DIR GET "${_BOARD_JSON}" "runtime")

include("${_QUICKJS_CMAKE}")

idf_component_register(
    SRCS "builtins.cpp"
    REQUIRES {driver_component_name} mikrojs
)

include("${_BYTECODE_CMAKE}")
mikrojs_generate_bytecode(
    RUNTIME_DIR "${_RUNTIME_DIR}"
    MODULES {board-name}
    MODULE_PREFIX "@mikrojs/{vendor}"
    SYMBOL_PREFIX "mik_{vendor_safe}"
    TARGET gen_{vendor_safe}_{board_safe}_bytecode
    WORKING_DIRECTORY "${_BOARD_PKG_DIR}"
)
add_dependencies(${COMPONENT_LIB} gen_{vendor_safe}_{board_safe}_bytecode)
target_include_directories(${COMPONENT_LIB} PRIVATE "${gen_{vendor_safe}_{board_safe}_bytecode_INCLUDE_DIR}")

mikrojs_force_include_builtins({builtin_id})
```

The `REQUIRES` field lists the ESP-IDF component names for all drivers this board uses. The component name is the driver's component directory name (typically `driver_<chipset>`, e.g. `driver_bme280`).

### 5. Per-board builtins.cpp

```cpp
#include "mikro/mikrojs.h"

#include "gen/mik_{vendor_safe}_{board_safe}.h"

MIK_REGISTER_BUILTIN({builtin_id},
                      "@mikrojs/{vendor}/{board-name}",
                      mik_{vendor_safe}_{board_safe}_bytecode,
                      mik_{vendor_safe}_{board_safe}_bytecode_size)
```

### 6. Per-board sdkconfig.defaults

Board-specific ESP-IDF settings. Common options:

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

### 7. Per-board runtime TypeScript ({board-name}.ts)

Re-exports drivers with board-specific pin configuration:

```typescript
import {createDisplay} from '@mikrojs/driver-{driver}'

export const pinMap = {
  // GPIO pin assignments for this board
} as const

// orPanic keeps the driver's error as the panic's cause, so the crash
// report shows what actually failed
export const display = createDisplay({
  // Board-specific configuration (pins, dimensions, etc.)
}).orPanic('display init failed')
```

## Adding a second board to an existing package

1. Create a new directory under `boards/`
2. Add `CMakeLists.txt`, `builtins.cpp`, `sdkconfig.defaults`, and `{name}.ts`
3. Add the export and `mikrojs.boards` entry to `package.json`
4. If the new board needs different drivers, add them to `dependencies` and `REQUIRES`

## Building and testing

1. Add the board package to the firmware's `package.json` dependencies
2. `pnpm install`
3. Build with the board selected:
   ```sh
   cd esp32
   rm sdkconfig
   idf.py set-target {chip}
   MIKROJS_BOARD={board-name} idf.py build flash monitor
   ```
4. Deploy a test app that imports from the board package

## Important notes

- The board directory name becomes the ESP-IDF component name.
- `cmake.js` reads `mikrojs.boards` from package.json, so adding a board there automatically includes it in builds.
- The `sdkconfig.defaults` is only applied when `sdkconfig` doesn't exist. Delete `sdkconfig` and re-run `set-target` after changing defaults.
- The board's runtime TypeScript is compiled to bytecode at firmware build time and embedded as a builtin. Users import it by the package's export name at runtime.
