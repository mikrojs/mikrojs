---
title: Developing for Microcontrollers
description: Practical tips for writing TypeScript on ESP32
---

# Developing for Microcontrollers

Writing TypeScript for an ESP32 is different from writing it for Node.js or a browser. This page covers the constraints you'll encounter and how to work within them.

## Memory

The chips Mikro.js targets have 384-520 KB of internal SRAM, all of it shared between WiFi/BLE stacks, the QuickJS runtime, your app, and any loaded modules. How much you have free after boot depends on the chip and which radios are active. Measure your own board with `/mem` rather than budgeting from the chip's datasheet total. Some boards include PSRAM (2-8 MB of external RAM), which lets larger apps allocate beyond the SRAM budget.

### Check your usage

Run `/mem` in the REPL to see the current usage:

```
Available: 239.3 KB

Usage:
  QuickJS:  78.4 / 317.7 KB used   (25%)
  System:  118.4 / 362.9 KB used   (33%, peak 33%)
  Reserved: 8.0 KB (memReserved for native subsystems)
```

**Available** is what's left to use. **System peak** is the highest usage seen since boot.

### What uses memory

| What                          | Typical cost                            |
| ----------------------------- | --------------------------------------- |
| Runtime startup               | ~75-80 KB (fixed)                       |
| Each imported module          | ~1 KB+ per module, more for larger ones |
| WiFi connection               | ~35 KB while the radio is up            |
| HTTP request                  | ~20-25 KB during the request            |
| HTTPS request (TLS handshake) | ~25-40 KB during the handshake, on top  |
| Your app code                 | Proportional to size                    |

WiFi + HTTPS is the biggest consumer. A simple blinky uses almost nothing, but a WiFi app doing HTTPS can leave you with only ~100KB free. The request and handshake costs are transient: they come back once the response is read, so the numbers to add up are the WiFi steady cost plus the largest burst your app produces.

### Practical tips

