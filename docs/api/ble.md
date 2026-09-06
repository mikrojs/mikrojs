---
title: ble
description: Bluetooth Low Energy peripheral, broadcaster and GATT server
---

# ble

```ts twoslash
import {ble, peripheral} from 'mikro/ble'
```

Bluetooth Low Energy peripheral currently supports two modes:

- **Broadcaster**: connectionless advertising with custom payload. For beacons, presence, and sensor broadcasts picked up by Home Assistant or similar.
- **GATT peripheral**: a connectable device with services and characteristics. Central devices (phones, other boards) can discover the GATT table, read and write characteristics, subscribe to notifications, and observe connection lifecycle.

Central role (scanning, connecting to other peripherals) is not currently supported.

## Broadcaster example

```ts twoslash
import {ble, peripheral} from 'mikro/ble'
// ---cut---
ble.name = 'mikrojs-beacon'

const handle = await peripheral.advertise({
  connectable: false,
  interval: {min: 1000, max: 1500},
  includeTxPower: true,
  manufacturerData: new Uint8Array([0xff, 0xff, 0x01, 0x02]),
})

if (!handle.ok) {
  console.error('ble.advertise failed:', handle.error)
}
```

## GATT peripheral example

```ts twoslash
import {ble, peripheral} from 'mikro/ble'
// ---cut---
ble.name = 'mikrojs-sensor'

peripheral.onConnect.subscribe((info) => {
  console.log('connected: %s mtu: %d', info.address, info.mtu)
})

const result = await peripheral.advertise({
  connectable: true,
  services: [
    {
      uuid: '180f', // Battery Service
      characteristics: [
        {
          uuid: '2a19', // Battery Level
          properties: ['read', 'notify'],
          value: new Uint8Array([87]),
        },
      ],
    },
  ],
})

if (result.ok) {
  // Push a new battery level. Connected subscribers see the change live.
  result.value.notify('180f', '2a19', new Uint8Array([86]))
}
```

## Global state

### ble.name

```ts
name: string
```

The device name advertised in the GAP packet and exposed via the GATT Device Name characteristic. Defaults to `"mikrojs-xxxxxx"` where the suffix is the last three bytes of the BLE MAC address. Writable at any time; takes effect on the next `advertise()` call or immediately if the stack is already running.

### ble.address

```ts
readonly address: string
```

The device's BLE MAC address in canonical `"aa:bb:cc:dd:ee:ff"` form. Read from efuse, does not require the BLE stack to be initialized.

### ble.txPower

```ts
txPower: number
```

Radio transmit power in dBm. Range depends on the chip (typically -24 to +9 on ESP32 family).

### ble.stop()

```ts
stop(): Result<void, BleError>
```

Tears down the BLE stack: disconnects any active centrals, stops advertising, unregisters the GATT table, disables the BT controller, and reclaims all BLE-related RAM. The next BLE call re-initializes from scratch. Idempotent.

## Advertising

### peripheral.advertise(options?)

```ts
advertise(options?: AdvertiseOptions): Promise<Result<AdvertiseHandle, BleError>>
```

Starts advertising with the given options. On first call, lazily initializes the NimBLE stack. On subsequent calls, validates that the services (if any) match the previously-registered set and restarts the GAP layer.

Returns an `AdvertiseHandle` with `stop()`, `setValue()`, and `notify()` methods.

```ts twoslash
import {peripheral} from 'mikro/ble'
// ---cut---
const result = await peripheral.advertise({
  name: 'mikrojs-sensor',
  connectable: true,
  interval: {min: 100, max: 150},
  includeTxPower: true,
})
```

### AdvertiseOptions

| Field              | Type                         | Description                                                                        |
| ------------------ | ---------------------------- | ---------------------------------------------------------------------------------- |
| `name`             | `string`                     | Override `ble.name` for this advertisement                                         |
| `connectable`      | `boolean`                    | Default `false`. Set to `true` with `services` for a GATT peripheral               |
| `services`         | `Service[]`                  | GATT services to register (first call only)                                        |
| `interval`         | `{min: number, max: number}` | Advertising interval in milliseconds. Spec bounds: 20–10240ms                      |
| `includeTxPower`   | `boolean`                    | Include the current TX power in the advertising packet (3 bytes)                   |
| `manufacturerData` | `Uint8Array`                 | Manufacturer-specific data. First two bytes must be the company ID (little endian) |

