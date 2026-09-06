---
title: http/server
description: HTTP server
---

# http/server

```ts twoslash
import {createServer} from 'mikro/http/server'
```

Serve HTTP from the device: config portals (in WiFi AP mode), small JSON APIs, dashboards, or webhook receivers. The server is low-level: a request handler returns a response, and routing is left to you (branch on `req.method` and `req.url`). Bring up the network first with [`wifi`](/api/wifi) (station or access point) before listening.

::: warning Plaintext, no built-in auth
The server speaks plain HTTP with no TLS and no authentication. Treat `req.url`, `req.headers`, and the request body as untrusted: validate and bound anything you read from them, and add your own auth if the device is reachable beyond a trusted network. Response header names and values containing CR, LF, or NUL are dropped to prevent header injection, but you remain responsible for the body you return.
:::

## Functions

### createServer(handler, options?)

```ts
function createServer(handler: Handler, options?: ServerOptions): Server
```

`createServer` is lazy: it does no I/O and never fails. Nothing binds until you call [`listen`](#server-listen-options). The handler runs once per request on the event loop and returns a [`ServerResponse`](#serverresponse) (or a promise of one).

```ts twoslash
import {createServer} from 'mikro/http/server'
// ---cut---
const server = createServer((req) => {
  const path = req.url.split('?')[0]
  if (req.method === 'GET' && path === '/') return {status: 200, body: 'Hello'}
  return {status: 404, body: 'Not found'}
})

const result = server.listen({port: 80})
if (!result.ok) {
  console.error('listen failed:', result.error)
}
```

Requests are handled one at a time (the underlying server blocks until your handler responds), which keeps memory bounded on-device.

### server.listen(options)

```ts
listen(options: {port: number}): Result<void, ServerError>
```

Binds the port and starts serving. Synchronous, and returns a [`Result`](/api/result) so a bind failure (for example when the port is taken) is handled explicitly. Only one server can listen per device; a second `listen` returns `AlreadyListening`.

### server.close()

```ts
close(): void
```

Stops serving and releases the port. Safe to call when not listening. You can `listen` again afterward.

### Routing

There is no built-in router; routing is left to your app. Strip the query string and branch on `req.method` and the path:

```ts twoslash
import {createServer, type ServerResponse} from 'mikro/http/server'
// ---cut---
const server = createServer((req): ServerResponse => {
  const path = req.url.split('?')[0]
  if (req.method === 'GET' && path === '/status') return {status: 200, body: 'ok'}
  if (req.method === 'GET' && path?.startsWith('/echo/')) {
    return {status: 200, body: decodeURIComponent(path.slice('/echo/'.length))}
  }
  return {status: 404, body: 'Not found'}
})
```

`req.text()`, `req.json()`, and `req.bytes()` each return a [`Result`](/api/result) and drain the body once; `req.body` is the underlying single-shot async iterable. The request body is buffered up to [`maxBodySize`](#serveroptions) before the handler runs; an oversized body surfaces as a [`BodyTooLarge`](/api/http-request#requesterror) error when you read it.

::: warning Header lookup is by name only
`req.headers` exposes `get(name)` / `getAll(name)` but **no** way to list all headers. ESP-IDF's HTTP server can fetch a header by name but cannot enumerate them. Look up the headers you need (`content-type`, `authorization`, custom `x-*` headers, and so on) by name. Lookups are case-insensitive.
:::

## Streaming responses

Return an async iterable (typically an async generator) as the `body` to stream a response chunk-by-chunk instead of buffering it. Chunks are sent with backpressure (the generator is only pulled as fast as the client drains), and pulling stops automatically if the client disconnects. Each chunk costs one event-loop round-trip (copy, send, ack), so prefer a handful of larger chunks over many tiny ones.

```ts twoslash
import {createServer} from 'mikro/http/server'
import {sleep} from 'mikro/sleep'
// ---cut---
async function* ticks() {
  for (let i = 0; ; i++) {
    yield `data: tick ${i}\n\n`
    await sleep(1000)
  }
}

const server = createServer(() => ({
  status: 200,
  headers: {'content-type': 'text/event-stream'},
  body: ticks(),
}))
```

A chunk may be a `string` (UTF-8 encoded) or a `Uint8Array`.

## Handling errors

A handler that **panics** (throws or rejects) is isolated to its one request: the server keeps serving and the client gets a `500`. Pass an `onPanic` hook to log the failure or return a custom response:

```ts twoslash
import {createServer} from 'mikro/http/server'
// ---cut---
const server = createServer(
  () => {
    throw new Error('boom')
  },
  {
    onPanic: (error, req) => {
      console.error('handler panicked for %s', req.url)
      return {status: 500, body: 'Something went wrong'}
    },
  },
)
```

If `onPanic` returns nothing (or itself throws), a plain `500` is sent. `onPanic` is a safety net for _unexpected_ throws, not where you do normal error handling: handle expected failures inside the handler (check the [`Result`](/api/result) from `req.json()`/`req.text()`) and return an explicit `{status, body}` for each case, so the handler never panics in the first place.

## Serving static files

There's no built-in static file server; compose one from [`fs`](/api/fs) and the request path:

```ts twoslash
// @noErrors
import {createServer} from 'mikro/http/server'
import {readFile} from 'mikro/fs'
// ---cut---
const types: Record<string, string> = {html: 'text/html', css: 'text/css', js: 'text/javascript'}

const server = createServer((req) => {
  const path = req.url.split('?')[0] ?? '/'
  const rel = path === '/' ? 'index.html' : path.replace(/^\/+/, '')

  const file = readFile(`/app/www/${rel}`)
  if (!file.ok) return {status: 404, body: 'Not found'}

  const ext = rel.split('.').pop() ?? ''
  return {
    status: 200,
    headers: {'content-type': types[ext] ?? 'application/octet-stream'},
    body: file.value,
  }
})
```

Validate or sandbox the path before opening files if untrusted clients can reach the server (reject `..` segments).

## Types

### ServerOptions

```ts
interface ServerOptions {
  maxBodySize?: number // cap on the buffered request body, bytes (default 16 KiB)
  onPanic?: PanicHandler
}
```

### Handler

```ts
type Handler = (req: ServerRequest) => ServerResponse | Promise<ServerResponse>
```

### ServerRequest

```ts
interface ServerRequest {
  readonly method: string
  readonly url: string // path plus query string, e.g. "/hello?x=1"
  readonly headers: {
    get(name: string): string | undefined
    getAll(name: string): string[]
  }
  readonly body: AsyncIterable<Result<Uint8Array, RequestError>>
  text(): Promise<Result<string, RequestError>>
  json(): Promise<Result<unknown, RequestError>>
  bytes(): Promise<Result<Uint8Array, RequestError>>
}
```

### ServerResponse

```ts
interface ServerResponse {
  status: number // always explicit; there is no implicit 200
  headers?: [string, string][] | Record<string, string>
  body?: string | Uint8Array | AsyncIterable<Uint8Array | string>
}
```

String bodies are sent as UTF-8 bytes, but the response declares no charset unless you set one. For `text/html` (or any text type) with non-ASCII content, include it explicitly: `'content-type': 'text/html; charset=utf-8'`. Otherwise browsers can decode the bytes as Latin-1 and mangle them. JSON and `text/event-stream` are UTF-8 by spec and need no charset.

### PanicHandler

```ts
type PanicHandler = (
  error: unknown,
  req: ServerRequest,
) => ServerResponse | undefined | void | Promise<ServerResponse | undefined | void>
```

## Errors

### ServerError

Returned by [`listen`](#server-listen-options).

| Variant            | Fields            | Description                                       |
| ------------------ | ----------------- | ------------------------------------------------- |
| `AlreadyListening` | —                 | A server is already listening; `close()` it first |
| `OutOfMemory`      | `message: string` | The server could not be allocated                 |
| `StartFailed`      | `message: string` | Binding failed (the port is in use, for example)  |

Request body reads (`text`/`json`/`bytes`) return a [`RequestError`](/api/http-request#requesterror), shared with the HTTP client.

::: tip Plain HTTP only
The server speaks plain HTTP, which is the norm for LAN and AP-local use (config portals, dashboards, webhooks on a trusted network). There is no TLS server.
:::
