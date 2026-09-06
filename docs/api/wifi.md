---
title: wifi
description: WiFi station and access point
---

# wifi

```ts twoslash
import {wifi} from 'mikro/wifi'
```

The default export is a `Wifi` singleton. Use it to connect to networks, scan for access points, and start an AP.

## Connecting

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
const result = await wifi.connect({ssid: 'MyNetwork', passphrase: 'password123'})
if (!result.ok) {
  console.error('WiFi failed:', result.error)
} else {
  console.log('Connected, IP: %s', result.value.ip)
}
```

## Station methods

### wifi.connect(options)

```ts
connect(options: WifiConnectOptions): Promise<Result<WifiConnectionInfo, WifiError>>
```

Connects to a WiFi network. Returns connection info with IP, netmask, and gateway on success. Set `txPower` to cap the radio's transmit power (in dBm) for this session; the driver quantizes it to an allowed level.

```ts
interface WifiConnectOptions {
  ssid: string
  passphrase: string
  txPower?: number
}
```

### wifi.disconnect(options?)

```ts
disconnect(options?: WifiDisconnectOptions): Result<void, WifiError>
```

Disconnects from the current network. By default this also powers the radio down and releases its memory. Pass `{shutdown: false}` to keep the radio up for a faster reconnect.

```ts
interface WifiDisconnectOptions {
  shutdown?: boolean // default: true
}
```

### wifi.scan(options?)

```ts
scan(opts?: ScanOptions): Promise<Result<ScanResult[], WifiError>>
```

Scans for nearby networks. Returns an array of `ScanResult` objects.

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
const result = await wifi.scan()
if (result.ok) {
  for (const ap of result.value) {
    console.log('%s %d dBm', ap.ssid, ap.rssi)
  }
}
```

### wifi.rssi()

```ts
rssi(): Result<number, WifiError>
```

Returns the signal strength of the current connection in dBm.

### wifi.ip()

```ts
ip(): string | undefined
```

Returns the current IP address, or `undefined` if not connected.

### wifi.status()

```ts
status(): WifiStatus
```

Returns the current connection status.

### wifi.ipConfig()

Get or set the IP configuration.

```ts
// Get current config
ipConfig(): Result<IpConfig | undefined, WifiError>

// Set static IP (or re-enable DHCP)
ipConfig(opts: StaticIpConfig): Result<void, WifiError>
```

## Station properties

| Property        | Type                           | Description                                         |
| --------------- | ------------------------------ | --------------------------------------------------- |
| `isConnected`   | `boolean`                      | Whether the station is connected                    |
| `mac`           | `string` (readonly)            | MAC address                                         |
| `hostname`      | `string \| undefined`          | Hostname (read/write)                               |
| `txPower`       | `number` (readonly)            | Transmit power in dBm; set via `connect({txPower})` |
| `rssiThreshold` | `number`                       | Low-RSSI event threshold (read/write)               |
| `powerSave`     | `PowerSaveMode`                | Power save mode: `'none'`, `'min'`, `'max'`         |
| `country`       | `WifiCountryCode \| undefined` | Regulatory country code                             |

## Station events

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
wifi.onConnect.subscribe((info) => {
  console.log('Connected: %s', info.ip)
})

wifi.onDisconnect.subscribe((reason) => {
  console.log('Disconnected: %s', reason)
})

wifi.onRssiLow.subscribe((rssi) => {
  console.log('Signal weak: %d dBm', rssi)
})
```

| Stream         | Payload                            | Description                          |
| -------------- | ---------------------------------- | ------------------------------------ |
| `onConnect`    | `Observable<WifiConnectionInfo>`   | Connected to network                 |
| `onDisconnect` | `Observable<WifiDisconnectReason>` | Disconnected from network            |
| `onRssiLow`    | `Observable<number>`               | Signal dropped below `rssiThreshold` |

Each property is an `Observable<T>` (see [`mikro/observable`](/api/observable)). Subscribers fire on the JS loop thread; call `unsubscribe()` on the returned `Subscription` to stop receiving values.

## Access point

The `wifi.ap` sub-interface lets you run the ESP32 as an access point.

### wifi.ap.start(options)

```ts
start(options: ApStartOptions): Result<void, WifiError>
```

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
wifi.ap.start({ssid: 'my-device', passphrase: 'secret123'}).orPanic('AP failed')
```

### wifi.ap.stop()

```ts
stop(): Result<void, WifiError>
```

### wifi.ap properties

| Property          | Type                             | Description                       |
| ----------------- | -------------------------------- | --------------------------------- |
| `isActive`        | `boolean` (readonly)             | Whether the AP is running         |
| `ip`              | `string \| undefined` (readonly) | AP IP address                     |
| `stations`        | `ApStationInfo[]` (readonly)     | Connected clients                 |
| `inactiveTimeout` | `number`                         | Kick idle clients after N seconds |

### wifi.ap.deauthStation(mac)

```ts
deauthStation(mac: string): Result<void, WifiError>
```

### wifi.ap events

```ts twoslash
import {wifi} from 'mikro/wifi'
// ---cut---
wifi.ap.onStationConnect.subscribe((info) => {
  console.log('client connected: %s', info.mac)
})

wifi.ap.onStationDisconnect.subscribe((info) => {
  console.log('client disconnected: %s', info.mac)
})
```

| Stream                | Payload                     | Description         |
| --------------------- | --------------------------- | ------------------- |
| `onStationConnect`    | `Observable<ApStationInfo>` | Client connected    |
| `onStationDisconnect` | `Observable<ApStationInfo>` | Client disconnected |

## Types

### WifiStatus

```ts
type WifiStatus =
  | 'STOPPED'
  | 'IDLE'
  | 'NO_SSID_AVAIL'
  | 'SCAN_COMPLETED'
  | 'CONNECTED'
  | 'CONNECT_FAILED'
  | 'CONNECTION_LOST'
  | 'DISCONNECTED'
```

### WifiConnectionInfo

```ts
interface WifiConnectionInfo {
  ip: string
  netmask: string
  gateway: string
}
```

### ScanResult

```ts
interface ScanResult {
  ssid: string
  bssid: string
  channel: number
  rssi: number
  authMode: AuthMode
  hidden: boolean
}
```

### AuthMode

```ts
type AuthMode = 'open' | 'wpa2-psk' | 'wpa3-psk' | 'wpa2-wpa3-psk' | 'unknown'
```

### WifiDisconnectReason

```ts
type WifiDisconnectReason = 'no-ssid' | 'auth-failed' | 'connection-lost' | 'disconnected'
```

## Errors

### WifiError

| Variant             | Fields    | Description                       |
| ------------------- | --------- | --------------------------------- |
| `CountryNotSet`     | —         | Regulatory country not configured |
| `StartFailed`       | `message` | Failed to start WiFi              |
| `ConnectFailed`     | `message` | Connection attempt failed         |
| `ConnectInProgress` | —         | Already connecting                |
| `DisconnectFailed`  | `message` | Failed to disconnect              |
| `ScanFailed`        | `message` | Scan failed                       |
| `ScanInProgress`    | —         | Already scanning                  |
| `NotInitialized`    | —         | WiFi not started                  |
| `ConfigFailed`      | `message` | IP configuration failed           |
| `ApStartFailed`     | `message` | Failed to start AP                |
| `ApStopFailed`      | `message` | Failed to stop AP                 |
| `SetFailed`         | `message` | Failed to set property            |
| `GetFailed`         | `message` | Failed to get property            |
