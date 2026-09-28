# Changelog

All notable changes to Plume are recorded here.

## Unreleased — Sentinel acquisition baseline

### Added

- Reproducible Arduino CLI bootstrap and build scripts pinned to ESP32 Arduino
  core 3.3.10 and the audited Cardputer/C5 dependency versions.
- Evidence-aware Axon classification: official vendor OUI `00:25:DF`, Bluetooth
  company identifier `0x034D`, and research-reported UUID `0xFC81` as
  corroboration only. OUI-only results are logged as non-alarming
  `AXON_ASSOC`; no product model is inferred.
- Separate Axon session accounting and Stats card.
- Host-side signature parity/policy check for the Cardputer and C5 sketches.
- Baseline, architecture audit, documentation-drift record, and Sentinel roadmap
  under `docs/`.

### Fixed

- SD buffers now retain unwritten records across ordinary open/write failures;
  CSV, WiFi-PCAP, and BLE-PCAP producer drops are counted in health telemetry.
- SD signature overrides stage and validate the entire recognized transaction
  before replacing active tables or reporting `SD` provenance.
- C5 alarm threshold and built-in fallback signatures now match the Cardputer.
- Both UART parsers discard complete overlong lines; C5 signature rows reject
  invalid OUIs and tiers.
- Exact OUI parsing, persisted-time termination, format arguments, and the
  deprecated RGB LED call were corrected.

### Documentation

- README now matches the current two scanner visualizations, Signal navigation,
  70-point threshold, signature tiers, SD cadence, BLE duty cycle, C5 support,
  export password length, ring size, GPS baud detection, and pinned CLI build.

## v1.3

The Signal screen, rebuilt against the `design_handoff_signal_screen` spec, and
a privacy fix in the export page. Cardputer sketch only — `c5-sniffer/` is
unchanged from v1.2 and does not need reflashing.

### Changed — menu: Charge Mode back, Signal out

- **Manual Charge Mode access restored**, reverting the removal in `6555969`.
  Automatic entry (low battery / brownout) was never removed and is unaffected;
  the `c` key and the menu row are back alongside it, along with
  `CHARGE_MODE_FULL_MV` so a deliberate top-up again holds near full rather than
  resuming at the 3750 mV safe floor.
- **`Signal` removed from the menu.** The target-tracking screen is now reachable
  only by picking a device out of the scanner feed, which is the only context
  where it means anything — it was already excluded from the `,`/`/` carousel by
  `screen_in_carousel()`, so the menu row was the last way to land on it with
  nothing targeted.
- Worth noting for anyone reading `handle_menu_select()`: idx 1 was part of a
  fall-through group (`case 0: case 1: case 2: case 3: case 4:`) sharing one
  body, so only the label came out — the body still serves screens 0, 2, 3 and 4.
  `menu_icon_signal()` stays too; cases 8 (Turbo) and 12 (5GHz) still call it.
  `MENU_ROW_COUNT` nets 19 → 18. Verified all 13 remaining idx values map 1:1 to
  both the handler and the icon dispatch, in both directions.

### Security — CSV formula injection, and silent signature replacement

- **CSV formula-injection guard.** A field beginning with `=`, `+`, `-` or `@`
  executes as a formula in Excel, LibreOffice and Google Sheets, and opening
  these logs in a spreadsheet is the documented workflow. An attacker controls
  their own AP's SSID completely, so an SSID of `=1+1` became a live formula in
  the operator's spreadsheet. `csv_defuse_formula()` prefixes an apostrophe on
  `clean_name` and `clean_extra` only — after the comma strips that keep the 21
  columns aligned. No RFC 4180 quoting: `load_sd_history()` and the
  delete-rewrite path both parse with fixed comma offsets and would break on
  quoted fields. Truncation bounds verified for every input length 1–79 against
  `clean_name[64]`.
- **Signature replacement is no longer silent.** `signatures_load_from_sd()` can
  clear and replace the compiled OUI/SSID/name tables from the SD card. Someone
  with brief access to the card could remove their own hardware from the
  signature set, and the device would keep scanning, keep showing green, and
  simply stop detecting them. This does not prevent that — it removes the silent
  part: a `SIGS FROM SD: n/n/n` toast on boot, and `SIGS: SD` / `SIGS: BUILT-IN`
  in the export page footer so provenance travels with anything downloaded.
  The toast fires after the boot reveal, not at load time — the reveal repaints
  the screen and would have wiped it. `TOAST_WARNING`, because a replaced
  database is "something changed", not a success.
