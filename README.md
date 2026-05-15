# Flock Finder for M5Cardputer ADV Edition

> **Note:** Project name is changing. Find-and-replace "Flock Finder" when the new name is chosen.

Passive RF scanner that detects Flock Safety ALPR cameras and Raven surveillance devices using WiFi promiscuous mode and Bluetooth Low Energy scanning. Runs on the M5Cardputer ADV (ESP32-S3) with a 240×135 color LCD, full QWERTY keyboard, GPS, and SD card logging.

🔒 No network connection required. No cloud. Everything runs locally on the device.

---

## 🛠 Hardware

- **M5Cardputer ADV** (ESP32-S3, no PSRAM variant supported)
- **GPS module** connected via UART (pins 15/13, 115200 baud) — NEO-6M, BN-220, or similar
- **MicroSD card** (FAT32, any size) — for detection logs, packet captures, and stats

## ⚡ Flashing

1. Install the Arduino IDE with ESP32-S3 board support
2. Install libraries: `M5Cardputer`, `NimBLE-Arduino`, `TinyGPSPlus`
3. Set board to `M5Cardputer` (or `ESP32-S3 Dev Module` with USB CDC enabled)
4. Set partition scheme to include SPIFFS/LittleFS
5. Before first flash, edit the WiFi credentials near the top of the file if you want export mode to work out of the box:
   ```c
   #define EXPORT_WIFI_SSID "YOUR_SSID_HERE"
   #define EXPORT_WIFI_PASS "YOUR_PASSWORD_HERE"
   ```
6. Flash via USB-C

## 🚀 Quick Start

Power on. The boot screen shows initialization progress, then a title card appears briefly before transitioning to the scanner screen. The device immediately begins scanning WiFi and BLE channels for known Flock/Raven signatures.

When a detection occurs:
- 🔊 An audible alarm plays (pitch pattern indicates confidence level and protocol)
- 💡 The LED flashes
- 📋 A toast notification appears with the device name and confidence percentage
- 💾 The detection is logged to the SD card with GPS coordinates, signal strength, and detection method

Navigate between screens using the arrow keys (`;` up, `.` down, `,` left, `/` right) or press `m` to open the menu.

---

## 📺 Screens

### 1️⃣ Scanner

The main operating screen. Split into two panels:

**Left panel** cycles through three visualizations (press `v` to cycle):
- **SCAN** — Proximity radar with phosphor sweep. Devices appear as icons (triangles = WiFi, diamonds = BLE) positioned by signal strength. Flock detections pulse in amber.
- **LINE** — 13-channel WiFi spectrum curve showing packet density per channel. A scan line tracks the current hopping channel. Shifts toward purple when BLE is actively scanning.
- **TIME** — 5-minute layered timeline showing WiFi (teal) and BLE (purple) activity as 3D isometric ribbons. Flock detection pips appear as amber dots on the curve.

**Right panel** shows a live activity feed of all observed WiFi and BLE devices, sorted by recency. Flock devices are marked with amber symbols. Press `f` to expand the feed fullscreen, then use arrows to select a device and `t` to target it for signal tracking.

### 2️⃣ Signal

RSSI proximity tracker for a targeted device. Shows:
- Target name and tracking status badge
- Live dBm reading with CLOSER/FARTHER trend indicator
- Signal strength bar with micro-jitter animation
- Rolling signal trace curve (2-minute history) with peak bookmark

To target a device: open the expanded feed (`f`), select a device with arrows, press `t`. Or from the Detections screen detail view, press `t`.

Press `l` to clear the current target.

### 3️⃣ Detections

Scrollable list of confirmed Flock/Raven detections loaded from the SD card log. Most recent first. Each row shows device name, type, RSSI, and a confidence bar.

Press Enter to open the detail view for the selected detection, showing MAC address, detection ID, signal strength, detection method, timestamp, date, and GPS coordinates.

From the detail view:
- `t` — target this device on the Signal screen
- `d` — delete this detection (press `d` or Enter again to confirm)
- `w` — whitelist this MAC (suppresses future alarms for this device)

### 4️⃣ GPS

Animated wireframe globe with orbital satellite indicators. Right panel shows:
- Lock status (GPS LOCKED / Searching...)
- Latitude and longitude
- Satellite count
- HDOP (horizontal dilution of precision)
- Local time (auto-computed from GPS longitude and US DST rules)

### 5️⃣ Stats

Scrollable grid of stat cards showing session and lifetime counters. Scroll with up/down arrows.

Cards: session detections, lifetime detections, WiFi count, BLE count, Raven count, session uptime, lifetime uptime, battery percentage, heap memory, packets scanned, SD card capacity, boot count, flash write count, firmware version, raw voltage.

---

## ⌨️ Keyboard Controls

### Global
| Key | Action |
|-----|--------|
| `,` `/` | Previous / next screen |
| `;` `.` | Up / down (screen-specific) |
| `m` | Open / close menu |
| `Tab` | Toggle help overlay |
| `Esc` | Close overlay or go home to scanner |
| `Del` | Back (close overlay / detail / go home) |
| `-` `+` | Volume down / up |
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
| `;` `.` | Navigate list |
| `Enter` | Open / close detail view |
| `d` | Delete detection (from detail view) |
| `t` | Target for signal tracking (from detail view) |
| `w` | Whitelist MAC (from detail view) |

