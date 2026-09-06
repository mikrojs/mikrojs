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

  // A short, finite stream. Note: the server handles one request at a time, so
  // a long-lived stream holds the connection until it ends.
  async function* countdown() {
    for (let i = 5; i > 0; i--) {
      yield `data: ${i}\n\n`
      await sleep(1000)
    }
    yield 'data: liftoff\n\n'
  }

  // Routing is left to the app: branch on req.method and the path. Strip the
  // query string, then match exact paths or a prefix for a "param".
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
      // A malformed %-encoding throws; the server isolates the handler and
      // returns 500 for that one request, so no try/catch is needed here.
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