- The CSV header row is deliberately unchanged: a provenance line ahead of it
  would be read as a data row by both CSV parsers, which only skip lines
  starting with `Uptime_ms`.

### Changed — WiFi config overlay

- Header reads **Export Mode Configuration** instead of `WIFI CONFIG`, which
  says what the screen is for rather than what it edits. Mixed case is
  deliberate and does not match the other overlay headers.
- **Outer card border removed.** The header strip already frames the screen, so
  the rounded rect boxed in an already-bounded panel. The `cx/cy/cw/ch` geometry
  stays — every field, label and button positions off it. Selection is still
  shown by border colour and never by fill, which is what keeps the form
  readable without the card.
- `PASS` label spelled out to `PASSWORD`.
- **`(not set)` placeholders no longer dimmed.** For a user who has not
  configured WiFi, that placeholder is the most actionable thing on screen, and
  `DIM_COLOR` read as disabled. Uses `TEXT_COLOR`, not a literal white — the
  token is `#FFD0D0` in the red night palette, and hardcoding white would blow
  out night vision.
- **DEL closes the overlay when not typing.** It previously jumped into editing
  mode and deleted the last character, so the one key that closes every other
  overlay instead silently mutated the field under the cursor. Backspace while
  editing is unchanged, and the close path clears `wifi_config_show_pass` like
  every other exit, so a revealed password is masked again on reopen.

### Added — C5 link orientation auto-detection

- **A reversed Grove cable now just works.** Swapping yellow and white is the
  most common wiring mistake with this link, and the only symptom was a dark
  `5G` badge with no explanation. Plume now probes both data pins, latches
  whichever one carries valid protocol traffic, and logs whether the cable is
  reversed.
- **Probing is receive-only.** `begin()` is called with the TX pin as `-1` until
  the orientation is proven. On a reversed cable the S3's TX line is already
  wired into the C5's TX output, so two push-pull drivers are fighting; that
  contention exists in hardware whatever the firmware does, but Plume no longer
  adds to it. A TX pin is configured only after a recognized tag has arrived.
- Validity is `c5_last_msg_ms`, which `c5_handle_line()` sets only for a
  recognized `H`/`F`/`D` tag — so "traffic on this pin" cannot drift out of sync
  with what the parser actually accepts. 4 s dwell per pin, one warning toast
  after three full A/B passes, then probing continues indefinitely so a C5
  powered up later is still found.
- `c5_link_end()` skips the `P|1` radio-idle write when the orientation never
  locked — there is no TX pin to write through, and a C5 we never reached was
  never told to scan. Toggling `5GHz Radio` off and on restarts the probe from
  scratch, which is what you want if the reason for toggling was reseating the
  cable.
- Presence detection (`c5_is_present()`, the 8 s heartbeat timeout) and the
  presence-edge signature push are untouched. Orientation and presence are
  separate problems.

### Fixed — PROTOCOL.md described a reverse channel that has been in use for releases

- The Link table claimed `C5 -> Cardputer (the reverse channel is wired but
  unused in v1)`. Plume transmits time sync, radio power state, LED mirroring and
  a full signature table. Direction is now documented as bidirectional, with an
  Orientation row for the new probe.
- Added a **Messages (Cardputer -> C5)** section covering `T|`, `P|`, `L|` and the
  `SB`/`SO|`/`SS|`/`SE|` signature transaction, and documented the `F|` ambient
  sighting tag, which was undocumented despite driving the 5 GHz feed. Field
  layouts were read from the printf call sites and the C5-side parser, not
  inferred.

### Fixed — ESC did not close the WiFi config overlay

