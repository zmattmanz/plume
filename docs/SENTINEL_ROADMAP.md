# Sentinel extension roadmap

This roadmap preserves native Plume firmware as the Cardputer ADV authority. Launcher compatibility is a later packaging question, not a present architecture constraint.

## Stage 1 — make the baseline trustworthy

Status: partially complete in the acquisition branch.

1. Completed: buffered-log failure semantics retain unwritten prefixes and count producer overflow. Remaining: put drop/write-failure totals in durable session metadata and define recovery for a short partial PCAP write.
2. Completed: C5 threshold/fallback signatures, `SO` validation, and overlong-line resynchronization are aligned without changing protocol version 1.
3. Started: the host static check covers OUI table parity, thresholds/capacity, and Axon policy boundaries. Remaining: extract parser/scoring helpers for executable synthetic unit tests covering locally administered addresses, SSID rules, CSV defusing, BLE parsing, and both line parsers.

Exit gate: both sketches build; synthetic tests pass; current Flock and Raven scoring fixtures do not regress; a physical Cardputer boot records stable heap and stack margins.

## Stage 2 — normalize evidence without a rewrite

Introduce a compact internal observation record at the parser/worker boundary while keeping existing UI, CSV, and PCAP outputs. It should retain monotonic time, UTC validity, protocol/channel, RSSI, address and address type, sanitized advertised text, company ID, service UUIDs, detector/signature ID, evidence flags, classification, confidence, source sensor, GPS snapshot, and capture reference.

Fix class-specific counters before routing new classifications through `log_detection()`. Preserve raw observations separately from aggregated device/entity records. Add an append-only `observations.jsonl` stream only after measuring write amplification and heap cost.

Every session should record firmware/version, git build ID, hardware, signature source, signature database version/hash, start time, UTC/GPS quality, and dropped-record counters.

## Stage 3 — field annotation

Add a separate append-only annotation log keyed by stable observation/entity ID:

- `CONFIRMED`
- `FALSE_POSITIVE`
- `UNKNOWN`
- timestamp, optional short sanitized note, and GPS snapshot

Annotations must never alter captured packets, raw observations, detector evidence, or original score. UI work should reuse the existing detection detail flow and remain usable without SD, while clearly showing whether an annotation is durable or only in memory.

## Stage 4 — evidence-aware Axon support

Start with three independent evidence bits, each carrying provenance:

- IEEE OUI `00:25:DF` → exact vendor attribution, supporting device-class evidence.
- Bluetooth company ID `0x034D` → TASER International/Axon lineage, stronger vendor evidence.
- BLE service UUID `0xFC81` → research-reported vendor service, candidate until primary captures confirm context and stability.

Represent at least `AXON`, `AXON-ASSOCIATED`, and unclassified candidate states. Do not infer an exact model. Require corroboration or controlled field validation before a HIGH result, record why every result fired, and keep shared chipset/modem OUIs out of definitive tiers.

Status: first classification slice implemented. `0x034D` is HIGH vendor evidence; company ID plus the public Axon OUI or research UUID is definitive vendor evidence; OUI-only observations are `AXON_ASSOC` and explicitly non-alarming; UUID-only advertisements do not classify. Remaining work is controlled capture validation, repeated-observation aggregation, and provenance-bearing external signature records.

## Stage 5 — physical validation and performance budget

On a positively identified Cardputer ADV:

1. Flash without separate erase; capture the full boot log and initial heap checkpoints.
2. Measure free heap, minimum free heap, largest block, Wi-Fi/BLE queue drops, and every task's stack high-water mark during a two-hour mixed-radio soak.
3. Exercise SD removal/insertion, full/slow card behavior, hard power loss, export start/abort/download, GPS lock/loss, C5 disconnect/reconnect/reversed cable, and low-power transitions.
4. Open both PCAP types in Wireshark and validate timestamps/link types with and without GPS.
5. Run controlled known-device passes and record annotations rather than tuning signatures from anecdotal proximity.

## Deferred

- Launcher packaging or compatibility work.
- Giant-bang modularization of the main sketch.
- A database on the ESP32.
- Active probing, association with observed networks, interference, spoofing, or exploitation.
- Model-specific labels without repeatable evidence.
