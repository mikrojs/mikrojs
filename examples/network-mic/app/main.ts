// Network microphone: capture the ICS-43434 over I2S and serve the live audio
// over HTTP (/stream.wav and /stream.raw). See README.md for wiring and run steps.

import {env} from 'mikro/env'
import {createServer, type ServerResponse} from 'mikro/http/server'
import {I2s} from 'mikro/i2s'
import {wifi} from 'mikro/wifi'

// Pins: SCK/BCLK -> GPIO 4, WS/LRCL -> GPIO 5, SD/DOUT -> GPIO 6. See README.md
// for the full wiring and the mic's usable sample-rate range.
const MIC = {sampleRate: 48_000, bckPin: 4, wsPin: 5, dinPin: 6}

// gainBits left-shifts each sample in C. 0 keeps faithful 16-bit levels; raise it
// for a hotter stream and expect clipping on loud sounds.
const GAIN_BITS = 0
const FRAME = MIC.sampleRate / 10 // ~100 ms of samples per HTTP chunk
const BYTES_PER_SAMPLE = 2 // mono int16

const ssid = env.require('WIFI_SSID')
const passphrase = env.require('WIFI_PASSPHRASE')

console.log('Connecting to %s...', ssid)
const connected = await wifi.connect({ssid, passphrase})
if (!connected.ok) {
  // onPanic: restart (mikro.config.ts) retries the connection on a clean boot.
  connected.orPanic('WiFi connect failed')
} else {
  const ip = connected.value.ip
  console.log('Connected.')

  const mic = new I2s(0, {
    bclk: MIC.bckPin,
    ws: MIC.wsPin,
    din: MIC.dinPin,
    sampleRate: MIC.sampleRate,
    bitsPerSample: 32,
    channels: 'mono',
    // Generous DMA buffering (~160 ms) so the mic keeps filling while the HTTP
    // chunk flushes between captures, so no samples drop.
    dmaFrames: 960,
    dmaBuffers: 8,
  })
  mic.begin().orPanic('Failed to start I2S')

  // 44-byte PCM WAV header. Sizes are 0xFFFFFFFF (unknown) since the stream has no
  // known length: players read the format, then stream the data chunk until close.
  function wavHeader(): Uint8Array {
    const buf = new ArrayBuffer(44)
    const dv = new DataView(buf)
    const ascii = (off: number, s: string) => {
      for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i))
    }
    ascii(0, 'RIFF')
    dv.setUint32(4, 0xffffffff, true) // RIFF size: unknown (streaming)
    ascii(8, 'WAVE')
    ascii(12, 'fmt ')
    dv.setUint32(16, 16, true) // fmt chunk size
    dv.setUint16(20, 1, true) // PCM
    dv.setUint16(22, 1, true) // channels
    dv.setUint32(24, MIC.sampleRate, true)
    dv.setUint32(28, MIC.sampleRate * BYTES_PER_SAMPLE, true) // byte rate
    dv.setUint16(32, BYTES_PER_SAMPLE, true) // block align
    dv.setUint16(34, 8 * BYTES_PER_SAMPLE, true) // bits per sample
    ascii(36, 'data')
    dv.setUint32(40, 0xffffffff, true) // data size: unknown (streaming)
    return new Uint8Array(buf)
  }

  // Endless body: optional WAV header, then 16-bit PCM frames packed in C by
  // capture(). The server awaits each chunk (backpressure) and ends the loop on
  // client disconnect; it serves one reader at a time.
  async function* audioBody(withHeader: boolean): AsyncGenerator<Uint8Array> {
    console.log('reader connected (%s)', withHeader ? 'wav' : 'raw')
    if (withHeader) yield wavHeader()
    for (;;) {
      const pcm = mic.capture(FRAME, {gainBits: GAIN_BITS})
      if (!pcm.ok) {
        console.error('mic capture failed:', pcm.error)
        break
      }
      yield new Uint8Array(pcm.value.buffer, pcm.value.byteOffset, pcm.value.byteLength)
    }
  }

  const server = createServer((req): ServerResponse => {
    if (req.method !== 'GET') return {status: 405, body: 'method not allowed'}
    const path = req.url.split('?')[0]

    if (path === '/stream.wav') {
      return {status: 200, headers: {'content-type': 'audio/wav'}, body: audioBody(true)}
    }
    if (path === '/stream.raw') {
      // audio/L16 is the IANA type for raw 16-bit PCM; rate/channels as params.
      return {
        status: 200,
        headers: {'content-type': `audio/L16; rate=${MIC.sampleRate}; channels=1`},
        body: audioBody(false),
      }
    }
    return {status: 404, body: 'not found'}
  })

  const listening = server.listen({port: 80})
  listening.orPanic('listen failed')
  console.log('Live mic streaming at http://%s/stream.wav', ip)
  console.log('Open it in a browser, or run: ffplay http://%s/stream.wav', ip)
}
