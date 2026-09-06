---
title: BLE Beacon
description: Broadcast a sensor reading over BLE without any central connecting
---

# BLE Beacon

Broadcast a fake temperature reading in the advertising packet's manufacturer-data field. This is broadcaster-mode BLE: the device never accepts connections; it just transmits a short payload that any scanner in range can pick up. See the [ble API reference](/api/ble) for the full type surface.

Useful for presence beacons, simple sensor broadcasts, iBeacon-style advertisers, or anything where you want to avoid the power and complexity of GATT connections.

## Hardware

- Any ESP32 family board with BLE (ESP32-C3, ESP32-C6, ESP32-S3, original ESP32). ESP32-S2 has no BLE radio and is not supported.
- USB cable for flashing and serial logging.
- A phone with [nRF Connect for Mobile](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile) or [LightBlue](https://apps.apple.com/us/app/lightblue/id557428110) to observe the beacon.

## Code

```ts twoslash
import {ble, peripheral} from 'mikro/ble'
import {sleep} from 'mikro/sleep'

ble.name = 'mikrojs-beacon'

// Encode a fake temperature reading as manufacturer-specific data.
// First two bytes are the company ID (0xFFFF = reserved for testing).
// Remaining bytes are a little-endian int16 in units of 0.01 °C.
function encodeTemperature(tempCelsius: number): Uint8Array {
  const centi = Math.round(tempCelsius * 100)
  return new Uint8Array([0xff, 0xff, centi & 0xff, (centi >> 8) & 0xff])
}

async function startAdvertising(tempCelsius: number) {
  return peripheral.advertise({
    name: 'mikrojs-beacon',
    connectable: false,
    interval: {min: 1000, max: 1500},
    includeTxPower: true,
    manufacturerData: encodeTemperature(tempCelsius),
  })
}

let temp = 21.5
let result = await startAdvertising(temp)

if (!result.ok) {
  console.error('ble.advertise failed:', result.error)
} else {
  console.log('BLE beacon advertising as %s at %s', ble.name, ble.address)

  while (true) {
    await sleep(5000)
    temp += (Math.random() - 0.5) * 0.2
    const stopResult = result.value.stop()
    if (!stopResult.ok) {
      console.warn('ble.stop (refresh) failed:', stopResult.error)
    }
    result = await startAdvertising(temp)
    if (!result.ok) {
      console.error('ble.advertise (refresh) failed:', result.error)
      break
    }
  }
}
```

## What you'll see

In nRF Connect / LightBlue, scan for nearby devices and look for `mikrojs-beacon`. Tapping into it reveals the advertising payload:

- **Complete Local Name**: `mikrojs-beacon`
- **Manufacturer Specific Data**: `FFFF5308` (or similar). Decoded as:
  - First two bytes `FFFF` = company ID (reserved for testing).
  - Next two bytes `5308` = little-endian int16 = 0x0853 = 2131 = 21.31 °C.
- **TX Power Level**: typically 0 or +9 dBm on ESP32-C6's default setting.
- **RSSI**: updates continuously as you move.

Every ~5 seconds the temperature bytes drift slightly as the loop adds random noise.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template ble-beacon
```

```sh [npm]
npm create mikro -- --template ble-beacon
```

```sh [yarn]
yarn create mikro --template ble-beacon
```

```sh [bun]
bun create mikro --template ble-beacon
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

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/ble-beacon)
