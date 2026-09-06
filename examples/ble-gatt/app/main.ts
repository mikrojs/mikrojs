import {ble, peripheral} from 'mikro/ble'
import {sleep} from 'mikro/sleep'

// Set a friendly name. If omitted, the runtime uses "mikrojs-<MAC suffix>".
ble.name = 'mikrojs-hello'

// Peripheral lifecycle listeners. These fire on the JS loop thread as
// centrals connect, renegotiate MTU, or disconnect. Register before
// advertise so no event is missed.
peripheral.onConnect.subscribe((info) => {
  console.log('connected: %s mtu: %d', info.address, info.mtu)
})

peripheral.onDisconnect.subscribe(async (info) => {
  console.log('disconnected: %s mtu: %d', info.address, info.mtu)
  // NimBLE stops advertising on connect. Re-advertise so the device stays
  // discoverable once the peer goes away.
  const r = await peripheral.advertise({
    name: 'mikrojs-hello',
    connectable: true,
    interval: {min: 100, max: 150},
    includeTxPower: true,
    services: currentServices,
  })
  if (!r.ok) console.error('re-advertise failed:', r.error)
})

peripheral.onMtu.subscribe((info) => {
  console.log('mtu renegotiated on %d new mtu: %d', info.id, info.mtu)
})

// Characteristic declarations. The command characteristic has an onWrite
// handler that runs asynchronously after a central writes to it — the
// write is already acknowledged at the GATT layer by the time this runs.
const currentServices = [
  {
    uuid: '180f', // Battery Service
    characteristics: [
      {
        uuid: '2a19', // Battery Level
        properties: ['read', 'notify'] as const,
        value: new Uint8Array([87]),
      },
    ],
  },
  {
    // Custom vendor service — replace the UUID for real projects.
    uuid: '7f3a1b00-4a2f-4e5b-9a3c-1e2d3f4a5b6c',
    characteristics: [
      {
        // Read-only info string.
        uuid: '7f3a1b01-4a2f-4e5b-9a3c-1e2d3f4a5b6c',
        properties: ['read'] as const,
        value: new TextEncoder().encode('mikrojs v0: hello from BLE'),
      },
      {
        // Writable command characteristic. The onWrite handler logs the
        // received bytes whenever a central writes to it.
        uuid: '7f3a1b02-4a2f-4e5b-9a3c-1e2d3f4a5b6c',
        properties: ['read', 'write', 'writeWithoutResponse'] as const,
        value: new Uint8Array([0]),
        onWrite: (bytes: Uint8Array) => {
          const hex = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join(' ')
          console.log('command write: %d bytes: %s', bytes.length, hex)
        },
      },
    ],
  },
]

const result = await peripheral.advertise({
  name: 'mikrojs-hello',
  connectable: true,
  interval: {min: 100, max: 150},
  includeTxPower: true,
  services: currentServices,
})

if (!result.ok) {
  console.error('ble.advertise failed:', result.error)
} else {
  console.log('BLE GATT peripheral advertising as %s', ble.name)
  console.log('address: %s', ble.address)
  console.log('open nRF Connect (or LightBlue) and connect to discover the services.')

  // Fake a battery draining from 87% down to 0% and wrapping back to 100%.
  // handle.notify both updates the cached value AND pushes the new bytes
  // to every subscriber that enabled notifications via the CCCD. If no
  // one is subscribed, it's a silent no-op — the value still updates, so
  // connecting later and reading Battery Level returns the current level.
  let level = 87
  while (true) {
    await sleep(2000)
    level = level > 0 ? level - 1 : 100
    const update = result.value.notify('180f', '2a19', new Uint8Array([level]))
    if (!update.ok) {
      console.error('notify failed:', update.error)
      break
    }
  }
}
