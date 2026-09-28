# Plume Cardputer ADV baseline

Baseline captured: 2026-09-27T21:52:43-04:00

## Source state

- Repository: `https://github.com/theProject/plume.git`
- Upstream: `https://github.com/zmattmanz/plume.git`
- Branch: `sentinel/audit-and-baseline`
- Source commit: `85736d204229834d55bfec813b529f95d64757a9`
- `origin/main`, `upstream/main`, and the working branch all pointed to that commit after fetching both remotes with `--prune`.
- Fork divergence at audit start: 0 commits ahead, 0 commits behind upstream.
- No upstream merge or rebase was performed.

The firmware source was not edited before this baseline was compiled. Arduino CLI requires the primary sketch filename to match its directory, so the build script copies the unchanged source into the ignored staging directory `.build/sketch/FlockDetection_Cardputer_ADV/`.

## Host and toolchain

| Component | Version / setting |
| --- | --- |
| Host | macOS on Apple Silicon |
| Arduino CLI | 1.5.1, Homebrew build dated 2026-06-05 |
| ESP32 Arduino core | `esp32:esp32@3.3.10` |
| ESP-IDF packaged by core | 5.5.4 |
| esptool | 5.3.0, bundled by the ESP32 core |
| Arduino ctags | `5.8-arduino11`, compiled natively for arm64 by the bootstrap script |
| Git | 2.54.0 (Apple Git-157) |

Core 3.3.10 and esptool 5.3.0 are independently present in the checked-in `upload.log`. The repository does not lock Arduino library versions. The library pins below are the closest reproducible reconstruction: versions available at the upstream commit date and compatible with the current source.

| Library | Pinned version |
| --- | --- |
| M5Cardputer | 1.1.1 |
| M5Unified | 0.2.19 |
| M5GFX | 0.2.26 |
| NimBLE-Arduino | 2.5.0 |
| TinyGPSPlus | 1.0.3 |

On Apple Silicon, the ESP32 package's bundled Arduino ctags executable is x86_64-only and failed with `bad CPU type in executable`. Homebrew Universal Ctags is not compatible with Arduino's prototype-generation behavior. `scripts/bootstrap-dev.sh` therefore builds the exact Arduino `5.8-arduino11` tag natively. A mechanical internal macro rename is required because modern macOS headers also define `__unused__`.

## Cardputer build configuration

Exact FQBN:

```text
esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app
```

Resolved board options from ESP32 Arduino core 3.3.10:

| Option | Resolved value |
| --- | --- |
| CPU frequency | 240 MHz compile-time default; firmware selects 160 MHz in normal runtime mode, 80 MHz low-power, 240 MHz turbo |
| USB mode | Hardware CDC and JTAG |
| USB CDC on boot | Enabled |
| Upload mode | UART0 / Hardware CDC |
| Flash image size | 4 MB |
| Flash frequency / mode in emitted flash args | 80 MHz / DIO |
| Partition scheme | Huge APP: 3 MB application, 896 KB SPIFFS-subtype data partition used by LittleFS, 64 KB coredump |
| PSRAM | Disabled |
| Arduino loop core | Core 1 |
| Arduino events core | Core 1 |
| Erase flash before upload | Disabled |

The ADV hardware identified in the checked-in upload log reports an embedded 8 MB flash chip, but the upstream 3.3.10 build metadata and the reproduced short FQBN both emit a 4 MB image. This is valid but leaves half of the physical flash unused. The older checked-in `build/` artifact instead records an explicit 8 MB/QIO configuration under core 3.3.5. This conflict is not resolved by repository configuration and must be treated as build-artifact drift, not silently guessed.

Partition table:

```text
nvs       data/nvs       0x009000  0x005000
otadata   data/ota       0x00e000  0x002000
app0      app/ota_0      0x010000  0x300000
spiffs    data/spiffs    0x310000  0x0e0000
coredump  data/coredump  0x3f0000  0x010000
```

Build command:

```sh
./scripts/build-cardputer.sh
```

The script uses repository-local, ignored Arduino data and build directories. The underlying compile is equivalent to:

