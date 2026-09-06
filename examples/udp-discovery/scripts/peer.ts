#!/usr/bin/env node --experimental-strip-types
/// <reference types="node" />
/* eslint-disable no-console */
// Tiny Node peer for the udp-discovery example. Joins the same multicast
// group + port as the device, announces a name every 2 seconds, and logs
// announcements from other peers.
//
//   node scripts/peer.ts                  # name = laptop-<pid>
//   node scripts/peer.ts --name kitchen   # custom name
//
// Useful when you only have one board on hand: run this on a laptop that's
// on the same WiFi network and the board will discover the laptop, and
// vice-versa.

import dgram from 'node:dgram'
import {networkInterfaces} from 'node:os'
import process from 'node:process'

const GROUP = '224.0.0.251'
const PORT = 7654
const ANNOUNCE_MS = 2000

// Pick a usable IPv4 interface. Skips loopback, link-local, and tunnel
// interfaces (utun/awdl/etc) so VPNs like Tailscale don't capture our
// multicast traffic. Prefers physical interfaces (en0, en1) and falls
// back to any non-internal IPv4 if nothing matches.
function pickInterface(): string | undefined {
  const ifaces = networkInterfaces()
  const physical: string[] = []
  const fallback: string[] = []
  for (const [name, entries] of Object.entries(ifaces)) {
    if (!entries) continue
    for (const e of entries) {
      if (e.family !== 'IPv4' || e.internal) continue
      // Tailscale CGNAT range — explicitly excluded.
      if (e.address.startsWith('100.')) {
        const second = parseInt(e.address.split('.')[1] ?? '0', 10)
        if (second >= 64 && second <= 127) continue
      }
      if (/^en\d/.test(name)) physical.push(e.address)
      else if (!/^(utun|awdl|llw|bridge|ipsec|ap)/.test(name)) fallback.push(e.address)
    }
  }
  return physical[0] ?? fallback[0]
}

const args = process.argv.slice(2)
const nameIdx = args.indexOf('--name')
const name: string =
  nameIdx >= 0 && args[nameIdx + 1] ? args[nameIdx + 1]! : `laptop-${process.pid}`
const ifaceIdx = args.indexOf('--iface')
const iface: string | undefined =
  ifaceIdx >= 0 && args[ifaceIdx + 1] ? args[ifaceIdx + 1] : pickInterface()

const socket = dgram.createSocket({type: 'udp4', reuseAddr: true})

socket.on('message', (msg: Buffer, from: dgram.RemoteInfo) => {
  const text = msg.toString('utf8')
  if (text === name) return
  console.log(`discovered ${text} at ${from.address}`)
})

socket.on('listening', () => {
  // Bind multicast to a specific interface so VPN tunnels (Tailscale, etc)
  // don't intercept the traffic. Override with --iface 192.168.x.y.
  socket.addMembership(GROUP, iface)
  if (iface) socket.setMulticastInterface(iface)
  console.log(`announcing as ${name} on ${GROUP}:${PORT}${iface ? ` via ${iface}` : ''}`)
  setInterval(() => {
    socket.send(name, PORT, GROUP, (err) => {
      if (err) console.error('send failed:', err)
    })
  }, ANNOUNCE_MS)
})

socket.on('error', (err: Error) => {
  console.error('socket error:', err)
  process.exit(1)
})

socket.bind(PORT)

process.on('SIGINT', () => {
  socket.close()
  process.exit(0)
})
