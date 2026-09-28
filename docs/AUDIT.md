# Plume acquisition audit

Audit date: 2026-09-27  
Audited upstream commit: `85736d204229834d55bfec813b529f95d64757a9`

## Scope and authority

This audit treats the Cardputer firmware as native authoritative firmware. M5Launcher, Bruce, and other launcher packaging were not used as architectural constraints. The observed-device path remains passive: Wi-Fi promiscuous reception, BLE advertisement scanning, GPS, logging, and a receive-oriented C5 co-processor link. The only network association found is the explicitly invoked operator export mode; scanning is stopped before that connection begins.

Evidence priority used here is current executable source, current changelog, `PROTOCOL.md`, then README/history.

## Architecture summary

- One large Cardputer sketch owns UI, Wi-Fi management-frame capture, BLE advertisement capture, GPS, SD/LittleFS persistence, export HTTP service, alerts, and the Cardputer side of the C5 link.
- Wi-Fi capture runs in a driver callback and copies bounded frame metadata plus a 128-byte frame snapshot into a 16-slot atomic ring. Parsing and scoring run later in the Arduino loop.
- BLE callbacks copy bounded advertisement data into a four-slot static pool and queue slot indexes to a Core 1 worker task. This avoids persistent heap churn in the hot callback.
- The scanner/channel-hop task and GPS task run on Core 0; Arduino loop, BLE worker, UI, and transient persistence work primarily use Core 1.
- Shared application state uses a recursive mutex. SD/FAT access uses a separate non-recursive mutex.
- LittleFS stores settings, whitelist, Wi-Fi configuration, and a compact history mirror. SD stores CSV, Wi-Fi PCAP, BLE PCAP, lifetime stats, and optional signature overrides.
- C5 protocol version 1 is line-oriented and bidirectional. The S3 pushes time, power state, LED state, and signature transactions; the C5 sends heartbeats, ambient sightings, and detections.

## Findings

### CRITICAL

No confirmed critical defect was found by source inspection and compilation. Runtime/hardware validation is incomplete, so this is not a field-readiness declaration.

### HIGH

1. **REPAIRED — buffered SD data was cleared before write success and lost on transient failures.** `flush_sd_buffer()` now snapshots without clearing and removes only each successfully written prefix. Ordinary open/write failures retain the remaining records for retry. A short partial PCAP record write still cannot be rolled back through the Arduino FS API and remains a physical-validation risk.

2. **REPAIRED — the C5 could suppress valid Cardputer-threshold detections.** Its gate is now 70, matching the Cardputer. Message fields did not change, so protocol version 1 remains correct.

3. **REPAIRED WITH REGRESSION CHECK — C5 built-in signatures were stale relative to the Cardputer table.** Its 54-entry OUI fallback and SSID patterns now match. `tests/check-signature-sync.sh` compares both ordered tables, thresholds, capacities, and Axon/Flock separation. Constants remain duplicated because the two Arduino sketches compile independently, so the check must remain a quality gate.

4. **REPAIRED — the SD signature replacement transaction was not fail-closed.** Recognized rows now load into bounded staging tables and commit only after complete validation. Malformed/overlong input, bad tiers/OUI syntax, or capacity overflow leaves the active tables and `BUILT-IN` provenance unchanged. Version, hash, and per-signature provenance are still absent.

5. **Runtime memory stability is not yet proven.** Static usage is acceptable, but the code documents free heap near 6 KB after radio initialization, performs periodic NimBLE restarts, skips filesystem work below 6 KB, and makes several runtime allocations. No live minimum-heap, largest-block, or task-stack measurements were possible without positively identified hardware.

### MEDIUM

1. **Wi-Fi credential storage provides confidentiality without authenticity, and the key is derivable.** AES-CBC uses a random IV, but no MAC/tag. Its 128-bit key is deterministically derived from the public eFuse MAC with a fixed salt. This prevents casual plaintext inspection, not an attacker with device/file access from decrypting or modifying credentials. Legacy XOR credentials remain accepted for migration.

