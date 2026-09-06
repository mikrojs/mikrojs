---
title: Blinky
description: Blink an LED, the "hello world" of microcontrollers
---

# Blinky

The classic first program: blink an LED on and off. Uses the [pin](/api/pin) and [sleep](/api/sleep) APIs.

## Hardware

- Any ESP32 board with a built-in LED, or an external LED connected to a GPIO pin
- USB cable

## Code

<CodeWiringTabs>
<template #code>

```ts twoslash
import {digitalWrite, pinMode} from 'mikro/pin'
import {sleep} from 'mikro/sleep'
import {memoryUsage} from 'mikro/sys'

const mem = memoryUsage()
console.log('free heap: %dKB', (mem.heapTotal - mem.heapUsed) / 1000)

let value: 0 | 1 = 0
// GPIO 15 is the built-in LED on XIAO ESP32C6. Replace with your board's LED pin.
const PIN = 15

pinMode(PIN, 'OUTPUT').orPanic('Failed to configure LED pin')

while (true) {
  value = value === 0 ? 1 : 0
  console.log(value ? 'ON' : 'OFF')

  const mem = memoryUsage()
  console.log('free memory: %dKB', (mem.heapTotal - mem.heapUsed) / 1000)

  const writeResult = digitalWrite(PIN, value)
  if (!writeResult.ok) {
    console.error('Write failed:', writeResult.error)
  }

  await sleep(1000)
}
```

</template>
<template #wiring>
<BlinkyDiagram />

Connect the LED's longer leg (anode) to GPIO 15 through a 220-ohm resistor. Connect the shorter leg (cathode) to GND. If your board has a built-in LED, check the board documentation for its pin number.

</template>
</CodeWiringTabs>

## Walkthrough

1. **Memory check.** `memoryUsage()` reports heap usage at startup. Useful for spotting leaks over time.

2. **Pin setup.** `pinMode(PIN, 'OUTPUT')` configures the pin for output (GPIO 15 on XIAO ESP32C6). `.orPanic()` crashes with a clear message if this fails (for example on an invalid pin number).

3. **Main loop.** Toggles the pin between HIGH and LOW every second. `digitalWrite` returns a [`Result`](/api/result), so the code checks `.ok` and logs the error variant name if it fails.

4. **Async delay.** `await sleep(1000)` pauses for 1 second.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template blinky
```

```sh [npm]
npm create mikro -- --template blinky
```

```sh [yarn]
yarn create mikro --template blinky
```

```sh [bun]
bun create mikro --template blinky
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

The LED blinks, and the console prints memory usage every second.

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/blinky)
