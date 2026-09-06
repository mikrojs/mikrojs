import {env} from 'mikro/env'
import {ota} from 'mikro/ota'
import * as otaClient from 'mikro/ota/client'
import {digitalWrite, pinMode} from 'mikro/pin'
import {Pwm} from 'mikro/pwm'
import {ok} from 'mikro/result'
import {restart} from 'mikro/sys'
import {wifi} from 'mikro/wifi'

async function main(config: OtaConfig) {
  console.log('Watching for OTA updates')
  const watching = otaClient.watch({
    beforeCheck: async () => {
      const ssid = env.require('WIFI_SSID')
      const passphrase = env.require('WIFI_PASSPHRASE')

      console.log('connecting to WiFi network %s…', ssid)
      const connected = await wifi.connect({ssid, passphrase})
      if (!connected.ok) {
        console.error('WiFi connect failed:', connected.error)
        return connected
      } else {
        console.log('WiFi connected')
        return ok(() => {
          wifi.disconnect().orPanic('Unable to disconnect WiFi')
          console.log('WiFi disconnected')
        })
      }
    },
    onConfig: (updatedConfig) => {
      console.log('Ota config updated: ', updatedConfig)
      console.log('Restarting…')
      restart()
    },
    checkinIntervalMs: config.checkinInterval,
  })
  if (!watching.ok) {
    // The app still runs, it just gets no updates. A restart after
    // `mikro ota enroll` is what turns them on.
    console.error('OTA updates are disabled:', watching.error)
  }

  console.log('OTA Config: ', config)
  if (!config.on) {
    pinMode(config.pin, 'OUTPUT').orPanic('Unable to set pin mode')
    digitalWrite(config.pin, 1).orPanic('Unable to write pin')
    return
  }

  const led = new Pwm(config.pin, {freq: config.pwm.freq})
  // Breathe: smoothly fade in and out forever
  while (true) {
    const fadeIn = await led.fade(config.pwm.duty, config.interval / 2)
    if (!fadeIn.ok) {
      console.error('Fade in failed:', fadeIn.error)
      break
    }

    const fadeOut = await led.fade(0.0, config.interval / 2)
    if (!fadeOut.ok) {
      console.error('Fade out failed:', fadeOut.error)
      break
    }
  }
}

await main(ota.config())