2. **Export authentication is intentionally weak.** The HTTP service uses Basic authentication without TLS. Its eight-hex password is a deterministic FNV-1a value derived from the public device MAC. The exposure window is operator-triggered and auto-closes after ten minutes, which reduces risk, but anyone on the joined WLAN who knows the MAC can derive credentials.

3. **Several allocations and task creations are unchecked.** Mutex creation, BLE queue creation, the persistent BLE worker, scanner task, GPS task, and LED task are not consistently checked before use. Allocation failure in a low-heap boot can become a null-handle fault or partially alive instrument instead of a clear fail-closed boot error.

4. **REPAIRED — C5 inbound signature rows were insufficiently validated.** Invalid OUI syntax or tiers now abort staging, and both UART parsers discard an entire overlong line through its terminating newline.

5. **The C5 callback ring relies on `volatile`, not explicit synchronization.** The Cardputer Wi-Fi ring uses acquire/release atomics; the C5 ring publishes `ready` and counters through `volatile` fields. The ESP32-C5 is single-core, which lowers practical risk, but compiler-visible ordering and counter increments should use the same explicit atomic pattern.

6. **LittleFS recovery can erase durable state automatically.** `LittleFS.begin(true)` formats on mount failure, followed by a full partition erase/retry if it still fails. This favors autonomous recovery but can destroy settings, whitelist, credential material, and the flash history mirror after a transient mount problem. SD logs remain separate.

7. **PARTIALLY REPAIRED — PCAP/CSV buffer overflow was silent.** Separate CSV, WiFi-PCAP, and BLE-PCAP drop counters now appear in serial health telemetry. They are not yet included in exported session metadata.

8. **REPAIRED FOR SESSION METRICS — detection bookkeeping assumed Flock.** Axon events now have an independent session counter and Stats card, and no longer increment Flock WiFi/BLE counters. The persisted `lifetime_flock_total` identifier is retained for storage compatibility even though its displayed meaning is total logged detections; a future schema migration should rename it deliberately.

9. **PCAP timestamps without GPS look like real wall-clock values.** The fallback is a fixed synthetic epoch plus uptime. PCAP files are structurally readable, but consumers cannot distinguish the synthetic clock from real UTC without external session metadata.

10. **Build configuration is not stored as source-of-truth configuration.** Historical build folders conflict on core, flash size, flash mode, and source revision. The new scripts establish a reproducible 3.3.10 baseline, but library/version pins currently live only in the bootstrap script rather than a package manifest supported by Arduino CLI.

### LOW

1. **REPAIRED.** Project-source variadic format warnings were corrected with matching casts/specifiers; remaining build warnings are unused UI helpers/locals, conservative UI truncation diagnostics, and dependency code.
2. **REPAIRED.** The persisted history loader now uses the bounded copy helper for `??:??:??`.
3. **REPAIRED.** Both OUI parsers now require exactly eight characters with colons in fixed positions and six hexadecimal digits.
4. `is_flock_ssid_format()` recognizes only `Flock-` and `flock-`, not other case combinations, while substring tables are case-insensitive.
5. C5 signature capacity (72 OUIs) differs from Cardputer capacity (64). Current pushes cannot overflow the C5, but the asymmetric contract is undocumented.
6. Several UI buffer-truncation warnings are impossible under current bounded state but obscure more meaningful diagnostics.
7. Debug labels such as `DELDIAG` remain in production serial output.

### GOOD / PRESERVE

- Receive-only scope is clear. No deauthentication, jamming, spoofing, target authentication, exploitation, active target probing, or target-network association was found.
- The Wi-Fi callback checks minimum frame length, caps suspicious lengths, takes a bounded snapshot, and defers IE parsing/scoring.
- Wi-Fi OUI scoring rejects locally administered transmitter addresses. `addr1` scoring rejects multicast, broadcast, and locally administered addresses.
- Unknown wildcard-probe behavior is surfaced as a candidate but deliberately contributes no confidence without a known OUI.
- Generic component OUIs are separated from the one registry-specific Flock OUI in current Cardputer source.
- BLE advertisement work is deferred through a fixed pool and queue, substantially reducing hot-path heap fragmentation.
- Export mode stops both radios' scanning before joining the operator's configured WLAN and disables persistent ESP32 Wi-Fi credential caching.
- CSV formula prefixes are defused for the two externally controlled CSV fields, after comma removal preserves column alignment.
- File downloads are fixed-route, not caller-selected paths, and require authentication.
- LittleFS writes use temp-file/rename replacement and check content writes before committing.
- The C5 signature sync stages a whole batch and checks terminal counts before commit; the wire protocol is append-tolerant and ignores unknown tags.
- C5 orientation detection begins RX-only, preventing deliberate TX contention until valid traffic proves the cable orientation.
- The 54 KB display sprite is allocated before radio/filesystem heap fragmentation, and low-heap guards protect FAT/LittleFS operations.
- Existing CSV and PCAP formats remain intact.

