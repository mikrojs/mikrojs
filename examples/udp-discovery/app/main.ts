import {env} from 'mikro/env'
import {sleep} from 'mikro/sleep'
import {deviceId} from 'mikro/sys'
import {bind} from 'mikro/udp'
import {wifi} from 'mikro/wifi'

// IPv4 site-local discovery group. 224.0.0.251 is the mDNS group, which
// home routers almost always forward (Bonjour/AirPlay/Chromecast use it).
// Note: real mDNS is on port 5353; we use a different port to avoid
// colliding with system mDNS responders.
const GROUP = '224.0.0.251'
const PORT = 7654
const ANNOUNCE_MS = 2000

const ssid = env.require('WIFI_SSID')
const passphrase = env.require('WIFI_PASSPHRASE')

console.log('Connecting to %s...', ssid)
const connected = await wifi.connect({ssid, passphrase})
if (!connected.ok) {
  console.error('WiFi connect failed:', connected.error)
} else {
  console.log('Connected. IP: %s', connected.value.ip)

  const bound = await bind({port: PORT, family: 'ipv4'})
  if (!bound.ok) {
    console.error('bind failed:', bound.error)
  } else {
    const sock = bound.value

    const joined = sock.joinMulticastGroup({address: GROUP})
    if (!joined.ok) {
      console.error('join failed:', joined.error)
      sock.close()
    } else {
      const me = deviceId

      sock.onMessage.subscribe(({msg, from}) => {
        const text = new TextDecoder().decode(msg)
        // Multicast loops back to ourselves on the same host; skip.
        if (text === me) return
        console.log('discovered %s at %s', text, from.address)
      })

      console.log('announcing as %s on %s:%d', me, GROUP, PORT)

      while (true) {
        const sent = await sock.send(me, {address: GROUP, port: PORT, family: 'ipv4'})
        if (!sent.ok) console.error('send failed:', sent.error)
        await sleep(ANNOUNCE_MS)
      }
    }
  }
}