- **The key labelled `esc` now backs out of WiFi config.** The overlay footer
  said "ESC close" and a `0x1B` handler was there to do it, but the key never
  produced `0x1B`: on the Cardputer the esc key *is* the `` ` `` key, and the
  translation above the handler (`if (status.fn && c == 0x60) c = 0x1B`) only
  fires when the Fn chord is held. A bare press arrived as `` ` ``, matched no
  branch in navigation mode, and was swallowed — so the documented way out of
  the screen did nothing unless you knew to press Fn+`.
- Navigation mode now accepts a bare `` ` `` as well as `0x1B`. Editing mode
  deliberately still requires the Fn chord: `` ` `` is a valid character in an
  SSID or password and has to stay typable, so trading that away to save a
  chord would be the wrong fix.

### Changed — GPS globe hoists per-meridian trig out of the inner loop

- **`proj()` now takes longitude as a precomputed cos/sin pair** instead of an
  angle. It was computing `cosf(lon)`/`sinf(lon)` internally on every call, and
  the meridian loop calls it 48 times per meridian with the *same* longitude —
  1,152 trig calls per frame producing 24 distinct values. Longitude is now
  computed once per meridian; the latitude circles, where longitude genuinely
  varies per step, just moved the same trig to the call site.
- Also lifted `cosf(-1.5707f)`/`sinf(-1.5707f)` — the south-pole start latitude,
  identical for all 12 meridians — into two constants above the loop, where it
  was being evaluated on a literal 12 times per frame.
- **1,184 fewer `sinf`/`cosf` per frame** (3,092 → 1,908), and the GPS screen is
  on the 60 fps fast path, so roughly 71,000 fewer per second. Output is
  pixel-identical: no loop restructuring, no change to `N_MER`/`M_STEPS`/`STEPS`,
  `lats[]`, tilt/roll, the `avg_z > 0.0f` culling, or any brightness math.
  **No additional RAM** — DRAM is unchanged at 81,632 bytes; no lookup tables
  were added.
- What is left is dominated by the per-step `clat`/`slat` in the meridian inner
  loop: 1,152 calls, 60% of the remaining total. Latitude genuinely varies per
  step so it cannot be hoisted, though note the sequence is identical across all
  12 meridians.

### Removed — the unreachable post-boot title-card path

- **`draw_title_card()` and its state are gone.** A second title-card renderer
  existed for showing the card *after* boot, with its own hold/fade timeline
  (`TITLE_CARD_HOLD_MS`, `TITLE_CARD_FADE_MS`) driven from `draw_current_screen()`
  case 0. It could never run: `setup()` phase 4 clears `title_card_active` before
  returning, the boot reveal is unconditional, and `run_charge_mode()` returns
  into a normal boot — so the flag is always false by the time `loop()` starts.
  Also removed the now-dead `title_card_active` term in the render fast-path
  condition and the keypress-wake block that cleared it.
- The boot card itself is untouched. `draw_title_card_overlay()`,
  `draw_title_card_impl()`, and `draw_title_grid()` are all live — the overlay
  draws the card during the Phase 2 reveal *and* during the Phase 4 dissolve, and
  both call sites remain. No behavioural change.

### Changed — boot is ~2 seconds shorter

- **Dropped the title-card hold.** Phase 3 sat on the finished card for a flat
  2 s of animated grid before the dissolve. The card already fades in during
  Phase 2 and the chime plays over it, so the hold added nothing but delay
  between power-on and a working scanner. Phase 4's dissolve is unchanged, and
  the phase comments are deliberately left numbered 1, 2, 4 to keep the blame
  trail readable.
- One consequence to watch: the card is static through the ~650 ms chime, since
  those are blocking `delay()` calls with no redraw. That pause used to be hidden
  by the 2 s of animation after it; now it is the last thing on screen before the
  dissolve. If it reads as a stall, trim the chime's delays rather than putting
  the hold back.

### Changed — dead asset, boot dissolve, struct packing

Three unrelated cleanups. **None of the three changes behaviour** — the audio
path, the visual result of the boot fade, and MAC dedup all work exactly as
before.

- **Removed the unused `ui_beep` audio asset.** `ui_beep.h` carried a
  3,609-sample 16-bit PCM clip — 22 KB of source converted from an mp3 — and
  nothing referenced `ui_beep_pcm`, `UI_BEEP_SAMPLES`, `UI_BEEP_RATE`, or
  `HAS_UI_BEEP`. `Speaker.playRaw()` is never called; the header's own usage
  comment was the only thing that described how to play it. The build came out
  byte-identical, since an unreferenced `static const` array was already being
  discarded. The real audio path — `beep()`, `play_escalated_alarm()`,
  `AlarmTask`, all 19 `Speaker.tone()` sites — is untouched.
- **The boot dissolve writes the sprite buffer directly.** Phase 4 faded the
  scanner in with a nested `readPixel`/`drawPixel` loop over 240×115: roughly 1.7
  million of each call across the ~62 frames, every one paying bounds checks and
  colour-format dispatch to touch one pixel of a flat array. Now a single linear
  pass using `read_pixel_logical`/`write_pixel_logical`, the same accessors the
  scanner viz uses. Byte-order detection is now called explicitly instead of
  being inherited — it previously happened only as a side effect of
  `draw_scanner_viz_scan()` running inside `draw_current_screen()`, which held
  purely because `scanner_viz_mode` defaults to 0. `getBuffer()` can return null,
  so the original loop is kept as a fallback and the fade still runs in that
  case. Duration, frame delay, and alpha math unchanged; costs 72 bytes of flash
  for the retained fallback.
- **`SeenMacEntry` reordered to drop 256 bytes of padding.** `char mac[18]`
  leading forced 2 bytes before `ts` and 3 at the tail — 28 bytes per instance.
  Leading with the 4-byte-aligned member gives 24. At 64 instances
  (`seen_mac_table[32]` plus the `static temp[32]` in `seen_mac_expire`) that is
  256 bytes of DRAM, and the build confirms it to the byte: 81,896 → 81,640. No
  call site changed — checked specifically for positional initializers, which
  would have silently misassigned under the new order, and there are none.

### Removed — dead code, second pass

Five commits, no behavioural change. Every item had zero readers.

- **The superseded menu section model** — `MenuItem`, `MenuSection`,
  `nav_items`, `settings_items`, `tools_items`, `menu_sections`,
  `MENU_SECTION_COUNT`. Two menu models coexisted; the live one is `MENU_ROWS` +
  `menu_next_idx()` + the switch in `handle_menu_select()`. The dead one had also
  drifted — no Charge Mode entry, where `MENU_ROWS` has one — so anyone reading
  it to understand the menu got the wrong answer. That is the real cost of a
  second model, not the bytes.
- **`drawCard()`** — two lines, no callers. `drawPill()`, `drawPill_lcd()`, and
  `charge_pill()` are all live and untouched.
- **Ten write-only globals** — `session_wifi`, `session_ble`, `last_cap_rssi`,
  `last_cap_confidence`, `last_cap_seq_num`, `last_ble_scan`, `last_blip_time`,
  `scanner_flash_color`, `signal_target_id`, and the
  `last_rendered_trace_head`/`_count` pair. Most shared a line with live state,
  so these came out statement-by-statement rather than line-by-line —
  `session_wifi++` sat beside `lifetime_wifi++` and `session_flock_wifi++`, and
  the `DEBUG_KEYS` simulate path decrements three live counters on the same two
  lines. Verified with `DEBUG_KEYS=1` as well as the default build.
- **The half-built peak-location capture** — `signal_peak_lat`,
  `signal_peak_lng`, `signal_peak_has_gps` were filled on every new RSSI peak
  and read by nothing; the display/export half was never built. Kept as its own
  commit so it can be reverted cleanly if the feature is ever finished.
  `signal_peak_rssi` and `signal_peak_seq` are live and still drive the hero
  value and the trace's peak tick.
- **Two dead locals** — `num_x` in `draw_boot_screen()`, `prev_mode` in the `v`
  handler.

Reclaimed 24 bytes of DRAM (`.dram0.bss` 57,712 → 57,688; `.dram0.data`
unchanged) and 180 bytes of flash. Worth recording that the DRAM figure is well
short of the ~59 bytes those declarations nominally occupy: small variables sit
inside the alignment padding of their neighbours, so removing one frees no
addressable space until a whole slot clears. `.data` did not move at all despite
losing two non-zero-initialised statics.

### Removed — dead code

Two deletions, no behavioural change to anything reachable.

- **The TIMELINE scanner visualization is gone** (448 lines). `ENABLE_TIMELINE_VIZ`
  had been 0 for several releases, the README had stopped documenting the TIME
  viz, and nothing outside the guarded blocks referenced a timeline symbol. The
  preprocessor was already excluding all of it, so the build came out
  **byte-identical** — same flash, same DRAM — which is the cleanest possible
  proof it was unreachable. `SCANNER_VIZ_COUNT` was an `#if`/`#else` pair and is
  now unconditionally 2; `v` cycles SCAN and LINE. No migration needed —
  `scanner_viz_mode` is a runtime static and was never persisted, so no saved
  value can point at the removed mode.
