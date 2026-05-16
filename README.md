![Plume — Surveillance Awareness](banner.png)

# Plume — M5Cardputer ADV Edition

Passive RF scanner that detects Flock Safety ALPR cameras and Raven surveillance devices using WiFi promiscuous mode and BLE scanning. Runs on the M5Cardputer ADV (ESP32-S3) with a 240×135 color LCD, full QWERTY keyboard, GPS, and SD card logging.

No network connection. No cloud. Everything runs locally on the device. It receives publicly broadcast RF signals but never transmits, connects to, or interacts with any detected device.

---

## Hardware

- **M5Cardputer ADV** (ESP32-S3, no PSRAM variant supported)
- **GPS module** via UART (pins 15/13, 115200 baud) — NEO-6M, BN-220, or similar
- **MicroSD card** (FAT32, any size) — for detection logs, packet captures, and stats

## Flashing

1. Install the Arduino IDE with ESP32-S3 board support
2. Install libraries: `M5Cardputer`, `NimBLE-Arduino`, `TinyGPSPlus`
3. Set board to `M5Cardputer` (or `ESP32-S3 Dev Module` with USB CDC enabled)
4. Set partition scheme to include SPIFFS/LittleFS
5. Flash via USB-C

WiFi credentials for export mode are configured on-device via Menu → WiFi Config. No need to edit source code.

## Quick Start

Power on. You'll see a boot screen, then an animated title card, then it drops into the scanner. It's already scanning WiFi and BLE channels for known Flock/Raven signatures.

When it finds something:
- An audible alarm plays (ascending tones for WiFi, descending for BLE — pitch indicates confidence)
- The LED flashes the protocol color for 15 seconds
- A toast notification shows the device name and confidence percentage
- The detection is logged to SD with GPS coordinates, signal strength, and detection method

Navigate screens with `,` and `/`. Press `m` to open the menu. `Tab` shows a help overlay on any screen.

---

## Screens

### Scanner

The main screen. Split into two panels.

**Left panel** cycles through three visualizations (press `v`):
- **SCAN** — Proximity radar with phosphor sweep. Devices show up as triangles (WiFi) or diamonds (BLE), positioned by signal strength. Flock detections pulse amber. Icons fade as devices age.
- **LINE** — 13-channel WiFi spectrum with Catmull-Rom smoothing. A scan line tracks the current hopping channel. Shifts purple when BLE is scanning. Channel labels at 1, 6, 11, 13.
- **TIME** — 5-minute layered timeline showing WiFi (teal) and BLE (purple) activity as ribbons. Flock detections show as amber dots. Time marks from "now" to "-5m".

**Right panel** is a live feed of all observed devices, sorted by recency. Each row shows a protocol symbol, device name, and signal strength. Flock devices show in amber. Rows fade at 30 seconds and disappear at 90. Press `f` to expand the feed fullscreen with RSSI columns, then use arrows to select a device and `t` to target it for signal tracking.

### Signal

RSSI proximity tracker for a targeted device. Shows:
- Target name with a status badge ("Hunting" for Flock, "Tracking" for everything else)
- Live dBm reading with CLOSER/FARTHER trend
- Signal strength bar with smoothing and micro-jitter
- Rolling signal trace (2-minute history, 2-second samples) with peak bookmark and current-position dot

To target a device: expand the feed (`f`), pick one with arrows, press `t`. Or from the Detections detail view, press `t`. Press `l` to clear the target.

When signal drops out, the trace drops to the floor rather than freezing at the last reading.

### Detections

Scrollable list of confirmed Flock/Raven detections from the SD card log. Most recent first. Each row shows device name, type, RSSI, and a confidence bar. Up to 8 entries in the recent view.

Press Enter to open the detail view: protocol symbol, device name, confidence bar with label (CERTAIN/HIGH/MEDIUM), MAC address, detection ID, signal strength with tier label, detection method, timestamp, date, and GPS coordinates.

From detail view:
- `t` — target this device on the Signal screen
- `d` — delete this detection (confirm with `d` or Enter again)
- `w` — whitelist this MAC (suppresses future alarms)
- `DEL` — close detail view

Deleted detections are batched and flushed to the CSV on screen exit or after 5 seconds idle. A backup file is created during the rewrite to protect against power loss.

