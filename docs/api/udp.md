---
title: udp
description: Datagram sockets for IPv4 and IPv6
---

# udp

```ts twoslash
import {bind} from 'mikro/udp'
```

UDP sockets for IPv4 and IPv6. The module unblocks CoAP, mDNS / DNS-SD, NTP-style protocols, and custom binary protocols between mikrojs nodes. UDP itself is link-agnostic, so it works equally over WiFi STA, WiFi AP, or future Thread / Ethernet.

## Usage

```ts twoslash
import {bind} from 'mikro/udp'

const result = await bind({port: 5683})
if (!result.ok) {
  console.error('bind failed:', result.error)
} else {
  const socket = result.value

  socket.onMessage.subscribe(({msg, from}) => {
    console.log('%d bytes from %s:%d', msg.length, from.address, from.port)
  })

  await socket.send('hello', {address: '192.168.1.42', port: 5683, family: 'ipv4'})
}
```

## Functions

### bind(options)

```ts
bind(options: BindOptions): Promise<Result<UdpSocket, UdpError>>
```

Creates a UDP socket and binds it to a local address and port. Default is dual-stack (accepts both IPv4 and IPv6). Pass `port: 0` for an ephemeral port; the actual port is readable on `socket.port`.

```ts twoslash
import {bind} from 'mikro/udp'
// ---cut---
// Dual-stack on an ephemeral port
const r = await bind({port: 0})

// IPv4 only on a specific port
const v4 = await bind({port: 5683, family: 'ipv4'})

// Bind to a specific local address
const local = await bind({port: 0, address: '192.168.1.10', family: 'ipv4'})
```

## Socket methods

### socket.send(data, to)

```ts
send(data: Uint8Array | string, to: PeerAddress): Promise<Result<void, UdpError>>
```

Sends a single datagram. String data is UTF-8 encoded. The `to` argument must include `family` so the address can be parsed unambiguously.

```ts twoslash
import {bind} from 'mikro/udp'
declare const socket: NonNullable<Awaited<ReturnType<typeof bind>>['value']>
// ---cut---
const result = await socket.send('ping', {
  address: '192.168.1.42',
  port: 5683,
  family: 'ipv4',
})
```

### socket.joinMulticastGroup(group)

```ts
joinMulticastGroup(group: {address: string}): Result<void, UdpError>
```

Joins an IPv4 or IPv6 multicast group. The socket's family determines which group type is accepted. An IPv4 interface (for example connected WiFi) must be up before joining an IPv4 group.

```ts twoslash
import {bind} from 'mikro/udp'
declare const socket: NonNullable<Awaited<ReturnType<typeof bind>>['value']>
// ---cut---
// mDNS multicast group
socket.joinMulticastGroup({address: '224.0.0.251'})

// CoAP all-nodes IPv6 group
socket.joinMulticastGroup({address: 'ff02::fd'})
```

### socket.leaveMulticastGroup(group)

```ts
leaveMulticastGroup(group: {address: string}): Result<void, UdpError>
```

### socket.close()

```ts
close(): void
```

Closes the socket. Idempotent. Subsequent `send` calls resolve to `{name: 'Closed'}`. There is no GC finalizer that closes sockets; explicit `close()` is required to release the file descriptor.

## Socket properties

| Property    | Type                         | Description                                                  |
| ----------- | ---------------------------- | ------------------------------------------------------------ |
| `port`      | `number`                     | Actual bound port (resolved when `bind` was called with `0`) |
| `family`    | `'ipv4' \| 'ipv6' \| 'dual'` | Address family the socket was opened with                    |
| `dropped`   | `number`                     | Counter of inbound datagrams dropped (see below)             |
| `onMessage` | `Observable<UdpMessage>`     | Stream of inbound datagrams. Subscribe to start receiving    |

## Receiving

`socket.onMessage` is an `Observable<UdpMessage>` (see `mikro/observable`). Subscribe to receive packets — each emission is `{msg, from}` with a freshly allocated `Uint8Array` and a `PeerAddress` describing the sender. The first subscribe attaches the native dispatch; the last `unsubscribe()` detaches it again.

```ts twoslash
import {bind} from 'mikro/udp'
declare const socket: NonNullable<Awaited<ReturnType<typeof bind>>['value']>
// ---cut---
socket.onMessage.subscribe(({msg, from}) => {
  const text = new TextDecoder().decode(msg)
  console.log('%s:%d -> %s', from.address, from.port, text)
})
```

The receive buffer is 1500 bytes (typical Ethernet MTU). Datagrams larger than that are dropped rather than truncated and increment `socket.dropped`. This covers all common protocols (CoAP is capped at 1152 bytes by spec; mDNS, DNS, SNTP all stay well under the MTU).

## Backpressure

Each socket has a bounded receive queue (default 8). When full, the newest datagram is dropped and `socket.dropped` is incremented. There is no separate drop event; applications that care can poll the counter periodically.

```ts twoslash
import {bind} from 'mikro/udp'
// ---cut---
// Larger queue for a busy responder
const r = await bind({port: 5683, recvQueue: 32})
```

`socket.dropped` also increments when a packet arrives but no subscriber is attached to `onMessage`, or when a datagram is larger than the receive buffer.

## Address normalization

Addresses are exposed in canonical form on receive:

- IPv4: `192.168.1.1` (v4-mapped v6 like `::ffff:192.168.1.1` is normalized back to plain v4).
- IPv6 global / mesh-local: `fd00:db8::1` (RFC 5952 canonical).
- IPv6 link-local with scope: `fe80::1%2` (numeric interface index).

Addresses passed to `send` and `bind` accept the same forms, including the `%scope` suffix on link-local IPv6.

## Types

### BindOptions

```ts
interface BindOptions {
  port: number // 0 = ephemeral
  address?: string // local address (default: wildcard)
  family?: 'ipv4' | 'ipv6' // default: dual-stack v6
  recvQueue?: number // default: 8
}
```

### PeerAddress

```ts
interface PeerAddress {
  address: string
  port: number
  family: 'ipv4' | 'ipv6'
}
```

## Errors

### UdpError

| Variant            | Fields    | Description                                            |
| ------------------ | --------- | ------------------------------------------------------ |
| `BindFailed`       | `message` | Generic bind failure (an invalid address, for example) |
| `AddressInUse`     | —         | Local port already in use                              |
| `SendFailed`       | `message` | Generic send failure (an invalid peer, for example)    |
| `MessageTooLarge`  | —         | Datagram exceeded the path MTU                         |
| `NotReachable`     | —         | No route to host or interface down                     |
| `OutOfMemory`      | —         | lwIP / kernel out of buffer memory                     |
| `JoinGroupFailed`  | `message` | Multicast join failed (interface down or invalid)      |
| `LeaveGroupFailed` | `message` | Multicast leave failed                                 |
| `Closed`           | —         | Operation on a closed socket                           |
| `NotBound`         | —         | Operation on a socket that was never bound             |