- **Three unused geo helpers** — `haversine_m()`, `bearing_to()`,
  `bearing_to_compass()` — each defined once and called from nowhere, left over
  from a distance/bearing-to-detection feature that was never built. Worth
  noting for anyone estimating a similar cleanup: this reclaimed **16 bytes**,
  not the few hundred the non-static linkage suggests. `nm` against the
  pre-removal ELF shows the linker had already garbage-collected all three out
  of the flashed image; the symbols survived in debug sections only. The value
  here is that the file no longer advertises a capability the firmware lacks.

### Removed — stealth mode

- **Stealth mode is gone.** One keypress (`s`) dimmed the screen to brightness 5,
  suppressed the LED, and suppressed detection alarms — and it persisted across
  reboots. Toggle it, forget it, and you had a device that looked dead, sounded
  dead, and silently declined to alert on a real camera. The three suppressions
  were bundled with no indication beyond a 6×8 pixel "S" in the bottom-right
  corner, on a screen dimmed almost to black.
- No migration code. The settings loader is an `if`/`else if` chain with no final
  `else`, so a `stealth=1` line left in an existing session file is ignored on
  read and dropped on the next write. Devices with it set come back at normal
  brightness on their own.
- `s` is now unbound globally. It is still live inside the WiFi-config overlay,
  where it toggles plaintext password reveal — that handler is overlay-local and
  untouched.
