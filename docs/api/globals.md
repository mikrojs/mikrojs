---
title: Globals and Built-ins
description: Global APIs provided by the Mikro.js runtime
---

# Globals and Built-ins

These are the global APIs that Mikro.js adds on top of the standard QuickJS environment. For standard JavaScript built-ins (Array, Object, Promise, Map, Set, and the rest), see the [QuickJS-NG documentation](https://quickjs-ng.github.io/quickjs/).

## Timers

```ts
function setTimeout(callback: (...args: any[]) => void, ms?: number): number
function clearTimeout(id: number): void

function setInterval(callback: (...args: any[]) => void, ms?: number): number
function clearInterval(id: number): void
```

Standard timer functions. These drive the Mikro.js event loop.

## console

```ts
const console: {
  log(...args: unknown[]): void
  info(...args: unknown[]): void
  warn(...args: unknown[]): void
  error(...args: unknown[]): void
}
```

Logs to the serial console on device, or stdout in the simulator.

Pass error values as their own argument, never formatted into the message. An `Error` prints as its name and message, any extra own fields, the stack, and the `cause` chain; a plain `Result` error prints as an object with all its fields, and a `cause` field chains below it the same way:

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
const result = await wifi.connect({ssid: 'net', passphrase: 'pw'})
if (!result.ok) {
  console.error('WiFi connect failed:', result.error)
}
```

```
WiFi connect failed: { name: 'ConnectFailed', message: 'auth failed' }
```

Formatting with `%s`, `.name`, `.message`, or a template string throws those details away. See [Logging errors](/error-handling#logging-errors).

## btoa / atob

```ts
function btoa(data: string): string
function atob(data: string): string
```

Base64 encoding and decoding. `btoa` encodes a binary (latin1) string to base64. `atob` decodes a base64 string back.

```ts twoslash
btoa('Hello') // "SGVsbG8="
atob('SGVsbG8=') // "Hello"
```

`btoa` throws a `RangeError` if the string contains characters outside the latin1 range. `atob` throws a `SyntaxError` on invalid base64 input.

## TextEncoder / TextDecoder

```ts
class TextEncoder {
  encode(input?: string): Uint8Array
}

class TextDecoder {
  constructor(label?: 'utf-8' | 'utf8')
  decode(input?: Uint8Array, options?: {stream?: boolean}): string
}
```

UTF-8 encoding and decoding. Pass `{stream: true}` to hold a trailing
incomplete multi-byte sequence across calls, so codepoints split across
chunk boundaries decode correctly. A final `decode()` with no argument
flushes any held bytes (emitting U+FFFD if the held sequence is
incomplete).

```ts twoslash
const decoder = new TextDecoder()
// "café" = 63 61 66 C3 A9. Split inside 'é'.
const a = decoder.decode(new Uint8Array([0x63, 0x61, 0x66, 0xc3]), {stream: true})
const b = decoder.decode(new Uint8Array([0xa9]), {stream: true})
a + b // "café"
decoder.decode() // "". Nothing held.
```

### Differences from WHATWG

This is a deliberate subset of the [WHATWG `TextEncoder`/`TextDecoder`](https://encoding.spec.whatwg.org/) spec, trimmed for microcontroller use:

- **UTF-8 only.** The constructor accepts `'utf-8'` or `'utf8'` (case-insensitive) as a `label`, matching what WHATWG resolves to the UTF-8 decoder. Any other label throws a `RangeError`. No `utf-16le`, `windows-1252`, or other legacy encodings.
- **No `{fatal: true}`.** Invalid byte sequences always decode to `U+FFFD`. If you need to detect bad UTF-8, scan the output for `\uFFFD`.
- **No `{ignoreBOM}`.** A leading BOM is passed through. Strip manually if needed: `str.replace(/^\uFEFF/, '')`.
- **No `.encoding` / `.fatal` / `.ignoreBOM` getters.**
- **`decode()` input is `Uint8Array`-only.** Generic `ArrayBuffer` and other typed-array views are not accepted.
- **`TextEncoder.encodeInto()` is not implemented.**

## AbortController / AbortSignal

```ts
class AbortController {
  readonly signal: AbortSignal
  abort(reason?: unknown): void
}

class AbortSignal {
  readonly aborted: boolean
  readonly reason: unknown
  throwIfAborted(): void
  onabort: (() => void) | null
  addEventListener(type: 'abort', fn: () => void): void
  removeEventListener(type: 'abort', fn: () => void): void
  static abort(reason?: unknown): AbortSignal
  static timeout(ms: number): AbortSignal
  static any(signals: AbortSignal[]): AbortSignal
}
```

Cooperative cancellation. Most useful with [`request`](/api/http-request) for timeouts:

```ts twoslash
import {request} from 'mikro/http/request'

// Cancel if no response within 5 seconds
const result = await request('https://api.example.com/data', {
  signal: AbortSignal.timeout(5000),
})

if (!result.ok && result.error.name === 'Aborted') {
  console.error('Request timed out')
}
```

Manual cancellation:

```ts twoslash
import {request} from 'mikro/http/request'
// ---cut---
const controller = new AbortController()
setTimeout(() => controller.abort(), 3000)

const result = await request('https://slow.example.com', {
  signal: controller.signal,
})
```

On ESP32, aborting a request cancels the underlying HTTP task and frees its TLS buffers immediately, which can reclaim 16-40 KB of heap.

### AbortError / TimeoutError

The default abort reason is an `AbortError`, and `AbortSignal.timeout()` uses a `TimeoutError`. Both are `Error` subclasses:

```ts
const signal = AbortSignal.abort()
signal.reason instanceof AbortError // true
signal.reason.name // "AbortError"

const timeout = AbortSignal.timeout(1000)
// after timeout fires:
timeout.reason instanceof TimeoutError // true
timeout.reason.name // "TimeoutError"
```

::: info Difference from web standard
Browsers use `DOMException` with `name: "AbortError"` and `name: "TimeoutError"`. Mikro.js uses dedicated `AbortError` and `TimeoutError` classes instead, because `DOMException` is a DOM API with a large spec surface that doesn't belong on a microcontroller. Code that checks `error.name === "AbortError"` works the same way in both environments.
:::

`AbortSignal.any()` combines multiple signals:

```ts twoslash
// @noErrors
import {request} from 'mikro/http/request'
declare const url: string
// ---cut---
const timeout = AbortSignal.timeout(10000)
const manual = new AbortController()

const result = await request(url, {
  signal: AbortSignal.any([timeout, manual.signal]),
})
```

## import.meta

Available inside ES modules:

| Property               | Type                                  | Description                                                                                                                                               |
| ---------------------- | ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `import.meta.url`      | `string`                              | URL of the current module                                                                                                                                 |
| `import.meta.main`     | `boolean`                             | Whether this is the entry module                                                                                                                          |
| `import.meta.dirname`  | `string`                              | Directory of the current module                                                                                                                           |
| `import.meta.basename` | `string`                              | Filename of the current module                                                                                                                            |
| `import.meta.path`     | `string`                              | Full path of the current module                                                                                                                           |
| `import.meta.env`      | `Record<string, string \| undefined>` | Environment variables (from NVS on device). Returns `undefined` for missing keys. Prefer [`mikro/env`](/api/env) for explicit required/optional handling. |

`dirname`, `basename`, `path`, and `env` are only available for file-based modules (not built-in modules).

## Why isn't `request` a global?

In browsers and Node.js, `fetch` is a global function. In Mikro.js, you import [`request`](/api/http-request) from `mikro/http/request` instead. It returns a [`Result`](/api/result) instead of throwing on network errors and uses a slimmer request/response shape tuned for microcontrollers, so exposing it as a global under the name `fetch` would be misleading.

```ts twoslash
import {request} from 'mikro/http/request'

const result = await request('https://example.com')
if (!result.ok) {
  // network error, not an exception
}
```

## WinterTC compatibility

Mikro.js is **not** [WinterTC](https://wintertc.org/) compatible, and full compliance is not a goal. Where feasible, we align with WinterTC APIs (for example `TextEncoder`, `TextDecoder`, `AbortSignal`, and timers), but the runtime is designed for microcontrollers, not server-side JavaScript, and some APIs don't make sense in this context.
