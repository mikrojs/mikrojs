---
title: kv
description: Key-value storage with RTC and NVS backing stores
---

# kv

Key-value storage with two backing stores: RTC memory (volatile, fast) and NVS flash (persistent, wear-limited). Each backend lives at its own subpath so apps that only need one don't pay for the other's native code.

```ts twoslash
import {rtcStorage} from 'mikro/kv/rtc'
import {nvsStorage} from 'mikro/kv/nvs'
```

## Choosing a store

| Store        | Import path    | Survives deep sleep | Survives power off | Capacity | Write wear              |
| ------------ | -------------- | ------------------- | ------------------ | -------- | ----------------------- |
| `rtcStorage` | `mikro/kv/rtc` | Yes                 | No                 | ~2 KB    | None (RAM)              |
| `nvsStorage` | `mikro/kv/nvs` | Yes                 | Yes                | ~24 KB   | ~100k cycles per sector |

`rtcStorage` is plain RAM with no write-wear concerns. `nvsStorage` is flash-backed and survives power cycles, but has limited write endurance. Avoid high-frequency writes on flash; use `rtcStorage` for those and only flush to NVS when needed.

NVS keys are limited to 15 characters.

## rtcStorage

Store small values in RTC memory that persist across deep sleep cycles. Lost on hard reset or power loss. Values are [CBOR](/api/cbor)-encoded.

## nvsStorage

Store values in NVS flash that persist across power cycles. Keys limited to 15 characters. Values are [CBOR](/api/cbor)-encoded.

::: warning Storage is not encrypted
NVS values are stored as plaintext in flash. Anyone with physical access to the device can dump the flash and read them.
:::

### Storage ownership

NVS flash is divided into namespaces with distinct owners:

| Namespace           | Holds                                    | Owner       | Cleared by            |
| ------------------- | ---------------------------------------- | ----------- | --------------------- |
| `mik.env`/`mik.sec` | Environment variables (and secret flags) | the project | deploy sync           |
| `mik.kv`            | App key-value data                       | your app    | `nvsStorage.clear()`  |
| `mik.sys`           | Runtime-internal state                   | mikrojs     | `clear({full: true})` |

Everything you store through `nvsStorage` lives in `mik.kv`, fully separate from the runtime's namespaces. No key names are reserved: a key called `sys.foo` or `ota.url` is yours alone and collides with nothing. Nothing you write is affected by the runtime's own housekeeping in `mik.sys`, such as the OTA config-pairing keys a `mikro deploy` resets.

All namespaces draw from the same NVS partition entry pool. The runtime's own use is small and bounded, but a nearly full partition can fail writes regardless of namespace (NVS needs free space for its internal garbage collection); `set()` surfaces that as a `StorageFull` or `WriteFailed` error.

## Shared API

Both stores share the same `createValue` API.

### createValue(key, options?)

```ts
createValue<S extends StorableSchema, O extends KVOptions<S>>(
  key: string,
  options?: O,
): KVValue<InferOpts<S, O>>
```

Create a handle to a named value. Without a schema, values are untyped (`unknown`). Pass a schema for type-safe storage.

Values are [CBOR](/api/cbor)-encoded. Supported schema types: `s.number()`, `s.string()`, `s.boolean()`, `s.unknown()`, `s.optional()`, `s.array()`, `s.tuple()`, and `s.object()`. See [schema](/api/schema) for details.

::: warning `format` is not checked here
Bounds are checked on every read and write, so `s.number({max: 100})` rejects 500. Types, structure and required fields are checked too.

