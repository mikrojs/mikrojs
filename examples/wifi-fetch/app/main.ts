import {env} from 'mikro/env'
import {request} from 'mikro/http/request'
import {memoryUsage} from 'mikro/sys'
import {wifi} from 'mikro/wifi'

const ssid = env.require('WIFI_SSID')
const passphrase = env.require('WIFI_PASSPHRASE')

// Connect to WiFi
console.log(`Connecting to ${ssid}...`)
const connectResult = await wifi.connect({ssid, passphrase})
if (!connectResult.ok) {
  console.error('WiFi connect failed:', connectResult.error)
} else {
  console.log('Connected! IP: %s', connectResult.value.ip)

  // Request JSON from an API
  const result = await request('https://httpbingo.org/json')
  if (result.ok) {
    if (!result.value.ok) {
      console.error(`HTTP error: ${result.value.status}`)
    } else {
      const data = await result.value.json()
      if (!data.ok) {
        console.error('Body decode failed:', data.error)
      } else {
        console.log('Fetched: %o', data.value)
      }
    }
  } else {
    console.error('Request failed:', result.error)
  }
}

const mem = memoryUsage()
console.log('free heap: %dKB', (mem.heapTotal - mem.heapUsed) / 1000)