### GPS

Animated wireframe globe with latitude circles, longitude meridians, and depth-based shading. When satellites are acquired, wireframe tetrahedrons orbit and tumble. Background starfield with per-star twinkle.

Right panel shows status (GPS LOCKED / Searching... / Reattempting...), coordinates, satellite count, HDOP (color-coded by quality), and local time auto-computed from GPS longitude using US timezone bands and DST rules.

### Stats

Scrollable grid of stat cards with eased scrolling. Digits that change do a roll-up animation — only the ones that actually moved.

Cards: Session Detections / Lifetime Detections, WiFi / BLE / Raven, Session Uptime / Lifetime Uptime, Battery / Heap, Packets / SD Card, Boots / Flash Writes, Version / Voltage.

---

## Keyboard Controls

### Global
| Key | Action |
|-----|--------|
| `,` `/` | Previous / next screen |
| `;` `.` | Up / down (screen-specific — never changes screens) |
| `m` | Open / close menu |
| `Tab` | Toggle help overlay |
| `Esc` | Close overlay or go home to scanner |
| `Del` | Back (close overlay / detail / go home) |
| `-` `+` | Volume down / up (with visual overlay) |
| `` ` `` | Toggle mute |
| `n` | Toggle night mode |
| `b` | Cycle brightness (dim / mid / full) |
| `s` | Toggle stealth mode (screen nearly off) |
| `f` | Toggle expanded feed overlay |
| `t` | Target device for signal tracking |
| `l` | Clear signal target |
| `\` | Single press: toggle LED breathing. Double press: random LED color |
| `v` | Cycle scanner visualization (scanner screen only) |

### Detections Screen
| Key | Action |
|-----|--------|
| `;` `.` | Navigate list (with key-hold repeat) |
| `Enter` | Open / close detail view |
| `d` | Delete detection (from detail view, requires confirmation) |
| `t` | Target for signal tracking (from detail view) |
| `w` | Whitelist MAC (from detail view) |

---

## Menu

Press `m`. Navigate with up/down, select with Enter.

**Screens:** Scanner, Signal, Detections, GPS Status, Device Stats — current screen is marked.

**Settings:**
- Night Mode — red-shifted palette for dark environments
- Low Power Mode — 80 MHz CPU, reduced BLE scan duty (50%), longer battery life. Mutually exclusive with turbo.
- Mute Beeps — silences all tones
- Turbo Mode — 240 MHz CPU, faster channel hopping (150ms vs 250ms), 30-second dedup window (vs 5 minutes). Mutually exclusive with low power.

**Tools:**
- WiFi Config — set SSID and password for export mode (AES-encrypted on flash)
- Export Mode — starts an HTTP server to download logs from a browser (toggles to "Stop Export" when active)
- Clear All Stats — resets all session and lifetime counters

---

## Export Mode

Export mode pauses all scanning — WiFi promiscuous mode and BLE shut down completely — and starts a local HTTP server. You can download detection logs and packet captures from any device on the same network.

### Setup
1. Menu → WiFi Config
2. Enter your WiFi SSID and password using the on-screen keyboard
3. Save (credentials are AES-128-CBC encrypted on flash with a device-unique key derived from the ESP32's eFuse MAC via HMAC-SHA256)
4. Press `s` in the password field to toggle plaintext reveal

### Using It
1. Menu → Export Mode
2. The device connects to WiFi (UI stays responsive during connect) and shows the server URL and password
3. Open the URL in any browser on the same network
4. Username is `plume`, password is the displayed 4-character hex code (deterministic per device, derived from eFuse MAC)
5. Download files from the web interface

Export auto-exits after 10 minutes (extended while someone is downloading). Press `m` → Stop Export to exit manually. NimBLE is fully deinitialized during export to free ~20-30KB of heap for the WiFi TCP stack, then reinitialized on exit.

### Files
| File | What It Is |
|------|------------|
| `PlumeLog.csv` | All detections — timestamps, GPS, RSSI, confidence, detection method, detection ID |
| `Threats.pcap` | Raw WiFi packet captures with GPS timestamps (Wireshark, link type 802.11) |
| `BLE_Threats.pcap` | Raw BLE advertisement captures (Wireshark, link type Bluetooth LE LL) |

---

## SD Card Structure

```
/PLUME/
├── logs/
│   └── PlumeLog.csv
├── captures/
│   ├── Threats.pcap
│   └── BLE_Threats.pcap
└── stats/
    └── lifetime.txt