---

## 📋 Menu

Press `m` to open. Navigate with arrows, select with Enter.

**🧭 Navigate:** Scanner, Signal, Detections, GPS, Stats

**⚙️ Settings:**
- Night Mode — red-shifted palette for dark environments
- Low Power Mode — 80 MHz CPU, reduced scan cadence, longer battery life
- Mute Beeps — silences all tones
- Turbo Mode — 240 MHz CPU, faster channel hopping (150ms vs 250ms), 30-second dedup window (vs 5 minutes). Mutual exclusion with low power mode.

**🔧 Tools:**
- WiFi Config — configure SSID and password for export mode
- Export Mode — starts an HTTP server to download logs from a browser (see below)
- Clear All Stats — resets all session and lifetime counters

---

## 📤 Export Mode

Export mode pauses all scanning and starts a local web server so you can download detection logs, WiFi packet captures, and BLE packet captures from any device on the same network.

### Setup
1. Open the menu → WiFi Config
2. Enter your WiFi network SSID and password
3. Save (credentials are AES-encrypted on flash)

### Using Export
1. Menu → Export Mode (or select from tools)
2. The device connects to WiFi and shows the server URL and password
3. Open the URL in any browser on the same network
4. Enter username `flock` and the displayed 4-character hex password
5. Download files from the web interface

Export auto-exits after 10 minutes (extended while a client is actively downloading). Press `m` → Stop Export to exit manually.

### 📁 Files
| File | Description |
|------|-------------|
| `FlockLog.csv` | All detections with timestamps, GPS, RSSI, confidence, detection method |
| `Threats.pcap` | Raw WiFi packet captures (open in Wireshark) |
| `BLE_Threats.pcap` | Raw BLE advertisement captures (open in Wireshark) |

---

## 💾 SD Card Structure

```
/FLOCK_FINDER/
├── logs/
│   └── FlockLog.csv
├── captures/
│   ├── Threats.pcap
│   └── BLE_Threats.pcap
└── stats/
    └── lifetime.txt
```

The SD card can be inserted or removed while the device is running — hot-plug is supported with automatic remount and a toast notification.

---

## 🔍 Detection Methods

The firmware scores each observed device against a multi-factor signature database. A confidence score of 75% or higher triggers an alarm.

**📡 WiFi signatures:**
- Known Flock MAC prefixes (tier 1: high confidence, tier 2: supporting evidence)
- SSID patterns (`Flock-XXXX` format, known Flock SSIDs)
- Wildcard probe requests from known OUIs (DeFlockJoplin technique)
- WPA2-PSK on Flock-format SSIDs
- Receiver MAC analysis (catches sleeping cameras via addr1 field)
- Vendor OUI collection from tagged parameters

**📶 BLE signatures:**
- Flock manufacturer ID (`0x09C8`)
- Raven custom service UUIDs (3100–3500 range)
- Known device name patterns (Penguin numeric names, FS- prefix, RWLS- prefix)
- TN serial in manufacturer data
- Static address analysis for supporting evidence

**🎯 Scoring:**
- Definitive match (SSID format, Raven multi-UUID, name + manufacturer): 100%
- Strong match (tier 1 MAC + SSID, wildcard probe from known OUI): 60–100%
- Weak supporting evidence (tier 2 MAC, name pattern): 25% per factor, cumulative
- Bonuses: strong RSSI (+10%), stationary behavior pattern (+15%)

---

## 🛡 Whitelisting

From the Detections detail view, press `w` to whitelist a MAC address. Whitelisted devices are permanently suppressed from future alarms. The whitelist is stored in LittleFS flash and survives reboots. Maximum 16 entries.

## 💾 Persistence

Session state, lifetime stats, detection IDs, and user settings are saved to LittleFS flash every 60 seconds and on clean shutdown. Settings that persist across reboots:

- Night mode, brightness, low power, stealth, mute, turbo mode
- WiFi credentials (AES-128-CBC encrypted)
- Lifetime counters (detections, boots, flash writes, uptime)
- Next detection ID sequence
- Volume level

## 🔋 Battery

Battery percentage uses a 9-point piecewise linear LiPo discharge curve for accuracy across the voltage range. Load-aware telemetry compensates for voltage sag during WiFi, BLE, and speaker activity.

Warnings appear at 3.7V (low) and 3.5V (critical), with periodic re-warns every 10 minutes (low) or 2 minutes (critical).

**Approximate runtime:**
- ⚡ Normal mode (160 MHz): 4–6 hours depending on RF density
- 🔋 Low power mode (80 MHz): 6–8 hours
- 🚀 Turbo mode (240 MHz): 2.5–4 hours

## 🌙 Night Mode

Press `n` or toggle in the menu. Shifts the entire UI to a red-shifted palette that preserves dark adaptation. The LED syncs to red breathing. All colors are remapped — teal becomes red, purple becomes rose, amber stays amber for caution indicators.

