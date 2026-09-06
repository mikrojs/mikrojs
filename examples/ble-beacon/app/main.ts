import {ble, peripheral} from 'mikro/ble'
import {sleep} from 'mikro/sleep'

// Set a friendly name. If omitted, the runtime uses "mikrojs-<MAC suffix>".
ble.name = 'mikrojs-beacon'
console.log('hi')

// Encode a fake temperature reading as manufacturer-specific data.
// First two bytes are the company ID (0xFFFF = "no company", reserved for testing).
// Remaining bytes are a little-endian int16 in units of 0.01°C.
function encodeTemperature(tempCelsius: number): Uint8Array {
  const centi = Math.round(tempCelsius * 100)
  return new Uint8Array([
    0xff,
    0xff, // company ID (little-endian): 0xFFFF
    centi & 0xff, // low byte
    (centi >> 8) & 0xff, // high byte
  ])
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
    // Drift the reading slightly so a watcher can see it change.
    temp += (Math.random() - 0.5) * 0.2

    // Broadcaster mode does not support updating the manufacturer-data
    // payload in place, so stop and restart the advertisement to publish
    // the new value.
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
console.log('hi')
