import {Pwm} from 'mikro/pwm'

// GPIO 15 is the built-in LED on XIAO ESP32C6. Replace with your board's LED pin.
const LED_PIN = 15

const led = new Pwm(LED_PIN, {freq: 50, duty: 0})

// Breathe: smoothly fade in and out forever
while (true) {
  const fadeIn = await led.fade(1.0, 1000)
  if (!fadeIn.ok) {
    console.error('Fade in failed:', fadeIn.error)
    break
  }

  const fadeOut = await led.fade(0.0, 1000)
  if (!fadeOut.ok) {
    console.error('Fade out failed:', fadeOut.error)
    break
  }
}
