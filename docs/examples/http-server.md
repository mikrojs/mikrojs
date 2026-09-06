---
title: HTTP Server
description: Serve an HTTP API on the local network
---

# HTTP Server

Join an existing WiFi network and serve a small HTTP API: a JSON status endpoint, a route with a path param, and a server-sent events stream. See the [http/server API reference](/api/http-server) for the full type surface.

## Hardware

- Any ESP32 board with WiFi
- USB cable
- A WiFi network you can join, plus a phone or laptop on the same network

## Code

```ts twoslash
import {env} from 'mikro/env'
import {createServer, type ServerResponse} from 'mikro/http/server'
import {sleep} from 'mikro/sleep'
import {memoryUsage, uptime} from 'mikro/sys'
import {wifi} from 'mikro/wifi'

const ssid = env.require('WIFI_SSID')
const passphrase = env.require('WIFI_PASSPHRASE')

console.log('Connecting to %s...', ssid)
const connected = await wifi.connect({ssid, passphrase})
if (!connected.ok) {
  console.error('WiFi connect failed:', connected.error)
} else {
  console.log('Connected. Open http://%s/', connected.value.ip)

  const json = (status: number, value: unknown): ServerResponse => ({
    status,
    headers: {'content-type': 'application/json'},
    body: JSON.stringify(value),
  })

  // A short, finite stream. The server handles one request at a time, so a
  // long-lived stream holds the connection until it ends.
  async function* countdown() {
    for (let i = 5; i > 0; i--) {
      yield `data: ${i}\n\n`
      await sleep(1000)
    }
    yield 'data: liftoff\n\n'
  }

  // Routing is left to the app: branch on req.method and the path.
  const server = createServer((req): ServerResponse => {
    if (req.method !== 'GET') return json(405, {error: 'method not allowed'})
    const path = req.url.split('?')[0]

    if (path === '/') {
      return {
        status: 200,
        headers: {'content-type': 'text/html; charset=utf-8'},
        body: `<h1>Mikro.js HTTP server</h1>
<ul>
  <li><a href="/api/status">/api/status</a>: JSON</li>
  <li><a href="/api/echo/hello">/api/echo/&lt;msg&gt;</a>: path param</li>
  <li><a href="/events">/events</a>: server-sent events</li>
</ul>`,
      }
    }

    if (path === '/api/status') {
      const mem = memoryUsage()
      return json(200, {uptime: uptime(), freeHeapBytes: mem.heapTotal - mem.heapUsed})
    }

    if (path?.startsWith('/api/echo/')) {
      return json(200, {echo: decodeURIComponent(path.slice('/api/echo/'.length))})
    }

    if (path === '/events') {
      return {status: 200, headers: {'content-type': 'text/event-stream'}, body: countdown()}
    }

    return json(404, {error: 'not found'})
  })

  server.listen({port: 80}).orPanic('Failed to start HTTP server')
  console.log('Listening on port 80')
}
```

## Walkthrough

1. **Credentials.** `env.require()` reads `WIFI_SSID` and `WIFI_PASSPHRASE` from `.env`, throwing a clear error if they're missing.

2. **Connect, then serve.** `wifi.connect()` joins the network and returns the device's IP. The server is created only after a successful connection.

3. **Routing is yours.** There is no built-in router. Strip the query string from `req.url` and branch on `req.method` and the path. The `/api/echo/<msg>` route reads a path param by slicing the prefix.

4. **JSON responses.** A small `json()` helper sets `content-type` and stringifies the body. Returning a plain `{status, body}` object is all a handler does.

5. **Streaming.** Returning an async generator as the `body` streams a response chunk-by-chunk (here, a short countdown as server-sent events) instead of buffering it.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template http-server
```

```sh [npm]
npm create mikro -- --template http-server
```

```sh [yarn]
yarn create mikro --template http-server
```

```sh [bun]
bun create mikro --template http-server
```

:::

## Run it

Set your network credentials in `.env`:

```sh
WIFI_SSID=your-network
WIFI_PASSPHRASE=your-password
```

::: code-group

```sh [pnpm]
pnpm install
pnpm mikro flash  # only needed once per board
pnpm mikro dev
```

```sh [npm]
npm install
npx mikro flash  # only needed once per board
npx mikro dev
```

```sh [yarn]
yarn install
yarn mikro flash  # only needed once per board
yarn mikro dev
```

```sh [bun]
bun install
bunx mikro flash  # only needed once per board
bunx mikro dev
```

:::

The device prints its IP on boot. Open `http://<that-ip>/api/status` from a browser or `curl` on the same network.

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/http-server)