## 💤 Ambient Mode

After 2 minutes of no keyboard input, the screen dims to minimum brightness and the scanner continues running at reduced frame rate (~15 fps). Any keypress wakes the device immediately. Disabled automatically when signal tracking is active or export mode is running.

---

## 🦅 Raven Detection (SoundThinking / ShotSpotter)

The firmware detects Raven acoustic surveillance devices via BLE service UUID fingerprinting. Raven devices advertise a distinctive combination of custom and standard BLE service UUIDs that vary by firmware version:

| UUID | Service | Firmware |
|------|---------|----------|
| `00003100-...` | GPS / Location | 1.2.x+ |
| `00003200-...` | Power Management | 1.2.x+ |
| `00003300-...` | Network Status | 1.2.x+ |
| `00003400-...` | Uptime / Health | 1.3.x+ |
| `00003500-...` | Error Reporting | 1.3.x+ |
| `0000180a-...` | Device Information | 1.1.x (legacy) |
| `00001809-...` | Health Thermometer | 1.1.x (legacy) |
| `00001819-...` | Location and Navigation | 1.1.x (legacy) |

Detection confidence: 1 custom UUID = strong match. 3+ UUIDs = definitive. The firmware identifies the approximate firmware generation (1.1.x-LEGACY, 1.2.x, 1.3.x) and logs it in the detection extra data field.

---

## 🏗 Architecture

The firmware uses both cores of the ESP32-S3:

- **Core 0** — Scanner task (WiFi channel hopping, BLE scan scheduling), GPS parsing
- **Core 1** — Main loop (UI rendering, keyboard input, SD flushing, alarm output), BLE worker task (advertisement processing, detection scoring), LED task, persist task

A recursive FreeRTOS mutex (`dataMutex`) protects all shared state between cores. A separate mutex (`sdMutex`) serializes SD card I/O. SD writes are buffered and flushed every 500ms or when buffers fill. Session state persists to LittleFS flash every 60 seconds via a one-shot task on Core 1.

## 📟 Prior Hardware

This firmware is the M5Cardputer ADV edition. The original Flock Finder ran on a [Seeed Studio XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) with an SSD1306 128×64 monochrome OLED, external buzzer, and NEO-6M GPS module. The detection engine, signature database, and scoring logic are shared between both editions. The original XIAO version is available at [github.com/zmattmanz/flock-detection](https://github.com/zmattmanz/flock-detection).

---

## 🙏 Credits & Acknowledgments

This project builds on the work of the surveillance detection community:

- **[Colonel Panic / flock-you](https://github.com/colonelpanichacks/flock-you)** — Original detection logic, MAC/SSID identification research, and the OUI-SPY hardware platform. Available at [colonelpanic.tech](https://colonelpanic.tech).
- **[f1yaw4y / FlockSquawk](https://github.com/f1yaw4y/FlockSquawk)** — Primary inspiration for the UI and field-ready implementation.
- **[Will Greenberg (@wgreenberg)](https://github.com/wgreenberg)** — BLE manufacturer company ID detection method (0x09C8 / XUNTONG).
- **[GainSec / Ryan O'Horo](https://github.com/GainSec)** — Raven BLE service UUID dataset (`raven_configurations.json`) enabling detection of SoundThinking/ShotSpotter acoustic surveillance devices across firmware versions 1.1.x through 1.3.x. Also documented the `Flock-XXXX` SSID format and Penguin firmware naming changes.
- **[NitekryDPaul / DeFlockJoplin](https://github.com/colonelpanichacks/flock-you)** — Field-validated 31-prefix WiFi OUI research list and the addr1 receiver-side promiscuous mode detection technique for catching sleeping cameras. The Joplin field test (11/12 cameras detected, 2 false positives) validated the wildcard probe request signature.
- **[DeFlock (FoggedLens/deflock)](https://github.com/FoggedLens/deflock)** — Crowdsourced ALPR location data and detection methodologies. Visit [deflock.me](https://deflock.me) to contribute sightings.
- **[FlockBack](https://github.com/FlockBack)** — Community detection data contributions.

---

## ⚠️ Known Limitations

- **No PSRAM:** The device runs on ~17 KB of free heap after all subsystems initialize. Memory-intensive operations (export mode, BLE stack restart) manage heap carefully but can fail under extreme conditions.
- **GPS accuracy:** Position coordinates are GPS-dependent (3–5 m CEP). Indoor or urban canyon environments may have degraded fix quality.
- **RSSI accuracy:** Signal strength is affected by multipath, antenna orientation, and environmental factors. Use the Signal screen as a relative proximity guide, not an absolute distance measure.
- **Detection is passive:** The device never transmits. It cannot detect cameras that are powered off or not actively beaconing.
- **LittleFS corruption:** Intermittent flash corruption may occur on hard power loss. The firmware auto-recovers by reformatting, but persisted settings reset to defaults when this happens.

## ⚖️ Legal

This tool is intended for security research, privacy auditing, FOIA documentation, and educational purposes. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions. Always comply with local laws regarding wireless scanning and signal interception.

## 📄 License

MIT

---

**v1.0-beta**
