# Changelog

## 0.20.2 (2026-09-06)

### Bug fixes

- improve error logging pattern ([#367](https://github.com/mikrojs/mikro/pull/367))
- **runtime:** report failing imports instead of silently hanging ([#365](https://github.com/mikrojs/mikro/pull/365))

## 0.20.1 (2026-09-05)

### Features

- **firmware:** enable custom firmware to disable WiFi stack ([#363](https://github.com/mikrojs/mikro/pull/363))

## 0.20.0 (2026-09-05)

### Breaking changes

- **ota:** drop the step primitives report() and settle() subsume ([#355](https://github.com/mikrojs/mikro/pull/355))

### Features

- **dev:** census the retained heap of importing each builtin ([#362](https://github.com/mikrojs/mikro/pull/362))
- **watchdog:** add watchdog support ([#359](https://github.com/mikrojs/mikro/pull/359))
- **ota:** let own-transport clients report a declined offer ([#353](https://github.com/mikrojs/mikro/pull/353))

### Bug fixes

- **test:** keep the boot gate steady across a memReserved change ([#361](https://github.com/mikrojs/mikro/pull/361))
- **test:** measure retained heap per suite ([#360](https://github.com/mikrojs/mikro/pull/360))
- **repl:** keep serving through the panic grace window ([#358](https://github.com/mikrojs/mikro/pull/358))
- resume device when runtime pause times out during deploy ([#356](https://github.com/mikrojs/mikro/pull/356))

### Other

- **e2e:** update heap snapshots after memory optimization ([#357](https://github.com/mikrojs/mikro/pull/357))

## 0.19.0 (2026-08-30)

### Breaking changes

- **cli:** key heap snapshots by chip and tolerate small drift ([#347](https://github.com/mikrojs/mikro/pull/347))

### Features

- **schema:** make the DSL native to cut heap and check bounds on device ([#352](https://github.com/mikrojs/mikro/pull/352))
- gate features on both silicon and compiled stack ([#346](https://github.com/mikrojs/mikro/pull/346))
- schema annotations ([#344](https://github.com/mikrojs/mikro/pull/344))
- **ota:** fold the own-transport check-in into report() and settle() ([c8ae3dc](https://github.com/mikrojs/mikro/commit/c8ae3dcaa175928b2a1516b74ea1e0259f64e262))

### Bug fixes

- **firmware:** memReserved guardrails and measured heap diagnostics ([#350](https://github.com/mikrojs/mikro/pull/350))
- **test:** fail on beforeAll errors and tweak e2e memory gates ([#348](https://github.com/mikrojs/mikro/pull/348))
- **ota:** make watch() error on an un-enrolled device ([#345](https://github.com/mikrojs/mikro/pull/345))
- **firmware:** fsync log-file flushes so they survive a crash and read back live ([324b510](https://github.com/mikrojs/mikro/commit/324b510a42a2d9da0928aeeb4ec5b375d160a3a5))

### Performance

- **native:** port http request/helpers and wifi builtins to c++ ([#349](https://github.com/mikrojs/mikro/pull/349))
- **native:** drop the per-allocation size header on ESP32 ([b229c0b](https://github.com/mikrojs/mikro/commit/b229c0b3d9b35795c86a30a17ce53e62dada2f7e))
- **firmware:** allocate log-file buffers only when logging is configured ([122ff22](https://github.com/mikrojs/mikro/commit/122ff22fd44ef130ad187546851a70ccf0c0ce8d))
- **firmware:** allocate test-supervisor scratch only during test runs ([3257c77](https://github.com/mikrojs/mikro/commit/3257c776a0405dcce365cadfe7031a975a8e2a81))
- **firmware:** make assert() silent to unpin its strings from RAM ([c77a687](https://github.com/mikrojs/mikro/commit/c77a687a7500232f29d175c1737d0f7608d0c412))
- **firmware:** drop the extra WiFi IRAM optimization ([12e0a0d](https://github.com/mikrojs/mikro/commit/12e0a0dcf3491df15e4b3c9263a0224d529b539c))
- **firmware:** run the WiFi modem-sleep RX path from flash ([6f7204d](https://github.com/mikrojs/mikro/commit/6f7204d8b4b583e67b847299b5a172983f33b673))

### Other

- **ota:** copy edits ([#351](https://github.com/mikrojs/mikro/pull/351))
- **repo:** record the heap-accounting change and refresh the optimize skill ([dbb72cd](https://github.com/mikrojs/mikro/commit/dbb72cd82be888c6e2236772f4835c4ca244a7cd))
- **repo:** cover the modem-sleep wake path and the file logger ([5220dbd](https://github.com/mikrojs/mikro/commit/5220dbdf5eb89dcdaa167602db1bf00d78061015))
- stop cold-start suites timing out under concurrent load ([#340](https://github.com/mikrojs/mikro/pull/340))
