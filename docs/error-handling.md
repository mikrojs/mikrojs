---
title: Error Handling
description: How Mikro.js uses typed Results instead of exceptions
---

# Error Handling in Mikro.js

Mikro.js uses typed results instead of exceptions. If you're coming from a `try`/`catch` background, this guide explains why and how to work with them.

## The problem with try/catch

Consider reading a sensor value:

```ts twoslash
// @noErrors
declare function analogRead(pin: number): number
// ---cut---
// Hypothetical API that throws on failure (NOT how Mikro.js works)
const value = analogRead(34)
console.log(`Sensor: ${value}`)
```

This compiles without any warnings. TypeScript says `analogRead` returns `number`, so `value` is `number`. But at runtime, this can crash your program. If pin 34 isn't a valid ADC pin, the function throws. Nothing in the type signature tells you this can happen. You have to _know_ to add error handling.

You might wrap it in try/catch:

```ts twoslash
// @noErrors
declare function analogRead(pin: number): number
// ---cut---
try {
  const value = analogRead(34)
  console.log(`Sensor: ${value}`)
} catch (err) {
  //      ^?
  //
  //
  console.error('Something went wrong:', err)
}
```

`err` is `unknown`. Is it a string? An `Error`? Does it have a `.message`? A `.code`? You end up writing `if (err instanceof Error)` checks, guessing at property names, or logging the raw value.

And this uncertainty is **contagious**. Any function that calls `analogRead` might also throw, but there's no way to know from its signature. You either wrap everything in try/catch defensively, or you don't and hope for the best. Neither option is good.

## How Mikro.js handles errors

Every Mikro.js function that can fail returns a [`Result`](/api/result):

```ts twoslash
import {analogRead} from 'mikro/pin'

const result = analogRead(34)
//    ^?
//
//
if (result.ok) {
  console.log(`Sensor: ${result.value}`)
} else {
  switch (result.error.name) {
    //                 ^?
    //
    //
    //
    case 'InvalidAdcPin':
      console.error('Pin 34 is not a valid ADC pin')
      break
    case 'AdcInitFailed':
      console.error('ADC hardware failed:', result.error)
      break
    default:
      console.error('Read failed:', result.error)
  }
}
```

The key differences:

- The type signature is honest: `analogRead` returns `Result<number, PinError>`, so you can see it might fail.
- The error is typed: `PinError` is a union of specific variants, and TypeScript tells you exactly which errors are possible and what data each one carries.
- No try/catch needed: errors are values you check, not exceptions you catch.

## The Result type

A `Result<T, E>` is either an `Ok` holding a value of type `T`, or an `Err` holding an error of type `E`:

```ts twoslash
import {ok, err} from 'mikro/result'

const success = ok(42)
//    ^?
//
//
const failure = err('oops')
//    ^?
//
//
```

### Early returns

The most common pattern is checking and returning early:

```ts twoslash
import {ok, err, type Result} from 'mikro/result'
import {analogRead, type PinError} from 'mikro/pin'
declare const PublishError: {
  NetworkError: (message: string) => {name: 'NetworkError'; message: string}
}
declare type PublishError = {name: 'NetworkError'; message: string}
declare function publish(value: number): Promise<Result<void, PublishError>>
// ---cut---
async function readAndPublish(pin: number) {
  const reading = analogRead(pin)
  if (!reading.ok) return reading

  const published = await publish(reading.value)
  if (!published.ok) return published

  return ok()
}
```

Notice how TypeScript infers a return type equivalent to `Result<void, PinError | PublishError>`, capturing every way the function can fail. Each early return propagates its own error type, and the caller sees exactly which errors are possible.