```

Hot-plug is supported — you can insert or remove the SD card while the device is running. It auto-remounts (directory creation, PCAP header verification, history reload) with a toast notification. Removal is detected via `SD.cardType()` probe every 5 seconds. Writes are buffered and flushed every 500ms or when buffers fill. A heap check skips flushes below 6KB free to prevent FAT driver allocation failures.

---

## Detection Methods

The firmware scores each device against a multi-factor signature database. 75% confidence or higher triggers an alarm. Scores cap at 100%.

### WiFi MAC OUI Prefixes

The firmware checks the source MAC (addr2) of every WiFi frame against two tiers of known Flock-associated OUI prefixes. Locally-administered (randomized) MACs — where bit 1 of byte 0 is set — are excluded from OUI scoring since randomized addresses can coincidentally match a Flock prefix. Those still get checked against SSID patterns.

It also checks the destination MAC (addr1) using the NitekryDPaul technique. Flock cameras spend most of their time asleep and never transmit, but nearby APs still address frames to them. Checking addr1 catches these sleeping cameras that would otherwise be invisible.

**Tier 1 — Strong Match (20 OUIs)**

These are high-confidence Flock Safety OUIs. A tier 1 match alone scores 60%.

| OUI | Association |
|-----|-------------|
| `8c:1f:64` | Flock Safety |
| `4c:6e:44` | Flock Safety |
| `ec:1b:bd` | Flock Safety |
| `58:8e:81` | Flock Safety |
| `d8:a0:d8` | Penguin battery BLE |
| `dc:54:75` | Flock Safety |
| `e0:e2:e6` | Flock Safety |
| `f0:9f:c2` | Flock Safety |
| `68:b6:b3` | Flock Safety |
| `a0:b7:65` | Flock Safety |
| `24:0a:c4` | Flock Safety |
| `a4:e5:7c` | Flock Safety |
| `78:e3:6d` | Flock Safety |
| `fc:f5:c4` | Flock Safety |
| `b0:b2:1c` | Flock Safety |
| `b4:1e:52` | Flock Safety |
| `90:35:ea` | Flock Safety |
| `f0:82:c0` | Flock Safety |
| `b4:e3:f9` | Flock Safety |
| `04:0d:84` | Cradlepoint modem |

**Tier 2 — Supporting Evidence (34 OUIs)**

These are lower-confidence associations — component vendors, shared chipsets, and field-reported OUIs that need corroboration from other signals. A tier 2 match contributes 25% as supporting evidence and stacks with other factors.

Includes 14 OUIs from the NitekryDPaul 31-prefix research list, field-validated during the Joplin test (11/12 cameras detected, 2 false positives).

| OUI | Association |
|-----|-------------|
| `48:e7:29` | Flock Safety (newer hardware) |
| `74:4c:a1` | LiteOn (WCBN3510A WiFi/BT in Falcon V2) |
| `94:34:69` | Flock Safety |
| `38:5b:44` | Flock Safety |
| `94:08:53` | Flock Safety |
| `1c:34:f1` | Flock Safety |
| `a4:cf:12` | Espressif (ext battery modules use ESP32) |
| `3c:91:80` | Flock Safety |
| `80:30:49` | Flock Safety |
| `14:5a:fc` | Flock Safety |
| `9c:2f:9d` | Flock Safety |
| `e4:aa:ea` | Flock Safety |
| `c8:c9:a3` | Flock Safety (Condor PTZ models) |
| `70:c9:4e` | LiteOn |
| `d0:39:57` | Flock Safety |
| `24:b2:b9` | Flock Safety |
| `00:f4:8d` | Flock Safety |
| `e0:0a:f6` | Flock Safety |
| `08:3a:88` | Murata (BLE modules) |
| `d8:f3:bc` | Flock Safety |
| `b8:35:32` | NitekryDPaul research list |
| `c0:35:32` | NitekryDPaul research list |
| `f4:6a:dd` | NitekryDPaul research list |
| `f8:a2:d6` | NitekryDPaul research list |
| `e8:d0:fc` | NitekryDPaul research list |
| `e0:4f:43` | NitekryDPaul research list |
| `b8:1e:a4` | NitekryDPaul research list |
| `70:08:94` | NitekryDPaul research list |
| `3c:71:bf` | NitekryDPaul research list |
| `58:00:e3` | NitekryDPaul research list |
| `5c:93:a2` | NitekryDPaul research list |
| `64:6e:69` | NitekryDPaul research list |
| `48:27:ea` | NitekryDPaul research list |
| `82:6b:f2` | NitekryDPaul research list |

### WiFi SSID Patterns

| Pattern | Type | Score |
|---------|------|-------|
| `Flock-XXXX` (hex format, e.g. `Flock-3A88`) | Definitive | 100% |
| `FS Ext Battery` | Known name | Supporting |
| `Penguin` | Known name | Supporting |
| `FlockOS` | Known name | Supporting |
| `flocksafety` | Known name | Supporting |
| `OFS_IoT` | Field-reported infra | Supporting |
| `PFS_` | Field-reported prefix | Supporting |
| `test_flck` | Dev/test unit | Supporting |

The firmware also checks for WPA2-PSK on Flock-format SSIDs via RSN IE tag 48 AKM suite parsing — real Flock cameras use WPA2-PSK, so an open network named `Flock-XXXX` would score lower.

### WiFi Wildcard Probe Requests

The DeFlockJoplin technique: when a device sends a probe request with an empty SSID (wildcard), management frame subtype 4, from a known Flock OUI — that's a definitive match at 100%. This catches cameras that are actively scanning for their configured AP but not yet associated.

### BLE Signatures

**Flock Manufacturer ID**

Company ID `0x09C8` (XUNTONG) in the BLE manufacturer data field. This is a strong single-factor match.

**BLE Device Name Patterns**

| Pattern | What It Is |
|---------|------------|
| `FS Ext Battery` | Flock external battery BLE relay |
| `FS-XXXXXX` | Newer Flock BLE device naming (e.g. `FS-85068D`) |
| `RWLS-[MAC]` | Field-observed Flock device pattern (e.g. `RWLS-38:5B:44:B3:0F:5A`) |
| Penguin (10-digit numeric) | Penguin devices — firmware dropped the `Penguin-` prefix in March 2025, now just broadcasts a bare number like `1234567890` |
| `Pigvision` | Pigvision system |
| `FlockCam` | Seen in some BLE advertisements |

**TN Serial Detection**

The firmware extracts TN serial numbers from BLE manufacturer data. Presence of a TN serial is a strong match signal.

**BLE Address Type**

Public addresses and random static addresses (what Flock batteries use) get a confidence bonus. Random resolvable addresses — the kind phones rotate every ~15 minutes — get nothing. This filters out pedestrian devices from the scoring.

### Raven (SoundThinking / ShotSpotter) BLE Service UUIDs

Raven acoustic surveillance devices advertise a distinctive combination of custom and standard BLE service UUIDs. The firmware fingerprints the approximate firmware generation based on which UUIDs are present.

| UUID | Service | Firmware |
|------|---------|----------|
| `00003100-0000-1000-8000-00805f9b34fb` | GPS / Location | 1.2.x+ |
| `00003200-0000-1000-8000-00805f9b34fb` | Power Management | 1.2.x+ |
| `00003300-0000-1000-8000-00805f9b34fb` | Network Status | 1.2.x+ |
| `00003400-0000-1000-8000-00805f9b34fb` | Uptime / Health | 1.3.x+ |
| `00003500-0000-1000-8000-00805f9b34fb` | Error Reporting | 1.3.x+ |
| `0000180a-0000-1000-8000-00805f9b34fb` | Device Information | 1.1.x (legacy) |
| `00001809-0000-1000-8000-00805f9b34fb` | Health Thermometer | 1.1.x (legacy) |
| `00001819-0000-1000-8000-00805f9b34fb` | Location and Navigation | 1.1.x (legacy) |

1 custom UUID (3100–3500) = strong match. 3+ UUIDs = definitive (100%). The firmware classifies as 1.1.x-LEGACY, 1.2.x, or 1.3.x and logs the generation.

### Confidence Scoring

| Condition | Score | Notes |
|-----------|-------|-------|
| `Flock-XXXX` SSID format match | 100% | Definitive — hex format with partial MAC |
| Raven 3+ UUID match | 100% | Definitive — unique service combination |
| BLE name + manufacturer ID match | 100% | Definitive — two independent strong signals |
| Wildcard probe from known OUI | 100% | Definitive — DeFlockJoplin technique |
| Tier 1 MAC + SSID pattern | 100% | Two strong corroborating signals |
| Tier 1 MAC alone | 60% | Strong single factor |
| Tier 2 MAC | 25% | Supporting — needs corroboration |
| BLE name pattern match | 25% | Supporting |
| Static BLE address | 25% | Supporting |
| RSSI above -50 dBm | +10% | Bonus — strong signal suggests proximity |
| Stationary behavior detected | +15% | Bonus — fixed installation pattern |

Supporting evidence stacks. A tier 2 MAC (25%) plus a name pattern match (25%) plus strong RSSI (+10%) gets you to 60%. Add stationary behavior (+15%) and you're at 75% — alarm threshold.

### Stationary Detection

Two algorithms run against the RSSI sample buffer for each tracked device:

**Peak-shape** — you're driving past a fixed installation. RSSI rises as you approach, peaks, then falls as you pass. The firmware looks for ≥6 dB range across the sample window. This is the classic "drive-past" pattern.

**Flat variance** — both you and the device are stationary. RSSI stays within ≤3 dB range across all samples. You're parked near a camera, or stopped at a light next to one.

---

## Whitelisting

From the Detections detail view, press `w` to whitelist a MAC address. Whitelisted devices are permanently suppressed from future alarms — checked before `log_detection()` in both WiFi and BLE paths. Stored in LittleFS flash (`/wl.txt`), survives reboots. Max 16 entries.

## Persistence

Session state, lifetime stats, detection IDs, and user settings are saved to LittleFS flash every 60 seconds via a dedicated one-shot task on Core 1 (never blocks the main loop). Atomic writes with temp file + rename to protect against power-loss corruption. What persists across reboots:

Night mode, brightness, low power, stealth, mute, turbo, WiFi credentials (AES-encrypted), lifetime counters, next detection ID, volume level.

Flash wear monitoring toasts at 80K writes (warning) and 100K (critical).

## Battery

Battery percentage uses a 9-point piecewise linear LiPo curve (4200mV=100% through 3300mV=0%). Load-aware telemetry compensates for voltage sag during WiFi promiscuous mode (+45mV), active BLE scanning (+35mV), and speaker playback (+80mV). EMA-filtered ADC readings at 250ms intervals.

Warnings at 3.7V (low) and 3.5V (critical), with 100mV hysteresis to prevent strobe on noisy ADC. Re-warnings every 10 min (low) or 2 min (critical). Auto-restart at 3.0V to prevent deep discharge.

**Approximate runtime:**
- Normal mode (160 MHz): 4–6 hours depending on RF density
- Low power (80 MHz): 6–8 hours
- Turbo (240 MHz): 2.5–4 hours

## Night Mode

Press `n` or toggle in the menu. Shifts the entire UI to a red-shifted palette that preserves dark adaptation. LED syncs to red breathing. Teal header becomes #FF5A5A, purple BLE becomes rose (#FF9696), dim text lifts for readability (#B45A5A), amber stays amber. Background shifts to deep red (#0A0000).

## Ambient Mode

After 2 minutes with no input, the screen dims to minimum brightness and the scanner keeps running at ~15 fps. Forces scanner screen if you idled on another screen. Any keypress wakes it. Disabled when signal tracking is active, export mode is running, or a toast is showing.

---

## Architecture

Both cores of the ESP32-S3 are in use:

- **Core 0** — Scanner task (WiFi channel hopping with configurable dwell, BLE scan scheduling with hang detection), GPS parsing
- **Core 1** — Main loop (UI rendering via DMA-pushed sprite, keyboard, SD flushing, alarms), BLE worker task (advertisement processing from lock-free pool, detection scoring), LED breathing, one-shot persist task

WiFi packets flow through an 8-slot lock-free ring buffer — the promiscuous callback writes raw frame snapshots, the main loop drains and parses tagged parameters. BLE advertisements use a 4-slot static pool with atomic CAS for slot claiming, fed through a FreeRTOS queue to the worker. Zero heap allocation in either hot path.

A recursive FreeRTOS mutex protects shared state between cores (500ms timeout, diagnostic logging on contention). A separate SD mutex serializes card I/O with short timed takes so nothing blocks the main loop for more than one frame.

Channel hopping pauses for 10 seconds on detection (channel lock) to maximize packet capture. BLE scan parameters adapt to power mode: full duty cycle in normal, 50% in low power. Periodic BLE stack restart every 30 minutes prevents NimBLE state corruption during long sessions.

## Prior Hardware

This is the M5Cardputer ADV edition. The original version ran on a [Seeed Studio XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) with an SSD1306 128×64 monochrome OLED, external buzzer, and NEO-6M GPS. The detection engine, signature database, and scoring logic are shared between both. The XIAO version is at [github.com/zmattmanz/flock-detection](https://github.com/zmattmanz/flock-detection).

---

## Credits

This project builds on work from the surveillance detection community:

- **[Colonel Panic / flock-you](https://github.com/colonelpanichacks/flock-you)** — Original detection logic, MAC/SSID identification research, and the OUI-SPY hardware platform. [colonelpanic.tech](https://colonelpanic.tech).
- **[f1yaw4y / FlockSquawk](https://github.com/f1yaw4y/FlockSquawk)** — Primary UI inspiration and field-ready implementation.
- **[Will Greenberg (@wgreenberg)](https://github.com/wgreenberg)** — BLE manufacturer company ID detection method (0x09C8 / XUNTONG).
- **[GainSec / Ryan O'Horo](https://github.com/GainSec)** — Raven BLE service UUID dataset (`raven_configurations.json`) for detecting SoundThinking/ShotSpotter devices across firmware versions 1.1.x through 1.3.x. Also documented the `Flock-XXXX` SSID format and Penguin firmware naming changes.
- **[NitekryDPaul / DeFlockJoplin](https://github.com/colonelpanichacks/flock-you)** — Field-validated 31-prefix WiFi OUI research list and the addr1 receiver-side detection technique for catching sleeping cameras. The Joplin field test (11/12 cameras detected, 2 false positives) validated the wildcard probe request signature.
- **[DeFlock (FoggedLens/deflock)](https://github.com/FoggedLens/deflock)** — Crowdsourced ALPR location data and detection methodologies. [deflock.me](https://deflock.me).
- **[FlockBack](https://github.com/FlockBack)** — Community detection data contributions.

---

## Known Limitations

- **No PSRAM.** The device runs on ~17 KB of free heap after all subsystems initialize. Memory-intensive operations manage heap carefully but can fail under extreme conditions. Auto-restart at 3KB free, SD/LittleFS writes skipped below 6KB.
- **GPS accuracy** depends on environment. 3–5 m CEP typical. Indoor or urban canyon fix quality will suffer.
- **RSSI is relative, not absolute.** Multipath, antenna orientation, and environmental factors all affect it. Use the Signal screen as a proximity guide, not a distance meter.
- **Passive only.** Can't detect cameras that are powered off or not beaconing.
- **LittleFS corruption** can happen on hard power loss. The firmware auto-recovers via `LittleFS.begin(true)` with partition erase fallback, but persisted settings reset to defaults.
- **Dedup window.** Same device is only alerted once per 5-minute window (30 seconds in turbo). Re-detections within the window are silently logged for RSSI tracking but don't trigger alarms.

## Legal

**This software is provided "as is" without warranty of any kind.** The authors accept no responsibility for any use, misuse, or consequences. You use Plume entirely at your own risk.

Nothing here is legal advice. Laws on wireless signal reception and surveillance device detection vary by jurisdiction. It's on you to know and comply with your local laws before running this.

Detection results are probabilistic. False positives and false negatives will happen. Confidence scores and device identifications should not be treated as conclusive evidence for any purpose.

This tool is designed for security research, privacy awareness, FOIA documentation, education, and personal awareness. It operates passively — receives publicly broadcast RF signals, never transmits. The only network transmission is during export mode, which connects to your own WiFi to serve a local file download page.

**You are solely responsible for your use of this tool.**

## License

MIT — see [LICENSE](LICENSE) for full text.

---

**v1.0-beta**
