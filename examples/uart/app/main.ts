import {sleep} from 'mikro/sleep'
import {Uart} from 'mikro/uart'

// UART loopback example for ESP32C6: connect TX (GPIO 17) to RX (GPIO 16) with a jumper wire
const TX_PIN = 16
const RX_PIN = 17
await sleep(2000)
const uart = new Uart(1, {tx: TX_PIN, rx: RX_PIN, baudRate: 115200})

uart.begin().orPanic('Failed to start UART')

const message = new TextEncoder().encode('Hello from UART!\n')
uart.write(message).orPanic('Failed to write')

const reader = uart.read()

if (!reader.ok) {
  console.error('Failed to start reading:', reader.error)
} else {
  // Give data a moment to loop back
  await sleep(1000)
  for await (const chunk of reader.value) {
    if (!chunk.ok) {
      console.error('UART read error:', chunk.error)
      break
    }
    console.log('Received: %s', new TextDecoder().decode(chunk.value))
  }
}

uart.end().orPanic('Failed to stop UART')
console.log('Done!')