## Signature inventory and status

### Flock

- Compiled Wi-Fi SSID patterns: `FS Ext Battery`, `Penguin`, `Pigvision`, `FlockOS`, `flocksafety`, `OFS_IoT`, `PFS_`, and bare case-insensitive substring `flock`.
- Definitive Wi-Fi OUI tier: only `b4:1e:52`, assigned by IEEE to Flock Safety.
- Generic/supporting table: 53 component, field-observed, ambiguous, or unvalidated prefixes. These cannot classify independently at ordinary RSSI, but Tier-2 wildcard behavior plus the close-range bonus reaches the current 70 threshold.
- BLE company ID: `0x09C8`, with optional `TN` manufacturer-data corroboration.
- Built-in IE fingerprint table is intentionally empty pending repeatable captures.
- `cc:cc:cc`, `4c:6e:44`, and `d8:a0:d8` are explicitly retained as weak/unvalidated evidence and should be measured, not promoted.

### Raven / SoundThinking

- BLE custom short UUID codes: `0x3100` through `0x3500`.
- Standard corroborating UUID codes: `0x180A`, `0x1809`, `0x1819`.
- One custom UUID or multiple recognized UUIDs currently produces a definitive Raven classification. Firmware-family labels are inferred from combinations and should be treated as heuristic, not proof of exact model/firmware.

### Axon

Research performed during this audit verified and implemented the following bounded policy:

- IEEE's current registry (`https://standards-oui.ieee.org/oui/oui.txt`) assigns OUI `00:25:DF` to **Axon Enterprise, Inc.**
- The Bluetooth SIG public assigned-numbers repository (`https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml`) assigns company identifier `0x034D` to **TASER International, Inc.**, Axon's former corporate name.
- OUI-SPY detector research (`https://github.com/colonelpanichacks/ouispy-detector`) reports vendor service UUID `0xFC81`, but it is not an adopted Bluetooth SIG service UUID and no primary Axon/FCC capture was found in this pass that proves its advertisement semantics.

Implemented confidence policy:

- `0x034D` plus public `00:25:DF` or UUID `0xFC81`: `AXON_BLE`, 100.
- `0x034D` alone: `AXON_BLE`, 85 (HIGH vendor classification, no product/model claim).
- public `00:25:DF` alone on WiFi or BLE: `AXON_ASSOC`, 60, logged but explicitly excluded from RSSI/TX bonuses, channel lock, and alarms.
- `0xFC81` alone: no classification; it remains corroboration only.

Axon has a separate session counter and Stats card. Stored evidence strings identify the company ID, OUI, and/or UUID that fired. Controlled hardware captures are still required to measure false-positive/false-negative behavior.

No model-specific body-camera or ALPR label is justified from these identifiers alone.

### SD overrides and provenance

`/flock_signatures.csv` can transactionally replace OUI, SSID, and BLE name tables independently. The source is shown as `BUILT-IN` or `SD`, and OUI/SSID tables are pushed to the C5. Invalid recognized input leaves the active tables unchanged. The file has no schema version, database version, source URL per signature, creation time, signature ID, or hash. BLE company IDs and service UUIDs are not SD-loadable or synchronized to C5.

## Documentation drift repaired

