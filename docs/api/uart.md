---
title: uart
description: UART serial communication
---

# uart

```ts twoslash
import {Uart} from 'mikro/uart'
```

Communicate with serial peripherals such as cellular modems, GPS modules, sensors, and other microcontrollers over UART.

## Usage

```ts twoslash
import {Uart} from 'mikro/uart'

const uart = new Uart(1, {tx: 17, rx: 16, baudRate: 9600})
uart.begin().orPanic('UART init failed')

// Write data
uart.write(new Uint8Array([0x41, 0x54, 0x0d, 0x0a])).orPanic('write failed')

// Read data (async iterator of Result<Uint8Array, UartError>)
const reader = uart.read().orPanic('read failed')
for await (const chunk of reader) {
  if (!chunk.ok) {
    console.error('uart read failed:', chunk.error)
    break
  }
  console.log('received: %s', new TextDecoder().decode(chunk.value))
  break
}

uart.end()
```

## Constructor

### new Uart(port, options)

```ts
new Uart(port: number, options: {tx: number; rx: number; baudRate: number}): Uart & UartTx & UartRx
new Uart(port: number, options: {tx: number; baudRate: number}): Uart & UartTx
new Uart(port: number, options: {rx: number; baudRate: number}): Uart & UartRx
```

Creates a new UART instance. Provide at least one of `tx` or `rx`. The available methods depend on which pins are provided:

- **Both TX and RX**: `write()` and `read()` available
- **TX only**: `write()` available, `read()` is a compile-time error
- **RX only**: `read()` available, `write()` is a compile-time error

**Parameters:**

- `port`: UART port number (0, 1, or 2 depending on chip)
- `options`: see below

| Option     | Type     | Required | Description                            |
| ---------- | -------- | -------- | -------------------------------------- |
| `tx`       | `number` | no\*     | TX GPIO pin                            |
| `rx`       | `number` | no\*     | RX GPIO pin                            |
| `baudRate` | `number` | yes      | Baud rate (for example 9600 or 115200) |

\* At least one of `tx` or `rx` must be provided.

::: warning UART0
UART0 is typically used for the console/REPL. On chips with USB Serial/JTAG (ESP32-C6, ESP32-S3, and similar), UART0 pins are free when the console runs over USB. If you open a port that the console is using, `begin()` will return a `DriverInstallFailed` error.
:::

## Methods

### uart.begin()

```ts
begin(): Result<void, UartError>
```

Install the UART driver and configure pins. Must be called before `write()` or `read()`. Calling `begin()` on an already-started UART is a no-op.

Hardcoded to 8N1 (8 data bits, no parity, 1 stop bit) with a 2048-byte receive buffer.

### uart.end()

```ts
end(): Result<void, UartError>
```

Uninstall the UART driver and release resources. Cancels any active `read()` iterator. Calling `end()` on an already-stopped UART is a no-op.

### uart.write(data)

```ts
write(data: Uint8Array): Result<void, UartError>
```

Write bytes to the TX pin. Blocks until all bytes are written to the FIFO. Only available when `tx` was provided in the constructor.

### uart.read()

```ts
read(): Result<AsyncIterable<Result<Uint8Array, UartError>>, UartError>
```

Start reading from the RX pin. The outer Result wraps the initial open. The iterable yields `Result<Uint8Array, UartError>`: ok-wrapped chunks on success, a single terminal err item if the port becomes unavailable mid-iteration (for example after `end()` was called). Only available when `rx` was provided in the constructor.

Each yielded chunk contains whatever bytes have accumulated in the receive buffer since the last read. Chunk boundaries do not correspond to message boundaries; higher-level framing (line splitting, packet parsing) is the caller's responsibility.

Only one reader can be active at a time. Calling `read()` while another reader is active returns an `AlreadyReading` error. Breaking out of the `for await` loop cleanly closes the reader, and `read()` can be called again.

```ts twoslash
import {Uart} from 'mikro/uart'
const uart = new Uart(1, {tx: 17, rx: 16, baudRate: 115200})
uart.begin().orPanic('UART init failed')
// ---cut---
const reader = uart.read().orPanic('read failed')

for await (const chunk of reader) {
  if (!chunk.ok) {
    console.error('uart read failed:', chunk.error)
    break
  }
  const text = new TextDecoder().decode(chunk.value)
  console.log(text)
  if (text.includes('OK')) break // break is safe, read() can be called again
}
```

## Types

### UartTx

```ts
interface UartTx {
  write(data: Uint8Array): Result<void, UartError>
}
```

### UartRx

```ts
interface UartRx {
  read(): Result<AsyncIterable<Result<Uint8Array, UartError>>, UartError>
}
```

## Errors

### UartError

| Variant               | Fields    | Description                               |
| --------------------- | --------- | ----------------------------------------- |
| `DriverInstallFailed` | `message` | UART driver installation failed           |
| `SetPinFailed`        | `message` | GPIO pin configuration failed             |
| `InvalidParam`        | `message` | Invalid UART parameters                   |
| `WriteFailed`         | `message` | Write operation failed                    |
| `ReadFailed`          | `message` | Read operation failed                     |
| `NotStarted`          | --        | `begin()` was not called                  |
| `AlreadyReading`      | --        | Another `read()` iterator is still active |
| `NoRxPin`             | --        | `read()` called but no RX pin configured  |
| `NoTxPin`             | --        | `write()` called but no TX pin configured |
