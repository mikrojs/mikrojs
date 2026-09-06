import * as s from 'mikro/schema'

// Define a schema for sensor readings
const SensorReading = s.object({
  temperature: s.number(),
  humidity: s.number(),
  label: s.optional(s.string()),
})

// Define a schema for device commands using tagged unions
const Command = s.taggedUnion('type', {
  setInterval: s.object({ms: s.number()}),
  calibrate: s.object({offset: s.number()}),
  reset: s.object({}),
})

// ── Validate a sensor reading ───────────────────────────────────────

const goodReading: unknown = {temperature: 22.5, humidity: 45.2, label: 'living-room'}
const badReading: unknown = {temperature: 'warm', humidity: 45.2}

const result1 = s.parse(SensorReading, goodReading)
if (result1.ok) {
  console.log(`Temperature: ${result1.value.temperature}`)
  console.log(`Humidity: ${result1.value.humidity}`)
  console.log(`Label: ${result1.value.label ?? '(none)'}`)
} else {
  console.error('Invalid reading:', result1.error)
}

const result2 = s.parse(SensorReading, badReading)
if (result2.ok) {
  console.log('This should not happen')
} else {
  console.error('Caught bad reading:', result2.error)
}

// ── Validate a tagged union ─────────────────────────────────────────

const commands: unknown[] = [
  {type: 'setInterval', ms: 5000},
  {type: 'calibrate', offset: -1.2},
  {type: 'reset'},
  {type: 'explode'},
]

for (const cmd of commands) {
  const result = s.parse(Command, cmd)
  if (result.ok) {
    console.log(`Valid command: ${result.value.type}`)
  } else {
    console.error('Invalid command:', result.error)
  }
}
