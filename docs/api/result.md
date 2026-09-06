---
title: result
description: Result type for typed error handling
---

# result

```ts twoslash
import {ok, err, matchError} from 'mikro/result'
import type {Result, OkResult, ErrResult} from 'mikro/result'
```

The `Result` type is how Mikro.js represents operations that can fail. Instead of throwing exceptions, functions return `Result<T, E>` where `T` is the success value and `E` is a typed error.

See the [Error Handling](/error-handling) guide for a full introduction.

## Functions

### ok()

Creates a successful result.

```ts
function ok(): OkResult<void>
function ok<T>(value: T): OkResult<T>
```

```ts twoslash
import {ok} from 'mikro/result'
// ---cut---
const result = ok(42)
result.ok // true
result.value // 42
```

### err()

Creates a failed result.

```ts
function err<E>(error: E): ErrResult<E>
```

```ts twoslash
import {err} from 'mikro/result'
// ---cut---
const result = err({name: 'NotFound' as const})
result.ok // false
result.error // {name: 'NotFound'}
```

### matchError()

Exhaustive dispatch on a tagged error's `name` field. The handler map must cover every variant or TypeScript fails the call site at compile time.

```ts
function matchError<E extends {name: string}, R>(
  error: E,
  handlers: {[K in E['name']]: (error: Extract<E, {name: K}>) => R},
): R
```

```ts twoslash
type SensorError = {name: 'ReadFailed'; message: string} | {name: 'NotConnected'}
// ---cut---
import {matchError} from 'mikro/result'

declare const e: SensorError
const summary = matchError(e, {
  ReadFailed: (e) => `read failed: ${e.message}`,
  NotConnected: () => 'sensor not connected',
})
```

## Defining a typed error union

Mikro.js modules expose their failures as tagged unions on `name`. The recommended pattern is a const factory object plus a `ReturnType` extraction:

```ts twoslash
const SensorError = {
  ReadFailed: (message: string) => ({name: 'ReadFailed', message}) as const,
  NotConnected: () => ({name: 'NotConnected'}) as const,
}
type SensorError = ReturnType<(typeof SensorError)[keyof typeof SensorError]>

// Usage: `err(SensorError.ReadFailed('timeout'))` yields
// ErrResult<{name: 'ReadFailed'; message: string}>
```

The factory is plain JS — no runtime indirection, no closure overhead beyond the variant functions themselves. Inline `err({name: 'X' as const, ...})` literals are fine too when a variant only has one or two call sites.

## Result methods

Both `OkResult` and `ErrResult` share the same method interface. The behavior depends on whether the result is Ok or Err.

### .ok

`true` for success, `false` for failure. Use this to narrow the type:

```ts twoslash
// @noErrors
import {pinMode} from 'mikro/pin'
// ---cut---
const result = pinMode(20, 'OUTPUT')
if (!result.ok) {
  console.error('pinMode failed:', result.error)
  return
}
// result.value is available here
```

### .value / .error

- `OkResult` has `.value: T` and no `.error`
- `ErrResult` has `.error: E` and no `.value`

### .map(fn)

Transforms the success value, leaving errors untouched.

```ts twoslash
import {ok, err} from 'mikro/result'
// ---cut---
const doubled = ok(21).map((v) => v * 2) // OkResult<number>, value: 42
const failed = err('oops').map((v) => v * 2) // ErrResult<string>, unchanged
```

### .mapErr(fn)

Transforms the error value, leaving successes untouched.

```ts twoslash
import {err} from 'mikro/result'
// ---cut---
const result = err('oops').mapErr((e) => ({message: e}))
// ErrResult<{message: string}>
```

### .andThen(fn)

Chains a function that itself returns a `Result`. Useful for sequencing operations that can each fail.

```ts twoslash
import {ok} from 'mikro/result'
import {pinMode} from 'mikro/pin'
// ---cut---
const result = ok(20)
  .andThen((pin) => pinMode(pin, 'OUTPUT'))
  .andThen(() => ok('ready'))
```

### .match(handlers)

Exhaustive pattern matching on the result.

```ts twoslash
import {pinMode} from 'mikro/pin'
const result = pinMode(20, 'OUTPUT')
// ---cut---
const message = result.match({
  ok: (value) => `Got: ${value}`,
  err: (error) => `Failed: ${error.name}`,
})
```

### .orPanic(message)

Returns the value if Ok, or crashes the program with the given message if Err. The error is included as the `cause`.

```ts twoslash
import {pinMode} from 'mikro/pin'
// ---cut---
const value = pinMode(20, 'OUTPUT').orPanic('Failed to set pin mode')
```

Use this when failure is truly unrecoverable (for example during setup).

## Types

### Result\<T, E\>

```ts
type Result<T, E> = OkResult<T> | ErrResult<E>
```
