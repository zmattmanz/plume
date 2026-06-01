# Plume ⇄ C5 wire protocol

The Cardputer (ESP32-S3, runs Plume) and the ESP32-C5 (runs `c5-sniffer/`) are
two independent processors connected by a single UART over the Grove port. This
file is the **single source of truth** for the line format they exchange. Both
sides must agree on it.

**Current `PROTOCOL_VERSION`: 1**

## Link

| | |
|---|---|
| Transport | UART, 115200 8N1, newline (`\n`) terminated ASCII |
| Cardputer side | UART1 on Grove `G1`/`G2` (GPIO1 = S3 TX, GPIO2 = S3 RX) — see `c5_link.h` |
| C5 side | UART0 on the `TXD`/`RXD` pads — see `c5-sniffer.ino` |
| Direction | C5 → Cardputer (the reverse channel is wired but unused in v1) |
| Levels | 3.3 V both ends, no level shifter |

## Messages (C5 → Cardputer)

### Detection
```
D|mac|name|rssi|ch|conf|methods
```
| Field | Meaning |
|---|---|
| `mac` | lower-case `aa:bb:cc:dd:ee:ff` of the reported device |
| `name` | SSID, or `Hidden`; any `\|` / newline is replaced with `_` |
| `rssi` | signed dBm, e.g. `-67` |
| `ch` | 5 GHz channel (36–165) — the band marker on the Plume side |
| `conf` | confidence 0–100 |
| `methods` | space-separated match tags (last field; may contain spaces) |

Example: `D|aa:bb:cc:dd:ee:ff|flock-1a2b|-67|149|85|ssid_fmt`

### Hello / heartbeat
```
H|fw|ver
```
Sent every ~3 s. `fw` is a firmware id (`plume-c5`), `ver` is `PROTOCOL_VERSION`.
Plume uses any well-formed line as proof-of-life and lights the **5G** badge for
8 s after the last one. `ver` lets Plume warn on a protocol mismatch.

## Rules for changing this protocol

1. **Append-only.** New fields go on the **end** of a `D|` line. Plume reads the
   first 7 fields and ignores extras, so a newer C5 stays compatible with an
   older Plume.
2. **Bump `PROTOCOL_VERSION`** in *both* `c5-sniffer.ino` and `c5_link.h` only
   when the format changes in a non-append-only way.
3. **Keep the detection logic in sync.** These must match between
   `c5-sniffer.ino` and `FlockDetection_Cardputer_ADV.ino`:
   - `kSsidPatterns` / `wifi_ssid_patterns`
   - `kMacTier1` / `mac_prefixes_tier1`, `kMacTier2` / `mac_prefixes_tier2`
   - scoring weights (`SCORE_*`), `ALARM_THRESHOLD`, `IGNORE_WEAK_RSSI`
   - the `Flock-XXXX` format and `test_flck` / wildcard-probe / addr1 rules

## Repo layout

```
Plume/
  FlockDetection_Cardputer_ADV.ino   ← Cardputer/S3 firmware (the brain + UI)
  ui_beep.h
  PROTOCOL.md                         ← this file
  c5-sniffer/
    c5-sniffer.ino                    ← C5 firmware (5 GHz radio ear)
```
