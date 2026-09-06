---
title: SNTP
description: Sync the device clock via NTP for accurate timestamps
---

# SNTP

Connect to WiFi and synchronize the device clock via NTP, so `new Date()` returns real wall-clock time. Uses the [wifi](/api/wifi) and [sntp](/api/sntp) APIs.

## Hardware

- Any ESP32 board with WiFi
- USB cable
- A WiFi network with internet access

## Setup

Set your WiFi credentials as environment variables:

::: code-group

```sh [pnpm]
pnpm mikro env set WIFI_SSID YourNetworkName --no-secret
pnpm mikro env set WIFI_PASSPHRASE
```

```sh [npm]
npx mikro env set WIFI_SSID YourNetworkName --no-secret
npx mikro env set WIFI_PASSPHRASE
```

```sh [yarn]
yarn mikro env set WIFI_SSID YourNetworkName --no-secret
yarn mikro env set WIFI_PASSPHRASE
```

```sh [bun]
bunx mikro env set WIFI_SSID YourNetworkName --no-secret
bunx mikro env set WIFI_PASSPHRASE
```

:::

## Code

```ts twoslash
import {env} from 'mikro/env'
import {sleep} from 'mikro/sleep'
import {sntp} from 'mikro/sntp'
import {wifi} from 'mikro/wifi'

await sleep(2000)
console.log('Current time at startup: %s', new Date())

const ssid = env.require('WIFI_SSID')
const passphrase = env.require('WIFI_PASSPHRASE')

// Connect to WiFi
console.log(`Connecting to ${ssid}...`)
const connectResult = await wifi.connect({ssid, passphrase})
if (!connectResult.ok) {
  console.error('WiFi connect failed:', connectResult.error)
} else {
  console.log('Connected! IP: %s', connectResult.value.ip)

  // One-shot time sync
  console.log('Syncing time...')
  const syncResult = await sntp.sync({timezone: 'CET-1CEST,M3.5.0,M10.5.0/3'})
  if (!syncResult.ok) {
    console.error('SNTP sync failed:', syncResult.error)
  } else {
    console.log('Time synced: %s', syncResult.value.time)
    console.log('Current time: %s', new Date())
  }
}
```

## Walkthrough

1. **Before sync.** `new Date()` returns a timestamp near epoch 0 at startup, since the ESP32 has no battery-backed clock. The initial log shows this.

2. **WiFi first.** Time sync requires an internet connection. The example connects to WiFi and checks the result before attempting SNTP.

3. **Timezone string.** The `timezone` option uses POSIX TZ format. `CET-1CEST,M3.5.0,M10.5.0/3` means Central European Time (UTC+1), with daylight saving (CEST) starting the last Sunday of March and ending the last Sunday of October.

4. **One-shot vs. background.** `sntp.sync()` does a single time sync by default. Pass `{background: true}` to keep re-syncing periodically. The returned object includes a `stop()` function to cancel background sync.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template sntp
```

```sh [npm]
npm create mikro -- --template sntp
```

```sh [yarn]
yarn create mikro --template sntp
```

```sh [bun]
bun create mikro --template sntp
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

The time jumps from near-epoch to the correct wall-clock time after the sync completes.

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/sntp)
