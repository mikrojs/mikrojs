---
title: PWM LED
description: Fade an LED in and out using PWM
---

# PWM LED

A "breathing" LED effect: smoothly fades an LED in and out using PWM. See the [pwm API reference](/api/pwm) for the full type surface.

## Hardware

- Any ESP32 board with a GPIO pin that supports PWM
- An LED connected through a 220-ohm resistor
- USB cable

## Code

```ts twoslash
import {Pwm} from 'mikro/pwm'

// GPIO 15 is the built-in LED on XIAO ESP32C6. Replace with your board's LED pin.
const LED_PIN = 15

const led = new Pwm(LED_PIN, {freq: 50, duty: 0})

// Breathe: smoothly fade in and out forever
while (true) {
  const fadeIn = await led.fade(1.0, 1000)
  if (!fadeIn.ok) {
    console.error('Fade in failed:', fadeIn.error)
    break
  }

  const fadeOut = await led.fade(0.0, 1000)
  if (!fadeOut.ok) {
    console.error('Fade out failed:', fadeOut.error)
    break
  }
}
```

## Walkthrough

1. **PWM setup.** `new Pwm(pin, {freq, duty})` creates a PWM channel on the given pin. `freq` sets the PWM frequency in Hz; `duty` sets the initial duty cycle (0.0 to 1.0).

2. **Hardware fading.** `led.fade(target, durationMs)` smoothly transitions the duty cycle to `target` over `durationMs` milliseconds. This runs in hardware on the ESP32's LEDC peripheral, so it does not block the event loop.

3. **Error handling.** Both `fade()` calls return a `Promise<Result>` ([see Result](/api/result)). If something goes wrong (for example an invalid pin), the loop breaks with a clear error message.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template pwm-led
```

```sh [npm]
npm create mikro -- --template pwm-led
```

```sh [yarn]
yarn create mikro --template pwm-led
```

```sh [bun]
bun create mikro --template pwm-led
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

The LED fades smoothly in and out in a continuous breathing pattern.

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/pwm-led)