## GATT services

### Service

```ts
interface Service {
  uuid: string
  characteristics: Characteristic[]
}
```

### Characteristic

```ts
interface Characteristic {
  uuid: string
  properties: CharacteristicProperty[]
  value?: Uint8Array
  onWrite?: (value: Uint8Array) => void
  writeMode?: 'advisory'
  security?: 'open'
}
```

### CharacteristicProperty

```ts
type CharacteristicProperty = 'read' | 'write' | 'writeWithoutResponse' | 'notify' | 'indicate'
```

UUIDs can be either the 4-character 16-bit shorthand (for example `"180f"` for Battery Service) or the 36-character canonical 128-bit form (`"6e400001-b5a3-f393-e0a9-e50e24dcca9e"`). Case-insensitive on input; normalized to lowercase internally.

## AdvertiseHandle

The handle returned by `peripheral.advertise()`.

### handle.stop()

```ts
stop(): Result<void, BleError>
```

Stops this advertising session. Already-connected centrals stay connected and can continue reading and writing characteristics. The GATT table stays registered. Call `ble.stop()` for a full teardown.

### handle.setValue(serviceUuid, characteristicUuid, value)

```ts
setValue(serviceUuid: string, characteristicUuid: string, value: Uint8Array): Result<void, BleError>
```

Updates the cached value of a characteristic. Subsequent GATT reads return the new bytes. Does **not** push updates to subscribers; use `notify()` for that.

### handle.notify(serviceUuid, characteristicUuid, value)

```ts
notify(serviceUuid: string, characteristicUuid: string, value: Uint8Array): Result<void, BleError>
```

Updates the cached value AND sends a notification to every subscribed central. The characteristic must declare `'notify'` in its properties, and the central must have enabled notifications via the CCCD descriptor.

- If no central is subscribed, this is a silent no-op.
- If the payload exceeds the smallest subscriber MTU minus 3 bytes (the ATT header), returns `err(ValueTooLarge)` without sending.
- Per-subscriber failures (backpressure, dropped connection) are logged at warn level and do not fail the overall call. Notify is explicitly best-effort.

## Peripheral events

```ts twoslash
import {peripheral} from 'mikro/ble'
// ---cut---
peripheral.onConnect.subscribe((info) => {
  console.log('connected: %s mtu: %d', info.address, info.mtu)
})

peripheral.onDisconnect.subscribe((info) => {
  console.log('disconnected: %s', info.address)
})

peripheral.onMtu.subscribe((info) => {
  console.log('mtu renegotiated to %d', info.mtu)
})
```

| Stream         | Payload          | Description                                                        |
| -------------- | ---------------- | ------------------------------------------------------------------ |
| `onConnect`    | `ConnectionInfo` | A central has connected                                            |
| `onDisconnect` | `ConnectionInfo` | A central has disconnected. `mtu` reflects the last known value    |
| `onMtu`        | `MtuInfo`        | ATT MTU renegotiated mid-session (typically shortly after connect) |

Each property is an `Observable<T>` (see `mikro/observable`). Subscribers fire on the JS loop thread; subscribe before calling `advertise()` so no events are missed during startup. To stay discoverable after a central disconnects, call `peripheral.advertise()` again from the `onDisconnect` subscriber. Call `unsubscribe()` on the returned `Subscription` to stop receiving values.

## Types

### ConnectionInfo

```ts
interface ConnectionInfo {
  id: number // opaque NimBLE conn_handle
  address: string // "aa:bb:cc:dd:ee:ff"
  mtu: number // current negotiated ATT MTU in bytes
}
```

### MtuInfo

```ts
interface MtuInfo {
  id: number
  mtu: number
}
```

## Errors

### BleError