If `publish` were to throw instead, that would be a panic: an unexpected bug, not a recoverable error. See [What about exceptions?](#what-about-exceptions) below.

### Transforming values

Use `.map()` to transform the success value without unwrapping:

```ts twoslash
import {analogReadMillivolts} from 'mikro/pin'
// ---cut---
const voltage = analogReadMillivolts(34).map((mv) => mv / 1000)
```

Use `.andThen()` to chain operations that themselves return Results:

```ts twoslash
import {pinMode, digitalWrite} from 'mikro/pin'
// ---cut---
const result = pinMode(4, 'OUTPUT').andThen(() => digitalWrite(4, 1))
```

### Converting to panics

When failure is unrecoverable and you want to crash explicitly, use `.orPanic()`:

```ts twoslash
import {analogRead} from 'mikro/pin'
// ---cut---
const value = analogRead(34).orPanic('ADC must work at this point')
```

Use it sparingly, only when you've decided that failure at this point means the program cannot continue.

### Exhaustive matching

Use `.match()` to handle both cases:

```ts twoslash
import {analogRead} from 'mikro/pin'
// ---cut---
const message = analogRead(34).match({
  ok: (value) => `Reading: ${value}`,
  err: (error) => `Failed: ${error.name}`,
})
```

## Defining errors

Mikro.js modules define their errors as tagged unions, keyed on `name`. The recommended shape is a const factory object plus a `ReturnType` extraction:

```ts twoslash
const SensorError = {
  NotConnected: () => ({name: 'NotConnected'}) as const,
  ReadFailed: (message: string) => ({name: 'ReadFailed', message}) as const,
  OutOfRange: (value: number, min: number, max: number) =>
    ({name: 'OutOfRange', value, min, max}) as const,
}

type SensorError = ReturnType<(typeof SensorError)[keyof typeof SensorError]>
```

This gives you:

- **Constructor functions:** `SensorError.ReadFailed("timeout")` creates `{name: "ReadFailed", message: "timeout"}`
- **A union type:** `SensorError` is `{name: "NotConnected"} | {name: "ReadFailed", message: string} | {name: "OutOfRange", value: number, min: number, max: number}`
- **Exhaustive switching:** TypeScript enforces that you handle every variant in a `switch` block, or via [`matchError`](/api/result#matcherror)

Errors are plain data, not class instances. Cheap to create and easy to inspect.

For variants with only one or two call sites, an inline `err({name: 'X' as const, ...})` literal is fine; the factory pattern is just discoverability over the same shape.

## Logging errors

Pass the error as its own argument to `console.error` or `console.warn`, after a short context string:

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
const result = await wifi.connect({ssid: 'net', passphrase: 'pw'})
if (!result.ok) {
  console.error('WiFi connect failed:', result.error)
}
```

The console prints the whole value. A `Result` error prints as an object with every field, and a `cause` field chains below it:

```
WiFi connect failed: { name: 'ConnectFailed', message: 'auth failed' }
  [cause]: { name: 'Dhcp', message: 'no lease' }
```

An `Error` instance prints its name and message, any extra own fields, the stack, and the `cause` chain, nested under it:

```
update failed: Error: download failed { code: 7 }
    at update (main.js:12:3)
  [cause]: { name: 'Timeout', message: 'no response after 5000ms' }
```

Do not format the error into the message. Each of these keeps one piece and throws away the rest:

```ts
// Keeps the name, loses message, fields, stack and cause
console.error('WiFi connect failed: %s', result.error.name)
console.error(`WiFi connect failed: ${result.error.message}`)
console.error('WiFi connect failed: ' + String(result.error))
```

A failure with no error value is logged the same way, with the value that describes it:

```ts
console.error('checkin returned status:', response.status)
```

### Passing errors along

The same rule applies when an error moves instead of being logged. Keep the original value:

- **Returning** from a function: propagate the typed error as-is (`if (!r.ok) return r`), or add context and keep the original as `cause`: `err(new Error('download failed', {cause: r.error}))`. Never flatten to `{message: r.error.name}`.
- **Panicking**: `result.orPanic('WiFi required')` keeps the error as the panic's cause, and the crash report prints it. Do not build the message yourself with ``panic(`failed: ${result.error.name}`)``.
- **Converting a caught exception** to a `Result`: carry the thrown value as `cause`, not `e.message`. See [Creating drivers](/develop/creating-drivers) for the native-boundary pattern.

## What about exceptions?

Exceptions still exist in Mikro.js, but they're treated as **panics**: unrecoverable bugs, not expected error conditions.

The distinction:

|                  | Result errors                                | Exceptions (panics)                                |
| ---------------- | -------------------------------------------- | -------------------------------------------------- |
| **When**         | Expected failures (bad pin, network timeout) | Bugs (null dereference, out of memory)             |
| **Typed?**       | Yes, visible in function signature           | No, `unknown`                                      |
| **Handle?**      | Yes, check `result.ok`                       | No, let it crash and read the stack trace          |
| **Stack trace?** | No, you know what failed from the type       | Yes, you need it because the failure is unexpected |

If you see an exception in Mikro.js, it means something is broken, not that a sensor read failed.

Note that some standard JavaScript functions can still throw, such as `JSON.parse()` with invalid input, `atob()` with a malformed string, or `decodeURIComponent()` with invalid sequences. These are not Mikro.js APIs, so they follow standard JavaScript behavior. Catch them at the call site and return a `Result`: see [`no-try-catch`](/eslint-rules#no-try-catch) for the pattern and the lint disable it needs.

When you need to intentionally crash (for example on missing required configuration), use `env.require` from [`mikro/env`](/api/env) or `panic` from [`mikro/sys`](/api/sys):

```ts twoslash
import {env} from 'mikro/env'

// env.require() panics with a clear message if the variable is not set
const ssid = env.require('WIFI_SSID')
```

The runtime restarts the device after an uncaught exception, so a deployed app recovers on its own. Use [`onPanic`](/config#onpanic) to control this: `delay` sets how long the device waits before acting (use a longer delay during development to give yourself time to recover the device, a shorter one in production), and `mode` chooses whether it restarts right away or deep-sleeps to save power first:

```ts twoslash
import {defineConfig} from 'mikro'

export default defineConfig({
  onPanic: {mode: 'restart', delay: 500},
})
```

This is the right response to a panic: restart and try again. Result errors, on the other hand, are handled in your code and never trigger a restart.

::: warning Battery-powered devices
Auto-restart only self-heals if the panic was transient. If the same panic recurs every boot (a coding bug, a permanently missing sensor, bad NVS state), the device crash-loops and a battery-powered one will drain itself doing so. Save `.orPanic()` for boot-time invariants you can fix by reflashing; for anything that might fail in the field, handle the `Result`.
:::

## Lint rules

Mikro.js includes an [ESLint plugin](/eslint-rules) that enforces these conventions. For example, the [`no-unhandled-result`](/eslint-rules#no-unhandled-result) rule warns you when a `Result` is ignored:

```ts
pinMode(4, 'OUTPUT')
// error: Result must be handled (@mikrojs/no-unhandled-result)
```

Other rules flag `throw`, `try/catch`, `Promise.reject()`, and `.catch()` to keep error handling consistent. See [ESLint Rules](/eslint-rules) for the full list.

## Quick reference

```ts
import {ok, err, matchError, type Result} from 'mikro/result'

// Check a result
if (result.ok) {
  result.value // the success value
} else {
  result.error // the typed error
}

// Early return on error
const result = analogRead(pin)
if (!result.ok) return result

// Log an error: context string, then the error itself (never %s / .name / .message)
if (!result.ok) console.error('read failed:', result.error)

// Add context while returning: keep the original as cause
if (!result.ok) return err(new Error('sensor read failed', {cause: result.error}))

// Transform success
result.map((value) => value * 2)

// Chain fallible operations
result.andThen((value) => anotherOperation(value))

// Handle both cases
result.match({
  ok: (value) => {
    /*…*/
  },
  err: (error) => {
    /*…*/
  },
})

// Get value or crash
const value = result.orPanic('must succeed here')

// Switch on error variant
switch (result.error.name) {
  case 'InvalidAdcPin': {
    /*…*/
  }
  case 'AdcInitFailed': {
    /*…*/
  }
}

// Define custom errors
const MyError = {
  NotFound: (id: string) => ({name: 'NotFound', id}) as const,
  Timeout: (ms: number) => ({name: 'Timeout', ms}) as const,
}
type MyError = ReturnType<(typeof MyError)[keyof typeof MyError]>

// Use in function signatures
function myFunction(): Result<string, MyError> {
  if (somethingWrong) return err(MyError.Timeout(5000))
  return ok('done')
}
```