- `PERSIST_MIN_BYTES` retuned to 120 and its field list corrected. The comment
  had listed 15 fields since before `turbo` and `c5` were added, so the stated
  sum of 130 described a file layout that had not existed for two releases while
  the true floor was 143.

After this: `is_muted` is the only audio suppression path, ambient mode is the
only automatic backlight reduction, and every detection above threshold produces
an audible alarm unless the user has explicitly muted.

### Fixed — the export page phoned home

- **The export page no longer contacts any third party.** Its stylesheet opened
  with an `@import` from `fonts.googleapis.com` for JetBrains Mono and Share Tech
  Mono, so every time the page was opened the browser handed Google an IP
  address, a timestamp, and a referrer — from a tool whose whole pitch is "no
  network connection, no cloud." The README said one thing and the page did
  another.
- Both faces are replaced by one system stack (`ui-monospace`, SF Mono, Menlo,
  Consolas, Liberation Mono), defined once as `--mono` and referenced by all five
  rules that used to name a webfont. The page now renders fully offline, which
  matters beyond privacy: export mode runs the device as its own access point,
  so the client usually has no route to the internet at all — those fonts were
  never going to load anyway, and the page was paying a DNS timeout for them.
- Losing the two-face distinction costs nothing. `.ht`, `.sl`, and `.kv2` carry
  their identity through letter-spacing, uppercase, and accent colour, not the
  typeface. The one real degradation is that system mono stacks rarely ship the
  intermediate weights, so `font-weight:500`/`600` now round to normal or bold.
- 128 bytes smaller in flash.

### Changed — Signal screen, rebuilt to spec

Rebuilt against the `design_handoff_signal_screen` handoff. The screen now answers
one question — is this getting stronger or weaker, and how close have I ever
gotten? Still no distance readout: RSSI is relative, so a foot/metre figure would
be a lie. Everything on it is dBm, percent of scale, or elapsed time.

- **Peak dBm is the hero, not the live reading.** The live value flickers several
  dB between adverts, so the big numeral used to jitter constantly and blank to
  `--` on every dropout. Peak only ever improves, which is also the number you
  actually care about when walking a perimeter.
- **`SEEN <n>s AGO` replaces the blank state.** An age is always printable, so
  there is no longer a reading that vanishes and reappears.
- **Peak-hold meter.** The fill follows the live level and eases; a white hairline
  parks at the strongest reading and never falls back while the target is held.
- **Trace is a plain polyline with a solid 14% fill**, replacing the Catmull-Rom
  spline and its per-column alpha ramp. The x axis scales to the samples on hand
  and pins at two minutes, so a fresh target plots full width instead of a stub
  in an empty box; under 8 samples it draws vertex dots so a short plot reads as
  deliberate. A white tick marks where the peak happened, and is dropped once the
  peak scrolls out of the window rather than re-pointed at the in-window maximum,
  which would contradict the hero value.
- **Protocol glyph** — purple diamond for BLE, teal triangle for WiFi, matching
  the Scanner contact shapes. It stays on the identity colour when the target
  goes stale; it is what the thing *is*, not how it is doing.
- Status pill is `TRACKING` / `HUNTING` / `STALE`. HUNTING is amber now — it was
  mint, which had a confirmed Flock device reading calmer than an unknown one.
- Visualization range widened from −70…−30 dBm to −95…−45 (trace) and −95…−40
  (meter). The old floor clamped anything past about 20 m to zero, so the plot
  sat flat for most of an approach.
- Dropped: the CLOSER/FARTHER trend chip and the `(#nnn)` detection ID, both
  covered better by the trace and the Detections screen respectively.