[`format`](/api/schema#format) is the exception: matching a URL or an email address needs regular expressions the device has no engine for, so `s.string({format: 'email'})` will not reject a malformed address here. Check that in your own code if you need it.
:::

```ts twoslash
import {nvsStorage} from 'mikro/kv/nvs'
import {rtcStorage} from 'mikro/kv/rtc'
import * as s from 'mikro/schema'
// ---cut---
const counter = rtcStorage.createValue('counter', {schema: s.optional(s.number())})
const brightness = nvsStorage.createValue('brightness', {schema: s.optional(s.number())})
const state = rtcStorage.createValue('state', {
  schema: s.optional(s.object({temp: s.number(), humidity: s.number()})),
})
```

With an error handler for corrupt data:

```ts twoslash
import {nvsStorage} from 'mikro/kv/nvs'
import * as s from 'mikro/schema'
const SensorReading = s.object({temp: s.number(), humidity: s.number()})
// ---cut---
const fallback = {temp: 0, humidity: 0}
const state = nvsStorage.createValue('state', {
  schema: SensorReading,
  initialValue: fallback,
  onReadError: (error) => {
    console.warn('corrupt:', error)
    return fallback
  },
})
```

### clear()

Erase all data in the store.

- `rtcStorage.clear(): void`
- `nvsStorage.clear(options?: {full?: boolean}): Result<void, KVError>`

`nvsStorage.clear()` erases app data only; the runtime's system store is never touched.

::: danger clear({full: true})
`nvsStorage.clear({full: true})` additionally erases the runtime's system store, which holds runtime-internal state. Intended for deliberate factory-reset flows; there is no undo. Both wipes are attempted even if one fails; an error result means at least one store may still hold data.
:::

The CLI's `mikro clean --full` shares the word but not the scope: it wipes environment variables, files, and the deployed app, and never touches key-value storage.

### info()

Returns storage usage information.

- `rtcStorage.info()` returns `{used, total, entries}` (bytes)
- `nvsStorage.info()` returns `{entries, used, total, free}` (entry slots; partition-wide)

## KVValue methods

### value.get()

```ts
get(): T | undefined
```

Read the value. Returns `undefined` if the key doesn't exist, or `initialValue` when one was given. On data that can't be read, `onReadError` decides what comes back (default: `undefined`); see [onReadError](#onreaderror) for what happens to the stored bytes in each case.

```ts twoslash
import {rtcStorage} from 'mikro/kv/rtc'
import * as s from 'mikro/schema'
const counter = rtcStorage.createValue('counter', {schema: s.optional(s.number())})
// ---cut---
const value = counter.get() ?? 0
```

### value.set(value)

```ts
set(value: T | undefined): Result<T, KVError>
```

Write a value. Returns the written value on success. Passing `undefined` deletes the key (same as `delete()`).

```ts twoslash
import {rtcStorage} from 'mikro/kv/rtc'
import * as s from 'mikro/schema'
const counter = rtcStorage.createValue('counter', {schema: s.optional(s.number())})
// ---cut---
counter.set(42).orPanic('store failed')
counter.set(undefined) // deletes the key
```

### value.update(updater)

```ts
update(updater: (value: T | undefined) => T | undefined): Result<T, KVError>
```

Read, transform, and write. Returns the updated value on success. Receives `undefined` if key is missing. Return `undefined` to delete.

```ts twoslash
import {rtcStorage} from 'mikro/kv/rtc'
import * as s from 'mikro/schema'
const counter = rtcStorage.createValue('counter', {schema: s.optional(s.number())})
// ---cut---
counter.update((n) => (n ?? 0) + 1)
```

### value.delete()

Remove the key from storage.

## onReadError

Called when stored data can't be read ([CBOR](/api/cbor) decode failure, [schema](/api/schema) mismatch, or a storage read failure). Receives a `KVError` (or `KVError` | [`SchemaError`](/api/schema#parse) when a schema is provided).

On decode failure (corrupt data), the key is deleted before calling the handler. Return a fallback to write back, or `undefined` to leave it empty.

On schema mismatch, the stored data is left intact (it decoded fine, just doesn't match the current schema). The handler will be called again on every subsequent read until the data is overwritten or deleted.

On a storage read failure (the flash read itself failed, usually starved of memory), the stored data is also left intact: the value is still there, this read just couldn't retrieve it. A later read that succeeds returns it unchanged.

Default: `() => undefined`.

## KVError

Errors returned by `set()` and `update()`, or passed to `onReadError`:

| Variant            | When                                                                      |
| ------------------ | ------------------------------------------------------------------------- |
| `StorageFull`      | RTC memory or NVS partition is full, or the value is too large            |
| `EncodeFailed`     | Value is not [CBOR](/api/cbor)-encodable, or the key is empty or too long |
| `WriteFailed`      | NVS open/commit failed (hardware error)                                   |
| `ValidationFailed` | Schema validation failed (has `path` field)                               |
| `Unknown`          | Native code returned an error not in the curated set (`code`)             |