- Connect to WiFi once and stay connected, rather than reconnecting per request.
- Talk to local endpoints over plain HTTP when RAM is scarce. TLS costs up to ~40 KB per handshake.
- If your app never uses WiFi (it networks over a cellular modem, say), build [custom firmware](./develop/custom-firmware#overriding-defaults) with `CONFIG_MIKROJS_WIFI=n`. The WiFi driver takes about 20 KB of RAM at boot even when the radio is never started.
- Concatenating strings in a loop allocates a new string every iteration. Push to an array and `.join()` once.
- Load modules only when you need them, with a dynamic import:

```ts
if (sensorConnected) {
  const {readSensor} = await import('./sensor.js')
  readSensor()
}
```

### Unload modules you're done with

Some apps run in distinct phases: sense, then send, then sleep. Each one pulls in code you won't need again, and you can free that memory once the phase is over.

Wrap the phase in `withUnload()`: it loads the module, runs your callback with it, then unloads it and reclaims its memory:

```ts twoslash
// @filename: phases/modem.ts
export function runPhase(input: unknown) {
  return {ok: true}
}
// @filename: app.ts
import {withUnload} from 'mikro/module'
declare const input: unknown
// ---cut---
const status = await withUnload(import('./phases/modem.js'), (modem) => modem.runPhase(input))
// The modem phase is unloaded and its heap reclaimed by the time
// this resolves.
```

Inside the callback you use the module exactly as you normally would: `modem.runPhase(...)`, destructuring, and so on. When the callback finishes (or throws), the module is unloaded, along with any transitive dependency whose only importer was this module.

In practice this can free tens of KB. One modem phase of seven modules freed about 66 KB.

A few things to keep in mind:

- Non-standard module behavior: standard JavaScript modules evaluate once and stay loaded for the life of the program; nothing in the language unloads them. `withUnload()` evicts the module from the loader cache, so a later `import` re-evaluates it from scratch. Avoid it on modules that run one-time setup or hold singleton state assuming a single evaluation.
- Don't return the module's exports from the callback. Whatever the callback returns outlives the module, so returning one of its functions (or an object holding one) keeps the whole phase alive and frees little or nothing. Return plain values (numbers, strings, simple objects), or nothing.
- A module stays loaded if something else still imports it. Only modules whose last importer was the disposed phase are unloaded; a shared module that another live part of your app imports is left intact.
- Built-in modules can't be unloaded. If the import resolves to a `mikro/*` or `native:*` module, `withUnload()` throws.
- It pays off for large phases. Unloading one or two small modules barely helps; the win comes when a phase loads a big tree of code you're done with.

If none of this applies, just use normal dynamic imports. The runtime frees values on its own as they go out of scope. `withUnload()` is for memory-tight apps that work in phases.

### Configuring `memReserved`

The runtime reserves part of the heap for native subsystems (WiFi, TLS, drivers). You can tune this in `mikro.config.ts`:

```ts twoslash
import {defineConfig} from 'mikro'

export default defineConfig({
  memReserved: 16 * 1024,
})
```

- **64 KB** (default): good for WiFi + HTTPS apps.
- **16-32 KB**: if your app doesn't use the network stack (for example a sensor logger over UART, or a BLE-only app).
- **Don't go below ~8 KB.** Native subsystems always need some room.

If JavaScript runs out of memory, you get an `InternalError`. It's catchable, but the handler runs with almost no memory left, so the realistic options are logging a short pre-built message and restarting. If the system heap runs out (native code), you get a hard reset. The reserve keeps these from happening at the same time.

Two things worth knowing about how the reserve works:

- `memReserved` is measured once, against free heap at boot. It covers the native subsystems' future demand from that point, and it caps how much JavaScript may allocate; it does not partition the heap, and native code is not held to it.
- Treat it as choosing which side runs out first, not as a hard guarantee. JavaScript hits its limit with a catchable error while native subsystems keep their room to work. Large native bursts (a TLS handshake, a WiFi reconnect) are transient, which is why the 64 KB default is enough for most network apps even though the subsystems' combined peak can exceed it.

### Finding memory issues

If you're hitting out-of-memory crashes, use [`memoryUsage()`](/api/sys#memoryusage) to narrow it down:

```ts twoslash
import {memoryUsage} from 'mikro/sys'
// ---cut---
function memCheckpoint(label: string): void {
  const m = memoryUsage()
  const freeKb = ((m.heapTotal - m.heapUsed) / 1024).toFixed(1)
  const sysMinFreeKb = (m.systemMinFree / 1024).toFixed(1)
  console.debug(`[mem] ${label}: ${freeKb} KB free (sys min-free ${sysMinFreeKb} KB)`)
}
```

Place calls before and after suspect operations. The `systemMinFree` field is especially useful: it tracks the all-time lowest free memory, catching transient spikes that have since been freed.

Once you've found the hot spot, the fix is usually one of:

- Build data incrementally, instead of holding the full result and its source in memory at once.
- Shrink the payload: fewer fields, shorter keys, less history.
- If your native subsystems don't need the full reservation, lower `memReserved`.
- Free references early by scoping variables tightly, or nulling them before the next big allocation. QuickJS is reference counted, so the memory comes back the moment the last reference drops.

## Deep sleep for battery projects

Deep sleep drops power draw from ~80-160mA to ~10-20µA. The tradeoff: the CPU resets on wake, so your program starts from scratch.

```ts twoslash
import {deepSleep} from 'mikro/sleep'
import {rtcStorage} from 'mikro/kv/rtc'
import * as s from 'mikro/schema'

const readings = rtcStorage.createValue('readings', {
  schema: s.optional(s.number()),
})
readings.update((n) => (n ?? 0) + 1)

// Do your work (read sensor, send data, etc.)

// Sleep for 5 minutes
deepSleep({timer: 5 * 60 * 1000})
```

Use [`rtcStorage`](/api/kv) to persist state across sleep cycles. It survives deep sleep but not power loss.

## Don't block the event loop

Mikro.js runs a single-threaded event loop, like Node.js. WiFi, timers, and everything else depends on it.

- Break long computations into chunks with `await sleep(0)` to yield
- Don't use tight `while(true)` loops without an `await` inside
- WiFi events are delivered through the event loop; blocking it delays reconnection handling

## Filesystem

On device, the filesystem uses [LittleFS](https://github.com/littlefs-project/littlefs) on a flash partition. Files persist across reboots and deep sleep.

Flash memory has limited write cycles (~100,000 per sector), so avoid writing on every loop iteration. Buffer data in RAM and write periodically, or use [`rtcStorage`](/api/kv) for smaller, frequently changing values.

## Error handling matters more

On a server, an unhandled exception crashes the process and a supervisor restarts it. On a microcontroller, an unhandled exception can leave your device stuck, potentially miles from anyone who can reset it.

This is why Mikro.js uses [typed Results](/error-handling) instead of exceptions. Every expected failure is visible in the type signature.

```ts twoslash
import {wifi} from 'mikro/wifi'
declare const ssid: string
declare const passphrase: string
// ---cut---
const result = await wifi.connect({ssid, passphrase})
if (!result.ok) {
  console.error('WiFi failed:', result.error)
  // Retry, fall back, or sleep and try later
}
```

For truly unrecoverable errors (missing config, hardware failure at boot), use `.orPanic()`. The runtime always restarts the device after an uncaught exception so apps in the field can self-heal; tune the grace window and recovery behavior via [`onPanic`](/config#onpanic).

## Host simulator

`mikro sim dev` runs your code on your computer. It goes through the same build pipeline as device mode and reloads on file changes.

Hardware-dependent builtins (WiFi, HTTP, GPIO, I2C, SPI, sleep) are stubbed. When your code first uses one, the simulator creates a default stub file in your project's `sim/` directory that you can customize:

```
my-project/
  app/main.ts
  sim/
    wifi.stub.ts     <- auto-created when your code uses WiFi
    pin.stub.ts      <- auto-created when your code uses GPIO
  package.json
```

Each stub is a module that exports a class implementing the builtin's interface (`SimWifi`, `SimPin`, `SimI2c`, and the rest from `mikro/sim`), replacing the native module in the simulator. The scaffolded stub implements every method with a sensible default; edit the bodies you care about:

```ts twoslash
// sim/pin.stub.ts
import {ok} from 'mikro/result'
import type {SimPin} from 'mikro/sim'

export class Pin implements SimPin {
  pinMode(pin: number, mode: number) {
    console.log('[sim] pinMode:', pin, mode)
    return ok()
  }
  digitalWrite(pin: number, value: number) {
    console.log('[sim] digitalWrite:', pin, value)
    return ok()
  }
  digitalRead(_pin: number) {
    return 0
  }
  analogRead(_pin: number, _attenuation: number) {
    return ok(2048)
  }
  analogReadMillivolts(_pin: number, _attenuation: number) {
    return ok(1650)
  }
}
```

Stubs run inside QuickJS, the same runtime as your app, so Node.js APIs like `fetch` and filesystem access are not available.

Builtins that work without stubs: `console`, `kv`, `nvs_kv` (in-memory storage), timers, `sys`.