- Frees the 964-byte curve cache that was malloc'd on entering the screen, and
  240 bytes of static smoothing state.

### Known issues

- The Signal screen's `LOST` gate (10 s, empties the meter and floors the trace)
  fires before the `STALE` gate (30 s, turns the labelling amber) — deliberate,
  so the live elements react faster than the wording, but it does mean a target
  can sit with an empty meter while the pill still reads TRACKING. Both are
  untuned guesses against real advertisement intervals.

## v1.2

Detection accuracy, power, and a UI pass. Both firmwares changed — the Cardputer
sketch and `c5-sniffer/` each need flashing.

### Fixed — detection

- **The addr1 branch could DOWNGRADE a confirmed detection.** It assigned
  `SCORE_STRONG` rather than raising, so a hit already at 100 (the `test_flck`
  CVE probe, or the `Flock-<hex>` SSID format) fell to 60 whenever the
  transmitter OUI was unknown and addr1 matched Tier 1 — under the alarm
  threshold, so it logged silently and never sounded. Fixed in both firmwares.
- **The −80 dBm cutoff discarded known Flock silicon.** Both the WiFi and BLE
  paths bailed before any signature check ran, capping detection range blind. A
  known OUI now overrides it; `check_mac_prefix()` is 54 three-byte `memcmp`s so
  the check is effectively free. The WiFi path tests addr1 too, since the
  sleeping-device technique keys off the receiver address.
- Names arriving over the C5 link are scrubbed. `clean_device_name_char()` had
  only ever been applied to Plume's own WiFi and BLE paths, so 5 GHz names
  reached the UI and CSV with control characters intact.

### Changed — signature table, verified against three registries

Every Tier-1 OUI was resolved against IEEE `oui.txt` (MA-L), `manuf2` (56,505,
incl. MA-M/MA-S) and Ringmast4r/OUI-Master-Database (88,873 merged, incl. IAB and
CID plus Wireshark and Nmap). Only one of the nine was Flock Safety.

- **Tier 1 is now `b4:1e:52` alone** — all three sources agree it is the only OUI
  Flock has ever registered. (Beware `8c:1f:64`, which is "Flock Audio Inc.", an
  unrelated pro-audio company.)
- Eight demoted to Tier 2: Liteon, Shenzhen Shixuntong, **Espressif**
  (`a0:b7:65` — at Tier 1 this alarmed on Plume's own ESP32-S3 and the ESP32-C5
  sniffer), and three Silicon Laboratories prefixes. They still match and still
  log; they can no longer alarm on proximity alone, only on camera behaviour.
- `4c:6e:44` resolved as an IEEE Registration Authority **subdivided** block, so
  a 3-byte match identifies no vendor at all. `d8:a0:d8` has zero matches across
  all 88,873 merged entries — genuinely unassigned. Both kept at Tier 2 for
  logging only.
- Alarm threshold 75 → 70, so a Tier-2 OUI wildcard-probing at close range
  (`SCORE_STRONG` 60 + RSSI bonus 10) now alarms. Previously the 45 Tier-2
  prefixes could log but never beep. Side effects, both close-range only: a
  Tier-1 OUI and an `addr1_t1` sleeping-device hit now alarm unaided.
- Added a bare `"flock"` SSID pattern, and `cc:cc:cc` (Tier 2, unvalidated).

### Fixed — Charge Mode wake

- **The CPU clock is restored to 80 MHz on exit.** Charge Mode drops to 40 MHz
  and the 80 MHz for the brown-out-prone window is set *above* the gate, so the
  entire post-charge boot ran at half clock. This was the actual cause of the
  apparent freeze after pressing a key.
- The wait-for-release loop is time-boxed and feeds the WDT. On the ADV the key
  list is built from TCA8418 events and never rescanned, so a lost release event
  latched a key down forever and that loop spun into a panic reboot.
- Any observed press exits. `update()` drains one FIFO event per call, so a quick
  tap read as pressed on one frame and released the next — the old two-frame
  debounce silently ate short taps.
- Boot-time TCA8418 health check. `Adafruit_TCA8418::begin()` returns only its
  *last* register write, and the reader takes an early return on failure —
  skipping `matrix()`, `flush()`, `attachInterruptArg()` **and**
  `enableInterrupts()`. The keyboard was then silently dead for the whole
  session, likeliest on a marginal rail, which is exactly when Charge Mode runs.
  Now verified by reading `CFG` back, and retried up to 3×.
