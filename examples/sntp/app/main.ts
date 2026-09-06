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

// Or use background mode to keep re-syncing periodically:
// const {time, stop} = await sntp.sync({background: true})
// ... later: stop()
