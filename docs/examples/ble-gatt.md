---
title: BLE GATT
description: A connectable GATT peripheral with live notifications and write handlers
---

# BLE GATT

A full GATT peripheral: the device advertises as connectable, exposes a standard Battery Service that pushes live updates, and a custom vendor service with a writable command characteristic that logs incoming bytes to the serial console.

Demonstrates connect/disconnect events, notifications, write dispatch, and the declarative service registration API.

## Hardware

- Any ESP32 family board with BLE (ESP32-C3, ESP32-C6, ESP32-S3, original ESP32).
- USB cable for flashing and serial logging.
- A phone with [nRF Connect for Mobile](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile) or similar. macOS users can use [LightBlue](https://apps.apple.com/us/app/lightblue/id557428110) for reads, but writes and subscribe support vary by tool.

## Code

```ts twoslash
import {ble, peripheral} from 'mikro/ble'
import {sleep} from 'mikro/sleep'

ble.name = 'mikrojs-hello'

peripheral.onConnect.subscribe((info) => {
  console.log('connected: %s mtu: %d', info.address, info.mtu)
})

peripheral.onDisconnect.subscribe(async (info) => {
  console.log('disconnected: %s', info.address)
  // NimBLE stops advertising on connect. Re-advertise so the device stays
  // discoverable once the peer hangs up.
  const r = await peripheral.advertise({
    name: 'mikrojs-hello',
    connectable: true,
    interval: {min: 100, max: 150},
    services: currentServices,
  })
  if (!r.ok) console.error('re-advertise failed:', r.error)
})

peripheral.onMtu.subscribe((info) => {
  console.log('mtu renegotiated to %d', info.mtu)
})

const currentServices = [
  {
    uuid: '180f', // Battery Service
    characteristics: [
      {
        uuid: '2a19', // Battery Level
        properties: ['read', 'notify'] as const,
        value: new Uint8Array([87]),
      },
    ],
  },
  {
    // Custom vendor service. Substitute your own UUID for real projects.
    uuid: '7f3a1b00-4a2f-4e5b-9a3c-1e2d3f4a5b6c',
    characteristics: [
      {
        uuid: '7f3a1b01-4a2f-4e5b-9a3c-1e2d3f4a5b6c',
        properties: ['read'] as const,
        value: new TextEncoder().encode('mikrojs v0: hello from BLE'),
      },
      {
        uuid: '7f3a1b02-4a2f-4e5b-9a3c-1e2d3f4a5b6c',
        properties: ['read', 'write', 'writeWithoutResponse'] as const,
        value: new Uint8Array([0]),
        onWrite: (bytes: Uint8Array) => {
          const hex = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join(' ')
          console.log('command write: %d bytes: %s', bytes.length, hex)
        },
      },
    ],
  },
]

const result = await peripheral.advertise({
  name: 'mikrojs-hello',
  connectable: true,
  interval: {min: 100, max: 150},
  services: currentServices,
})

if (!result.ok) {
  console.error('ble.advertise failed:', result.error)
} else {
  console.log('BLE GATT peripheral advertising as %s', ble.name)

  let level = 87
  while (true) {
    await sleep(2000)
    level = level > 0 ? level - 1 : 100
    const update = result.value.notify('180f', '2a19', new Uint8Array([level]))
    if (!update.ok) {
      console.error('notify failed:', update.error)
      break
    }
  }
}
```

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template ble-gatt
```

```sh [npm]
npm create mikro -- --template ble-gatt
```

```sh [yarn]
yarn create mikro --template ble-gatt
```

```sh [bun]
bun create mikro --template ble-gatt
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

## What you'll see

1. **Scan in nRF Connect**, find `mikrojs-hello`, tap **Connect**. The serial console logs `connected: aa:bb:cc:dd:ee:ff mtu: 23` and shortly after `mtu renegotiated to 256`.

2. **Expand Battery Service** (`180f`). Tap the **subscribe** icon (double-down arrow) next to Battery Level (`2a19`). The value starts ticking down every 2 seconds as the simulated battery drains from 87% toward 0%, then wraps to 100%.

3. **Expand the custom vendor service** (`7f3a1b00-...`). Read the first characteristic to get the UTF-8 string `"mikrojs v0: hello from BLE"`.

4. **Write bytes to the command characteristic** (`7f3a1b02-...`). nRF Connect's write dialog accepts hex like `deadbeef`. The serial console logs `command write: 4 bytes: de ad be ef`. Reading the characteristic back returns the same bytes.

5. **Disconnect**. Serial logs `disconnected: aa:bb:cc:dd:ee:ff`. The disconnect handler calls `peripheral.advertise()` again, so scanning once more finds the device and you can reconnect.

## Key concepts

### Subscribe to lifecycle Observables before `advertise()`

Register `onConnect`/`onDisconnect`/`onMtu` subscribers first so you never miss a `connect` event during startup. Each subscription stays active across multiple `advertise()` calls until you call `unsubscribe()` on the returned `Subscription`.

### `handle.notify()` updates the cached value AND pushes

Calling `handle.notify(svcUuid, charUuid, bytes)` both updates the characteristic's cached value (so subsequent reads see the new bytes) and sends a notification to every currently-subscribed central. If nobody is subscribed, it's a silent no-op.

For write-without-push updates, use `handle.setValue()` instead.

### `onWrite` is advisory, not rejecting

By the time your `onWrite` handler runs on the JS loop, the write has already been acknowledged to the central at the GATT layer. You cannot reject a write from inside the handler. For application-level validation, use a paired "command + status" characteristic pattern: the central writes to one, and you notify the result on another.

### Automatic re-advertising on disconnect

NimBLE stops advertising as soon as a central connects. To stay discoverable after the central hangs up, the `disconnect` handler calls `peripheral.advertise()` again with the same service set. Passing the identical `currentServices` array lets the runtime's deep-equal check recognize it as the same GATT table and just restart the GAP layer instead of re-registering.

## Next steps

See the [ble API reference](../api/ble) for the full type surface, error taxonomy, and current limitations.