- The task WDT is retained across the Charge Mode exit and `boot_animate()` pets
  it at every checkpoint. Protection had been dropped precisely as control
  entered a boot phase with no watchdog of its own.
- Titles render as standard app pills instead of hand-rolled letter-spacing.

### Fixed — telemetry that was lying

- **V CHANGE reported roughly +73 mV at boot with no real change.** The baseline
  was captured 308 lines before `system_fully_booted`, so with sag = 0, while
  every later reading carried 45–80 mV of sag compensation. It also drifted
  ±35 mV as BLE scanning cycled, and was taken 169 lines *before* the EMA prime
  loop whose own comment says it exists "before taking the baseline". Now an
  unsagged EMA, sampled once the sag model reaches steady state.

### Changed — power

- New screen-off idle tier: backlight off and panel asleep after 10 min idle
  (2 min in low power), rendering suspended. Detection is unaffected — it all
  runs in background tasks — and a detection raises a toast, which wakes the
  screen.
- **GPS standby is re-asserted every 30 s in low power.** It was fired once at
  the toggle, but the `$PCAS12` window unit is ambiguous across CASIC firmwares —
  if milliseconds, `65535` is only ~65 s and the receiver silently resumed
  acquiring at 25–40 mA. Charge Mode already defended against this; low power
  never did.
- SD hot-plug backoff (5 s → 15 s → 60 s on consecutive failures, all modes). The
  absent-card path runs a full `SD.begin()` that its own comment calls "several
  hundred ms", previously every 5 s forever on a cardless device.
- Low power also slows the idle timeouts and the persist cadence (60 s → 5 min),
  and caps rendering at ~20 fps.
- Ambient brightness is clamped: `AMBIENT_BRIGHTNESS` (40) exceeds
  `BRIGHTNESS_LEVELS[0]` (32), so at 1/4 idling used to make the screen *brighter*.
- The per-second `[bat]` trace is gated behind `DEBUG_BAT_LOG` (off).

### Changed — C5 co-processor

- **Plume can now idle the C5's radio** (`P|<idle>`). Nothing had ever told the
  C5 anything about power: `low_power_mode` never reached it, and
  `c5_link_end()` only closed Plume's own UART while the C5 kept sniffing at full
  duty into a dead line. So "5GHz RADIO OFF" and low power both saved nothing on
  that board.
