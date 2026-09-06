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

## 0.18.2 (2026-08-27)

### Features

- **ota:** let an own-transport client deliver device config ([#337](https://github.com/mikrojs/mikro/pull/337))

### Bug fixes

- **firmware:** require esp-idf 6.1 ([#339](https://github.com/mikrojs/mikro/pull/339))
- **native:** stop modules sharing data slots across runtimes ([#335](https://github.com/mikrojs/mikro/pull/335))

### Other

- **deps:** upgrade quickjs-ng to v0.16.2 ([#336](https://github.com/mikrojs/mikro/pull/336))

## 0.18.1 (2026-08-25)

### Features

- **ota:** app-defined device config ([#334](https://github.com/mikrojs/mikro/pull/334))
- **schema:** annotations, applyDefaults, and config helpers ([#332](https://github.com/mikrojs/mikro/pull/332))

### Bug fixes

- **http:** report heap figures in fetch connect errors ([#333](https://github.com/mikrojs/mikro/pull/333))
- **kv:** distinguish absent keys from failed reads ([#330](https://github.com/mikrojs/mikro/pull/330))
- **firmware:** stop repeated TLS handshakes running out of memory ([#329](https://github.com/mikrojs/mikro/pull/329))
- **native:** bound CBOR string spans against the buffer end ([#326](https://github.com/mikrojs/mikro/pull/326))
- **native:** reject truncated CBOR containers ([#325](https://github.com/mikrojs/mikro/pull/325))
- **native:** stop inspect leaving getter exceptions pending ([#323](https://github.com/mikrojs/mikro/pull/323))
- **fs:** flush before fstat so File.stat sees unflushed writes ([#322](https://github.com/mikrojs/mikro/pull/322))
- **native:** render symbol descriptions without leaving an exception ([#321](https://github.com/mikrojs/mikro/pull/321))
- **native:** resolve relative imports from root-level main modules ([#320](https://github.com/mikrojs/mikro/pull/320))

### Other

- **ota:** corrupt the unpack fixture by xor so the flip cannot be no-op ([#331](https://github.com/mikrojs/mikro/pull/331))
- **repo:** add coverage:lib:open for browsing the html coverage report ([#328](https://github.com/mikrojs/mikro/pull/328))
- **native:** add host test suite for the portable library ([#327](https://github.com/mikrojs/mikro/pull/327))
- raise vitest timeouts for flaky tests under full-suite load ([#324](https://github.com/mikrojs/mikro/pull/324))
- **native:** C++ coverage gate, OOM injection harness, sanitizer CI ([#319](https://github.com/mikrojs/mikro/pull/319))
- drop vendored generic skills ([#318](https://github.com/mikrojs/mikro/pull/318))
- improve writing, add ai disclosure ([#316](https://github.com/mikrojs/mikro/pull/316))

## 0.18.0 (2026-08-12)

### Breaking changes

- **quickjs:** upgrade quickjs-ng to v0.16.0 ([53364c3](https://github.com/mikrojs/mikro/commit/53364c374f850b6734b9ffff5a4848e77ee4bdff))

### Features

- **firmware:** show quickjs-ng version in the boot banner ([3b75d1b](https://github.com/mikrojs/mikro/commit/3b75d1be4d7f6c046659d8ffd49012f7d2a59d97))
- **wifi:** default the DHCP hostname to the device's name ([#293](https://github.com/mikrojs/mikro/pull/293))

### Bug fixes

- **native:** release what a producer acquired when its setup throws ([#311](https://github.com/mikrojs/mikro/pull/311))
- **native:** panic on every uncaught error, not just rejected promises ([36f6880](https://github.com/mikrojs/mikro/commit/36f688044f97a6710a1ecb9ca434fd973cedcfb1))
- **observable:** correct the documented error semantics ([706bd4c](https://github.com/mikrojs/mikro/commit/706bd4c53179b483db92597e85b4deecd3ffcaf4))
- **firmware:** grow the Xtensa main-task stack for quickjs-ng 0.16 frame sizes ([98bc191](https://github.com/mikrojs/mikro/commit/98bc1913b1241cc919c813b8b998e45c4de5ed99))
- **quickjs:** shrink the arena allocator to fit small heaps ([3ad3e0f](https://github.com/mikrojs/mikro/commit/3ad3e0f3366b52b3387df04139e0bbad58409e86))
- **native:** escalate unschedulable observable panics instead of dropping them ([c0cd920](https://github.com/mikrojs/mikro/commit/c0cd9205a1f124ea3cd9be6700af2e8c023ec5c9))
- **native:** free the addTeardown atom leaked on every multicast subscribe ([58fa10d](https://github.com/mikrojs/mikro/commit/58fa10d7a3c9794f2d9188e2d2c2de42326390d3))
- various build hygiene fixes ([#304](https://github.com/mikrojs/mikro/pull/304))
- **quickjs:** guard the configure-time patch sync for CMake script mode ([#301](https://github.com/mikrojs/mikro/pull/301))
- **quickjs:** cap GC threshold re-arm at the memory limit ([#297](https://github.com/mikrojs/mikro/pull/297))

### Performance

- **firmware:** reclaim the esp32 stack bump the trampoline made moot ([69bc288](https://github.com/mikrojs/mikro/commit/69bc288331a66767b2727a08ccb85e31e12ece72))
- **native:** trampoline observable dispatch to O(1) stack per delivery ([d4560d0](https://github.com/mikrojs/mikro/commit/d4560d0103b56a28d7d073a975229abd950cef56))
- **quickjs:** free module init bytecode once evaluation completes ([#298](https://github.com/mikrojs/mikro/pull/298))
- **firmware:** define NDEBUG for quickjs to drop per-string debug overhead ([#296](https://github.com/mikrojs/mikro/pull/296))
- **native:** preallocate module file buffer to avoid 1.5x growth peak ([#295](https://github.com/mikrojs/mikro/pull/295))

### Other

- **deps:** update dependency tar to v7.5.21 [security] ([#312](https://github.com/mikrojs/mikro/pull/312))
- **site:** fix broken links and contradictory error-handling guidance ([6cadfa8](https://github.com/mikrojs/mikro/commit/6cadfa839930cc7b315c413818a64926f0443f6c))
- update heap baselines ([37c0da4](https://github.com/mikrojs/mikro/commit/37c0da49f9ccd294b278e64eaf71a3426f1db778))
- **native:** mark consumed rejections handled via JS_PromiseMarkAsHandled ([a01c7b6](https://github.com/mikrojs/mikro/commit/a01c7b6fa1104bc0ebfe178c14c53750d60d1886))
- **ota:** drop bytecode version as build key ([#303](https://github.com/mikrojs/mikro/pull/303))
- **cli:** stop the git fixture from hitting the real repo under hooks ([#302](https://github.com/mikrojs/mikro/pull/302))
- **quickjs:** apply the patch series at CMake configure time ([#299](https://github.com/mikrojs/mikro/pull/299))

## 0.17.0 (2026-07-28)

### Breaking changes

- **kv:** reserve the mik.sys NVS namespace for runtime state ([#274](https://github.com/mikrojs/mikro/pull/274))

### Features

- **cli:** refuse mikro flash overwriting custom firmware ([#292](https://github.com/mikrojs/mikro/pull/292))
- **ota:** install deploys at fresh boot ([#290](https://github.com/mikrojs/mikro/pull/290))
- **ota:** provide ota client from 'mikro/ota/client' ([#289](https://github.com/mikrojs/mikro/pull/289))
- **releaser:** keep 0.x features a patch bump ([#288](https://github.com/mikrojs/mikro/pull/288))
- **ota:** unify command output and stop stray build tarballs ([#286](https://github.com/mikrojs/mikro/pull/286))
- **ota:** show the setup login code before opening the browser ([#285](https://github.com/mikrojs/mikro/pull/285))
- **ota:** record source repository and commit on registry builds ([#282](https://github.com/mikrojs/mikro/pull/282))
- **ota:** release channels ([#278](https://github.com/mikrojs/mikro/pull/278))
- **ota:** generate a dev version with --snapshot ([#277](https://github.com/mikrojs/mikro/pull/277))
- **ota:** over-the-air app build updates ([#276](https://github.com/mikrojs/mikro/pull/276))
- **cli:** offer to re-run the original command after reflashing ([#266](https://github.com/mikrojs/mikro/pull/266))

### Bug fixes

- **firmware:** don't freeze the resolved partitions.csv path into sdkconfig ([#291](https://github.com/mikrojs/mikro/pull/291))
- **ota:** start polling for setup complete immediately ([#287](https://github.com/mikrojs/mikro/pull/287))
- **cli:** clarify firmware-mismatch prompt and allow aborting a flash ([#280](https://github.com/mikrojs/mikro/pull/280))
- **docs:** keep the ota release command inline so the site builds ([#279](https://github.com/mikrojs/mikro/pull/279))
- **firmware:** revive the on-device test suite after the mikro rename ([#275](https://github.com/mikrojs/mikro/pull/275))
- **releaser:** derive changelog range from version bump commit ([#263](https://github.com/mikrojs/mikro/pull/263))

### Other

- **ota:** order snapshot versions by build time ([#284](https://github.com/mikrojs/mikro/pull/284))
- **ota:** order snapshot versions by build time ([#283](https://github.com/mikrojs/mikro/pull/283))
- **ota:** rename device credential to update key ([#281](https://github.com/mikrojs/mikro/pull/281))
- **deps:** update dependency acorn to v8.17.0 ([#272](https://github.com/mikrojs/mikro/pull/272))
- **deps:** update dependency @types/node to v24.13.3 ([#271](https://github.com/mikrojs/mikro/pull/271))
- **deps:** update dependency @shikijs/vitepress-twoslash to v4.3.1 ([#270](https://github.com/mikrojs/mikro/pull/270))
- **firmware:** recommend esp-idf 6.0.2 installs to match CI ([#269](https://github.com/mikrojs/mikro/pull/269))
- **deps:** update espressif/idf docker tag to v6.0.2 ([#265](https://github.com/mikrojs/mikro/pull/265))
- **deps:** update dependency vue to v3.5.40 ([#264](https://github.com/mikrojs/mikro/pull/264))
- **deps:** update dependency vitepress-plugin-llms to v1.13.3 ([#260](https://github.com/mikrojs/mikro/pull/260))
- **deps:** update dependency npm-run-all2 to v9.0.2 ([#259](https://github.com/mikrojs/mikro/pull/259))

## 0.16.0 (2026-07-18)

### Breaking changes

- **wifi:** configure txPower on connect, power radio down on disconnect ([#250](https://github.com/mikrojs/mikro/pull/250))

### Bug fixes

- **cli:** root the deploy tree at the entry's top-level dir ([#258](https://github.com/mikrojs/mikro/pull/258))

### Other

- **deps:** update actions/cache digest to caa2961 ([#252](https://github.com/mikrojs/mikro/pull/252))
- **deps:** update actions/setup-node digest to 2499707 ([#253](https://github.com/mikrojs/mikro/pull/253))
- **deps:** update pnpm/action-setup digest to 0ebf471 ([#254](https://github.com/mikrojs/mikro/pull/254))
- **deps:** update dependency lefthook to v2.1.10 ([#257](https://github.com/mikrojs/mikro/pull/257))
- **deps:** update dependency vite to v8.0.16 [security] ([#249](https://github.com/mikrojs/mikro/pull/249))
- **deps:** update dependency @swc/core to v1.15.43 ([#256](https://github.com/mikrojs/mikro/pull/256))
- **deps:** update dependency esbuild to v0.28.1 [security] ([#231](https://github.com/mikrojs/mikro/pull/231))
- **deps:** update dependency semver to v7.8.5 ([#226](https://github.com/mikrojs/mikro/pull/226))
- **deps:** update react monorepo ([#204](https://github.com/mikrojs/mikro/pull/204))
- **deps:** update dependency eslint-plugin-package-json to v1.6.0 ([#206](https://github.com/mikrojs/mikro/pull/206))
- **deps:** update vitest monorepo to v4.1.10 ([#200](https://github.com/mikrojs/mikro/pull/200))
- **deps:** update dependency @sveltejs/acorn-typescript to v1.0.11 ([#255](https://github.com/mikrojs/mikro/pull/255))
- **deps:** update dependency tsx to v4.23.1 ([#195](https://github.com/mikrojs/mikro/pull/195))
- **deps:** update actions/checkout digest to df4cb1c ([#208](https://github.com/mikrojs/mikro/pull/208))
- **deps:** update dependency @clack/prompts to v1.7.0 ([#213](https://github.com/mikrojs/mikro/pull/213))
- **deps:** update dependency type-fest to v5.8.0 ([#198](https://github.com/mikrojs/mikro/pull/198))
- **deps:** update typescript-eslint monorepo to v8.64.0 ([#205](https://github.com/mikrojs/mikro/pull/205))

## 0.15.0 (2026-06-14)

### Breaking changes

- **native:** scope `native:` module names to their owning package ([#243](https://github.com/mikrojs/mikro/pull/243))

### Features

- **i2s:** add I2S audio module for mic capture and playback ([#246](https://github.com/mikrojs/mikro/pull/246))

### Bug fixes

- **native:** export runtime/i2s/types from the published package ([#248](https://github.com/mikrojs/mikro/pull/248))
- **firmware:** stop spi/uart/i2c write over-reading buffer views ([#247](https://github.com/mikrojs/mikro/pull/247))
- **cli:** treat native: specifiers as firmware builtins in the tracer ([#245](https://github.com/mikrojs/mikro/pull/245))
- **release:** apply quickjs patches before packing tarball ([#242](https://github.com/mikrojs/mikro/pull/242))
- **repl:** echo input immediately instead of waiting on device timing ([#240](https://github.com/mikrojs/mikro/pull/240))

### Other

- **drivers:** use a publishable `@my-scope` single-package example ([#244](https://github.com/mikrojs/mikro/pull/244))

## 0.14.0 (2026-06-13)

### Features

- **docs:** publish llms.txt for AI coding assistants ([#230](https://github.com/mikrojs/mikro/pull/230))
- **cli:** offer to reflash incompatible device firmware ([#227](https://github.com/mikrojs/mikro/pull/227))

### Bug fixes

- **firmware:** fix external build issues ([#229](https://github.com/mikrojs/mikro/pull/229))
- **native:** keep '/*' out of a block comment ([#239](https://github.com/mikrojs/mikro/pull/239))
- **releaser:** order prerelease versions by publish time ([#236](https://github.com/mikrojs/mikro/pull/236))
- **firmware:** make third-party driver packages work in custom firmware builds ([#235](https://github.com/mikrojs/mikro/pull/235))
- **cli:** keep reflash prompt hints from reading as a menu ([#234](https://github.com/mikrojs/mikro/pull/234))
- **firmware:** pick up version changes without a clean build ([#233](https://github.com/mikrojs/mikro/pull/233))
- **cli:** don't flag identical prerelease firmware as incompatible ([#232](https://github.com/mikrojs/mikro/pull/232))
- **wifi:** refuse start under low heap instead of PHY abort ([#225](https://github.com/mikrojs/mikro/pull/225))
- **firmware:** guard MIK_InitPromise OOM to avoid gc_decref double-free ([#224](https://github.com/mikrojs/mikro/pull/224))

### Other

- **release:** mint the App token before release-PR guard ([#238](https://github.com/mikrojs/mikro/pull/238))
- **release:** guard the release PR from being merged mid-regeneration ([#237](https://github.com/mikrojs/mikro/pull/237))
- **repo:** drop leftover CommonJS require() ([#228](https://github.com/mikrojs/mikro/pull/228))
- **deps:** update dependency prettier to v3.8.4 ([#222](https://github.com/mikrojs/mikro/pull/222))

## 0.13.0 (2026-06-11)

### Features

- **examples:** enable log file in every example ([#219](https://github.com/mikrojs/mikro/pull/219))
- **logs:** add `mikro logs reset` to clear on-device log files ([#209](https://github.com/mikrojs/mikro/pull/209))
- **native:** expose last reset reason via sys.resetReason ([5df308f](https://github.com/mikrojs/mikro/commit/5df308f209d6361084b14de1ccf1dbd925d0ee14))

### Bug fixes

- **http:** name out-of-memory when an alloc-class TLS error surfaces ([#220](https://github.com/mikrojs/mikro/pull/220))
- **native:** keep cycle GC and the idle task alive through long job storms ([#216](https://github.com/mikrojs/mikro/pull/216))
- **native:** ship abort polyfill as precompiled bytecode ([#217](https://github.com/mikrojs/mikro/pull/217))
- **cli:** make `mikro test` survive slow devices and missing results ([#215](https://github.com/mikrojs/mikro/pull/215))
- **native:** unbreak the simulator after the quickjs-ng v0.15 upgrade ([#214](https://github.com/mikrojs/mikro/pull/214))
- **repl:** follow device by USB serial across port changes on reconnect ([#212](https://github.com/mikrojs/mikro/pull/212))
- **native:** defer unhandled-rejection reporting to end of turn ([05e10f8](https://github.com/mikrojs/mikro/commit/05e10f8c5fe3df793db89e317e2c0677e1437ef4))

### Other

- **deps:** update dependency oxfmt to ^0.53.0 ([#201](https://github.com/mikrojs/mikro/pull/201))
- refresh esp32-optimize and correct binaryObjectSize ([#221](https://github.com/mikrojs/mikro/pull/221))
- **e2e:** fit the suite on esp32c3's ~172KB JS heap ([#218](https://github.com/mikrojs/mikro/pull/218))
- **deps:** update pnpm to v11.5.3 ([#207](https://github.com/mikrojs/mikro/pull/207))
- **deps:** update dependency wrangler to v4.96.0 ([#202](https://github.com/mikrojs/mikro/pull/202))

## 0.12.0 (2026-06-01)

### Breaking changes

- **firmware:** read the device id from the base MAC ([#191](https://github.com/mikrojs/mikro/pull/191))

### Features

- **test:** report per-suite memory used and OOM margin ([#197](https://github.com/mikrojs/mikro/pull/197))
- **cli:** read devices on mismatched firmware for post-mortem ([#194](https://github.com/mikrojs/mikro/pull/194))
- **firmware:** show the device id in the boot banner ([#190](https://github.com/mikrojs/mikro/pull/190))
- add support for http/server ([#187](https://github.com/mikrojs/mikro/pull/187))
- **cli:** support device aliases ([#188](https://github.com/mikrojs/mikro/pull/188))

### Bug fixes

- **repl:** don't reboot the device on a REPL eval error ([#189](https://github.com/mikrojs/mikro/pull/189))

### Other

- **deps:** update dependency knip to v6.15.0 ([#196](https://github.com/mikrojs/mikro/pull/196))
- **docs:** deploy docs from CI, gate production on stable releases ([#193](https://github.com/mikrojs/mikro/pull/193))
- **cli:** document device aliases and naming ([#192](https://github.com/mikrojs/mikro/pull/192))
- **paths:** resolve per-user cache/config dirs via env-paths ([5276449](https://github.com/mikrojs/mikro/commit/5276449e76ea4330044a22e686d8f0ccd81ddb00))

## 0.11.0 (2026-05-31)

### Features

- **create:** accept human-friendly project names by slugifying ([#183](https://github.com/mikrojs/mikro/pull/183))

### Other

- **deps:** replace `@vercel/detect-agent` with std-env ([#184](https://github.com/mikrojs/mikro/pull/184))
- **deps:** update dependency eslint-plugin-package-json to v1.2.0 ([#159](https://github.com/mikrojs/mikro/pull/159))
- **deps:** update dependency node-addon-api to v8.8.0 ([#146](https://github.com/mikrojs/mikro/pull/146))
- **deps:** update dependency eslint to v10.4.1 ([#176](https://github.com/mikrojs/mikro/pull/176))
- **deps:** update dependency ink to v7.0.5 ([#177](https://github.com/mikrojs/mikro/pull/177))
- **deps:** update dependency @clack/prompts to v1.5.0 ([#170](https://github.com/mikrojs/mikro/pull/170))
- **deps:** update dependency npm-run-all2 to v9 ([0845c05](https://github.com/mikrojs/mikro/commit/0845c051dc324227dea4e7af5dde364fb7ce3ba2))
- **deps:** catalog vitest pair and pin react-hooks to stable ([ed86aae](https://github.com/mikrojs/mikro/commit/ed86aae79e0b4a9b7a61d78fb5548bc81fdbfe23))
- **create:** drop broken `cd my-*` follow-ups after create ([9e69d7f](https://github.com/mikrojs/mikro/commit/9e69d7f9a1bf78b92b2c7ccf653b5fe091e0e805))
- rename mikrojs to mikro throughout documentation and examples ([846a139](https://github.com/mikrojs/mikro/commit/846a139fb44230b121427041e2902bc9896f6b54))
- add npmx package link to social nav ([d46eda0](https://github.com/mikrojs/mikro/commit/d46eda035195e83b394c8edf6e28659987c2cc0c))

## 0.10.0 (2026-05-30)

### Breaking changes

- **mikro:** rename mikrojs package to mikro ([c6b643a](https://github.com/mikrojs/mikro/commit/c6b643af03cd0fcc528c48a4cc2ae21493fa4f1f))

### Bug fixes

- **cli:** silence node strip-types ExperimentalWarning in output ([#174](https://github.com/mikrojs/mikro/pull/174))

### Other

- **ci:** publish release PR previews to the `next` dist-tag ([#175](https://github.com/mikrojs/mikro/pull/175))
- **scripts:** make trust-setup declarative ([#173](https://github.com/mikrojs/mikro/pull/173))
- **create-mikrojs:** re-add as thin alias for create-mikro ([91f151f](https://github.com/mikrojs/mikro/commit/91f151f001036604d5074456fc4d434fdabc3f71))
- **mikrojs:** re-add as deprecation stub pointing to mikro ([a2dcc08](https://github.com/mikrojs/mikro/commit/a2dcc084014d2601026d420c2d59a2dd8ac16e79))
