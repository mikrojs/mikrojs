---
title: Captive Portal
description: A WiFi captive portal that redirects DNS and serves an HTTP page
---

# Captive Portal

Turn the device into a WiFi captive portal: run an access point, answer every DNS lookup with the device's IP, and serve the same page for every request. When a phone or laptop joins, its connectivity check fails to reach the internet and the page opens automatically. Combines the [http/server](/api/http-server), [udp](/api/udp), and [wifi](/api/wifi) APIs.

## Hardware

- Any ESP32 board with WiFi
- USB cable
- A phone or laptop to join the access point

## Code

```ts twoslash
import {createServer} from 'mikro/http/server'
import {bind} from 'mikro/udp'
import {wifi} from 'mikro/wifi'

const SSID = 'MikroJS-Portal'

// Start an open access point.
wifi.ap
  .start({ssid: SSID, authMode: 'open', channel: 6, maxConnections: 4})
  .orPanic('Failed to start access point')

const ip = wifi.ap.ip ?? '192.168.4.1'
const octets = ip.split('.').map((n) => Number(n))
console.log('Open access point "%s" ready', SSID)
console.log('Portal: http://%s/', ip)

// Answer every DNS query with our own IP, so the client's captive-portal
// check lands on this device instead of the internet.
const dns = await bind({port: 53, family: 'ipv4'})
if (!dns.ok) {
  console.error('DNS bind failed:', dns.error)
} else {
  const sock = dns.value
  sock.onMessage.subscribe(({msg, from}) => {
    const reply = dnsReply(msg, octets)
    if (reply) void sock.send(reply, from)
  })
  console.log('DNS responder listening on :53')
}

const PAGE = `<!doctype html>
<html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Mikro.js Captive Portal</title>
  </head>
  <body style="font-family: sans-serif; text-align: center; padding: 2rem">
    <h1>Hello from Mikro.js</h1>
    <p>You reached the captive portal running on an ESP32.</p>
  </body>
</html>`

// Serve the same page for every path so the OS connectivity check fails and
// opens its sign-in page.
const server = createServer(() => ({
  status: 200,
  headers: {'content-type': 'text/html; charset=utf-8'},
  body: PAGE,
}))
server.listen({port: 80}).orPanic('Failed to start HTTP server')
console.log('Captive portal ready. Join the network and a sign-in page should open.')

// Build a DNS response that points the queried name at `ip` (4 octets). Handles
// a single-question query: answers A records, leaves other types unanswered.
function dnsReply(query: Uint8Array, ip: number[]): Uint8Array | undefined {
  if (query.length < 12) return undefined
  let i = 12
  while (i < query.length && query[i] !== 0) i += query[i]! + 1
  if (i >= query.length) return undefined
  const qend = i + 5 // null label + qtype(2) + qclass(2)
  if (qend > query.length) return undefined
  const isA = ((query[i + 1]! << 8) | query[i + 2]!) === 1

  const question = query.subarray(12, qend)
  const header = new Uint8Array(12)
  header[0] = query[0]! // transaction id
  header[1] = query[1]!
  header[2] = 0x81 // QR=1, recursion-desired echoed
  header[3] = 0x80 // recursion available
  header[5] = 0x01 // QDCOUNT = 1
  header[7] = isA ? 0x01 : 0x00 // ANCOUNT
  if (!isA) {
    const resp = new Uint8Array(12 + question.length)
    resp.set(header, 0)
    resp.set(question, 12)
    return resp
  }

  const a = [0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 0x3c, 0x00, 0x04, ...ip]
  const answer = new Uint8Array(a)
  const resp = new Uint8Array(12 + question.length + answer.length)
  resp.set(header, 0)
  resp.set(question, 12)
  resp.set(answer, 12 + question.length)
  return resp
}
```

## Walkthrough

1. **Open access point.** `wifi.ap.start()` with `authMode: 'open'` brings up a passwordless network, so clients can join without credentials (how captive portals normally work). The device prints the SSID and portal URL on boot.

2. **DNS redirect.** A [`mikro/udp`](/api/udp) socket on port 53 answers every query with the device's own IP, so the client's captive-portal probe resolves to this device instead of the internet.

3. **Catch-all HTTP.** The server returns the same page for every path. Because the OS connectivity check doesn't get its expected response, the OS opens its sign-in page.

Auto-popup behavior depends on the client OS; opening the printed AP IP always works.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template captive-portal
```

```sh [npm]
npm create mikro -- --template captive-portal
```

```sh [yarn]
yarn create mikro --template captive-portal
```

```sh [bun]
bun create mikro --template captive-portal
```

:::

## Run it

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

Join the open **MikroJS-Portal** network, and a sign-in page opens. If it doesn't, browse to the AP IP printed on boot.

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/captive-portal)