| Variant                      | Fields                 | Description                                                            |
| ---------------------------- | ---------------------- | ---------------------------------------------------------------------- |
| `StackInitFailed`            | `message`              | NimBLE host init failed                                                |
| `ControllerInitFailed`       | `message`              | BT controller init failed                                              |
| `AlreadyAdvertising`         | —                      | Called `advertise()` while already advertising                         |
| `AdvertiseStartFailed`       | `message`              | NimBLE refused to start advertising                                    |
| `AdvertiseStopFailed`        | `message`              | NimBLE refused to stop advertising                                     |
| `AdvertisingPayloadTooLarge` | `bytes`, `max`         | Advertising packet would exceed the 31-byte limit                      |
| `InvalidInterval`            | `min`, `max`           | Interval outside spec range (20ms–10240ms, min ≤ max)                  |
| `InvalidUuid`                | `value`                | UUID string is not a valid 16-bit or 128-bit form                      |
| `DuplicateCharacteristic`    | `uuid`                 | Two characteristics in the same service have the same UUID             |
| `InvalidProperties`          | `uuid`, `reason`       | Characteristic property set is invalid                                 |
| `GattRegistrationFailed`     | `message`              | NimBLE refused to register the GATT table                              |
| `GattAlreadyRegistered`      | —                      | Tried to register a different service set without a prior `ble.stop()` |
| `NoSuchCharacteristic`       | `uuid`                 | Called `setValue()` / `notify()` with an unknown UUID                  |
| `ValueTooLarge`              | `uuid`, `bytes`, `max` | Notify payload exceeds the minimum subscriber MTU minus 3              |
| `NotifyFailed`               | `message`              | NimBLE refused to send the notification                                |
| `NotConnected`               | —                      | Operation requires an active connection                                |
| `StackShutdown`              | —                      | Stack was torn down while an operation was pending                     |
| `SetFailed`                  | `message`              | Generic setter failure                                                 |
| `GetFailed`                  | `message`              | Generic getter failure                                                 |

## What's supported

This module covers broadcaster mode and a connectable GATT peripheral with reads, writes, and notifications. The following are **not supported**:

### No indicate delivery

Characteristics can declare `'indicate'` in their properties, and centrals can enable indications via the CCCD descriptor. Subscription tracking records the indicate bit correctly. However, there is no `handle.indicate()` method to send indications. Use `notify` instead.

### No central role

Only the peripheral role is implemented. Scanning for other devices, connecting as a central, discovering remote GATT tables, and reading or writing remote characteristics are not available.

### No pairing, bonding, or encryption

All characteristics are accessible without authentication. The `security` field on `Characteristic` only accepts the value `'open'`.

### No rejectable writes

The `onWrite` handler is advisory and runs asynchronously on the JS loop thread **after** the write has already been acknowledged at the GATT layer. There is no way to reject a write once the central has sent it. The `writeMode` field only accepts `'advisory'`.

For application-level validation of writes, use a paired "command + status" characteristic pattern: the central writes a command to one characteristic, and the peripheral updates a status characteristic with the result, which the central reads or subscribes to.

### No per-connection notify targeting

`handle.notify()` broadcasts to every subscribed central. There is no way to send a notification to a specific connection only. Most peripherals only serve one central at a time, so this rarely matters in practice.

### No dynamic GATT table changes

The service and characteristic set is frozen at first `advertise()` call. To change the GATT table (add a service, change a characteristic's UUID, change a property set) you must call `ble.stop()` and advertise again from scratch. Connected centrals will be disconnected. Mikro.js does not offer `addService()` or `removeService()` methods. Dynamic GATT changes cause handle churn that breaks connected centrals' attribute caches.

### No descriptors beyond CCCD

Notify and indicate characteristics automatically get a CCCD descriptor (managed by NimBLE). Other descriptors (CUD, presentation format, valid range, and others) are not declarable from JS.

### No extended advertising

The advertising packet is capped at the classic 31-byte limit. BLE 5.0 extended advertising (255 bytes, 1650 bytes with chaining) is not supported.

### Bounds

- **Maximum simultaneous centrals**: 4 slots in the runtime's connection tracking table. Must match or exceed `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`.
- **Maximum characteristics per GATT table**: 32. Bounded by the uint32 width of the per-connection subscription bitmap. Registering a 33rd characteristic fails with `GattRegistrationFailed`.
- **Maximum write payload per onWrite**: 256 bytes. Larger writes are dropped with a warn log. This is the size of each buffer in the write payload pool, which is fixed at 8 buffers to avoid heap fragmentation on the hot path.
- **Advertising payload**: 31 bytes total, including mandatory flags and device name.
