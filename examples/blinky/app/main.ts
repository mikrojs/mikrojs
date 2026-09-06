import {digitalWrite, pinMode} from 'mikro/pin'
import {sleep} from 'mikro/sleep'

let value: 0 | 1 = 0
// GPIO 15 is the built-in LED on XIAO ESP32C6. Replace with your board's LED pin.
const PIN = 15

pinMode(PIN, 'OUTPUT').orPanic('Failed to configure LED pin')

while (true) {
  value = value === 0 ? 1 : 0
  const writeResult = digitalWrite(PIN, value)
  if (!writeResult.ok) {
    console.error('Write pin failed:', writeResult.error)
  }

  await sleep(1000)
}