```sh
arduino-cli compile \
  --fqbn 'esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app' \
  --build-path '.build/cardputer/work' \
  --output-dir '.build/cardputer/output' \
  --warnings all \
  '.build/sketch/FlockDetection_Cardputer_ADV'
```

## Result

Build status: **PASS**

```text
Sketch uses 1586815 bytes (50%) of program storage space. Maximum is 3145728 bytes.
Global variables use 81680 bytes (24%) of dynamic memory, leaving 246000 bytes for local variables. Maximum is 327680 bytes.
```

The static-RAM result does not establish runtime stability. The source itself reports runtime free heap near 6 KB after radio initialization on the no-PSRAM target, allocates a roughly 54 KB sprite early, and contains health/restart thresholds specifically for low-heap operation. Runtime free heap, minimum free heap, largest block, and task high-water marks still require a serial-monitored hardware run.

Primary output:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `FlockDetection_Cardputer_ADV.ino.bin` | 1,587,072 | `de43d5f392861a4f003350dbad287ade08e034312b57d4510a5dd190b8cc21fe` |
| `FlockDetection_Cardputer_ADV.ino.partitions.bin` | 3,072 | `aaae2888c5a6a348004b5b436f47abb25ae32e72d9003902955a998eda723edd` |
| `FlockDetection_Cardputer_ADV.ino.merged.bin` | 4,194,304 | `7c6425fa111d559cccd3496cb645a5bc4eb036326f122defdd4bfe4ea578135a` |

Outputs are under `.build/cardputer/output/` and are intentionally ignored by Git.

## Warnings

The build passes with warnings visible. Project-source warnings include:

- `uint32_t` format mismatches in the export password, CSV epoch, CPU log, and load telemetry.
- Deprecated `neopixelWrite`; core 3.3.10 recommends `rgbLedWrite`.
- `strncpy(..., "??:??:??", 8)` does not append a NUL byte. The destination may therefore retain stale trailing data.
- Several conservative `snprintf` truncation diagnostics in UI-only buffers.
- Three unused UI locals and one unused drawing helper.

Dependency/core warnings include TinyGPSPlus's intentional-looking switch fallthrough and missing-field initializer diagnostics in ESP-IDF/TinyUSB headers. None were hidden or converted to errors for the baseline.

## C5 compile check

The companion sketch was independently compiled, without source changes, using:

```text
esp32:esp32:esp32c5:CDCOnBoot=cdc
```

Result: **PASS**

```text
Sketch uses 1041115 bytes (79%) of program storage space. Maximum is 1310720 bytes.
Global variables use 62708 bytes (19%) of dynamic memory, leaving 264972 bytes for local variables. Maximum is 327680 bytes.
```

Project-source warnings are limited to deprecated C++ operations on `volatile` queue counters/indexes; the remaining warnings originate in ESP32-C5 core headers.

## Hardware and flashing status

`arduino-cli board list` found one generic `ESP32 Family Device` at `/dev/cu.usbmodem1101`. The USB identity did not uniquely establish that it was the Cardputer ADV rather than another owned ESP32 device. Per the acquisition rules, **nothing was flashed**.

Once the port is positively identified, `scripts/build-cardputer.sh` prints the exact upload command. It uses `EraseFlash=none`; it does not separately erase flash. Uploading the application still replaces the application partition, while the LittleFS data partition should remain intact.

## Checked-in artifact drift

- `compile.log` and `upload.log` last changed at commit `713b061195a42cc9785b0d99d9e3b046988dcc9c` on 2026-07-16, before current v1.3 source changes.
- `compile.log` reports 1,560,599 flash bytes and 88,248 static RAM bytes, which do not match the current source build.
- `build/esp32.esp32.m5stack_cardputer/build.options.json` records core 3.3.5 and an explicit 8 MB/QIO configuration.
- `tmpplumebuild/build.options.json` records core 3.3.10 and the short `huge_app` FQBN, but its generated sketch identifies itself as v1.0-beta and differs substantially from the current source.

These files were preserved unchanged as requested. They are historical evidence, not a reproducible current baseline.
