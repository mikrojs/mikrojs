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

  const answer = new Uint8Array([
    0xc0,
    0x0c, // name: pointer to the question at offset 12
    0x00,
    0x01, // type A
    0x00,
    0x01, // class IN
    0x00,
    0x00,
    0x00,
    0x3c, // TTL 60s
    0x00,
    0x04, // RDLENGTH 4
    ip[0]!,
    ip[1]!,
    ip[2]!,
    ip[3]!,
  ])
  const resp = new Uint8Array(12 + question.length + answer.length)
  resp.set(header, 0)
  resp.set(question, 12)
  resp.set(answer, 12 + question.length)
  return resp
}