- Cross-channel wildcard-probe tracker ported to 5 GHz, indexed by position in
  `kChannels[]` (Plume's version masks by `channel - 1`, meaningless above 16).
  Every captured detection in the flock-back proof set was a 5 GHz wildcard
  probe, so this was the one place with no unknown-OUI discovery at all.
- Status LED mirrors Plume's exactly, including the brightness gate — that gate
  is a *hardware* constraint on the Cardputer (its WS2812 runs off the backlight
  boost rail), so the C5 is held to it deliberately to keep the pair in step.
- Byte-based OUI matching (was `snprintf` plus `strncmp` per frame), event queue
  16 → 64 slots, and enq/drop/drop_pct telemetry — dropped frames had been
  entirely silent.
- Target guards on both sketches: building one with the other's board selected
  now fails with one clear message instead of two misleading ones.

### Changed — UI

- The feed shows protocol and band (`2.4GHz` / `5GHz` / `BLE`) in place of the
  dBm column, which was redundant — the `SIGNAL` column beside it is a lossy
  function of the same value. Deliberately not colour-coded: a fourth protocol
  hue would compete with amber, which means flock.
- **The expanded feed can reach every entry.** `FEED_SIZE` is 8, the renderer
  draws 6, and there was no scroll offset — the oldest two were unreachable.
- Signal → Target throughout, and removed from the screen carousel: it is only
  meaningful once something is selected, so it is reached from the feed or the
  menu rather than by cycling onto an empty screen. Content starts with the
  device name instead of a duplicate TARGET label.
- Idle no longer yanks you back to the scanner from Stats or GPS.
- Two-press confirm on CLEAR STATS, the one irreversible menu action.
- Header pills: `A`/`N`/`S` dropped (each announced a state already visible on
  screen) and nothing is left on `DIM_COLOR`.
- The 5GHz menu toggle and the `5G` badge now reflect that low power has idled
  the radio, rather than claiming coverage that is not happening. `c5_enabled`
  remains the user's preference and is never rewritten behind their back.
- Boot screen condensed from 16 steps to 7, about 1.1 s faster, every label
  naming work that actually happens ("calibrating battery" had been labelling a
  20-iteration EMA prime loop).
- Detections empty state centred on both axes, in white.
- `TEXT_LEFT` 4 → 8, with a symmetric right margin on the status pills.

### Known issues

- Charge Mode's flat-read check is an ADC-frozen detector only. It cannot see an
  I2C or keyboard failure: the battery is ADC1/GPIO10 with no I2C involved, while
  the TCA8418 is the only device on the bus. The boot-time health check now
  covers the keyboard case separately.
- Mute still leaks on the WiFi-config screen — five bare `Speaker.tone()` calls
  there bypass `beep()` and its guards, so key clicks sound even with mute on.
  Removing stealth mode did not fix this: those calls never consulted `is_muted`
  either. Fixing it would also unblock powering the I2S amp down when muted.
- The charge screen maps voltage through the discharge curve while charging, so
  the percentage reads optimistically, and `CHARGE_MODE_FULL_MV` (4150) tops out
  at 95%.
- Tier-2 OUIs now alarm on wildcard-probe behaviour at close range, which
  includes three Silicon Laboratories prefixes and Liteon. If that proves noisy,
  remove `cc:cc:cc` first (unvalidated, no capture behind it), then consider
  restoring the 75 threshold and raising only the `wildcard_probe_t2` path to 65.

## v1.1

Charge Mode reliability. Every item below is in `run_charge_mode()` or its boot
gate unless noted.

### Fixed

- **Charge screen could freeze for 30s and panic-reboot.** The wait-for-release
  loop on the exit path spun without feeding the task watchdog. On the ADV the
  key list is built from TCA8418 press/release *events* and never rescanned, so
  a lost release event latches a key down permanently and that loop never
  exited. It is now time-boxed to 3s and resets the WDT each pass.
- **Quick taps on the charge screen were ignored.** `update()` drains exactly
  one TCA8418 event per call, so a tap reads as pressed on one frame and
  released on the next — the old two-frame debounce discarded it and forced the
  user to hold a key. Any observed press now exits. The debounce was inherited
  from the original Cardputer's scanned matrix and does not apply to the ADV's
  hardware-debounced controller.
- **Nothing detected "not actually charging".** With no charge-status line and
  `isCharging()` always returning unknown, a dead cable or an unplug left the
  screen lit and draining the cell it was meant to fill, ending in a brownout
  that re-entered Charge Mode and repeated until the pack was deeply
  discharged. A stall watch now tracks the running peak; a 40mV fall sustained
  for 5 minutes shows a NOT CHARGING notice and powers the panel down, a 40mV
  recovery restores the readout, and reaching 3250mV enters deep sleep (~20uA)
  with keyboard wake armed. A deep-sleep wake is not a brownout reset, which is
  what breaks the drain/reboot cycle.
- **A failed battery ADC init caused an unbreakable boot loop.** A failed
  `adc_oneshot` init returns 0mV, which the boot gate read as an empty cell and
  gated into Charge Mode, which rebooted ~6s later, forever. An implausible
  reading is now treated as sensor failure: the boot gate skips the voltage
  criterion and Charge Mode resumes the app instead of rebooting.

### Changed

- The charge-loop health check no longer claims to detect I2C wedges. On the
  ADV the battery is read from ADC1/GPIO10 with no I2C involved, while the
  keyboard's TCA8418 is the only device on the bus — so the check could never
  see a keyboard failure. It is now scoped and documented as an ADC-frozen
  detector, with the implausible-read case split out and handled separately.
- The export page version badge derives from `VERSION_SHORT` instead of being
  hardcoded.

### Known issues

- A keyboard that fails to initialize at boot (`Adafruit_TCA8418::begin()`
  returning false on a marginal rail) is silently dead for the whole session —
  no ISR is attached, so no key ever registers. Not yet detected or retried.
- After a Charge Mode exit the CPU stays at 40MHz until radio init rather than
  returning to the 80MHz the boot path intends. Slower boot; not a safety
  issue, since a lower clock draws less peak current.
- The charge screen maps voltage through the discharge curve while charging, so
  the percentage reads optimistically high, and `CHARGE_MODE_FULL_MV` (4150mV)
  tops out at 95%.
- The +/-mV figure is cumulative since Charge Mode started, so it cannot show
  that charging has resumed after an earlier discharge, and it renders at 1mV
  precision despite a noise floor several times larger.

## v1.0-beta

Initial public beta.