| Topic | README claim | Current executable/changelog reality |
| --- | --- | --- |
| Scanner visualizations | `SCAN`, `LINE`, and `TIME` | `TIME` was removed; `SCANNER_VIZ_COUNT` is 2 |
| Menu | Signal is a menu screen | v1.3 removed Signal from the menu; Target remains reachable from a feed item |
| Alarm threshold | 75% | 70 in Cardputer v1.3; C5 is now synchronized at 70 |
| Tier 1 OUIs | 20 strong OUIs | Exactly one registry-specific OUI, `b4:1e:52` |
| Tier 2 OUIs | 34 supporting OUIs | Current compiled table contains 53 generic/supporting entries |
| SD flush cadence | Every 500 ms or when full | Service check is 500 ms; scheduled flush is 10,000 ms, with immediate full-buffer attempts and explicit drop counters |
| BLE normal duty | Full duty cycle | 60 ms window / 100 ms interval to leave coexistence time for Wi-Fi |
| C5 support | Effectively absent from README | Substantial bidirectional v1 protocol, 5 GHz feed, time/power/LED/signature sync, and cable orientation detection exist |
| Build setup | Install unpinned libraries and choose any LittleFS-capable partition | Working baseline requires core 3.3.10, specific library versions, the M5Cardputer board, and `huge_app` |
| Concurrency | SD timed takes ensure nothing blocks the main loop for more than one frame | Some SD takes permit 2–5 seconds and several data-mutex sections use `portMAX_DELAY` |
| Checked build logs | Appear current by location | Logs predate v1.3 and disagree with current flash/RAM usage |

README statements above were reconciled only after executable behavior and the current changelog were checked. The large duplicate OUI tables were replaced with concise counts/policy and a pointer to the executable source plus static parity test.

## Changes made during the acquisition audit

| File(s) | Reason | Behavior and evidence |
| --- | --- | --- |
| `.gitignore`, `scripts/bootstrap-dev.sh`, `scripts/build-cardputer.sh`, `scripts/build-c5.sh` | Reproducible CLI baseline | Pins core/libraries, handles native Apple-Silicon ctags, stages Arduino-compatible sketch names, and builds both targets into ignored `.build/` output. Both builds pass. |
| `FlockDetection_Cardputer_ADV.ino` | Data integrity and parser hardening | Retains unwritten SD buffer prefixes, exposes three drop counters, makes SD signature replacement transactional, tightens OUI/UART parsing, corrects format/NUL warnings, and uses the current RGB LED API. |
| `c5-sniffer/c5-sniffer.ino` | Restore detector parity | Synchronizes threshold and fallback signatures, rejects malformed signature rows, and resynchronizes after overlong UART input without changing protocol v1. |
| `FlockDetection_Cardputer_ADV.ino` | Evidence-aware Axon support | Adds exact OUI/company/UUID evidence handling, non-alarming associated state, evidence strings, PCAP/CSV logging, and an independent Axon session metric. No model is inferred. |
| `tests/check-signature-sync.sh` | Host-testable regression boundary | Confirms ordered OUI parity, threshold/capacity compatibility, Axon isolation from Flock, and the Axon classification/alarm policy constants. |
| `README.md`, `docs/BASELINE.md`, `docs/AUDIT.md`, `docs/SENTINEL_ROADMAP.md` | Record evidence and repair drift | Documents the untouched upstream build, current architecture, remaining risks, current UI/scoring/build behavior, and staged next work. |

## Verification limits

- Cardputer post-change build passes at 1,588,999 bytes flash and 81,760 bytes static RAM (245,920 bytes reported available for stack/heap): +2,184 flash and +80 static RAM versus the untouched upstream baseline.
- C5 post-change build passes at 1,041,451 bytes flash and 62,708 bytes static RAM (264,972 bytes reported available). The untouched baseline was 1,041,115 bytes / 62,708 bytes: +336 flash and no static-RAM change.
- No device was flashed because `/dev/cu.usbmodem1101` was only identified generically as an ESP32-family device.
- No serial boot, runtime heap, stack high-water, SD hotplug, PCAP Wireshark, GPS, C5 UART, or RF field test was performed.
- Axon values were registry-checked and policy-tested statically, but no controlled Axon hardware capture was available.
