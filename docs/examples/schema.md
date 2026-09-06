---
title: Schema
description: Runtime data validation with typed schemas
---

# Schema

Validates JSON data received over HTTP using typed schemas. This is a typical use case: data arrives from an external API as `unknown`, and you need to verify its shape before using it. See the [schema API reference](/api/schema) for caveats, when to use, and the full type surface.

## Hardware

- Any ESP32 board with WiFi
- USB cable

## Code

```ts twoslash
import * as s from 'mikro/schema'

// Define a schema for the API response
const WeatherResponse = s.object({
  temperature: s.number(),
  humidity: s.number(),
  description: s.optional(s.string()),
})

// Simulate data arriving from an HTTP API
const raw: unknown = JSON.parse('{"temperature": 22.5, "humidity": 45.2}')

const result = s.parse(WeatherResponse, raw)
if (result.ok) {
  console.log(`Temperature: ${result.value.temperature}`)
  console.log(`Humidity: ${result.value.humidity}`)
} else {
  console.error('Invalid response:', result.error)
}
```

## Walkthrough

1. **Define a schema.** `s.object({...})` describes the expected shape. Each field gets a type validator like `s.number()` or `s.string()`.

2. **Parse untrusted data.** `s.parse()` checks the data against the schema and returns a [`Result`](/api/result). On success, `result.value` is fully typed. On failure, `result.error` tells you what went wrong and where.

3. **No exceptions.** Validation never throws. You always get a [`Result`](/api/result) to handle both cases explicitly.

## Create project

::: code-group

```sh [pnpm]
pnpm create mikro --template schema
```

```sh [npm]
npm create mikro -- --template schema
```

```sh [yarn]
yarn create mikro --template schema
```

```sh [bun]
bun create mikro --template schema
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

The console prints the valid readings and commands, and clear error messages for the invalid ones.

[View source on GitHub](https://github.com/mikrojs/mikro/tree/main/examples/schema)
