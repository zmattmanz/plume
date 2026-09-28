// ============================================================================
// PLUME v1.3
// ============================================================================

#include <M5Cardputer.h>

// ── Target guard ────────────────────────────────────────────────────────────
// This repo holds TWO firmwares for TWO different chips: this sketch (ESP32-S3,
// Cardputer / Cardputer ADV) and c5-sniffer/ (ESP32-C5). Building one with the
// other's board still selected in the IDE is an easy mistake, and the natural
// errors are misleading — you get "esp_sleep_enable_ext0_wakeup was not declared"
// (the C5 has no EXT0 wakeup) and "HardwareSerial has no member setTxTimeoutMs"
// (no USB-CDC-on-boot, so Serial isn't HWCDC), neither of which hints at the
// real cause. Fail loudly and specifically instead.
// MUST sit after the first include: the target macros come in via sdkconfig.h in
// that chain. Above it the guard compiles but can never fire.
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Wrong board selected. Plume targets the ESP32-S3 (M5Stack Cardputer / Cardputer ADV). If you were just building c5-sniffer, switch the IDE board back."
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <vector>
#include <algorithm>
#include <new>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_task_wdt.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "driver/gpio.h"
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void draw_header_lcd(int screen_num, const char* name_override = nullptr);
static void draw_overlay_header_lcd(const char* label);
static void render_frame();
void draw_toast_spr();
void draw_vol_overlay();
static void drawPill(int x, int y, const char* text, uint16_t accent_col,
                     float bg_accent_pct = 0.18f, bool filled = false);
void draw_current_screen();
void draw_capture_history_screen();
static void perform_detection_delete(int idx);
static void flush_pending_deletes();
void transition_screen(int new_screen, int dir);
void play_escalated_alarm(int confidence, int source);
void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b);
template<typename WriteFn> static bool littlefs_atomic_write(const char* target_path, WriteFn write_fn);
void beep(int frequency, int duration_ms);
void apply_color_palette();
void draw_help_overlay();
void handle_menu_select();
static void set_turbo_mode(bool on);

void save_stats_to_sd();
void draw_feed_expanded_overlay();
void draw_wifi_config_overlay();
static void save_wifi_credentials();
static void load_wifi_credentials();
void draw_gps_screen();
void load_sd_history();
void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type);
static void set_toast_direct(const char* text, uint16_t accent, bool is_info = true);
static void apply_ble_scan_params();
// ── ESP32-C5 5 GHz co-processor link (Grove/UART1) — defined in C5 LINK section ──
void c5_link_begin();
void c5_link_end();
void process_c5_serial();
bool c5_is_present();
struct WifiEvent;
struct IeParse;
static void parse_wifi_event(struct WifiEvent* ev, struct IeParse* ie);
static void update_channel_histogram();
static void draw_scanner_viz_scan(unsigned long frame_ms);
static void draw_scanner_viz_spectrum(unsigned long frame_ms);
static inline void compute_sincos(float angle, float* s, float* c);
void draw_signal_screen();
void draw_device_info_screen();

// ============================================================================
// DISPLAY & PALETTE VARIABLES (Swappable for Night Mode)
// =====================================================================================
#define DISP_W       240
#define DISP_H       135

uint16_t BG_COLOR, CARD_COLOR, CARD_BORDER;
uint16_t HEADER_COLOR, TEXT_COLOR, DIM_COLOR;
uint16_t TEAL_COLOR, PURPLE_COLOR;
uint16_t CAUTION_COLOR, GPS_COLOR;
uint16_t HATCH_COLOR;          // lerp(BG, CARD_BORDER, 0.80) — hatch fill lines
uint16_t GRID_LINE_DIM;        // lerp(BG, CARD_BORDER, 0.30) — faint grid/axis
uint16_t GRID_LINE_MED;        // lerp(BG, CARD_BORDER, 0.50) — medium grid lines
uint16_t SWEEP_LINE_COLOR;     // lerp(BG, HEADER, 0.60) — spectrum scan line
uint16_t HEADER_DIM_BLEND;     // lerp(BG, HEADER, 0.25) — dim header accent
uint16_t RING_COLOR;           // lerp(BG, HEADER, 0.25) — flatradar ring lines
uint16_t CENTER_DOT;           // lerp(BG, HEADER, 0.50) — flatradar center dot
uint16_t CENTER_DOT_BRIGHT;    // lerp(BG, HEADER, 0.80) — flatradar center highlight
uint16_t SCALE_LABEL_COLOR;    // lerp(BG, DIM, 0.80) — dBm scale tick labels
// ACCENT_COLOR is intentionally identical to HEADER_COLOR in all palette modes.
// Use HEADER_COLOR for UI chrome (headers, pills, outlines).
// Use ACCENT_COLOR for interactive highlights (selection bars, scrollbar thumbs).
// They are and will always be the same color — the distinction is semantic only.
#define ACCENT_COLOR HEADER_COLOR

// Toast severity tiers
#define TOAST_SUCCESS  HEADER_COLOR   // positive confirmations
#define TOAST_WARNING  CAUTION_COLOR  // errors, warnings, destructive actions
#define TOAST_NEUTRAL  DIM_COLOR      // informational, dismissed, hints
bool night_mode = false;
bool c5_enabled = true;   // 5 GHz C5 co-processor link (Grove/UART1); harmless with no C5 attached
bool show_help_overlay = false;
static unsigned long help_ease_start = 0;

// Ambient mode: dim minimal UI after sustained idle
static unsigned long last_user_input_ms = 0;
// 2 minutes — long enough to glance away and come back without re-waking,
// short enough that pocket-sitting doesn't burn unnecessary battery.
static const unsigned long AMBIENT_TIMEOUT_MS = 2UL * 60UL * 1000UL;
static const uint8_t AMBIENT_BRIGHTNESS = 40;

// ── Charge Mode ─────────────────────────────────────────────────────────────
// A minimal, radios-off holding state that (a) prevents the brown-out boot loop
// when the cell is too low to survive radio init, and (b) charges as fast as
// this fixed-charge-current hardware allows by cutting every avoidable load.
// Entered automatically at boot below CHARGE_MODE_ENTER_MV, or on demand via
// the 'c' key (which sets charge_mode_request in RTC RAM and reboots into it).
// Exits — resuming the normal app — once the cell HOLDS above CHARGE_MODE_EXIT_MV
// for a sustained window, or on any keypress. Thresholds are millivolts measured
// under charge-mode's own near-zero load; EXIT sits well above ENTER so the
// radios' load sag on resume can't immediately drop back under the brown-out
// floor and re-trigger the loop.
// ENTER is the *resting* (unloaded) floor below which we don't risk radio init.
// It can't predict a load-induced brownout on its own, so the boot gate ALSO
// enters on a brownout reset reason (the real loop-breaker). EXIT sits near the
// 3.72V level the full app was observed to boot+run at, with margin so the
// radios' load sag on resume can't drop straight back under the brownout floor.
// NOTE (multi-board): these three thresholds are single-LiPo voltages, so they
// are broadly valid on the regular Cardputer too. They are NOT sag-calibrated
// for its battery (120mAh internal + 1400mAh base) or its non-PMIC ADC read,
// and isCharging() returns "unknown" there (see get_filtered_voltage). If Charge
// Mode misbehaves on a regular Cardputer, re-tune here per plume_is_adv rather
// than in the loop logic — the algorithm is board-agnostic, only the numbers
// and the charge-status signal differ.
#define CHARGE_MODE_ENTER_MV  3550          // boot below this -> charge mode (anti-bootloop)
#define CHARGE_MODE_EXIT_MV   3750          // AUTO-entered: hold above this -> resume the app
// USER-requested charge (the 'c' key) isn't about brown-out safety — the user
// wants to top the cell up — so it must NOT resume at the 3.75V safe floor.
// It holds until the cell is essentially full or the user presses a key. On this
// trickle charger it may never reach full, which is fine: a keypress always exits.
#define CHARGE_MODE_FULL_MV   4150          // USER-requested: hold above this -> resume the app
// ── "Not actually charging" watch ───────────────────────────────────────────
// Nothing on this board reports charge state (isCharging() is always 2/unknown
// and there is no VBUS line), so a dead cable, a dead port, or an unplug while
// the charge screen is up is INVISIBLE to us: the loop would hold forever, lit,
// draining the cell it is supposed to be filling — then brown out, re-enter on
// the brownout reason, and repeat until the pack is deeply discharged.
// The only signal available is the cell itself falling. Compare against the
// RUNNING PEAK, not the entry voltage: dropping the app's load rebounds a tired
// cell +50..150mV over the first minutes, which would mask a real discharge
// measured from entry. 40mV is far outside the ±15mV ADC ripple, and 5 minutes
// under that bar rules out a transient.
// Tradeoff worth knowing: on the flat middle of the LiPo curve a 40mV fall at
// this load takes hours, so a stall is confirmed slowly up there. That is the
// harmless case (the pack is nearly full). Near the knee — the auto-entry case
// that actually endangers the cell — the curve is steep and it trips quickly.
#define CHARGE_STALL_DROP_MV    40          // below the running peak = losing charge
#define CHARGE_STALL_CONFIRM_MS 300000UL    // 5 min under that bar before believing it
#define CHARGE_STALL_RISE_MV    40          // rise above the stall low-water mark = charger is back
// Terminal protection floor. Below this, with no charger present, stop drawing
// power at all rather than run the pack down to damage.
#define CHARGE_MODE_DEAD_MV     3250
// Below this the reading is the battery ADC failing, not a flat cell: a
// connected single LiPo never reads this low (its protection IC cuts off around
// 2.5-3.0V), whereas a failed adc_oneshot init returns a flat 0. The two must
// be handled differently — see the ADC health block in run_charge_mode.
#define BATT_ADC_MIN_PLAUSIBLE_MV 100
#define CHARGE_MODE_MAGIC     0x50C0FFEEUL  // "enter charge mode on next boot" sentinel
// RTC_NOINIT, not RTC_DATA: an initialized RTC_DATA var gets reloaded from the
// app image on a normal reboot, so the request was being wiped by the very
// restart meant to carry it (charge mode came up only intermittently). NOINIT
// retains its value across esp_restart(); the boot gate additionally requires
// an ESP_RST_SW reset before honoring it, so power-on garbage can't false-fire.
RTC_NOINIT_ATTR uint32_t charge_mode_request;
static bool ambient_mode = false;
// Deeper idle tier below ambient: backlight fully off and the panel asleep.
// The backlight is the largest controllable load on this board (field note:
// ~duty 60 is break-even against the 60mA charger), and ambient still holds it
// at 40 indefinitely. Detection is unaffected — the WiFi callback, BLE worker
// and scanner all run as background tasks — and a detection raises a toast,
// which wakes the screen, so nothing is hidden.
static bool screen_off_mode = false;
static const unsigned long SCREEN_OFF_TIMEOUT_MS = 10UL * 60UL * 1000UL;

unsigned long vol_overlay_start = 0;
bool show_vol_overlay = false;
static bool show_feed_expanded = false;
static unsigned long feed_expand_ms = 0;
static volatile bool scanner_ready       = false;  // set true after boot; guards both callbacks
static int  feed_expanded_selected = 0;
// Scroll offset for the expanded feed — index of the row drawn at the top.
// Without this the renderer always started at the newest entry and drew at most
// FEED_EXPANDED_ROWS, so with FEED_SIZE 8 the oldest 2 entries were unreachable.
// Mirrors the detections screen's history_selected_idx/history_scroll_offset pair.
static int  feed_expanded_offset   = 0;
#define FEED_EXPANDED_ROWS 6   // visible rows; must match the renderer's max_rows
int  brightness_level = 3;  // 0=dimmest, 1=dim, 2=mid, 3=full — cycled by 'b' key

// ── WiFi Config overlay state ──
static bool wifi_config_open = false;
static int  wifi_config_field = 0;        // 0 = SSID, 1 = Password, 2 = Save, 3 = Clear
static bool wifi_config_editing = false;   // true = text input mode active
static bool wifi_config_show_pass = false; // 's' toggles plaintext password reveal
static char wifi_config_ssid_buf[33] = "";
static char wifi_config_pass_buf[65] = "";
static int  wifi_config_cursor = 0;        // cursor position in active field
static unsigned long wifi_config_open_ms = 0;

// ── Menu row table — single source of truth for BOTH layout and navigation ──
// type: 0 = section header, 1 = selectable item, 2 = gap.
// idx must match the case labels in handle_menu_select().
struct MRow { int type; int idx; const char* text; };
static const MRow MENU_ROWS[] = {
    {0, -1, "SCREENS"},
    {1,  0, "Scanner"},
    {1,  2, "Detections"},
    {1,  3, "GPS"},
    {1,  4, "Stats"},
    {2, -1, ""},
    {0, -1, "SETTINGS"},
    {1,  5, "Night Mode"},
    {1,  6, "Low Power"},
    {1,  7, "Mute Beeps"},
    {1,  8, "Turbo Mode"},
    {1, 12, "5GHz Radio"},
    {1, 13, "Charge Mode"},
    {2, -1, ""},
    {0, -1, "ACTIONS"},
    {1,  9, "WiFi Config"},
    {1, 10, "Export Mode"},
    {1, 11, "Clear All"},
};
static const int MENU_ROW_COUNT = sizeof(MENU_ROWS) / sizeof(MENU_ROWS[0]);

static_assert(MENU_ROW_COUNT == 18, "MENU_ROWS changed — update this guard and verify handle_menu_select() cases match");

static int menu_next_idx(int cur, int dir) {
    int pos = -1;
    for (int i = 0; i < MENU_ROW_COUNT; i++)
        if (MENU_ROWS[i].type == 1 && MENU_ROWS[i].idx == cur) { pos = i; break; }
    if (pos < 0) {
        for (int i = 0; i < MENU_ROW_COUNT; i++)
            if (MENU_ROWS[i].type == 1) return MENU_ROWS[i].idx;
        return cur;
    }
    for (int s = 1; s <= MENU_ROW_COUNT; s++) {
        int i = (((pos + dir * s) % MENU_ROW_COUNT) + MENU_ROW_COUNT) % MENU_ROW_COUNT;
        if (MENU_ROWS[i].type == 1) return MENU_ROWS[i].idx;
    }
    return cur;
}

// ── Menu state ──
static int  menu_selected = 0;  // bridged into handle_menu_select()

// ── Fullscreen menu state ──
static bool menu_open = false;
// Two-press guard on "CLEAR STATS" — the one irreversible menu action (it wipes
// session AND lifetime counters, then persists, so there is nothing to undo).
// Mirrors the hist_delete_confirming idiom used for single-capture deletion.
// Armed by the first ENTER, fired by the second. Disarmed by moving the
// selection, choosing any other item, or closing the menu, so a stale armed
// flag can never fire later by accident.
static bool stats_clear_confirming = false;
static unsigned long menu_open_ms = 0;
static int   menu_scroll_offset = 0;
static float menu_scroll_y_f    = 0.0f;
static unsigned long menu_last_frame_ms = 0;
static float menu_sel_y_f      = 0.0f;   // eased Y position of selection highlight
static bool  menu_sel_y_seeded = false;   // prevents first-frame pop

// Low-power mode: reduces scan cadence across WiFi/BLE for longer runtime
static bool low_power_mode = false;

// Is 5 GHz actually being scanned right now? Low power idles the C5's radio via
// P|1 while deliberately leaving c5_enabled and the link up — so the user's
// setting survives and coverage resumes the instant low power ends. But the menu
// toggle and the header 5G badge both read raw state and so claimed 5 GHz
// coverage that was not happening. This is the effective state; c5_enabled
// remains the user's preference and is never rewritten behind their back.
static inline bool c5_scanning_now() { return c5_enabled && !low_power_mode; }
static bool turbo_mode_active = false;
// Geometric spacing (~2x per step) reads as even perceptual jumps; 32 is the
// old dim tier (40) lowered 20%.
static const int BRIGHTNESS_LEVELS[4] = {32, 64, 128, 255};

// Effective backlight target. The Cardputer ADV has no software-controllable
// charger (M5Unified maps it to pmic_adc — setChargeCurrent() is a no-op), so
// the only way to stay charge-positive on USB is to cut load. The backlight is
// the single largest continuous draw, so low-power mode forces it down to the
// dim ambient level.
static inline uint8_t effective_brightness() {
    return low_power_mode ? AMBIENT_BRIGHTNESS : BRIGHTNESS_LEVELS[brightness_level];
}

// Low power exists to prioritise charging, so it should shed the backlight — the
// largest controllable load on this board — far sooner than normal use does.
// Waiting the normal 2 min / 10 min to dim and blank defeats the point.
static inline unsigned long current_ambient_timeout_ms() {
    return low_power_mode ? 30UL * 1000UL : AMBIENT_TIMEOUT_MS;
}
static inline unsigned long current_screen_off_timeout_ms() {
    return low_power_mode ? 2UL * 60UL * 1000UL : SCREEN_OFF_TIMEOUT_MS;
}

// RGB LED state — color cycles with C key, on/off with L when locator idle
static uint8_t led_r = 77, led_g = 219, led_b = 194;  // default teal (matches HEADER_COLOR)
static bool    led_breathing_on = true;
static const uint8_t LED_COLORS[][3] = {
    { 77, 219, 194},  // teal (matches HEADER_COLOR — default)
    { 80, 200, 255},  // cyan
    {  0, 200,   0},  // green
    {139, 124, 219},  // violet (matches PURPLE_COLOR)
    {255, 181,  71},  // amber (matches CAUTION_COLOR)
    {255, 255, 255},  // white
    {255,  80,   0},  // orange
    {255,  30,  30},  // red
};
static int led_col_idx = 0;

// Detection-flash state — overrides breathing color briefly on new detection
static unsigned long led_detection_flash_until = 0;
static uint8_t  led_detect_r = 0, led_detect_g = 0, led_detect_b = 0;
static bool     led_detect_active = false;

// Fixed-point color lerp — defined here so apply_color_palette can call it.
// t_256 is 0..256 (256 = exact tc, no off-by-one). Works in RGB565 component
// space directly; no lgfx::color565 overhead or float multiply.
static inline uint16_t lerp_col16_i(uint16_t fc, uint16_t tc, int t_256) {
    if (t_256 <= 0)   return fc;
    if (t_256 >= 256) return tc;
    int fr = (fc >> 11) & 0x1F, fg = (fc >> 5) & 0x3F, fb = fc & 0x1F;
    int tr = (tc >> 11) & 0x1F, tg = (tc >> 5) & 0x3F, tb = tc & 0x1F;
    int rr = fr + (((tr - fr) * t_256) >> 8);
    int rg = fg + (((tg - fg) * t_256) >> 8);
    int rb = fb + (((tb - fb) * t_256) >> 8);
    return (uint16_t)((rr << 11) | (rg << 5) | rb);
}
// Float wrapper — one float-to-int conversion replaces three float multiplies.
static inline uint16_t lerp_col16(uint16_t fc, uint16_t tc, float t) {
    return lerp_col16_i(fc, tc, (int)(t * 256.0f + 0.5f));
}

void apply_color_palette() {
    if (night_mode) {
        // Night: red chrome, lifted dim for readability, amber caution.
        BG_COLOR      = lgfx::color565( 10,   0,   0);   // #0A0000
        CARD_COLOR    = lgfx::color565( 58,  10,  10);   // #3A0A0A
        CARD_BORDER   = lgfx::color565( 90,  20,  20);   // #5A1414
        HEADER_COLOR  = lgfx::color565(255,  90,  90);   // #FF5A5A
        TEXT_COLOR    = lgfx::color565(255, 208, 208);   // #FFD0D0
        DIM_COLOR     = lgfx::color565(180,  90,  90);   // #B45A5A lifted red-dim
        CAUTION_COLOR = lgfx::color565(255, 181,  71);   // #FFB547 amber
        TEAL_COLOR    = lgfx::color565(255,  90,  90);   // = HEADER (collapsed in night)
        PURPLE_COLOR  = lgfx::color565(255, 150, 150);   // #FF9696 rose — BLE distinguishable
        GPS_COLOR     = lgfx::color565(255,  90,  90);   // = HEADER (collapsed)
        led_r = 255; led_g = 90; led_b = 90;              // sync LED to night chrome
    } else {
        // Option C: Analogous Cool — teal + blue-violet + amber.
        BG_COLOR      = lgfx::color565(  5,  10,  20);   // #050A14
        CARD_COLOR    = lgfx::color565( 29,  50,  88);   // #1D3258
        CARD_BORDER   = lgfx::color565( 46,  70, 112);   // #2E4670
        HEADER_COLOR  = lgfx::color565( 77, 219, 194);   // #4DDBC2 teal
        TEXT_COLOR    = lgfx::color565(232, 239, 255);   // #E8EFFF
        DIM_COLOR     = lgfx::color565(149, 165, 184);   // #95A5B8
        TEAL_COLOR    = lgfx::color565( 77, 219, 194);   // = HEADER (alias)
        PURPLE_COLOR  = lgfx::color565(139, 124, 219);   // #8B7CDB blue-violet
        CAUTION_COLOR = lgfx::color565(255, 181,  71);   // #FFB547 amber
        GPS_COLOR     = lgfx::color565( 77, 219, 194);   // = HEADER (unified)
        led_r = LED_COLORS[led_col_idx][0];               // restore user LED color
        led_g = LED_COLORS[led_col_idx][1];
        led_b = LED_COLORS[led_col_idx][2];
    }
    HATCH_COLOR       = lerp_col16(BG_COLOR, CARD_BORDER,  0.80f);
    GRID_LINE_DIM     = lerp_col16(BG_COLOR, CARD_BORDER,  0.30f);
    GRID_LINE_MED     = lerp_col16(BG_COLOR, CARD_BORDER,  0.50f);
    SWEEP_LINE_COLOR  = lerp_col16(BG_COLOR, HEADER_COLOR, 0.60f);
    HEADER_DIM_BLEND  = lerp_col16(BG_COLOR, HEADER_COLOR, 0.25f);
    RING_COLOR        = lerp_col16(BG_COLOR, HEADER_COLOR, 0.25f);
    CENTER_DOT        = lerp_col16(BG_COLOR, HEADER_COLOR, 0.50f);
    CENTER_DOT_BRIGHT = lerp_col16(BG_COLOR, HEADER_COLOR, 0.80f);
    SCALE_LABEL_COLOR = lerp_col16(BG_COLOR, DIM_COLOR,    0.80f);
}

// ── Module-level rendering helpers ──────────────────────────────────────────
// kprint: print text with +1 px inter-character spacing (kerning) at textSize=1
// Pass cx/cy from the current sprite cursor position before calling.
static void kprint(M5Canvas& s, const char* text, int extra = 1) {
    int cx = s.getCursorX(), cy = s.getCursorY();
    for (const char* p = text; *p; p++) {
        char ch[2] = {*p, '\0'};
        s.setCursor(cx, cy);
        s.print(ch);
        cx += 6 + extra;
    }
}

static inline void safe_copy(char* dest, const char* src, size_t dest_size) {
    if (dest_size == 0) return;
    strlcpy(dest, src, dest_size);
}

// A CSV field starting with = + - or @ executes as a formula when the log is
// opened in Excel, LibreOffice, or Google Sheets — and opening these logs in a
// spreadsheet is the documented workflow. An attacker controls their own AP's
// SSID completely, so prefix an apostrophe, which every major spreadsheet
// treats as "the rest of this cell is literal text".
//
// CSV-write concern only. Deliberately NOT folded into
// clean_device_name_char(), which also feeds the on-screen feed and the C5
// link — a name should render as itself there, not with a stray apostrophe.
static void csv_defuse_formula(char* s, size_t cap) {
    if (!s || !s[0] || cap < 3) return;
    char c0 = s[0];
    if (c0 != '=' && c0 != '+' && c0 != '-' && c0 != '@') return;
    size_t len = strlen(s);
    if (len + 2 > cap) len = cap - 2;   // truncate to make room for the quote + NUL
    memmove(s + 1, s, len);
    s[0] = '\'';
    s[len + 1] = '\0';
}

// ── Unified UI animation vocabulary ──
// One easing curve, three duration tiers, one slide distance.
// Reach for these instead of inventing new constants.
//
// Tier guide:
//   UI_ANIM_QUICK  — micro-feedback: digit rolls, key acks, color blips
//   UI_ANIM_NORMAL — standard transitions: slides, fades, intros
//   UI_ANIM_SLOW   — hero moments: gates, screen-level reveals
//
// All UI animations should call ui_ease() or ui_progress(); raw
// `1.0f - (1.0f - t) * (1.0f - t)` should not appear elsewhere in the file.

static const unsigned long UI_ANIM_QUICK  = 180;
static const unsigned long UI_ANIM_NORMAL = 320;
static const unsigned long UI_ANIM_SLOW   = 500;
static const int           UI_SLIDE_PX    = 14;

// ── Subsystem-specific timing constants ──
// Pulled out of their use sites so the timing model lives in one place.
// Tune here, not at the call sites.

// Stats screen: PACKETS card refresh cadence. ambient_packet_count churns
// every frame; this throttles how often the displayed value updates so the
// roll-up animation reads cleanly instead of strobing.
static const unsigned long STATS_PKT_REFRESH_MS = 3000;

// WiFi mode-transition delays. The ESP-IDF WiFi driver needs settle time
// between mode changes (STA <-> off, disconnect <-> reconnect, promiscuous
// on/off). These values are empirical — shortening risks the next call
// returning before the radio state actually transitioned.
static const unsigned long WIFI_MODE_SETTLE_SHORT_MS  = 50;
static const unsigned long WIFI_MODE_SETTLE_MEDIUM_MS = 100;
static const unsigned long WIFI_MODE_SETTLE_LONG_MS   = 200;

// Live activity feed cadence. FEED_MIN_PUSH_INTERVAL_MS gates normal scanner
// view; when the feed is expanded (full-screen) it pushes faster so the user
// sees more activity at a glance.
static const unsigned long FEED_PUSH_INTERVAL_EXPANDED_MS = 667;

// Feed entry aging. Rows fully visible for the first FEED_AGE_FULL_MS, then
// fade linearly until FEED_AGE_GONE_MS, after which they're skipped entirely.
static const unsigned long FEED_AGE_FULL_MS = 30000;
static const unsigned long FEED_AGE_GONE_MS = 90000;

// Overlay/toast fade tiers — names alias the purpose so call sites
// can self-document without inventing new durations.
static const unsigned long UI_FADE_IN_MS  = 150;  // overlays/toasts appearing
static const unsigned long UI_FADE_OUT_MS = 200;  // overlays/toasts disappearing

// Pulse period vocabulary — three tiers for sin-based oscillators.
// Slow: calm/idle. Medium: attention/status. Fast: active/urgent.
static const unsigned long UI_PULSE_BREATHE = 1200;
static const unsigned long UI_PULSE_MEDIUM  = 600;
static const unsigned long UI_PULSE_FAST    = 300;

// ── Type scale — 3 active tiers ──
// All UI text uses one of these three sizes. No other values
// should appear in setTextSize() calls (boot screen excepted —
// it renders directly to lcd, not the sprite).
//
//   TS_MICRO  — labels, footnotes, pills, field captions,
//               footer hints, channel numbers, timestamps,
//               scrollbar labels, RSSI dBm values
//   TS_BODY   — primary UI text: kprint, feed row names,
//               header screen names, status badge text,
//               section titles, overlay subtitles
//   TS_STRONG — hero inline values: stat card numbers,
//               menu item labels, locator target name,
//               locator dist/signal, detection detail names
static const float TS_MICRO  = 1.0f;
static const float TS_BODY   = 1.2f;
static const float TS_STRONG = 1.6f;

// Char width in pixels for a given type-scale tier.
// Base font is 6px wide; textSize multiplies it.
// Replaces all hardcoded 6/7/9 magic numbers in layout math.
static inline int ts_char_w(float size) {
    return (int)(size * 6.0f);
}

// ── Spacing — 4-step scale ──
//   UI_PAD_XS — hairline gaps, pill vertical inset
//   UI_PAD_SM — card inner padding, icon-to-text gap
//   UI_PAD_MD — card-to-card gap, section breaks
//   UI_PAD_LG — header strip height, menu row pitch
static const int UI_PAD_XS  = 2;
static const int UI_PAD_SM  = 6;
static const int UI_PAD_MD  = 12;
static const int UI_PAD_LG  = 18;

// Content area starts at this Y on every screen. Header = 0..CONTENT_Y-1.
static const int CONTENT_Y  = 20;
static const int SPR_H      = DISP_H - CONTENT_Y;  // 115 — content-only sprite height
// Left text margin — aligns header + viz titles + pills, and (via TL) the whole
// target screen. Raised 4 -> 8: at 4px the text visually touched the bezel.
// Also widens the right margin wherever a right-aligned element is placed at
// DISP_W - TEXT_LEFT (e.g. the target screen's status badge).
static const int TEXT_LEFT   = 8;

// ui_ease — the single curve we use for every UI animation
// (ease_out_quad). Decelerating motion: fast start, soft landing.
// Reads as "natural" UI motion.
static inline float ui_ease(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t);
}

// Returns eased 0..1 progress. Pass start_ms=0 to mean "settled / not animating".
static inline float ui_progress(unsigned long start_ms, unsigned long duration_ms) {
    if (start_ms == 0) return 1.0f;
    unsigned long now = millis();
    if (now <= start_ms) return 0.0f;
    unsigned long elapsed = now - start_ms;
    if (elapsed >= duration_ms) return 1.0f;
    return ui_ease((float)elapsed / (float)duration_ms);
}

// ── Animation primitives ───────────────────────────────────────────
// Built on ui_ease/ui_progress. New animation code should reach for
// these instead of hand-rolling the math at every site.
//
// For smoothed values that persist across screen visits, use
// anim_filter_seed() to avoid the first-frame pop from an unseeded
// initial value. Pass a static bool alongside the static float.

// Returns the current y for an element animating from
// (target_y + slide_distance) to target_y over duration_ms with the
// standard UI ease curve. Pass start_ms=0 for "settled" — returns
// target_y immediately. Once duration elapses, returns target_y exactly.
// Positive slide_distance means the element starts BELOW target; pass
// a negative distance to slide in from above.
static inline int anim_slide_in(int target_y, int slide_distance,
                                unsigned long start_ms, unsigned long duration_ms) {
    if (start_ms == 0) return target_y;
    float t = ui_progress(start_ms, duration_ms);
    if (t >= 1.0f) return target_y;
    return target_y + (int)((1.0f - t) * (float)slide_distance);
}

// Returns a value in [0.0, 1.0] oscillating sinusoidally with the given
// period. 0.5 is the resting midpoint. phase offsets the cycle (0.25
// starts at the peak, 0.75 at the trough). period_ms == 0 returns 0.5.
static inline float anim_pulse(unsigned long period_ms, float phase = 0.0f) {
    if (period_ms == 0) return 0.5f;
    float t = (float)(millis() % period_ms) / (float)period_ms + phase;
    return 0.5f + 0.5f * sinf(t * 2.0f * (float)M_PI);
}

// Frame-rate-independent exponential smoothing. time_constant_ms is
// the time to cover ~63% of the remaining gap; behavior is identical
// at any FPS. Common tiers: 80–120ms snappy, 200–400ms natural,
// 800ms+ slow drift.
static inline float anim_filter(float state, float target,
                                float time_constant_ms, float dt_ms) {
    if (time_constant_ms <= 0.0f) return target;
    float alpha = 1.0f - expf(-dt_ms / time_constant_ms);
    return state + alpha * (target - state);
}

// Self-seeding variant — snaps to target on the first call (when
// *initialized is false), then eases normally on subsequent calls.
// Eliminates the first-frame pop without requiring manual pre-seeding
// at every call site.
static inline float anim_filter_seed(float state, float target,
                                     float time_constant_ms, float dt_ms,
                                     bool* initialized) {
    if (!*initialized) { *initialized = true; return target; }
    return anim_filter(state, target, time_constant_ms, dt_ms);
}

// Writes 0..max_dots ASCII dots into out_buf, cycling at period_ms per
// dot. Buffer must be at least max_dots + 1 bytes. period_ms default
// 500 matches the existing "scanning..." pattern.
static inline void anim_ellipsis(char* out_buf, size_t out_len,
                                 unsigned long period_ms = 500,
                                 int max_dots = 3) {
    if (out_len == 0) return;
    int n = (int)(millis() / period_ms) % (max_dots + 1);
    int written = 0;
    for (int i = 0; i < n && written + 1 < (int)out_len; i++) {
        out_buf[written++] = '.';
    }
    out_buf[written] = '\0';
}

#define GPS_RX_PIN   15
#define GPS_TX_PIN   13
#define GPS_BAUD     9600     // AT6668/ATGM336H (Cap LoRa-1262) default; tried first by auto-detect

// Board identity, resolved once in setup() from M5Unified's auto-detection
// (M5GFX distinguishes board_M5Cardputer from board_M5CardputerADV at boot).
// ONE binary runs on both boards; the ADV-only hardware is gated on this flag.
// The GNSS receiver is the ADV's built-in module on UART2 (GPIO15/13) with a
// hardwired-on rail — the regular Cardputer has no GPS, so every routine that
// touches SerialGPS must no-op there (starting a UART on those pins would poke
// the regular board's expansion header). Defaults to true so any pre-detection
// use is harmless on the ADV; setup() overwrites it before GPS is touched.
static bool plume_is_adv = true;

#define MAX_CHANNEL 13
#define BLE_SCAN_DURATION 2
// BLE TX power. Detector is RX-dominated; TX only matters for active-scan
// SCAN_REQ. P3 (+3dBm) trims the peak draw with negligible detection loss.
// Bump to ESP_PWR_LVL_P6 if active-scan range matters in the field.
#define BLE_TX_POWER ESP_PWR_LVL_P3
// BLE_SCAN_INTERVAL removed — sessions run back-to-back (A1 timing)
// Brief global cooldown — prevents two alarms firing in the same audio
// envelope when multiple detections from different devices arrive in the
// same scanner tick. The 5-minute seen-MAC dedup is the real repeat
// suppression; this is just envelope spacing.
#define BUZZER_COOLDOWN 1500
#define IGNORE_WEAK_RSSI -80

#define MAX_LOG_BUFFER 4
#define MAX_PCAP_BUFFER 3
#define SD_FLUSH_INTERVAL 10000
#define CHANNEL_HOP_INTERVAL_NORMAL 250UL
#define CHANNEL_HOP_INTERVAL_TURBO  150UL
#define DEDUP_WINDOW_NORMAL_MS      300000UL
#define DEDUP_WINDOW_TURBO_MS        30000UL

static inline unsigned long current_channel_hop_interval() {
    return turbo_mode_active ? CHANNEL_HOP_INTERVAL_TURBO
           : (low_power_mode ? 800UL : CHANNEL_HOP_INTERVAL_NORMAL);
}
static inline unsigned long current_dedup_window_ms() {
    return turbo_mode_active ? DEDUP_WINDOW_TURBO_MS : DEDUP_WINDOW_NORMAL_MS;
}

#define RSSI_TRACK_MAX_DEVICES 10
#define RSSI_TRACK_SAMPLES 5
#define RSSI_TRACK_EXPIRY_MS 15000

#define PERSIST_INTERVAL_MS 60000
#define FLASH_FAIL_TOAST_THRESHOLD 3
#define PERSIST_FILE  "/flock_session.dat"
#define DETECT_FILE   "/flock_detections.txt"
#define TOAST_DURATION_MS 3500
#define DATA_MUTEX_TIMEOUT_MS 500


#define SCORE_DEFINITIVE 100  
#define SCORE_STRONG     60   
#define SCORE_WEAK       25   
#define SCORE_BONUS_RSSI 10
#define CONF_BONUS_TX_POWER  8    // corroborator only; below alarm threshold alone
#define TX_POWER_MIN_DBM     0    // dBm floor; phones often omit or use negative values

// Wildcard-probe behavioral tracker constants
#define WILDCARD_MIN_CHANNELS   3      // Distinct channels to confirm the cross-channel hopping signature
#define WILDCARD_WINDOW_MS      30000  // Observation window (ms) — reset per MAC on expiry
#define WILDCARD_TRACKER_SIZE   32     // Max simultaneously-tracked source MACs (fixed-size, no alloc)

// 70, lowered from 75, so a Tier-2 OUI emitting a wildcard probe at close range
// (SCORE_STRONG 60 + SCORE_BONUS_RSSI 10) reaches the alarm. Previously the 45
// Tier-2 prefixes could log but never beep, since 70 was the ceiling for
// Tier-2-only evidence.
//
// Deliberate side effects, both requiring RSSI > -50 (i.e. very close):
//   * a Tier-1 OUI now alarms on its own (60 + 10)
//   * an addr1_t1 sleeping-device hit now alarms on its own (60 + 10)
// This also moves the "MEDIUM" label boundary in confidence_label(), so scores
// of 70-74 now read MEDIUM rather than LOW.
#define CONFIDENCE_ALARM_THRESHOLD 70   
#define CONFIDENCE_HIGH            85   
#define CONFIDENCE_CERTAIN         100  

#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_CS_PIN       12

// Version strings — update BOTH when bumping. Everything in the sketch that
// shows a version derives from these (stats card, boot screen, export page
// badge and footer), so no other source line needs touching.
// Also update: CHANGELOG.md, README.md footer, and the file header above.
#define VERSION_STRING "PLUME v1.3"
#define VERSION_SHORT  "v1.3"

// Set to 1 to enable the 'x' key simulation trigger (development only).
// MUST be 0 for release builds — simulation creates permanent fake
// entries in the SD log and fires real detection alarms.
#define DEBUG_KEYS 0

// Per-second battery trace. Off by default: it costs an extra ADC read plus an
// isCharging() call every second purely to print a line.
#define DEBUG_BAT_LOG 0

// Arrow key characters — ADV Cardputer 4-key diamond layout:
// ';' = up, '.' = down, ',' = left, '/' = right
#define IS_KEY_UP(c)    ((c) == ';')
#define IS_KEY_DOWN(c)  ((c) == '.')
#define IS_KEY_LEFT(c)  ((c) == ',')
#define IS_KEY_RIGHT(c) ((c) == '/')

// Pre-configure WiFi credentials for export mode. User edits these in source
// once, then they're saved to flash on first boot. To change later, edit
// source and re-flash. This is a hobby/field-tool compromise.
#define EXPORT_WIFI_SSID ""
#define EXPORT_WIFI_PASS ""

// Screen count used by draw_header_lcd() and transition_screen().
#define NUM_SCREENS 5

// Screen 1 (TARGET) is deliberately OUT of the carousel. It is only meaningful
// once something is selected to track, so cycling onto it just showed an empty
// screen. It is now reached from the feed ([f] then [t], which jumps straight
// there) or from the menu — never by cycling. It keeps index 1 so screen_names,
// the menu's nav indices and any persisted current_screen all stay valid; only
// the rotation skips it.
#define SCREEN_TARGET 1
static inline bool screen_in_carousel(int s) { return s != SCREEN_TARGET; }
// Next/previous carousel screen, skipping any excluded index. The loop bound
// guarantees termination even if every screen were somehow excluded.
static inline int screen_step(int from, int dir) {
    int n = from;
    for (int i = 0; i < NUM_SCREENS; i++) {
        n = (n + dir + NUM_SCREENS) % NUM_SCREENS;
        if (screen_in_carousel(n)) return n;
    }
    return from;
}

// ============================================================================
// GLOBALS & STRUCTS
// ============================================================================
M5Canvas spr(&M5Cardputer.Display);
SPIClass sdSPI(FSPI);  // FSPI (SPI2_HOST) — matches the bmorcelli Launcher reference;
                       // M5GFX talks to the display via the ESP-IDF SPI driver directly, so
                       // sharing FSPI with a separate Arduino SPIClass for SD is safe.

TaskHandle_t ScannerTaskHandle;
TaskHandle_t GPSTaskHandle;
static TaskHandle_t BLEWorkerHandle = nullptr;
SemaphoreHandle_t dataMutex;
SemaphoreHandle_t sdMutex;    // guards all SD card I/O — FAT is not thread-safe

static inline bool take_data_mutex(TickType_t timeout_ticks = pdMS_TO_TICKS(DATA_MUTEX_TIMEOUT_MS)) {
    if (xSemaphoreTakeRecursive(dataMutex, timeout_ticks) == pdTRUE) return true;
    Serial.println("[MUTEX] dataMutex timeout -- skipping operation");
    return false;
}
static inline void give_data_mutex() {
    xSemaphoreGiveRecursive(dataMutex);
}

static uint8_t current_channel = 1;
static unsigned long last_channel_hop = 0;
static volatile unsigned long channel_lock_until = 0;
static uint8_t* scan_angle_lut       = nullptr;
static bool     scan_angle_lut_ready = false;
// Periodic BLE stack health restart — see loop().
static unsigned long last_ble_restart_ms = 0;
static const unsigned long BLE_RESTART_INTERVAL_MS = 1800000UL;  // 30 minutes
static unsigned long last_buzzer_time = 0;
static NimBLEScan* pBLEScan;
static uint32_t ble_scan_cycle = 0;
static volatile uint32_t ambient_packet_count = 0;
QueueHandle_t ble_event_queue;
bool sd_available = false;

// SD hot-plug state — poll every 5s to detect inserted/removed cards
static unsigned long last_sd_check_ms = 0;
static const unsigned long SD_CHECK_INTERVAL_MS = 5000;
static bool sd_was_available = false;

// Defined here rather than beside the other current_*() helpers because both
// constants they read (PERSIST_INTERVAL_MS, SD_CHECK_INTERVAL_MS) are declared
// further down the file than those are.
//
// Hot-plug polling is asymmetric, so the interval is too:
//
//   card PRESENT — the check is `SD.cardType() == CARD_NONE`, which reads a
//     cached driver field. No SPI command, no file I/O. Effectively free, so
//     poll at the normal rate and keep removal detection prompt.
//
//   card ABSENT  — every pass runs a full SD.begin(), which its own comment
//     notes "can take several hundred ms" (hence the WDT reset around it). At a
//     flat 5s that is a permanent tax on anyone running without a card.
//
// So back off on consecutive failures instead of slowing everything down: fast
// while you might just have inserted one, then settling out once it is clear
// there is no card coming. Any successful mount resets the streak, which also
// means a card you pull and re-insert is picked up quickly again.
static uint8_t sd_mount_fail_streak = 0;
static inline unsigned long current_sd_check_interval_ms() {
    if (sd_available) return SD_CHECK_INTERVAL_MS;          // ~free, stay prompt
    if (sd_mount_fail_streak < 3)  return  5UL * 1000UL;    // just inserted / booted
    if (sd_mount_fail_streak < 6)  return 15UL * 1000UL;
    return 60UL * 1000UL;                                   // settled: no card
}
// Each persist is a LittleFS + SD write burst (~200-800ms by its own comment).
// Stretching it in low power saves that energy and the flash wear with it.
static inline unsigned long current_persist_interval_ms() {
    return low_power_mode ? 300UL * 1000UL : PERSIST_INTERVAL_MS;
}
bool littlefs_available = false;
volatile int trigger_alarm_confidence = 0;
volatile int trigger_alarm_source = 0;   // 0 = WiFi, 1 = BLE
volatile bool is_alarming = false;

#define SD_LINE_LEN 384
char sd_write_buffer[MAX_LOG_BUFFER][SD_LINE_LEN];
int  sd_write_count = 0;
unsigned long last_sd_flush = 0;
static uint32_t sd_log_dropped = 0;
static uint32_t sd_wifi_pcap_dropped = 0;
static uint32_t sd_ble_pcap_dropped = 0;
static int flash_write_fail_count = 0; 

static const char* current_log_file = "/PLUME/logs/PlumeLog.csv";
static const char* current_pcap_file = "/PLUME/captures/Threats.pcap";
static const char* current_ble_pcap_file = "/PLUME/captures/BLE_Threats.pcap";

// Export server state
static WebServer* export_server = nullptr;
static bool export_mode_active = false;
static unsigned long export_mode_started_at = 0;
static const unsigned long EXPORT_MODE_MAX_MS = 600000UL;  // 10 min auto-exit

// Non-blocking WiFi-connect state machine for export mode.
// While export_connecting is true, loop() polls WiFi.status() instead of
// blocking the main thread so keyboard/feed/alarm handling stay live.
static bool export_connecting = false;
static unsigned long export_connect_start_ms = 0;
static const unsigned long EXPORT_CONNECT_TIMEOUT_MS = 5000UL;
static char export_ssid[33] = "";  // configured WiFi SSID (persisted)
static char export_pass[65] = "";  // configured WiFi password (persisted)
static char export_ip_str[20] = "0.0.0.0";
static char export_auth_pass[12] = "";
static bool export_grid_needs_reset = false;
static const char* export_auth_user = "plume";

// Derive a 4-char hex password from the device's eFuse MAC.
// Deterministic — same device always produces the same password.
// Called once before the server starts.
static void export_derive_password() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    // FNV-1a hash of the 6 MAC bytes → 32-bit → truncate to 16-bit → 4 hex chars
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    snprintf(export_auth_pass, sizeof(export_auth_pass), "%08lX", (unsigned long)h);
}

int current_screen = 0;
bool system_fully_booted = false;
static bool screen_dirty = true;   // Forces redraw; set by any state change
bool is_muted = false;
int current_volume = 150;

long session_flock_wifi = 0;
long session_flock_ble = 0;
long session_raven = 0;
long session_axon = 0;

unsigned long session_start_time = 0;
unsigned long lifetime_seconds = 0;

long lifetime_wifi = 0;
long lifetime_ble = 0;
long lifetime_flock_total = 0;
long lifetime_boots = 0;
long lifetime_flash_writes = 0;

#define MAX_SEEN_MACS 32

#define MAX_WHITELIST 16
static char mac_whitelist[MAX_WHITELIST][18];
static int  mac_whitelist_count = 0;
static const char WHITELIST_FILE[] = "/wl.txt";

// ts first on purpose. With char mac[18] leading, ts needs 2 bytes of padding to
// reach 4-byte alignment and the struct tail needs 3 more — 28 bytes. Putting
// the aligned member first drops that to 24. At 64 instances (seen_mac_table
// plus the static temp[] in seen_mac_expire) that is 256 bytes of DRAM.
struct SeenMacEntry {
    unsigned long ts;
    char   mac[18];
    bool   occupied;
};
static SeenMacEntry seen_mac_table[MAX_SEEN_MACS];

static uint32_t hash_mac(const char* mac) {
    uint32_t h = 2166136261u;
    for (const char* p = mac; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

bool is_mac_recently_seen(const char* mac) {
    uint32_t start = hash_mac(mac) % MAX_SEEN_MACS;
    unsigned long now = millis();
    for (uint32_t i = 0; i < MAX_SEEN_MACS; i++) {
        uint32_t idx = (start + i) % MAX_SEEN_MACS;
        SeenMacEntry& e = seen_mac_table[idx];
        if (!e.occupied) return false;
        if (strncmp(e.mac, mac, 17) == 0) {
            if ((now - e.ts) >= current_dedup_window_ms()) { e.ts = now; return false; }
            return true;
        }
    }
    return false;
}

static bool is_mac_whitelisted(const char* mac) {
    for (int i = 0; i < mac_whitelist_count; i++) {
        if (strcasecmp(mac_whitelist[i], mac) == 0) return true;
    }
    return false;
}

static bool whitelist_add(const char* mac) {
    if (!mac || strlen(mac) == 0) return false;
    if (is_mac_whitelisted(mac)) return false;
    if (mac_whitelist_count >= MAX_WHITELIST) return false;
    strncpy(mac_whitelist[mac_whitelist_count], mac, 17);
    mac_whitelist[mac_whitelist_count][17] = '\0';
    mac_whitelist_count++;
    return true;
}

// Atomic write: write content via the callback to a temp file, then rename
// over the target. Caller's lambda gets the open File and returns true on
// success. If the lambda returns false, or any FS step fails, the previous
// good file is left intact.
//
// Returns true if the target file now contains the new content.
template<typename WriteFn>
static bool littlefs_atomic_write(const char* target_path, WriteFn write_fn) {
    if (!littlefs_available) return false;

    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target_path);

    if (LittleFS.exists(tmp_path)) LittleFS.remove(tmp_path);

    File f = LittleFS.open(tmp_path, "w");
    if (!f) {
        Serial.printf("[FS] atomic_write: open '%s' failed\n", tmp_path);
        return false;
    }

    bool ok = write_fn(f);
    f.close();

    if (!ok) {
        Serial.printf("[FS] atomic_write: writer returned false for '%s'\n", target_path);
        LittleFS.remove(tmp_path);
        return false;
    }

    if (!LittleFS.rename(tmp_path, target_path)) {
        Serial.printf("[FS] atomic_write: rename '%s' -> '%s' failed\n",
                      tmp_path, target_path);
        LittleFS.remove(tmp_path);
        return false;
    }
    return true;
}

static void save_whitelist() {
    if (!littlefs_available) return;
    littlefs_atomic_write(WHITELIST_FILE, [&](File& f) -> bool {
        for (int i = 0; i < mac_whitelist_count; i++) {
            if (f.println(mac_whitelist[i]) <= 0) return false;
        }
        return true;
    });
}

static void load_whitelist() {
    if (!littlefs_available) return;
    if (!LittleFS.exists(WHITELIST_FILE)) return;
    File f = LittleFS.open(WHITELIST_FILE, "r");
    if (!f) return;
    mac_whitelist_count = 0;
    while (f.available() && mac_whitelist_count < MAX_WHITELIST) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 17) {
            strncpy(mac_whitelist[mac_whitelist_count], line.c_str(), 17);
            mac_whitelist[mac_whitelist_count][17] = '\0';
            mac_whitelist_count++;
        }
    }
    f.close();
}

void add_seen_mac(const char* mac) {
    uint32_t start = hash_mac(mac) % MAX_SEEN_MACS;
    unsigned long now = millis();

    for (uint32_t i = 0; i < MAX_SEEN_MACS; i++) {
        uint32_t idx = (start + i) % MAX_SEEN_MACS;
        SeenMacEntry& e = seen_mac_table[idx];
        if (!e.occupied) {
            strncpy(e.mac, mac, 17);
            e.mac[17] = '\0';
            e.ts       = now;
            e.occupied = true;
            return;
        }
        if (strncmp(e.mac, mac, 17) == 0) {
            e.ts = now;
            return;
        }
    }

    // Table full — evict the entry with the oldest timestamp.
    uint32_t oldest_idx = 0;
    unsigned long oldest_ts = seen_mac_table[0].ts;
    for (uint32_t i = 1; i < MAX_SEEN_MACS; i++) {
        if (seen_mac_table[i].ts < oldest_ts) {
            oldest_ts  = seen_mac_table[i].ts;
            oldest_idx = i;
        }
    }
    SeenMacEntry& victim = seen_mac_table[oldest_idx];
    strncpy(victim.mac, mac, 17);
    victim.mac[17] = '\0';
    victim.ts       = now;
}

// Expires old entries and rehashes to repair probe chains broken by removals.
// Called from loop() under dataMutex once per second.
void seen_mac_expire() {
    unsigned long now = millis();
    bool any_expired = false;

    for (uint32_t i = 0; i < MAX_SEEN_MACS; i++) {
        if (seen_mac_table[i].occupied &&
            (now - seen_mac_table[i].ts) >= current_dedup_window_ms()) {
            seen_mac_table[i].occupied = false;
            any_expired = true;
        }
    }

    if (!any_expired) return;

    // Two-pass rehash: collect all live entries, wipe the table, re-insert
    // from scratch. Avoids the Robin Hood forward-scan issue where an entry
    // re-inserted at a lower index than the current cursor gets visited twice
    // or forms a probe-chain cycle with a hash collision.
    // Static to avoid 6 KB on the stack (24 B × 256 entries).
    static SeenMacEntry temp[MAX_SEEN_MACS];
    int temp_count = 0;
    for (uint32_t i = 0; i < MAX_SEEN_MACS; i++) {
        if (seen_mac_table[i].occupied) {
            temp[temp_count++] = seen_mac_table[i];
            seen_mac_table[i].occupied = false;
        }
    }
    for (int t = 0; t < temp_count; t++) {
        uint32_t start = hash_mac(temp[t].mac) % MAX_SEEN_MACS;
        for (uint32_t j = 0; j < MAX_SEEN_MACS; j++) {
            uint32_t idx = (start + j) % MAX_SEEN_MACS;
            if (!seen_mac_table[idx].occupied) {
                seen_mac_table[idx] = temp[t];
                break;
            }
        }
    }
}

char last_cap_type[16]       = "None";
char last_cap_mac[18]        = "--:--:--:--:--:--";
char last_cap_name[65]       = "";
char last_cap_time[9]        = "00:00:00";
char last_cap_det_method[64] = "";

#define CAPTURE_HISTORY_SIZE 3
struct CaptureEntry {
    char  type[16];
    char  mac[18];
    char  name[65];
    int   rssi;
    int   confidence;
    char  time[9];
    double lat;
    double lng;
    int   id;       // sequential detection number; 0 = unknown (pre-feature SD load)
};
CaptureEntry capture_history[CAPTURE_HISTORY_SIZE];
int capture_history_count = 0;

// Sequential detection counter — 1-based, persisted in flash so IDs survive
// reboots. Bumped once per is_new detection inside log_detection().
long next_detection_id = 1;
bool sd_full_warned    = false;  // toast once on first SD-full write failure

// SD-backed detection history (recent detections screen)
#define SD_HIST_SIZE 8
struct SDHistEntry {
    char     type[16];
    char     mac[18];
    char     name[32];
    int      rssi;
    int      confidence;
    char     method[24];
    char     timestamp[9];    // "HH:MM:SS" session uptime
    int      id;              // sequential detection number; 0 = loaded from old SD log
    char     datestamp[12];   // "MM/DD/YY" from GPS date, or "--/--/--"
    double   lat;             // GPS latitude at detection time (0.0 if no fix)
    double   lng;             // GPS longitude at detection time (0.0 if no fix)
    uint32_t epoch_utc;       // Unix epoch seconds (0 if no GPS)
    unsigned long uptime_ms;  // raw Uptime_ms from CSV col 0 — dedup key for delete
};
SDHistEntry sd_hist[SD_HIST_SIZE];
int  sd_hist_count      = 0;
int  history_selected_idx = 0;
bool hist_detail_open      = false;
bool hist_delete_confirming = false;
volatile bool sd_hist_dirty = false;

#define MAX_PENDING_DELETES 16
struct PendingDelete {
    char mac[18];
    unsigned long uptime_ms;   // matches CSV col 0 (Uptime_ms) exactly
    int  rssi;
};
static PendingDelete pending_deletes[MAX_PENDING_DELETES];
static int pending_delete_count = 0;
static unsigned long pending_delete_dirty_ms = 0;


// Detections screen — selection ease + detail overlay open/close transition.
// hist_sel_y_f follows the target row y via anim_filter for smooth motion.
// The detail overlay tracks open/close timestamps so it can slide-up + fade
// on open and reverse on close (hist_detail_open stays true through the
// close anim and is cleared by the renderer when alpha hits zero).
static const int    HIST_ROW_H       = 28;
static const int    HIST_VISIBLE_ROWS = 4;
static const float  HIST_SEL_TC      = 80.0f;     // snappy
static float        hist_sel_y_f     = 0.0f;
static unsigned long hist_last_frame_ms  = 0;
static unsigned long hist_detail_open_ms = 0;
static int          history_scroll_offset = 0;

// Stats screen vertical scroll (Option D — uniform card grid, smoothed).
// Keys set stats_scroll_target (instant). Renderer eases stats_scroll_y_f
// toward the target via anim_filter() with a snappy 120 ms time constant.
static int   stats_scroll_target  = 0;
static float stats_scroll_y_f     = 0.0f;
static unsigned long stats_last_frame_ms = 0;
static const int STATS_CONTENT_H   = 330;  // 8×36 + 7×6 gaps (no hero)
static const int STATS_VIEW_H      = 115;  // DISP_H - CONTENT_Y
static const int STATS_SCROLL_STEP = 42;   // one standard card (36) + gap (6)
static const int STATS_MAX_SCROLL  = STATS_CONTENT_H - STATS_VIEW_H;
static const float STATS_SCROLL_TC = 80.0f;   // ms — snappier; 120 felt sluggish

// Stats screen roll-up animation state — one slot per card index.
// New value vs prev value is checked each draw; if changed, roll_start
// kicks off an anim_slide_in() roll for UI_ANIM_QUICK ms. PACKETS is
// additionally throttled to a 3 s update cadence so it doesn't churn.
enum StatsCardIdx {
    SC_DET_SESSION = 0, SC_DET_LIFETIME, SC_WIFI, SC_BLE, SC_RAVEN, SC_AXON,
    SC_SESSION, SC_LIFETIME,
    SC_BATTERY, SC_HEAP,
    SC_PACKETS, SC_SD,
    SC_BOOTS, SC_FLASH,
    SC_VERSION, SC_VOLTAGE,
    SC_VDELTA,
    STATS_CARD_COUNT
};
// Per-character roll animation: comparing the formatted string per char
// lets only the digits that actually changed slide up. Each card owns a
// row of timestamps (one per glyph column) — a non-zero entry within the
// last UI_ANIM_QUICK ms means that column is mid-roll. Sized to fit the
// longest formatted value we surface (uptime, packet counts, etc.).
#define STAT_MAX_CHARS 12
static char          stats_prev_strings[STATS_CARD_COUNT][STAT_MAX_CHARS] = {{0}};
static unsigned long stats_char_anim[STATS_CARD_COUNT][STAT_MAX_CHARS]    = {{0}};
static bool  stats_values_initialized = false;
static uint32_t stats_pkt_display       = 0;
static unsigned long stats_pkt_last_update = 0;

#define TOAST_QUEUE_SIZE 2
#define TOAST_TEXT_LEN   48
struct ToastEntry {
    char text[TOAST_TEXT_LEN];
    uint16_t accent;
    bool is_action;
};
static ToastEntry toast_queue[TOAST_QUEUE_SIZE];
static int toast_queue_head = 0;
static int toast_queue_count = 0;
static unsigned long toast_start = 0;
static bool toast_active = false;

unsigned long last_time_save = 0;
unsigned long last_sd_flush_check = 0;
unsigned long last_persist_save = 0;

// ── Live activity feed (scanner screen) ──
#define FEED_SIZE 8
#define FEED_DEDUP_WINDOW_MS   30000UL
#define FEED_MIN_PUSH_INTERVAL_MS 1200UL

struct FeedEntry {
    char     mac[18];
    char     name[20];
    int8_t   rssi;
    uint8_t  proto;        // 0=WiFi, 1=BLE
    bool     is_flock;
    bool     is_5ghz;      // WiFi band. Everything from the C5 is 5 GHz by
                           // construction (WIFI_BAND_MODE_5G_ONLY), the S3's own
                           // radio is 2.4-only, so no channel parsing is needed.
    unsigned long timestamp;
};
static FeedEntry feed_entries[FEED_SIZE];
static int feed_count = 0;
static int feed_head  = 0;
static unsigned long last_feed_push_ms = 0;
static FeedEntry feed_pending;
static bool feed_pending_valid = false;

// Scanner reactive-animation triggers — fired from log_detection() is_new
// path, consumed and decayed inside draw_scanner_screen().
static unsigned long scanner_flash_ms = 0;
static uint8_t       scanner_flash_proto = 0;  // 0=WiFi 1=BLE — protocol for flock pip routing

// Cycleable visualization in the scanner's bottom-left panel. 'v' key
// advances through the modes; the renderer dispatches on this value.
static int       scanner_viz_mode  = 0;   // 0=SCAN 1=SPECTRUM
static const int SCANNER_VIZ_COUNT = 2;

// Per-channel packet counter used by the SPECTRUM viz. Counts are
// incremented in wifi_sniffer_packet_handler() (single 32-bit store on
// ESP32 is atomic, no mutex needed); the renderer snapshots them every
// 2 s into channel_pkt_display and decays the live counts so the bars
// represent a rolling window rather than session totals.
#define NUM_WIFI_CHANNELS 13
static volatile uint32_t channel_pkt_counts[NUM_WIFI_CHANNELS] = {0};
static uint32_t          channel_pkt_display[NUM_WIFI_CHANNELS] = {0};
static unsigned long     channel_display_last_update = 0;
static uint32_t          channel_peak = 1;

// Per-channel smoothed [0..1] display height for the spectrum curve.
// Eased toward the normalised channel_pkt_display ratio every frame
// so the line glides instead of snapping with each 400ms hop.
static float             spectrum_smooth[NUM_WIFI_CHANNELS] = {0};
static unsigned long     spectrum_last_frame = 0;


// Eased x-coordinate of the spectrum scan line — slides smoothly
// between channel positions instead of snapping when the hopper
// advances. Initialised on first render so it doesn't fly in from x=0.
static float             scan_line_x_f = 0.0f;
static unsigned long     scan_line_last_frame = 0;
// BLE-active color blend for the spectrum curve. Eases toward 1.0
// when BLE is scanning, back to 0.0 when WiFi resumes.
static float             spectrum_ble_blend = 0.0f;


// Feed slide-in animation — when scan_local_head changes, all rows
// shift down together over FEED_SHIFT_ANIM_MS with an ease-out curve.
static int               feed_anim_prev_head = -1;
static unsigned long     feed_anim_shift_ms  = 0;
static const unsigned long FEED_SHIFT_ANIM_MS = 250;

// Feed snapshot — refreshed by draw_scanner_screen() once per ~500ms,
// read directly by the viz functions so they don't need a separate mutex.
static FeedEntry         scan_local_feed[FEED_SIZE];
static int               scan_local_count = 0;
static int               scan_local_head  = 0;
static unsigned long     scan_feed_last_snapshot = 0;

static const unsigned long DOUBLE_TAP_MS = 400;
static bool bs_pending_exists = false;
static unsigned long bs_pending_until = 0;
static unsigned long last_bs_press_ms = 0;

struct RSSITrack { 
    char mac[18];
    int samples[RSSI_TRACK_SAMPLES];
    int sample_count;
    unsigned long last_seen;
};
RSSITrack rssi_tracker[RSSI_TRACK_MAX_DEVICES];
int rssi_tracker_count = 0;
static int signal_tracker_idx = -1;  // cached index into rssi_tracker for locator target

static volatile bool signal_active = false;
char signal_target_mac[18]  = "";
char signal_target_name[65] = "";
char signal_target_type[16] = "";   // "WiFi", "BLE", or ""
unsigned long signal_newest_sample_ms = 0;
int signal_peak_rssi = -120;

// ── Locator signal trace ring buffer ──
#define SIG_TRACE_SIZE        60          // 2 minutes at 2-second intervals
#define SIG_TRACE_INTERVAL_MS 2000

// Signal meter and trace normalization ranges — single source of truth.
// Two ceilings on purpose: the meter runs a little hotter than the trace so a
// very close target still has bar left to travel after the plot has topped out.
// The floor is shared, and is also what a gap sample writes so a dropout reads
// as a fall to the bottom rather than a frozen line.
#define RSSI_VIS_FLOOR   (-95)
#define RSSI_TRACE_CEIL  (-45)
#define RSSI_TRACE_RANGE (RSSI_TRACE_CEIL - RSSI_VIS_FLOOR)  // 50
#define RSSI_METER_CEIL  (-40)
#define RSSI_METER_RANGE (RSSI_METER_CEIL - RSSI_VIS_FLOOR)  // 55

// Freshness gates, in seconds since the last frame/advertisement from the target.
// LOST is the shorter of the two by design: the live elements (meter fill, trace
// dot) go quiet quickly, while the labelling waits longer before crying STALE.
#define SIG_LOST_AFTER_S   10
#define SIG_STALE_AFTER_S  30

struct SigTraceEntry {
    int8_t rssi;  // raw dBm value; -128 = floor/gap
};

static SigTraceEntry sig_trace[SIG_TRACE_SIZE];
static int           sig_trace_head        = 0;  // next slot to write
static int           sig_trace_count       = 0;
static unsigned long sig_trace_last_sample = 0;
static unsigned long sig_trace_last_frame_ms = 0;
static float         signal_bar_smooth = 0.0f;
static bool          signal_bar_seeded = false;

// Monotonic count of trace samples written since the target was set, and the
// value it held when the peak was recorded. The ring only remembers the last
// SIG_TRACE_SIZE samples, so comparing signal_peak_seq against
// (sig_trace_total - sig_trace_count) tells us whether the peak is still on
// screen — if it has scrolled off, the peak tick is dropped rather than
// re-pointed at the in-window maximum, which would contradict the hero value.
static long signal_peak_seq  = -1;
static long sig_trace_total  = 0;


TinyGPSPlus gps;
HardwareSerial SerialGPS(2);

// GPS soft power control. The module's rail is hardwired on this board (no
// cutoff), so "off" means commanding the receiver into standby over UART
// (~2mA vs ~25-40mA acquiring). Both the CASIC form ($PCAS12 — the $GN@115200
// module here) and the MTK form ($PMTK161) are sent; receivers ignore unknown
// sentences. Wake is any UART byte plus an explicit standby-cancel.
static void gps_send_nmea(const char* body) {
    uint8_t cs = 0;
    for (const char* p = body; *p; ++p) cs ^= (uint8_t)*p;
    SerialGPS.printf("$%s*%02X\r\n", body, cs);
}
static void gps_standby(bool on) {
    if (!plume_is_adv) return;   // regular Cardputer has no GPS receiver
    if (on) {
        gps_send_nmea("PCAS12,65535");   // CASIC: standby, max window (~18h)
        gps_send_nmea("PMTK161,0");      // MTK: standby until next byte
    } else {
        // Wake = UART traffic only. Do NOT send $PCAS12,0 — the parameter is
        // a standby DURATION, so 0 is not "cancel"; sending it put the
        // receiver to sleep (observed: silent at all bauds right after).
        for (int i = 0; i < 8; i++) { SerialGPS.write((uint8_t)0xFF); delay(2); }
    }
}
// Blind form for contexts where the GPS UART isn't (or may not be) up — the
// boot charge gate runs before the baud probe. Sends at both module bauds;
// wrong-baud garbage is ignored, and any RX edge doubles as a wake signal.
static void gps_blind_cmd(bool standby) {
    if (!plume_is_adv) return;   // regular Cardputer has no GPS receiver
    const uint32_t bauds[2] = {115200, 9600};
    for (int i = 0; i < 2; i++) {
        SerialGPS.end();
        SerialGPS.begin(bauds[i], SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
        delay(20);
        gps_standby(standby);
        SerialGPS.flush();
    }
    SerialGPS.end();
}

// Probe common GNSS baud rates and leave SerialGPS open at the one that
// produces valid NMEA. Returns the detected baud, or 0 if none matched
// (caller falls back to GPS_BAUD). Tries the module default first.
static uint32_t gps_detect_baud() {
    if (!plume_is_adv) return 0;   // regular Cardputer has no GPS receiver
    // If charge mode (or a reboot out of it) left the receiver in standby it
    // would probe silent — wake it first. Runs on every boot; harmless awake.
    // Two sweeps: a just-woken receiver can need a moment before NMEA resumes.
    gps_blind_cmd(false);
    const uint32_t candidates[] = { GPS_BAUD, 115200, 38400 };
    char line[100];
    for (int attempt = 0; attempt < 2; attempt++)
    for (uint32_t b : candidates) {
        SerialGPS.end();
        delay(20);
        SerialGPS.setRxBufferSize(256);   // must precede begin()
        SerialGPS.begin(b, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

        // Discard the framing noise produced by the baud switch.
        uint32_t flush_until = millis() + 100;
        while (millis() < flush_until) { while (SerialGPS.available()) SerialGPS.read(); }

        // Listen up to 1200 ms for one clean sentence: '$G' ... '*' ... newline.
        int  idx = 0;
        bool in_sentence = false;
        uint32_t deadline = millis() + 1200;
        while (millis() < deadline) {
            while (SerialGPS.available()) {
                char c = (char)SerialGPS.read();
                if (c == '$') { idx = 0; in_sentence = true; line[idx++] = c; continue; }
                if (!in_sentence) continue;
                if (c == '\r' || c == '\n') {
                    line[idx] = '\0';
                    if (idx >= 7 && line[1] == 'G' && strchr(line, '*')) {
                        Serial.printf("[gps] detected baud=%u (%s)\n", (unsigned)b, line);
                        return b;
                    }
                    in_sentence = false; idx = 0;
                } else if (idx < (int)sizeof(line) - 1) {
                    line[idx++] = c;
                } else {
                    in_sentence = false; idx = 0;   // overrun -> resync
                }
            }
            delay(2);   // yield so the scheduler / idle WDT stay fed
        }
        Serial.printf("[gps] no NMEA at baud=%u\n", (unsigned)b);
    }
    return 0;
}

// ── Auto timezone from GPS ──────────────────────────────────────────
// Derived from longitude + US DST rules. Recomputed every 5 minutes.
// Applied at display time only — stored timestamps stay UTC.
static int8_t  auto_tz_offset    = 0;     // hours from UTC (e.g. -5 for EST, -4 for EDT)
static bool    auto_tz_valid     = false; // true once computed from a GPS fix
static unsigned long auto_tz_last_compute_ms = 0;
static const unsigned long AUTO_TZ_INTERVAL_MS = 300000UL;  // recompute every 5 min

// Day of week: 0=Sunday. Tomohiko Sakamoto's algorithm — valid for any Gregorian date.
static int tz_day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Returns true if the given UTC date falls within US DST (2007+ rules).
// DST starts: second Sunday of March. DST ends: first Sunday of November.
// Date-only check — off by up to 1 hour on transition days, acceptable here.
static bool tz_is_us_dst(int year, int month, int day) {
    if (month > 3 && month < 11) return true;
    if (month < 3 || month > 11) return false;
    if (month == 3) {
        int dow_mar1 = tz_day_of_week(year, 3, 1);
        int first_sun = (dow_mar1 == 0) ? 1 : (8 - dow_mar1);
        return (day >= first_sun + 7);
    }
    // month == 11
    int dow_nov1 = tz_day_of_week(year, 11, 1);
    int first_sun = (dow_nov1 == 0) ? 1 : (8 - dow_nov1);
    return (day < first_sun);
}

// Compute UTC offset from GPS coordinates and date.
// US-focused: 6 timezone bands by longitude. Non-US: longitude/15 rounding, no DST.
static void tz_compute(double lat, double lng, int year, int month, int day) {
    int8_t base_offset;
    bool apply_dst = false;

    bool is_conus  = (lat >=  24.0 && lat <=  50.0 && lng >= -125.0 && lng <= -67.0);
    bool is_alaska = (lat >=  51.0 && lat <=  72.0 && lng >= -170.0 && lng <= -130.0);
    bool is_hawaii = (lat >=  18.0 && lat <=  23.0 && lng >= -161.0 && lng <= -154.0);

    if (is_hawaii) {
        base_offset = -10;
        apply_dst   = false;
    } else if (is_alaska) {
        base_offset = -9;
        apply_dst   = true;
    } else if (is_conus) {
        if      (lng < -115.0) base_offset = -8;  // Pacific
        else if (lng < -100.0) base_offset = -7;  // Mountain
        else if (lng <  -85.0) base_offset = -6;  // Central
        else                   base_offset = -5;  // Eastern
        apply_dst = true;
    } else {
        base_offset = (int8_t)roundf((float)lng / 15.0f);
        apply_dst   = false;
    }

    if (apply_dst && tz_is_us_dst(year, month, day)) base_offset += 1;

    auto_tz_offset           = base_offset;
    auto_tz_valid            = true;
    auto_tz_last_compute_ms  = millis();
}

// ============================================================================
// PCAP RING BUFFER STRUCT
// ============================================================================
struct pcap_packet_header { 
    uint32_t ts_sec; 
    uint32_t ts_usec; 
    uint32_t incl_len; 
    uint32_t orig_len; 
};

struct PcapQueueItem {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
    uint8_t payload[256];
};
PcapQueueItem pcap_write_buffer[MAX_PCAP_BUFFER];
int pcap_write_count = 0;
PcapQueueItem ble_pcap_write_buffer[MAX_PCAP_BUFFER];
int ble_pcap_write_count = 0;

// ============================================================================
// AUDIO & LED
// ============================================================================
void beep(int frequency, int duration_ms) {
    if (!is_muted) { 
        M5Cardputer.Speaker.tone(frequency, duration_ms); 
    }
}

void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(21, r, g, b);
}

// ============================================================================
// LED TASK — drives the WS2812 charge LED. Re-introduced at priority 1 on
// Core 1 with a 2048-byte stack after the original Core-0/priority-5 build
// proved racy with the radio init.
// ============================================================================
static TaskHandle_t LedTaskHandle = nullptr;

void LedTask(void* pv) {
    for (;;) {
        uint8_t r = 0, g = 0, b = 0;
        unsigned long now = millis();
        if (led_detect_active && !night_mode && now < led_detection_flash_until) {
            float pulse = anim_pulse(UI_PULSE_FAST);
            r = (uint8_t)((float)led_detect_r * pulse);
            g = (uint8_t)((float)led_detect_g * pulse);
            b = (uint8_t)((float)led_detect_b * pulse);
        } else {
            led_detect_active = false;
            bool export_on = (export_mode_active || export_connecting);
            // No brightness gate here, deliberately.
            //
            // On the ADV the WS2812 is powered off the backlight boost rail
            // (GPIO 38 PWM) and physically cannot light unless the backlight is
            // at full duty — see the same note in run_charge_mode(). That is why
            // ADV users see no LED below 4/4 brightness, and it is not a bug:
            // the constraint is self-enforcing in hardware, so a software gate
            // added nothing except a second place for the rule to live.
            //
            // It also actively hurt on other Cardputer revisions. One binary runs
            // on both boards (see plume_is_adv), and the original Cardputer wires
            // its LED differently — there the gate suppressed an LED that may
            // work at any brightness. Dropping it costs the ADV nothing and
            // stops us overriding hardware we cannot test.
            bool show_led = !night_mode &&
                            (export_on || (led_breathing_on && !low_power_mode));
            if (show_led) {
                float breath = anim_pulse(export_on ? UI_PULSE_MEDIUM : UI_PULSE_BREATHE);
                float dim    = export_on ? (0.30f + breath * 0.55f) : (0.15f + breath * 0.35f);
                if (export_on) {
                    r = (uint8_t)(255.0f * dim);
                    g = (uint8_t)(130.0f * dim);
                    b = 0;
                } else {
                    r = (uint8_t)((float)led_r * dim);
                    g = (uint8_t)((float)led_g * dim);
                    b = (uint8_t)((float)led_b * dim);
                }
            }
        }
        set_cardputer_led(r, g, b);
        vTaskDelay(30 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// BATTERY ENGINE (OPTIMIZED)
// ============================================================================
static float ema_voltage = 0.0f;
const float EMA_ALPHA = 0.05f;          // discharge: heavy smoothing
const float EMA_ALPHA_CHARGING = 0.30f; // charging: track the rising cell (~8 samples to converge)

// Load-Aware Telemetry Variables
static int32_t current_load_sag_mv = 0;
const int32_t SAG_WIFI_PROMISC = 45; // Estimated mV drop for continuous Wi-Fi Rx
const int32_t SAG_BLE_SCAN = 35;     // Estimated mV drop for active BLE scanning
const int32_t SAG_SPEAKER = 80;      // Estimated mV drop during active PCM audio playback

// Session baseline for the Stats "V CHANGE" card: filtered voltage captured
// once in setup() after the charge gate. The card shows cumulative drift
// since boot, the same honest cumulative readout Charge Mode uses (+NmV).
static int32_t session_start_mv = 0;

// Unsagged companion to ema_voltage, used only by the V CHANGE card. That card
// needs a baseline and a current reading that are directly comparable, and the
// sag model breaks that: SAG_WIFI_PROMISC is gated on system_fully_booted so it
// switches on partway through boot, and SAG_BLE_SCAN follows
// pBLEScan->isScanning() so it toggles forever after. Comparing sagged readings
// therefore reported +45..80mV the moment boot finished with zero real change,
// then wobbled +/-35mV as BLE cycled. This EMA tracks the raw ADC only.
static float ema_voltage_unsagged = 0.0f;
int32_t get_unsagged_voltage() { return (int32_t)ema_voltage_unsagged; }

int32_t get_filtered_voltage() {
    static uint32_t last_adc_ms = 0;
    static int32_t cached_raw_mv = 0;
    uint32_t now_adc = (uint32_t)millis();
    if (cached_raw_mv != 0 && (now_adc - last_adc_ms) < 250) {
        return (int32_t)ema_voltage;
    }
    // When charging, the charger supplies the peripheral load, so the
    // discharge sag model does not apply — adding it would corrupt the
    // reading. We also converge faster so the display tracks the rising
    // cell instead of crawling behind the discharge-tuned filter.
    // isCharging(): 0=discharging, 1=charging, 2=charge_unknown.
    // The Cardputer has no charge-status line, so it always returns 2 (unknown).
    // Treat ONLY a definite "charging" (1) as charging; unknown/discharging apply
    // the load-sag correction. (== 1 used directly to avoid library enum-name drift.)
    bool    charging = (M5Cardputer.Power.isCharging() == 1);
    int32_t sag      = charging ? 0 : current_load_sag_mv;
    float   alpha    = charging ? EMA_ALPHA_CHARGING : EMA_ALPHA;

    int32_t adc_mv = M5Cardputer.Power.getBatteryVoltage();
    int32_t raw_mv = adc_mv + sag;
    if (ema_voltage_unsagged == 0.0f) ema_voltage_unsagged = (float)adc_mv;
    ema_voltage_unsagged = (alpha * adc_mv) + ((1.0f - alpha) * ema_voltage_unsagged);
    cached_raw_mv = raw_mv;
    last_adc_ms = now_adc;
    if (ema_voltage == 0.0f) {
        ema_voltage = (float)raw_mv;
    }
    ema_voltage = (alpha * raw_mv) + ((1.0f - alpha) * ema_voltage);
    return (int32_t)ema_voltage;
}

void update_load_sag() {
    int32_t total_sag = 0;

    // 1. Wi-Fi Promiscuous Mode
    // Only apply the Wi-Fi baseline sag if the boot sequence has finished and the radio is actually on.
    if (system_fully_booted) {
        total_sag += SAG_WIFI_PROMISC;
    }

    // 2. BLE Scanning
    // BLE is cycled on and off by the ScannerLoopTask. Only add sag if it is actively scanning.
    if (pBLEScan != nullptr && pBLEScan->isScanning()) {
        total_sag += SAG_BLE_SCAN;
    }

    // 3. Audio / Alarms
    // Check the global boolean to see if the speaker is currently being driven
    if (is_alarming) {
        total_sag += SAG_SPEAKER;
    }

    // Update global state for the battery thread
    current_load_sag_mv = total_sag;
}

// Piecewise linear LiPo discharge curve — 9 breakpoints interpolated.
// Replaces the linear (mv-3300)*100/900 mapping which is 10+ percentage
// points off in the 3.5–3.7V danger zone where accuracy matters most.
static int voltage_to_percent(int32_t mv) {
    struct BP { int32_t mv; int pct; };
    static const BP curve[] = {
        {4200, 100},
        {4100,  90},
        {3950,  75},
        {3830,  60},
        {3740,  40},
        {3680,  25},
        {3600,  15},
        {3500,   5},
        {3300,   0},
    };
    static const int N = sizeof(curve) / sizeof(curve[0]);

    if (mv >= curve[0].mv) return 100;
    if (mv <= curve[N - 1].mv) return 0;

    for (int i = 0; i < N - 1; i++) {
        if (mv >= curve[i + 1].mv) {
            int32_t range_mv  = curve[i].mv  - curve[i + 1].mv;
            int     range_pct = curve[i].pct - curve[i + 1].pct;
            return curve[i + 1].pct +
                   (int)((mv - curve[i + 1].mv) * range_pct / range_mv);
        }
    }
    return 0;
}

// Averaged raw battery read for Charge Mode. No sag model / EMA: the radios are
// off so the cell is barely loaded and the ADC reading already sits near
// open-circuit; a simple mean just denoises it.
static int32_t charge_mode_read_mv() {
    int32_t sum = 0;
    const int N = 16;
    for (int i = 0; i < N; i++) { sum += M5Cardputer.Power.getBatteryVoltage(); delay(2); }
    return sum / N;
}

// Charge Mode's own pill renderer. Geometry mirrors drawPill_lcd() exactly — width
// len*6+7, height 12, radius 3, 18% fill, 40% border, text inset +3/+2 — so this
// screen reads as part of the same app. It cannot simply CALL drawPill_lcd():
// that is defined further down the file AND reads BG_COLOR/TEXT_COLOR, which
// apply_color_palette() has not populated this early in boot. Hence the copy,
// taking its colours as arguments from Charge Mode's hardcoded palette.
static void charge_pill(int x, int y, const char* text,
                        uint16_t accent, uint16_t bg, uint16_t fg) {
    auto& lcd = M5Cardputer.Display;
    int tw = (int)strlen(text) * 6 + 7;   // 6px per char at text size 1
    int th = 12;
    uint16_t pill_bg  = lerp_col16(bg, accent, 0.18f);
    uint16_t pill_brd = lerp_col16(bg, accent, 0.40f);
    lcd.fillRoundRect(x, y, tw, th, 3, pill_bg);
    lcd.drawRoundRect(x, y, tw, th, 3, pill_brd);
    lcd.setTextColor(fg, pill_bg);
    lcd.setTextSize(1);
    lcd.setCursor(x + 3, y + 2);
    lcd.print(text);
}

// Terminal battery protection for Charge Mode. Reached only when the not-
// charging watch has confirmed no charger AND the pack has fallen to
// CHARGE_MODE_DEAD_MV: there is nothing coming in and nothing left to spend, so
// stop drawing power entirely. Deep sleep is ~20uA against the ~20mA the
// screen-off watch loop still costs, and — unlike the brownout it would
// otherwise end in — waking from deep sleep is NOT a reset reason the boot gate
// re-enters Charge Mode on, which is what breaks the drain/reboot cycle.
//
// M5.Power.deepSleep() owns the panel shutdown (Display.sleep() +
// waitDisplay()); the Cardputer sets no _wakeupPin, so passing touch_wakeup
// false leaves it with no wake source of its own. We add one: the ADV
// keyboard's TCA8418 interrupt line (GPIO 11, an RTC-capable pad), so a
// keypress brings the device back. It is ACTIVE LOW, so arm it only when it is
// currently idle-high — arming it over a pending event would wake instantly and
// spin. If it is low, or on the regular Cardputer (GPIO-matrix keyboard, no INT
// line), sleep with no wake source; the reset button still works.
static void charge_mode_sleep_forever() {
    auto& lcd = M5Cardputer.Display;
    const uint16_t COL_BG = lgfx::color565(0, 0, 0);

    lcd.wakeup();
    lcd.setBrightness(20);
    lcd.fillScreen(COL_BG);
    lcd.setTextDatum(MC_DATUM);
    lcd.setTextColor(lgfx::color565(230, 170, 40), COL_BG);
    lcd.setTextSize(2);
    lcd.drawString("BATTERY SAVER", DISP_W / 2, DISP_H / 2 - 12);
    lcd.setTextSize(1);
    lcd.setTextColor(lgfx::color565(255, 255, 255), COL_BG);
    lcd.drawString("charge, then press reset", DISP_W / 2, DISP_H / 2 + 16);
    lcd.setTextDatum(TL_DATUM);
    Serial.println("[CHARGE] battery protection -> deep sleep");
    Serial.flush();

    // Hold the notice long enough to be read, and drain any pending keyboard
    // events so the INT line settles high before we sample it. Keep feeding the
    // WDT: this task is still subscribed until the delete below.
    for (int i = 0; i < 24; i++) { esp_task_wdt_reset(); M5Cardputer.update(); delay(250); }

    lcd.setBrightness(0);
    esp_task_wdt_delete(NULL);

    // TCA8418 interrupt pin — DEFAULT_TCA8418_INT_PIN in M5Cardputer's
    // TCA8418KeyboardReader, which is constructed with the default (-1 -> 11).
    const gpio_num_t KB_INT_PIN = GPIO_NUM_11;
    if (plume_is_adv && digitalRead(KB_INT_PIN) == HIGH) {
        esp_sleep_enable_ext0_wakeup(KB_INT_PIN, 0);   // 0 = wake on active-low
    }
    M5Cardputer.Power.deepSleep(0, false);   // never returns
}

// Minimal charging screen + hold loop. Self-contained (hardcoded colors, direct
// LCD) because it runs during early boot before the runtime palette and the
// draw sprite exist. Returns when the user presses a key ("start now") or the
// cell holds above the resume threshold long enough. That threshold depends on
// WHY we're here: an auto entry (low battery / brownout) resumes at the
// safe-to-run floor CHARGE_MODE_EXIT_MV, while a user 'c'-request is a deliberate
// top-up and holds until CHARGE_MODE_FULL_MV. A brownout entry additionally
// ratchets the threshold above the current resting voltage (see below) so a
// boot that browns out ABOVE the floor can't retry-loop at a level that already
// failed. The caller then continues normal boot.
void run_charge_mode(bool user_requested, bool after_brownout) {
    int32_t resume_mv = user_requested ? CHARGE_MODE_FULL_MV : CHARGE_MODE_EXIT_MV;
    charge_mode_request = 0;                 // consume the request (auto-re-entry is voltage-driven)
    setCpuFrequencyMhz(40);                  // lowest safe clock = least load
    auto& lcd = M5Cardputer.Display;
    lcd.setRotation(1);

    // Screen stays ON (dim) so charge progress is always visible. The dim
    // backlight is a real but modest load — charging runs a bit slower than a
    // dark panel would, and the brownout ratchet still guards the exit. There
    // is deliberately NO LED indicator: the WS2812 is powered off the backlight
    // boost rail (GPIO 38 PWM) and only lights at full brightness, which would
    // make it the single biggest load — useless for charging.
    lcd.wakeup();
    // Brightness 20 (~8% duty): user-chosen floor. setBrightness here is 0..255
    // duty — do NOT port raw values from other firmware (Launcher's chargeMode
    // "5" is a different scale; 5/255 is below this panel's visible threshold
    // and the screen reads as completely dark). Charge slope was flat at
    // 60..33, so anything at or below 30 is charge-neutral; 20 trades a dimmer
    // readout for a little more rail margin.
    lcd.setBrightness(20);

    // Cut the other begin()-powered loads not needed to charge.
    set_cardputer_led(0, 0, 0);              // LED off (its rail is down with the screen anyway)
    M5Cardputer.Speaker.end();               // power down the I2S amp (setVolume(0) only mutes)
    // end() releases the I2S pins to floating inputs; a floating data/clock
    // line lets the amp input chatter (heard as rapid faint clicking). Pin
    // them low — the app's Speaker.begin() after resume reclaims them.
    {
        auto sc = M5Cardputer.Speaker.config();
        if (sc.pin_data_out >= 0) { pinMode(sc.pin_data_out, OUTPUT); digitalWrite(sc.pin_data_out, LOW); }
        if (sc.pin_bck      >= 0) { pinMode(sc.pin_bck,      OUTPUT); digitalWrite(sc.pin_bck,      LOW); }
        if (sc.pin_ws       >= 0) { pinMode(sc.pin_ws,       OUTPUT); digitalWrite(sc.pin_ws,       LOW); }
    }

    // GPS receiver to standby for the whole charge. It is rail-powered even
    // here (no hardware cutoff) and free-runs satellite acquisition at
    // ~25-40mA — nearly half the ~60mA charge budget, and the reason a lit
    // screen previously couldn't gain. With it asleep the readout can stay
    // on the whole time (matching bmorcelli/Launcher, which has no GPS load).
    // The boot-side probe wakes it again on every path back to the app.
    gps_blind_cmd(true);

    // ── Always-on charging readout ───────────────────────────────────────────
    // The readout stays on screen the whole time; pressing any key starts the
    // app immediately.
    const uint32_t SAMPLE_MS = 1000;

    int32_t  mv               = charge_mode_read_mv();   // prime a valid reading
    float    ema_mv           = (float)mv;
    uint32_t last_sample_ms   = millis();
    uint32_t exit_stable_since = 0;   // 0 = not currently above EXIT threshold
    bool     key_armed        = false;  // require one release before input acts, so the
                                        // 'c' that launched us can't fire instantly.
    const uint32_t arm_deadline_ms = millis() + 5000;  // see the force-arm below

    // ── Charge-progress tracking ─────────────────────────────────────────────
    // The definitive "is it actually charging?" signal. On this hardware the cell
    // climbs only ~0.5–3 mV/min (a ~60 mA TP4057 into 1750 mAh, and the LiPo mid
    // curve is flat), so a short-window slope is pure ADC noise. We instead track
    // the CUMULATIVE gain since entry (unambiguous over minutes) and derive an
    // AVERAGE rate + ETA from it — conservative, stable, and honest.
    const uint32_t charge_start_ms = millis();
    const int32_t  start_mv        = mv;      // resting voltage when charge mode began
    float          rate_mv_per_min = 0.0f;    // avg since entry; valid once settled
    bool           rate_valid      = false;

    // Brownout ratchet. A brownout with the cell ALREADY at/above the EXIT floor
    // means the floor wasn't enough for this cell/charger today — resuming at the
    // same level just replays exit -> radio surge -> brownout -> re-enter every
    // ~10s, which reads as "frozen on the charge screen". Demand a real gain
    // (+80mV over the current resting level) before retrying; each failed retry
    // re-enters at a higher resting voltage, so the bar ratchets up on its own
    // until a boot survives. Capped at 4000mV — a level any bootable pack must
    // reach — so a dying cell can't push the target beyond the charger. A
    // keypress still overrides at any time.
    if (after_brownout && !user_requested) {
        int32_t ratchet = start_mv + 80;
        if (ratchet > 4000)      ratchet = 4000;
        if (ratchet > resume_mv) resume_mv = ratchet;
        Serial.printf("[CHARGE] brownout entry at %dmV -> resume target %dmV\n",
                      (int)start_mv, (int)resume_mv);
    }

    // L1c palette (hardcoded: this runs before the runtime palette exists) and
    // gauge geometry, shared by the static redraw and the shimmer animation.
    const uint16_t COL_BG    = lgfx::color565(0, 0, 0);
    const uint16_t COL_TEXT  = lgfx::color565(255, 255, 255);
    const uint16_t COL_TRACK = lgfx::color565(32, 36, 44);
    const uint16_t COL_GOOD  = lgfx::color565(60, 210, 120);  // green: safe to resume
    const uint16_t COL_LOW   = lgfx::color565(230, 170, 40);  // amber: still charging
    const int PAD = 14;   // one shared margin: left edge of every element, plus
                          // the title's top gap and the footer's bottom gap.
    const int GAUGE_X = PAD, GAUGE_Y = 80, GAUGE_W = 150, GAUGE_H = 5, GAUGE_R = 2;
    uint32_t last_anim_ms = 0;
    bool     ui_safe      = (mv >= resume_mv);  // accent state (hysteresis below)
    bool     chrome_stale = true;               // full repaint on entry / safe flip
    int32_t  drawn_mv     = -1;                 // mv the value strips show (-1 = stale)
    int      shown_pct    = -1;                 // ripple-guarded hero percent (-1 = unset)
    int32_t  last_raw     = -1;                 // ADC health checks (see sample block)
    int      flat_raws    = 0;                  // consecutive bit-identical reads
    int      bad_raws     = 0;                  // consecutive implausible reads
    int      gps_renap    = 0;                  // re-issue GPS standby (see sample block)

    // ── Not-charging watch state (see CHARGE_STALL_* above) ──────────────────
    // stalled == false: normal charge readout. stalled == true: panel powered
    // down and sampling slowed, waiting for the charger to come back or for the
    // pack to reach the protection floor.
    int32_t  peak_mv      = mv;    // highest smoothed reading seen while charging
    uint32_t stall_since  = 0;     // 0 = not currently below the stall bar
    bool     stalled      = false;
    int32_t  stall_low_mv = 0;     // low-water mark while stalled (recovery reference)

    // Every dynamic element renders into its own small sprite and lands as ONE
    // blit, so the panel never shows a cleared-but-not-yet-redrawn state (the
    // flicker source). ~15KB total, trivial against boot-gate heap.
    const int HERO_W = 92,  HERO_H = 32;   // "100" at size 4 + '%' at size 2
    const int VOLT_W = 176, VOLT_H = 16;   // "4.06V  +1234mV" at size 2
    const int BOLT_W = 13,  BOLT_H = 15;   // 12x14 polygon + 1px slack
    M5Canvas gauge_spr(&lcd), bolt_spr(&lcd), hero_spr(&lcd), volt_spr(&lcd);
    gauge_spr.setColorDepth(16); bolt_spr.setColorDepth(16);
    hero_spr.setColorDepth(16);  volt_spr.setColorDepth(16);
    bool spr_ok = gauge_spr.createSprite(GAUGE_W, GAUGE_H)
               && bolt_spr.createSprite(BOLT_W, BOLT_H)
               && hero_spr.createSprite(HERO_W, HERO_H)
               && volt_spr.createSprite(VOLT_W, VOLT_H);
    Serial.printf("[CHARGE] ui L1c-v5, sprites %s\n",
                  spr_ok ? "ok" : "FAILED (animations off)");

    esp_task_wdt_add(NULL);   // charge loop self-recovers if it wedges
    for (;;) {
        esp_task_wdt_reset();
        M5Cardputer.update();
        uint32_t now = millis();

        bool pressed = M5Cardputer.Keyboard.isPressed();
        if (!pressed) { key_armed = true; }
        else if (!key_armed && now > arm_deadline_ms) {
            // isPressed() has been non-zero for 5 s straight, which no real
            // keypress does. The ADV builds its key list from TCA8418 press/
            // release events and never rescans, so a lost release event latches
            // a key "down" permanently — and key_armed would then never become
            // true, making Charge Mode impossible to leave by hand. Force-arm so
            // the next pass can exit. Nothing legitimate reaches this branch.
            Serial.println("[CHARGE] key stuck down >5s - force-arming exit");
            key_armed = true;
        }
        else if (key_armed) {
            // ANY observed press exits — there is deliberately no multi-frame
            // debounce. On the ADV the key list is built from TCA8418 FIFO
            // events (debounced in the controller), not from a matrix rescan,
            // and update() drains exactly ONE event per call. A quick tap
            // therefore shows up as "pressed" on one frame and "released" on
            // the next, so the old >=2-frame rule silently ate short taps and
            // forced the user to hold a key — which reads as a dead screen.
            //
            // Wait for release so the keystroke (e.g. 'b') can't leak into the
            // app's keyboard handler and cycle brightness the instant we
            // return — but time-box it and keep feeding the WDT. That same
            // event model means a LOST release event latches the key down
            // forever (no rescan ever clears it), and an unbounded wait here
            // would spin past the 30s task WDT into a panic reboot: a frozen
            // charge screen of our own making. A leaked keystroke is the far
            // cheaper failure.
            const uint32_t RELEASE_WAIT_MS = 3000;
            uint32_t rel_start = millis();
            while (M5Cardputer.Keyboard.isPressed()
                   && (millis() - rel_start) < RELEASE_WAIT_MS) {
                esp_task_wdt_reset();
                M5Cardputer.update();
                delay(10);
            }
            // Do NOT touch brightness here: charge mode holds one fixed level
            // and the app's boot owns brightness afterward (the boot probe
            // also wakes the GPS on this path). The panel may be asleep if the
            // not-charging watch powered it down, and the app draws into it
            // immediately on return.
            lcd.wakeup();
            // Deliberately do NOT unsubscribe from the task WDT here. The boot
            // that follows (sprite, SD, GPS probe, LittleFS, radios) has no
            // watchdog of its own, so dropping protection on the way out turned
            // any hang in it into a permanent freeze. boot_animate() now pets it
            // at every checkpoint, and loop()'s service_watchdog() tolerates an
            // already-subscribed task.
            return;
        }

        // Periodic cell sample + resume decision (NOT every frame). Once
        // stalled there is nothing on screen to keep current, so sample lazily.
        if (now - last_sample_ms >= (stalled ? 5000UL : SAMPLE_MS)) {
            last_sample_ms = now;
            int32_t raw = charge_mode_read_mv();

            // Re-issue GPS standby every ~50s: the $PCAS12 window unit is
            // ambiguous across CASIC firmwares (ms vs s); if it auto-wakes,
            // this puts it back to sleep before it burns meaningful charge.
            if (++gps_renap >= 50) { gps_renap = 0; gps_blind_cmd(true); }

            // ── Battery ADC health ───────────────────────────────────────────
            // Two distinct failures, two distinct responses.
            //
            // (1) IMPLAUSIBLE read. The ADC oneshot unit failed to initialize,
            //     so _getBatteryAdcRaw() returns 0 (M5Unified Power_Class.cpp).
            //     A reboot cannot fix that, and it is actively harmful: the boot
            //     gate reads 0mV as "empty cell", gates straight back into
            //     Charge Mode, and we land here again ~6s later — forever, which
            //     presents as a bricked device. Charge Mode exists to make a
            //     VOLTAGE decision; with no voltage it protects nothing and only
            //     traps the user. Bail out and let the app boot.
            // (2) FROZEN read. A healthy 16-sample mean of a ±15mV-ripple ADC is
            //     never bit-identical for 180 consecutive samples, so that means
            //     the ADC has genuinely stopped converting. A reboot CAN clear
            //     that, and the boot gate re-enters Charge Mode afterward.
            //
            // NOTE: neither check can see an I2C or KEYBOARD failure, despite
            // what this block used to claim. The battery read is ADC1_GPIO10
            // with no I2C involved (pmic_adc); the keyboard's TCA8418 is the only
            // device on the bus. A wedged keyboard leaves these reads perfectly
            // healthy — keyboard liveness needs its own watch, in the app, where
            // a dead keyboard actually strands the user (Charge Mode's own exits
            // are voltage-driven and survive it).
            if (raw <= BATT_ADC_MIN_PLAUSIBLE_MV) {
                if (++bad_raws >= 6) {
                    Serial.printf("[CHARGE] battery ADC implausible (%dmV) -> resuming app\n",
                                  (int)raw);
                    lcd.wakeup();
                    esp_task_wdt_delete(NULL);
                    return;
                }
            } else {
                bad_raws = 0;
                if (raw == last_raw) flat_raws++;
                else                 flat_raws = 0;
                last_raw = raw;
                if (flat_raws >= 180) {
                    Serial.println("[CHARGE] battery ADC frozen (180 identical reads) -> reboot");
                    delay(50);
                    esp_restart();
                }
            }

            // 0.08 (~12s TC at 1s samples, was 0.2/~5s): observed ±15mV ripple
            // wobbled the readout enough to read as discharge. The resume
            // decision already demands a 4s hold, so the slower filter only
            // delays exit by seconds.
            ema_mv = 0.08f * (float)raw + 0.92f * ema_mv;
            int32_t new_mv = (int32_t)ema_mv;
            mv = new_mv;

            // Average charge rate since entry. Gate on a settle window so the
            // first noisy seconds don't produce a wild slope; only trust an
            // upward trend (a flat/negative reading = not gaining, ETA hidden).
            uint32_t elapsed_ms = now - charge_start_ms;
            if (elapsed_ms >= 120000) {                       // 2 min settle
                rate_mv_per_min = (float)(mv - start_mv) / (elapsed_ms / 60000.0f);
                rate_valid = true;
            }

            // Serial trace mirrors the on-screen readout (handy over USB).
            Serial.printf("[CHARGE] t=%lus  %dmV  (%+dmV)  %s%.1f mV/min%s\n",
                          (unsigned long)(elapsed_ms / 1000), (int)mv,
                          (int)(mv - start_mv),
                          rate_valid ? "avg " : "settling ",
                          rate_valid ? rate_mv_per_min : 0.0f,
                          stalled ? "  [STALLED]" : "");

            // ── Not-charging watch ───────────────────────────────────────────
            // The cell falling away from its own peak is the only "no charger"
            // signal this board offers (see CHARGE_STALL_* above).
            if (stalled) {
                if (mv < stall_low_mv) stall_low_mv = mv;   // recovery reference
                if (mv >= stall_low_mv + CHARGE_STALL_RISE_MV) {
                    // Climbing again — the charger is back. Restore the readout.
                    stalled      = false;
                    stall_since  = 0;
                    peak_mv      = mv;
                    chrome_stale = true;   // full repaint; the panel was asleep
                    drawn_mv     = -1;
                    shown_pct    = -1;
                    Serial.printf("[CHARGE] charging resumed at %dmV -> screen on\n", (int)mv);
                    lcd.wakeup();
                    lcd.setBrightness(20);
                } else if (mv <= CHARGE_MODE_DEAD_MV) {
                    charge_mode_sleep_forever();   // never returns
                }
            } else if (mv > peak_mv) {
                peak_mv     = mv;
                stall_since = 0;
            } else if (mv <= peak_mv - CHARGE_STALL_DROP_MV) {
                if (stall_since == 0) stall_since = now;
                else if (now - stall_since >= CHARGE_STALL_CONFIRM_MS) {
                    // Confirmed: we are DISCHARGING on the charge screen. Say
                    // so, then cut the panel — the backlight is the one load
                    // still under our control, and holding it lit only hastens
                    // the brownout that would reboot us straight back here.
                    stalled      = true;
                    stall_low_mv = mv;
                    Serial.printf("[CHARGE] NOT CHARGING: %dmV, %dmV below peak %dmV -> screen off\n",
                                  (int)mv, (int)(peak_mv - mv), (int)peak_mv);

                    lcd.fillScreen(COL_BG);
                    lcd.setTextDatum(TL_DATUM);
                    charge_pill(PAD, PAD, "NOT CHARGING", COL_LOW, COL_BG, COL_TEXT);
                    lcd.setTextColor(COL_TEXT, COL_BG);
                    lcd.setTextSize(2);
                    lcd.drawString("check cable", PAD, 46);
                    lcd.setTextSize(1);
                    lcd.drawString("screen off to save power", PAD, 76);
                    lcd.setTextDatum(BL_DATUM);
                    lcd.drawString("press any key to start", PAD, DISP_H - PAD + 2);
                    lcd.setTextDatum(TL_DATUM);

                    // Hold it on screen long enough to be read, feeding the WDT.
                    // Deliberately NO M5Cardputer.update() in here: update()
                    // drains the TCA8418 FIFO, and a tap landing in this window
                    // would have its press and release both consumed before the
                    // key handler at the top of the loop ever saw it. Leaving
                    // the events queued means the press is still waiting when
                    // we get back there.
                    for (int i = 0; i < 24; i++) { esp_task_wdt_reset(); delay(250); }
                    lcd.setBrightness(0);
                    lcd.sleep();
                }
            } else {
                stall_since = 0;   // back inside the noise band of the peak
            }

            // Auto-resume once the SMOOTHED cell voltage HOLDS above the resume
            // threshold for a sustained window, so ripple can't bounce us into a
            // brown-out the instant the radios load the rail on resume. (resume_mv
            // is the safe-to-run floor when auto-entered, or the full-charge
            // target when the user requested this charge.)
            if (mv >= resume_mv) {
                if (exit_stable_since == 0)                    exit_stable_since = now;
                else if (now - exit_stable_since >= 4000) {
                    lcd.wakeup();   // no-op unless the stall watch slept the panel
                    return;   // app boot owns brightness; charge mode never changes it
                              // (WDT subscription intentionally retained — see above)
                }
            } else {
                exit_stable_since = 0;
            }
        }

        // Stalled: the panel is powered down, so there is nothing to render and
        // no reason to spin at UI cadence. Poll slowly — the key handler above
        // still runs every pass, and a press exits immediately.
        if (stalled) { delay(500); continue; }

        // ── Rendering ────────────────────────────────────────────────────────
        // Chrome (title/footer) paints on entry and repaints only when the
        // accent flips; everything dynamic lands as single sprite blits below.
        // The accent has hysteresis: ripple around resume_mv would otherwise
        // strobe amber<->green, and each flip repaints the whole screen.
        // Still NO time-to-full estimate — voltage rise can't be extrapolated
        // to a finish time on this hardware (early load-rebound inflates it,
        // the CV taper balloons it, there's no current-sense to coulomb-count),
        // so any ETA is garbage. (mV/min still goes to the serial trace above.)
        if (ui_safe) { if (mv <  resume_mv - 15) { ui_safe = false; chrome_stale = true; } }
        else         { if (mv >= resume_mv)      { ui_safe = true;  chrome_stale = true; } }
        int      pct    = voltage_to_percent(mv);
        // Ripple guard: ADC ripple (±15mV) flaps the mapped percent at curve
        // boundaries — worst at the bottom, where the hero bounces 1 <-> 0 and
        // reads as "losing charge" while actually charging. Percent rises
        // freely but only falls on a real >=2-point drop.
        if (shown_pct < 0 || pct > shown_pct || shown_pct - pct >= 2) shown_pct = pct;
        pct = shown_pct;
        uint16_t accent = ui_safe ? COL_GOOD : COL_LOW;

        if (chrome_stale) {
            chrome_stale = false;
            drawn_mv     = -1;      // force the value strips to repaint

            lcd.fillScreen(COL_BG);
            lcd.setTextDatum(TL_DATUM);

            // Title pill — same shape as every other pill in the app.
            charge_pill(PAD, PAD, "CHARGE MODE", accent, COL_BG, COL_TEXT);

            // Footer hint (instruction, not data). Bottom margin mirrors the
            // title's PAD, but measured to the lowercase BODY: bottom-anchoring
            // puts the 'p'/'y' descenders on the margin line, floating the body
            // ~2px high vs the all-caps title. The +2 nudges the body down so
            // the two visually match; descenders sit inside the margin.
            lcd.setTextDatum(BL_DATUM);
            lcd.setTextColor(COL_TEXT, COL_BG);
            lcd.setTextSize(1);
            lcd.drawString("press any key to start", PAD, DISP_H - PAD + 2);
            lcd.setTextDatum(TL_DATUM);

            // Sprite alloc failed: draw the bolt + gauge once, statically,
            // so the screen is still complete (animations stay off).
            if (!spr_ok) {
                int bx = DISP_W - 14 - 12, by = 13;
                lcd.fillTriangle(bx+7, by,    bx,    by+8,  bx+5,  by+8, accent);
                lcd.fillTriangle(bx+7, by,    bx+5,  by+8,  bx+7,  by+6, accent);
                lcd.fillTriangle(bx+5, by+8,  bx+5,  by+14, bx+12, by+6, accent);
                lcd.fillTriangle(bx+5, by+8,  bx+12, by+6,  bx+7,  by+6, accent);
                int fw = GAUGE_W * pct / 100;
                lcd.fillRoundRect(GAUGE_X, GAUGE_Y, GAUGE_W, GAUGE_H, GAUGE_R, COL_TRACK);
                if (fw >= GAUGE_R * 2 + 1)  lcd.fillRoundRect(GAUGE_X, GAUGE_Y, fw, GAUGE_H, GAUGE_R, accent);
                else if (fw > 0)            lcd.fillRect(GAUGE_X, GAUGE_Y + 1, fw, GAUGE_H - 2, accent);
            }
        }

        // Value strips — each composed off-screen and pushed as one blit,
        // only when mv moves. All flush-left at x=14 with the rest.
        if (mv != drawn_mv) {
            drawn_mv = mv;
            char line[24];

            // Percent hero — size-4 number with a size-2 '%' sharing its baseline.
            snprintf(line, sizeof(line), "%d", pct);
            if (spr_ok) {
                hero_spr.fillSprite(COL_BG);
                hero_spr.setTextDatum(TL_DATUM);
                hero_spr.setTextColor(COL_TEXT, COL_BG);
                hero_spr.setTextSize(4);
                hero_spr.drawString(line, 0, 0);
                int num_w = hero_spr.textWidth(line);
                hero_spr.setTextSize(2);
                hero_spr.drawString("%", num_w + 3, 8 * 4 - 8 * 2);
                hero_spr.pushSprite(PAD, 40);
            } else {
                lcd.fillRect(PAD, 40, HERO_W, HERO_H, COL_BG);
                lcd.setTextColor(COL_TEXT, COL_BG);
                lcd.setTextSize(4);
                lcd.drawString(line, PAD, 40);
            }

            // Voltage + cumulative gain since entry (proof it's climbing).
            snprintf(line, sizeof(line), "%d.%02dV  %+dmV",
                     (int)(mv / 1000), (int)((mv % 1000) / 10), (int)(mv - start_mv));
            if (spr_ok) {
                volt_spr.fillSprite(COL_BG);
                volt_spr.setTextDatum(TL_DATUM);
                volt_spr.setTextColor(accent, COL_BG);
                volt_spr.setTextSize(2);
                volt_spr.drawString(line, 0, 0);
                volt_spr.pushSprite(PAD, 94);
            } else {
                lcd.fillRect(PAD, 94, VOLT_W, VOLT_H, COL_BG);
                lcd.setTextColor(accent, COL_BG);
                lcd.setTextSize(2);
                lcd.drawString(line, PAD, 94);
            }
        }

        // Animations, each a single sprite blit (no on-panel clears):
        //   - Gauge "loading" pulse: the fill stays solid accent; a dimmer
        //     accent segment repeatedly grows out of the fill's leading edge
        //     (up to 16px) and resets — reads as charge flowing in at the
        //     current level. One hue, no sweeping bands.
        //   - Bolt pulse: breathes from near-off to full accent over 1.6s.
        // Proof-of-life: the voltage EMA can sit unchanged for minutes on the
        // flat of the LiPo curve; without motion the screen reads as frozen.
        // Step interval scales with rail health: on a marginal cell every blit
        // is a transient load spike, so step coarsely (3s, like the Launcher's
        // ~5s redraw); on a healthy cell (>=3450mV) step at 1s so the screen
        // visibly lives — at 3s + heavy EMA it reads as frozen.
        uint32_t anim_iv = (mv >= 3450) ? 1000 : 3000;
        if (spr_ok && now - last_anim_ms >= anim_iv) {
            last_anim_ms = now;
            const uint32_t CYCLE_MS = 1600;
            uint32_t ph = now % CYCLE_MS;

            int fw = GAUGE_W * pct / 100;
            gauge_spr.fillSprite(COL_BG);
            gauge_spr.fillRoundRect(0, 0, GAUGE_W, GAUGE_H, GAUGE_R, COL_TRACK);
            if (fw >= GAUGE_R * 2 + 1)  gauge_spr.fillRoundRect(0, 0, fw, GAUGE_H, GAUGE_R, accent);
            else if (fw > 0)            gauge_spr.fillRect(0, 1, fw, GAUGE_H - 2, accent);

            const int EXT_MAX = 16;
            int ext = (int)(ph * (EXT_MAX + 1) / CYCLE_MS);       // 0..EXT_MAX ramp
            int x1  = fw + ext;
            if (x1 > GAUGE_W - 1) x1 = GAUGE_W - 1;
            if (x1 > fw)
                gauge_spr.fillRect(fw, 1, x1 - fw, GAUGE_H - 2,
                                   lerp_col16(COL_TRACK, accent, 0.45f));

            gauge_spr.pushSprite(GAUGE_X, GAUGE_Y);

            // Bolt, top-right (right edge mirrors the 14px margin).
            // Polygon 7,0 0,8 5,8 5,14 12,6 7,6 split into two quads = four tris.
            float    t  = (ph < CYCLE_MS / 2) ? ph / (CYCLE_MS / 2.0f)
                                              : (CYCLE_MS - ph) / (CYCLE_MS / 2.0f);
            uint16_t bc = lerp_col16(lerp_col16(accent, COL_BG, 0.85f), accent, t);
            bolt_spr.fillSprite(COL_BG);
            bolt_spr.fillTriangle(7, 0,  0, 8,   5, 8,  bc);
            bolt_spr.fillTriangle(7, 0,  5, 8,   7, 6,  bc);
            bolt_spr.fillTriangle(5, 8,  5, 14, 12, 6,  bc);
            bolt_spr.fillTriangle(5, 8, 12, 6,   7, 6,  bc);
            bolt_spr.pushSprite(DISP_W - 14 - BOLT_W, 13);
        }

        delay(80);   // gentler poll; still responsive (keys need ~2 frames)
    }
}

// ============================================================================
// STRING SCRUBBER
// ============================================================================
void clean_device_name_char(char* str) {
    int read_idx = 0;
    int write_idx = 0;
    while (str[read_idx] != '\0') {
        unsigned char c = (unsigned char)str[read_idx];
        if (c == 0xE2
            && str[read_idx + 1] != '\0'
            && (unsigned char)str[read_idx + 1] == 0x80
            && str[read_idx + 2] != '\0'
            && ((unsigned char)str[read_idx + 2] == 0x98
                || (unsigned char)str[read_idx + 2] == 0x99)) {
            str[write_idx++] = '\'';
            read_idx += 3;
        } else if (c >= 32 && c <= 126) {
            str[write_idx++] = str[read_idx++];
        } else {
            read_idx++; 
        }
    }
    str[write_idx] = '\0';
}

enum OuiTier { OUI_NONE = 0, OUI_SPECIFIC = 1, OUI_GENERIC = 2 };
struct OuiEntry { const char* prefix; uint8_t tier; };

// ============================================================================
// SIGNATURE DATABASE
// ============================================================================
static const char* wifi_ssid_patterns[] = {
    "FS Ext Battery", "Penguin", "Pigvision", "FlockOS",
    "flocksafety", "OFS_IoT", "PFS_",
    // Bare "flock" — matched case-insensitively as a SUBSTRING, so it covers
    // names the more specific patterns miss (e.g. "Flock-Cam-7", "FlockLTE").
    // is_flock_ssid_format() only accepts a "Flock-" prefix with a 2..12 digit
    // HEX suffix, so non-hex variants previously fell through everything.
    // Deliberately low-yield by design: an SSID match alone scores SCORE_WEAK
    // (25), well under CONFIDENCE_ALARM_THRESHOLD, so a coincidental hit on a
    // home network cannot raise an alarm on its own. (Still true at the lowered
    // threshold of 70: an SSID match alone tops out at 25 + 10 = 35.)
    "flock"
};
static const int NUM_SSID_PATTERNS = sizeof(wifi_ssid_patterns) / sizeof(wifi_ssid_patterns[0]);

static const OuiEntry mac_prefixes[] = {
    // ── OUI_SPECIFIC — directly attributed to Flock Safety ──────────────────
    // Exactly one entry earns Tier 1. Verified 2026-07-26 against three
    // independent sources — IEEE oui.txt (MA-L), manuf2 (56,505, incl. MA-M/MA-S),
    // and Ringmast4r/OUI-Master-Database (88,873 merged, incl. IAB + CID, plus
    // Wireshark and Nmap). All three agree: b4:1e:52 is the ONLY OUI assigned to
    // Flock Safety. (Beware 8c:1f:64 — that is "Flock Audio Inc.", a pro-audio
    // company with no relation.) Since CONFIDENCE_ALARM_THRESHOLD dropped to 70, Tier 1
    // alarms unaided at close range, so membership here must mean "this IS
    // Flock", not "this was seen near Flock".
    {"b4:1e:52", OUI_SPECIFIC},   // Flock Safety — IEEE registered, sole entry
    // ── Demoted from Tier 1 on 2026-07-26 after registry lookup ─────────────
    // Every one of these is a real component vendor whose silicon appears both in
    // Flock hardware and in unrelated consumer products, so none can carry alarm
    // weight on its own. They lose nothing that matters: at Tier 2 a wildcard
    // probe still scores SCORE_STRONG + the RSSI bonus = 70, so they DO alarm
    // when exhibiting the camera's channel-hopping behaviour — just not merely
    // by existing nearby.
    {"e4:aa:ea", OUI_GENERIC},    // Liteon Technology Corp — most-observed Flock production OUI
    {"00:09:01", OUI_GENERIC},    // Shenzhen Shixuntong ("XUNTONG") — Flock Penguin battery
    {"a0:b7:65", OUI_GENERIC},    // Espressif Inc — plain ESP32. At Tier 1 this alarmed on our
                                  // OWN hardware: Plume is an ESP32-S3 and the C5 sniffer an
                                  // ESP32-C5. Joins d4:ad:fc / ac:67:b2 already listed below.
    {"f0:82:c0", OUI_GENERIC},    // Silicon Laboratories — commodity IoT radio
    {"b4:e3:f9", OUI_GENERIC},    // Silicon Laboratories
    {"04:0d:84", OUI_GENERIC},    // Silicon Laboratories
    // These two were "pending IEEE lookup" for a long time. Resolved 2026-07-26
    // against three sources: IEEE oui.txt (MA-L, 39k), manuf2 (56k incl. MA-M/
    // MA-S), and Ringmast4r/OUI-Master-Database (88,873 merged — IEEE MA-L +
    // MA-M + MA-S + IAB + CID, plus Wireshark and Nmap). They came back
    // different, and neither is a usable vendor signature:
    //
    //   4c:6e:44 — an IEEE Registration Authority SUBDIVIDED block (which is why
    //     the MA-L-only registries missed it). The 24-bit prefix is split into
    //     /28 and /36 sub-assignments across many unrelated companies — one known
    //     holder is Shenzhen iTayga Technology. So a 3-byte match here identifies
    //     no vendor at all; it is ambiguous by construction. Only a longer prefix
    //     could mean anything.
    //
    //   d8:a0:d8 — ZERO matches across all 88,873 merged entries. Genuinely
    //     unassigned, so it cannot be any production device's registered address.
    //     Almost certainly a transcription error or a randomised/spoofed MAC that
    //     got recorded as an observation.
    //
    // Both retained at Tier 2 for logging only. Delete either if it never
    // produces a corroborated hit.
    {"4c:6e:44", OUI_GENERIC}, {"d8:a0:d8", OUI_GENERIC},
    // ── OUI_GENERIC — commodity / component-vendor silicon ──
    {"74:4c:a1", OUI_GENERIC}, {"94:34:69", OUI_GENERIC}, {"38:5b:44", OUI_GENERIC},
    {"94:08:53", OUI_GENERIC}, {"1c:34:f1", OUI_GENERIC}, {"a4:cf:12", OUI_GENERIC},
    {"d4:ad:fc", OUI_GENERIC},   // Espressif ESP32-S3 (generic commodity silicon)
    {"ac:67:b2", OUI_GENERIC},   // Espressif ESP32-WROOM (generic commodity silicon)
    {"3c:91:80", OUI_GENERIC}, {"80:30:49", OUI_GENERIC}, {"14:5a:fc", OUI_GENERIC}, {"9c:2f:9d", OUI_GENERIC},
    {"c8:c9:a3", OUI_GENERIC}, {"70:c9:4e", OUI_GENERIC},
    {"24:b2:b9", OUI_GENERIC}, {"00:f4:8d", OUI_GENERIC},
    {"08:3a:88", OUI_GENERIC}, {"d8:f3:bc", OUI_GENERIC},
    // Field-validated additions from the NitekryDPaul 31-prefix research list.
    // NitekryDPaul entries #22, #23, #26 — demoted from Tier 1 for consistency
    // Same provenance as all other NitekryDPaul OUIs (field-validated component-vendor)
    {"ec:1b:bd", OUI_GENERIC}, {"58:8e:81", OUI_GENERIC}, {"90:35:ea", OUI_GENERIC},
    {"b8:35:32", OUI_GENERIC}, {"c0:35:32", OUI_GENERIC}, {"f4:6a:dd", OUI_GENERIC}, {"f8:a2:d6", OUI_GENERIC},
    {"e8:d0:fc", OUI_GENERIC}, {"e0:4f:43", OUI_GENERIC}, {"b8:1e:a4", OUI_GENERIC}, {"70:08:94", OUI_GENERIC},
    {"3c:71:bf", OUI_GENERIC}, {"58:00:e3", OUI_GENERIC}, {"5c:93:a2", OUI_GENERIC}, {"64:6e:69", OUI_GENERIC},
    {"48:27:ea", OUI_GENERIC}, {"82:6b:f2", OUI_GENERIC},
    // cc:cc:cc — from NSM-Barii/flock-back, listed there under "FS Ext Battery
    // devices". UNVALIDATED: no capture backs it in that repo, and the pattern
    // reads like a default/placeholder MAC rather than a real vendor allocation,
    // so it is the most false-positive-prone entry in this table. Tier 2 only.
    // NO LONGER CONTAINED. Tier 2 + wildcard probe = SCORE_STRONG (60) + 10 RSSI
    // bonus = 70, which now MEETS the lowered CONFIDENCE_ALARM_THRESHOLD (70).
    // So this unvalidated prefix can sound the alarm on its own at close range.
    // It is the least-evidenced entry in the table — flock-back lists it with no
    // capture behind it and it reads like a placeholder MAC. If you get spurious
    // alarms, delete this line first.
    {"cc:cc:cc", OUI_GENERIC},
    // LiteOn Technology Corporation — WCBN3510A WiFi+BT module
    // Confirmed in Falcon, Sparrow, Falcon Flex, Falcon LR
    // d0:39:57 via NitekryDPaul, e0:0a:f6 via IEEE lookup May 2026
    // e8:2a:44 through 94:97:4f via Field Reference May 2026
    {"d0:39:57", OUI_GENERIC}, {"e0:0a:f6", OUI_GENERIC},
    {"e8:2a:44", OUI_GENERIC}, {"30:d1:6b", OUI_GENERIC}, {"b8:ee:65", OUI_GENERIC},
    {"a4:db:30", OUI_GENERIC}, {"40:f0:2f", OUI_GENERIC}, {"30:52:cb", OUI_GENERIC}, {"94:97:4f", OUI_GENERIC},
};
static const int NUM_MAC_PREFIXES = sizeof(mac_prefixes) / sizeof(mac_prefixes[0]);

static const char* device_name_patterns[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision", "FlockCam", "RWLS-"
};
static const int NUM_NAME_PATTERNS = sizeof(device_name_patterns) / sizeof(device_name_patterns[0]);

// Raven service UUIDs are all Bluetooth-base-derived; only the 16-bit short
// code discriminates them. Store the codes and compare integers on the hot path.
static const uint16_t raven_custom_codes[]   = { 0x3100, 0x3200, 0x3300, 0x3400, 0x3500 };
static const uint16_t raven_standard_codes[] = { 0x180a, 0x1809, 0x1819 };
static const int NUM_RAVEN_CUSTOM_UUIDS   = sizeof(raven_custom_codes)   / sizeof(raven_custom_codes[0]);
static const int NUM_RAVEN_STANDARD_UUIDS = sizeof(raven_standard_codes) / sizeof(raven_standard_codes[0]);

// Extract the 16-bit short code from a canonical base UUID string, e.g.
// "00003100-0000-1000-8000-00805f9b34fb" -> 0x3100. Returns false if the
// string is not in Bluetooth-base form (in which case it cannot be a Raven UUID).
static bool uuid_base_short_code(const char* u, uint16_t* out) {
    if (strlen(u) != 36) return false;
    if (strncmp(u, "0000", 4) != 0) return false;
    if (strcasecmp(u + 8, "-0000-1000-8000-00805f9b34fb") != 0) return false;
    unsigned code;
    if (sscanf(u + 4, "%4x", &code) != 1) return false;
    *out = (uint16_t)code;
    return true;
}

#define FLOCK_MFG_COMPANY_ID 0x09C8

// Axon vendor evidence. IEEE assigns 00:25:DF to Axon Enterprise, Inc.; the
// Bluetooth SIG assigns company ID 0x034D to TASER International, Inc. (Axon's
// former corporate name). 0xFC81 is reported by open detector research but is
// not an adopted SIG service UUID, so it is corroboration only and never
// classifies by itself.
#define AXON_MFG_COMPANY_ID 0x034D
#define AXON_SERVICE_UUID   0xFC81
static const uint8_t AXON_OUI[3] = {0x00, 0x25, 0xDF};

// ── Runtime signature tables (seeded from defaults, optionally replaced by SD) ──
#define MAX_OUI_RT   64
#define MAX_SSID_RT  32
#define MAX_NAME_RT  32
#define SIG_STR_LEN  33     // max substring length incl. NUL
#define SIG_FILE     "/flock_signatures.csv"

struct OuiRT { char prefix[9]; uint8_t bytes[3]; uint8_t tier; };
OuiRT rt_oui[MAX_OUI_RT];                 int rt_oui_count  = 0;
char  rt_ssid[MAX_SSID_RT][SIG_STR_LEN];  int rt_ssid_count = 0;
char  rt_name[MAX_NAME_RT][SIG_STR_LEN];  int rt_name_count = 0;

// Parse an "aa:bb:cc" OUI prefix string into 3 raw bytes. Returns false on
// malformed input. Called only at signature-load time, never on the hot path.
static inline bool oui_prefix_to_bytes(const char* s, uint8_t out[3]) {
    if (!s || strlen(s) != 8 || s[2] != ':' || s[5] != ':') return false;
    static const uint8_t hex_pos[] = {0, 1, 3, 4, 6, 7};
    for (uint8_t i = 0; i < sizeof(hex_pos); i++) {
        if (!isxdigit((unsigned char)s[hex_pos[i]])) return false;
    }
    unsigned a, b, c;
    if (sscanf(s, "%2x:%2x:%2x", &a, &b, &c) != 3) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c;
    return true;
}

void signatures_seed_defaults() {
    rt_oui_count = 0;
    for (int i = 0; i < NUM_MAC_PREFIXES && rt_oui_count < MAX_OUI_RT; i++) {
        strncpy(rt_oui[rt_oui_count].prefix, mac_prefixes[i].prefix, 8);
        rt_oui[rt_oui_count].prefix[8] = '\0';
        oui_prefix_to_bytes(rt_oui[rt_oui_count].prefix, rt_oui[rt_oui_count].bytes);
        rt_oui[rt_oui_count].tier = mac_prefixes[i].tier;
        rt_oui_count++;
    }
    rt_ssid_count = 0;
    for (int i = 0; i < NUM_SSID_PATTERNS && rt_ssid_count < MAX_SSID_RT; i++) {
        strncpy(rt_ssid[rt_ssid_count], wifi_ssid_patterns[i], SIG_STR_LEN - 1);
        rt_ssid[rt_ssid_count][SIG_STR_LEN - 1] = '\0'; rt_ssid_count++;
    }
    rt_name_count = 0;
    for (int i = 0; i < NUM_NAME_PATTERNS && rt_name_count < MAX_NAME_RT; i++) {
        strncpy(rt_name[rt_name_count], device_name_patterns[i], SIG_STR_LEN - 1);
        rt_name[rt_name_count][SIG_STR_LEN - 1] = '\0'; rt_name_count++;
    }
}

// Provenance for the signature database. signatures_load_from_sd() can clear
// and replace the compiled OUI/SSID/name tables from the SD card, and used to do
// it silently — so someone with brief access to the card could remove their own
// hardware from the signature set and the device would keep scanning, keep
// showing green, and simply stop detecting them. This does not prevent that.
// It removes the silent part.
static bool sig_loaded_from_sd = false;
static int  sig_sd_oui = 0, sig_sd_ssid = 0, sig_sd_name = 0;

void signatures_load_from_sd() {
    if (!sd_available || !SD.exists(SIG_FILE)) {
        Serial.println(F("Signatures: compiled defaults (no SD override)"));
        return;
    }
    File f = SD.open(SIG_FILE, FILE_READ);
    if (!f) { Serial.println(F("Signatures: SD open failed, using defaults")); return; }

    // Stage the entire file off to the side and commit only after every
    // recognized row validates. This is a policy transaction: a typo on the
    // first OUI row must never clear a working built-in detector table.
    OuiRT* staged_oui = (OuiRT*)malloc(sizeof(rt_oui));
    char (*staged_ssid)[SIG_STR_LEN] =
        (char (*)[SIG_STR_LEN])malloc(sizeof(rt_ssid));
    char (*staged_name)[SIG_STR_LEN] =
        (char (*)[SIG_STR_LEN])malloc(sizeof(rt_name));
    if (!staged_oui || !staged_ssid || !staged_name) {
        free(staged_oui); free(staged_ssid); free(staged_name);
        f.close();
        Serial.println(F("Signatures: staging allocation failed, using defaults"));
        return;
    }

    memcpy(staged_oui, rt_oui, sizeof(rt_oui));
    memcpy(staged_ssid, rt_ssid, sizeof(rt_ssid));
    memcpy(staged_name, rt_name, sizeof(rt_name));
    int staged_oui_count = rt_oui_count;
    int staged_ssid_count = rt_ssid_count;
    int staged_name_count = rt_name_count;
    bool saw_oui = false, saw_ssid = false, saw_name = false;
    bool valid = true;
    int line_no = 0;
    char line[80];

    while (f.available() && valid) {
        line_no++;
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        if (len == (int)sizeof(line) - 1) {
            while (f.available()) {
                int c = f.read();
                if (c == '\n') break;
            }
            valid = false;
            break;
        }
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == ' ')) line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        char* comma = strchr(line, ',');
        if (!comma) { valid = false; break; }
        *comma = '\0';
        char* type  = line;
        char* value = comma + 1;
        char* tier_str = strchr(value, ',');
        if (tier_str) { *tier_str = '\0'; tier_str++; }

        if (strcasecmp(type, "oui") == 0) {
            if (!saw_oui) { staged_oui_count = 0; saw_oui = true; }
            if (staged_oui_count >= MAX_OUI_RT || strlen(value) != 8 ||
                !oui_prefix_to_bytes(value, staged_oui[staged_oui_count].bytes)) {
                valid = false; break;
            }
            if (tier_str && strcasecmp(tier_str, "specific") != 0 &&
                strcasecmp(tier_str, "generic") != 0) {
                valid = false; break;
            }
            strncpy(staged_oui[staged_oui_count].prefix, value, 8);
            staged_oui[staged_oui_count].prefix[8] = '\0';
            staged_oui[staged_oui_count].tier =
                (tier_str && strcasecmp(tier_str, "specific") == 0) ? OUI_SPECIFIC : OUI_GENERIC;
            staged_oui_count++;
        } else if (strcasecmp(type, "ssid") == 0) {
            if (!saw_ssid) { staged_ssid_count = 0; saw_ssid = true; }
            size_t value_len = strlen(value);
            if (staged_ssid_count >= MAX_SSID_RT || value_len == 0 || value_len >= SIG_STR_LEN) {
                valid = false; break;
            }
            memcpy(staged_ssid[staged_ssid_count], value, value_len + 1);
            staged_ssid_count++;
        } else if (strcasecmp(type, "name") == 0) {
            if (!saw_name) { staged_name_count = 0; saw_name = true; }
            size_t value_len = strlen(value);
            if (staged_name_count >= MAX_NAME_RT || value_len == 0 || value_len >= SIG_STR_LEN) {
                valid = false; break;
            }
            memcpy(staged_name[staged_name_count], value, value_len + 1);
            staged_name_count++;
        }
    }
    f.close();

    bool any_type = saw_oui || saw_ssid || saw_name;
    bool empty_type = (saw_oui && staged_oui_count == 0) ||
                      (saw_ssid && staged_ssid_count == 0) ||
                      (saw_name && staged_name_count == 0);
    if (!valid || !any_type || empty_type) {
        free(staged_oui); free(staged_ssid); free(staged_name);
        Serial.printf("Signatures: invalid SD override at/near line %d, kept previous tables\n",
                      line_no);
        return;
    }

    if (saw_oui) {
        memcpy(rt_oui, staged_oui, sizeof(OuiRT) * staged_oui_count);
        rt_oui_count = staged_oui_count;
    }
    if (saw_ssid) {
        memcpy(rt_ssid, staged_ssid, sizeof(rt_ssid[0]) * staged_ssid_count);
        rt_ssid_count = staged_ssid_count;
    }
    if (saw_name) {
        memcpy(rt_name, staged_name, sizeof(rt_name[0]) * staged_name_count);
        rt_name_count = staged_name_count;
    }
    free(staged_oui); free(staged_ssid); free(staged_name);

    sig_loaded_from_sd = true;
    sig_sd_oui  = saw_oui ? staged_oui_count : 0;
    sig_sd_ssid = saw_ssid ? staged_ssid_count : 0;
    sig_sd_name = saw_name ? staged_name_count : 0;
    Serial.printf("Signatures from SD: OUI=%d SSID=%d NAME=%d (absent types kept defaults)\n",
                  sig_sd_oui, sig_sd_ssid, sig_sd_name);
}

int check_mac_prefix(const uint8_t* mac) {
    for (int i = 0; i < rt_oui_count; i++) {
        if (memcmp(mac, rt_oui[i].bytes, 3) == 0) return rt_oui[i].tier;
    }
    return OUI_NONE;
}

// ── Wildcard-probe behavioral tracker ──────────────────────────────────────
// Tracks which WiFi channels each source MAC has emitted wildcard probe
// requests on within a rolling window. Returns true once a MAC has been
// seen on >= WILDCARD_MIN_CHANNELS distinct channels, indicating the
// channel-hopping pattern characteristic of Flock cameras. Called only
// from process_wifi_event_queue() (single-threaded consumer) — no mutex.
struct WildcardProbe {
    uint8_t       mac[6];
    uint16_t      channel_mask;   // bit (ch-1) set when a wildcard probe seen on that channel
    unsigned long first_seen;     // millis() of first wildcard probe in the current window
};
static WildcardProbe wildcard_tracker[WILDCARD_TRACKER_SIZE];
static int           wildcard_count = 0;

static bool wildcard_probe_observe(const uint8_t* mac, uint8_t channel) {
    if (channel < 1 || channel > MAX_CHANNEL) return false;
    unsigned long now = millis();
    int slot = -1, oldest = 0;

    for (int i = 0; i < wildcard_count; i++) {
        if (memcmp(wildcard_tracker[i].mac, mac, 6) == 0) { slot = i; break; }
        if (wildcard_tracker[i].first_seen < wildcard_tracker[oldest].first_seen) oldest = i;
    }

    if (slot >= 0 && (now - wildcard_tracker[slot].first_seen) > WILDCARD_WINDOW_MS) {
        // Window expired — restart accumulation for this MAC
        wildcard_tracker[slot].channel_mask = 0;
        wildcard_tracker[slot].first_seen   = now;
    }

    if (slot < 0) {
        if (wildcard_count < WILDCARD_TRACKER_SIZE) slot = wildcard_count++;
        else slot = oldest;   // evict oldest entry when full
        memcpy(wildcard_tracker[slot].mac, mac, 6);
        wildcard_tracker[slot].channel_mask = 0;
        wildcard_tracker[slot].first_seen   = now;
    }

    wildcard_tracker[slot].channel_mask |= (uint16_t)(1u << (channel - 1));
    return (__builtin_popcount(wildcard_tracker[slot].channel_mask) >= WILDCARD_MIN_CHANNELS);
}

bool check_ssid_pattern(const char* ssid) {
    if (!ssid || strlen(ssid) == 0) return false;
    for (int i = 0; i < rt_ssid_count; i++) if (strcasestr(ssid, rt_ssid[i])) return true;
    return false;
}

bool is_flock_ssid_format(const char* ssid) {
    if (!ssid) return false;
    if (strncmp(ssid, "Flock-", 6) != 0 && strncmp(ssid, "flock-", 6) != 0) return false;
    const char* suffix = ssid + 6; 
    int len = strlen(suffix);
    if (len < 2 || len > 12) return false;
    for (int i = 0; i < len; i++) {
        if (!isxdigit(suffix[i])) return false;
    }
    return true;
}

bool check_device_name_pattern(const char* name) {
    if (!name || strlen(name) == 0) return false;
    for (int i = 0; i < rt_name_count; i++) if (strcasestr(name, rt_name[i])) return true;
    return false;
}

bool is_penguin_numeric_name(const char* name) {
    if (!name) return false;
    int len = strlen(name);
    if (len != 10) return false;
    for (int i = 0; i < len; i++) {
        if (!isdigit(name[i])) return false;
    }
    return true;
}

static bool manufacturer_id_equals(const uint8_t* mfg_data, size_t mfg_len,
                                   uint16_t expected) {
    if (mfg_len >= 2) {
        uint16_t mfg_id = mfg_data[0] | (mfg_data[1] << 8);
        if (mfg_id == expected) return true;
    }
    return false;
}

bool check_manufacturer_id(const uint8_t* mfg_data, size_t mfg_len) {
    return manufacturer_id_equals(mfg_data, mfg_len, FLOCK_MFG_COMPANY_ID);
}

bool has_tn_serial(const uint8_t* mfg_data, size_t mfg_len) {
    if (mfg_len < 4) return false;
    for (size_t i = 2; i < mfg_len - 1; i++) {
        if (mfg_data[i] == 'T' && mfg_data[i + 1] == 'N') return true;
    }
    return false;
}

// ============================================================================
// HELPER FUNCTIONS & PCAP
// ============================================================================

// Convert a UTC Y/M/D h:m:s to Unix epoch seconds without depending on
// the system timezone. Valid for years 1970..2099.
// Uses int64_t for the final multiplication to avoid the 32-bit
// overflow at days ≈ 24855 (early 2038).
static uint32_t utc_to_epoch(int year, int mon, int day,
                             int hour, int min, int sec) {
    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    if (year < 1970) return 0;
    long y = year;
    long days = (y - 1970) * 365 + (y - 1969) / 4
              - (y - 1901) / 100 + (y - 1601) / 400;
    days += days_before_month[mon - 1];
    // Add leap day if this year is a leap year AND we're past February
    bool is_leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    if (is_leap && mon > 2) days += 1;
    days += (day - 1);
    int64_t total = (int64_t)days * 86400LL
                  + (int64_t)hour * 3600LL
                  + (int64_t)min * 60LL
                  + (int64_t)sec;
    return (uint32_t)total;
}

void format_time_buf(unsigned long total_sec, char* buf, size_t buf_len) {
    // h/m/s are always small; unsigned int + %u is warning-free on all platforms.
    unsigned int h = (unsigned int)(total_sec / 3600);
    unsigned int m = (unsigned int)((total_sec / 60) % 60);
    unsigned int s = (unsigned int)(total_sec % 60);
    snprintf(buf, buf_len, "%02u:%02u:%02u", h, m, s);
}

// Translate detection method codes into a plain-English summary.
static void methods_to_human(const char* methods, char* out, size_t out_len) {
    if (!methods || methods[0] == '\0' || out_len < 4) {
        if (out_len > 0) out[0] = '\0';
        return;
    }
    struct Token { const char* code; const char* human; };
    static const Token tokens[] = {
        {"raven_multi",    "3+ Raven UUIDs"},
        {"raven_custom",   "Raven custom UUID"},
        {"raven_uuid",     "Raven UUID"},
        {"axon_mfg_0x034D", "Axon company ID"},
        {"axon_uuid_fc81",  "Axon service UUID"},
        {"axon_oui",        "Axon vendor OUI"},
        {"mfg_0x09C8",     "Flock mfg ID"},
        {"tn_serial",      "TN serial"},
        {"ssid_fmt",       "Flock SSID format"},
        {"wildcard_probe",    "Wildcard probe (Flock sig)"},
        {"wildcard_probe_t2", "Wildcard probe (Tier 2 OUI)"},
        {"test_flck_cve",     "CVE-2025-59409 probe"},
        {"penguin_num",    "Penguin name"},
        {"name",           "Known name"},
        {"mac_t1",         "Known MAC"},
        {"mac_t2",         "Similar MAC"},
        {"ssid",           "SSID pattern"},
        {"static_addr",    "Static addr"},
        {"addr1_t1",       "Receiver MAC (known)"},
        {"addr1_t2",       "Receiver MAC (similar)"},
        {"pepwave_oui",    "Pepwave router OUI"},
        {"pepwave_ssid",   "Pepwave SSID"},
    };
    static const int N_TOKENS = (int)(sizeof(tokens) / sizeof(tokens[0]));

    out[0] = '\0';
    int off = 0;
    int matches = 0;

    for (int i = 0; i < N_TOKENS; i++) {
        const char* p = methods;
        bool found = false;
        size_t code_len = strlen(tokens[i].code);
        while ((p = strstr(p, tokens[i].code)) != NULL) {
            bool start_ok = (p == methods) || (*(p - 1) == ' ');
            const char* end = p + code_len;
            bool end_ok = (*end == '\0') || (*end == ' ');
            if (start_ok && end_ok) { found = true; break; }
            p++;
        }
        if (!found) continue;

        if (matches > 0) {
            if (off + 3 >= (int)out_len) break;
            out[off++] = ','; out[off++] = ' ';
        }
        int hlen = (int)strlen(tokens[i].human);
        if (off + hlen + 1 > (int)out_len) {
            if (off + 4 < (int)out_len) { out[off++] = '.'; out[off++] = '.'; out[off++] = '.'; }
            break;
        }
        memcpy(out + off, tokens[i].human, hlen);
        off += hlen;
        out[off] = '\0';
        matches++;
    }

    if (matches == 0) {
        strncpy(out, methods, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

const char* confidence_label(int score) {
    if (score >= CONFIDENCE_CERTAIN) return "CERTAIN";
    if (score >= CONFIDENCE_HIGH)    return "HIGH";
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return "MEDIUM";
    return "LOW";
}

// Prefer GPS UTC for PCAP timestamps so captures open in Wireshark with
// real wall-clock time. Falls back to a synthetic monotonic epoch if no
// GPS lock is available. Caller need not hold dataMutex; function acquires it internally.
static void compute_pcap_ts(uint32_t* sec, uint32_t* usec) {
    unsigned long ms = millis();
    bool got_gps = false;
    if (xSemaphoreTakeRecursive(dataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2020 && gps.date.year() <= 2099) {
            uint32_t epoch = utc_to_epoch(
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
            if (epoch > 0) { *sec = epoch; got_gps = true; }
        }
        xSemaphoreGiveRecursive(dataMutex);
    }
    if (!got_gps) {
        *sec = 1700000000UL + (uint32_t)(ms / 1000UL);
    }
    *usec = (uint32_t)((ms % 1000UL) * 1000UL);
}

void write_threat_pcap(const uint8_t* payload, uint32_t length) {
    if (!sd_available) return;
    uint32_t capture_len = (length > 256) ? 256 : length;
    uint32_t ts_sec = 0, ts_usec = 0;
    compute_pcap_ts(&ts_sec, &ts_usec);
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    if (pcap_write_count < MAX_PCAP_BUFFER) {
        pcap_write_buffer[pcap_write_count].ts_sec  = ts_sec;
        pcap_write_buffer[pcap_write_count].ts_usec = ts_usec;
        pcap_write_buffer[pcap_write_count].incl_len = capture_len;
        pcap_write_buffer[pcap_write_count].orig_len = length;
        memcpy(pcap_write_buffer[pcap_write_count].payload, payload, capture_len);
        pcap_write_count++;
    } else {
        sd_wifi_pcap_dropped++;
    }
    xSemaphoreGiveRecursive(dataMutex);
}

void write_ble_pcap(const uint8_t* payload, uint32_t length) {
    if (!sd_available) return;
    uint32_t capture_len = (length > 256) ? 256 : length;
    uint32_t ts_sec = 0, ts_usec = 0;
    compute_pcap_ts(&ts_sec, &ts_usec);
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    if (ble_pcap_write_count < MAX_PCAP_BUFFER) {
        ble_pcap_write_buffer[ble_pcap_write_count].ts_sec  = ts_sec;
        ble_pcap_write_buffer[ble_pcap_write_count].ts_usec = ts_usec;
        ble_pcap_write_buffer[ble_pcap_write_count].incl_len = capture_len;
        ble_pcap_write_buffer[ble_pcap_write_count].orig_len = length;
        memcpy(ble_pcap_write_buffer[ble_pcap_write_count].payload, payload, capture_len);
        ble_pcap_write_count++;
    } else {
        sd_ble_pcap_dropped++;
    }
    xSemaphoreGiveRecursive(dataMutex);
}

// ============================================================================
// HTTP EXPORT SERVER
// ============================================================================

// Returns true if the request is authenticated. If not, sends a 401
// response and returns false — caller should return immediately.
static bool export_check_auth() {
    if (!export_server) return false;
    if (!export_server->authenticate(export_auth_user, export_auth_pass)) {
        export_server->requestAuthentication(BASIC_AUTH, "Plume");
        return false;
    }
    return true;
}

// ── Export page HTML template (stored in flash via PROGMEM) ──────────────
// Dynamic placeholders (in order):
//   %s  = export_auth_user
//   %s  = export_auth_pass (mousedown reveal)
//   %s  = export_auth_pass (touchstart reveal)
//   %s  = file rows HTML (built separately based on sd_available)
//   %u  = remaining minutes
//   %02u = remaining seconds
//   %d  = timer bar fill percentage (0-100)
//   %s  = VERSION_STRING (footer)
//   %s  = signature source, "SD" or "BUILT-IN" (footer)
//   %lu = remaining milliseconds (for JS countdown)
static const char EXPORT_PAGE_TEMPLATE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plume Export</title>
<style>
:root{--bg:#050A14;--card:#1D3258;--cb:#2E4670;--h:#4DDBC2;--t:#E8EFFF;
--d:#95A5B8;--p:#8B7CDB;--ca:#FFB547;--hd:rgba(77,219,194,.15);
--gl:rgba(46,70,112,.5);
--mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,"Liberation Mono",monospace}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:var(--mono);background:var(--bg);color:var(--t);
min-height:100vh;overflow-x:hidden}
body::before{content:'';position:fixed;inset:0;background:repeating-linear-gradient(
135deg,transparent,transparent 7px,var(--gl) 7px,var(--gl) 8px);opacity:.25;
pointer-events:none;z-index:0}
.pg{position:relative;z-index:1;max-width:520px;margin:0 auto;padding:24px 20px 40px}
.hs{display:flex;align-items:center;justify-content:space-between;padding-bottom:12px;
border-bottom:1px solid var(--cb);margin-bottom:20px}
.ht{font-family:var(--mono);font-size:14px;color:var(--h);
letter-spacing:3px;text-transform:uppercase}
.pl{display:inline-flex;align-items:center;gap:4px;border:1px solid;border-radius:6px;
padding:2px 10px;font-family:var(--mono);font-size:11px;
letter-spacing:1px;line-height:1.4;white-space:nowrap;text-decoration:none;
transition:all .2s ease}
.po{border-color:rgba(149,165,184,.4);color:var(--t);background:rgba(149,165,184,.12)}
.ph{border-color:var(--h);color:var(--bg);background:var(--h);font-weight:600}
.ph:hover{filter:brightness(1.15);box-shadow:0 0 12px rgba(77,219,194,.4)}
.pp{border-color:var(--p);color:var(--bg);background:var(--p);font-weight:600}
.pp:hover{filter:brightness(1.15);box-shadow:0 0 12px rgba(139,124,219,.4)}
.sb{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--h);
border-radius:8px;padding:6px 14px;background:var(--hd);color:var(--h);
font-size:12px;letter-spacing:2px;font-weight:500;
animation:bg 3s ease-in-out infinite}
.sb .dt{width:6px;height:6px;border-radius:50%%;background:var(--h);
animation:dp 1.5s ease-in-out infinite}
@keyframes bg{0%%,100%%{box-shadow:0 0 8px rgba(77,219,194,.1)}
50%%{box-shadow:0 0 16px rgba(77,219,194,.2)}}
@keyframes dp{0%%,100%%{opacity:.5}50%%{opacity:1}}
.sl{font-family:var(--mono);font-size:11px;color:var(--h);
letter-spacing:3px;text-transform:uppercase;margin-bottom:10px;padding-bottom:6px;
border-bottom:1px solid var(--cb)}
.cd{border:1px solid var(--cb);border-radius:6px;padding:14px 16px;margin-bottom:14px;
background:transparent;transition:border-color .25s ease}
.cd:hover{border-color:rgba(77,219,194,.35)}
.kv{display:flex;justify-content:space-between;align-items:center;padding:4px 0}
.kl{font-size:11px;color:var(--h);letter-spacing:2px;text-transform:uppercase}
.kv2{font-size:13px;color:var(--t);font-weight:500;font-family:var(--mono);
letter-spacing:1px}
.fr{display:flex;align-items:center;justify-content:space-between;padding:10px 0;
border-bottom:1px solid rgba(46,70,112,.3);transition:background .15s}
.fr:last-child{border-bottom:none}
.fr:hover{background:rgba(77,219,194,.04);margin:0 -16px;padding-left:16px;
padding-right:16px;border-radius:4px}
.fi{display:flex;flex-direction:column;gap:3px}
.fn{font-size:13px;color:var(--t);font-weight:500;letter-spacing:.5px}
.fd{font-size:10px;color:var(--d);letter-spacing:1px;text-transform:uppercase}
.fs{width:20px;height:20px;margin-right:10px;flex-shrink:0;color:var(--h)}
.fp{color:var(--p)}
.fl{display:flex;align-items:center}
a.pl{cursor:pointer}a.pl:active{transform:scale(.96)}
.ft{display:flex;justify-content:space-between;align-items:center;margin-top:20px;
padding-top:12px;border-top:1px solid var(--cb)}
.fd2{font-size:10px;color:var(--d);letter-spacing:1px}
.tb{height:3px;background:var(--cb);border-radius:2px;margin-top:8px;overflow:hidden}
.tf{height:100%%;background:var(--h);border-radius:2px;transition:width 1s linear}
.ws{display:flex;align-items:center;gap:8px;padding:8px 12px;
border:1px solid rgba(255,181,71,.3);border-radius:6px;
background:rgba(255,181,71,.12);margin-bottom:14px}
.ws span{font-size:11px;color:var(--ca);letter-spacing:1px}
.fi2{opacity:0;transform:translateY(10px);animation:su .4s ease forwards}
@keyframes su{to{opacity:1;transform:translateY(0)}}
.fi2:nth-child(1){animation-delay:.05s}.fi2:nth-child(2){animation-delay:.12s}
.fi2:nth-child(3){animation-delay:.19s}.fi2:nth-child(4){animation-delay:.26s}
.fi2:nth-child(5){animation-delay:.33s}.fi2:nth-child(6){animation-delay:.4s}
.fi2:nth-child(7){animation-delay:.47s}
@media(max-width:400px){.pg{padding:16px 14px 32px}
.fr{flex-direction:column;align-items:flex-start;gap:8px}
.fr a.pl{align-self:flex-end}}
</style></head><body><div class="pg">
<div class="hs fi2"><span class="ht">Plume</span>
<div style="display:flex;gap:6px;align-items:center">
<span class="pl po">)rawhtml" VERSION_SHORT R"rawhtml(</span><span class="pl ph">EXP</span></div></div>
<div class="fi2" style="margin-bottom:18px"><div class="sb">
<span class="dt"></span>EXPORT ACTIVE</div></div>
<div class="ws fi2">
<svg width="14" height="14" viewBox="0 0 14 14" fill="none">
<path d="M7 1L13 12H1L7 1Z" stroke="#FFB547" stroke-width="1.5" fill="none"/>
<line x1="7" y1="5" x2="7" y2="8" stroke="#FFB547" stroke-width="1.5" stroke-linecap="round"/>
<circle cx="7" cy="10" r=".8" fill="#FFB547"/></svg>
<span>All scanning paused during export</span></div>
<div class="fi2"><div class="sl">Credentials</div><div class="cd">
<div class="kv"><span class="kl">User</span><span class="kv2">%s</span></div>
<div class="kv"><span class="kl">Pass</span><span class="kv2" style="display:flex;align-items:center;gap:6px"><span id="pw" style="letter-spacing:2px">&#x2022;&#x2022;&#x2022;&#x2022;</span><span id="pe" onmousedown="document.getElementById('pw').textContent='%s'" onmouseup="document.getElementById('pw').textContent='&#x2022;&#x2022;&#x2022;&#x2022;'" ontouchstart="document.getElementById('pw').textContent='%s'" ontouchend="document.getElementById('pw').textContent='&#x2022;&#x2022;&#x2022;&#x2022;'" style="cursor:pointer;opacity:0.5;font-size:10px">&#x1F441;</span></span></div>
</div></div>
<div class="fi2"><div class="sl">Files</div><div class="cd">%s</div></div>
<div class="fi2"><div class="sl">Session</div><div class="cd">
<div class="kv"><span class="kl">Time Left</span>
<span class="kv2" id="tm">%um %02us</span></div>
<div class="tb"><div class="tf" id="tf" style="width:%d%%"></div></div>
</div></div>
<div class="ft fi2"><span class="fd2">%s &middot; SIGS: %s</span>
<span class="pl po" style="font-size:10px">
<svg width="8" height="8" viewBox="0 0 8 8" fill="none">
<circle cx="4" cy="4" r="3" stroke="currentColor" stroke-width="1"/>
<line x1="4" y1="2" x2="4" y2="4.5" stroke="currentColor" stroke-width="1" stroke-linecap="round"/>
<line x1="4" y1="4.5" x2="5.5" y2="5.5" stroke="currentColor" stroke-width="1" stroke-linecap="round"/>
</svg>Auto-exit</span></div></div>
<script>var r=%lu;setInterval(function(){r-=1000;if(r<0)r=0;
var m=Math.floor(r/60000),s=Math.floor((r%%60000)/1000);
var e=document.getElementById('tm');if(e)e.textContent=m+'m '+String(s).padStart(2,'0')+'s';
var f=document.getElementById('tf');if(f)f.style.width=(r/600000*100)+'%%';},1000);
</script></body></html>)rawhtml";

static const char FILE_ICON_WIFI[] PROGMEM =
    "<svg class=\"fs\" viewBox=\"0 0 20 20\" fill=\"none\">"
    "<polygon points=\"10,3 18,17 2,17\" stroke=\"currentColor\" "
    "stroke-width=\"1.5\" fill=\"none\"/></svg>";

static const char FILE_ICON_BLE[] PROGMEM =
    "<svg class=\"fs fp\" viewBox=\"0 0 20 20\" fill=\"none\">"
    "<polygon points=\"10,2 18,10 10,18 2,10\" stroke=\"currentColor\" "
    "stroke-width=\"1.5\" fill=\"none\"/></svg>";

static const char DL_ICON[] PROGMEM =
    "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\" fill=\"none\">"
    "<path d=\"M5 1V7M5 7L2 4.5M5 7L8 4.5\" stroke=\"#050A14\" "
    "stroke-width=\"1.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
    "<line x1=\"1\" y1=\"9\" x2=\"9\" y2=\"9\" stroke=\"#050A14\" "
    "stroke-width=\"1.5\" stroke-linecap=\"round\"/></svg>";

static int build_file_row(char* buf, size_t buf_size,
                          const char* href, const char* name,
                          const char* desc, const char* icon,
                          const char* pill_class) {
    return snprintf(buf, buf_size,
        "<div class=\"fr\"><div class=\"fl\">%s"
        "<div class=\"fi\"><span class=\"fn\">%s</span>"
        "<span class=\"fd\">%s</span></div></div>"
        "<a href=\"%s\" class=\"pl %s\">%s DL</a></div>",
        icon, name, desc, href, pill_class, DL_ICON);
}

void export_server_setup_routes() {
    if (!export_server) return;

    export_server->on("/", HTTP_GET, []() {
        if (!export_check_auth()) return;

        // ── Build the dynamic file-rows block ──
        char file_rows[2048] = "";
        int foff = 0;
        if (sd_available) {
            foff += build_file_row(file_rows + foff, sizeof(file_rows) - foff,
                "/PlumeLog.csv", "PlumeLog.csv", "Detections log",
                FILE_ICON_WIFI, "ph");
            foff += build_file_row(file_rows + foff, sizeof(file_rows) - foff,
                "/Threats.pcap", "Threats.pcap", "WiFi packet capture",
                FILE_ICON_WIFI, "pp");
            foff += build_file_row(file_rows + foff, sizeof(file_rows) - foff,
                "/BLE_Threats.pcap", "BLE_Threats.pcap", "Bluetooth capture",
                FILE_ICON_BLE, "pp");
        } else {
            snprintf(file_rows, sizeof(file_rows),
                "<span style=\"font-size:11px;color:#95A5B8;letter-spacing:1px\">"
                "SD card unavailable</span>");
        }

        // ── Compute time remaining ──
        unsigned long elapsed = millis() - export_mode_started_at;
        unsigned long remaining_ms = (elapsed < EXPORT_MODE_MAX_MS)
                                   ? (EXPORT_MODE_MAX_MS - elapsed) : 0;
        unsigned int rm = (unsigned int)(remaining_ms / 60000UL);
        unsigned int rs = (unsigned int)((remaining_ms / 1000UL) % 60);
        int fill_pct = (int)(remaining_ms * 100UL / EXPORT_MODE_MAX_MS);

        // ── Render into a heap buffer ──
        // Template is ~3.5KB, dynamic content adds ~500B, total < 5KB.
        const size_t PAGE_BUF_SIZE = 10240;
        char* page = (char*)malloc(PAGE_BUF_SIZE);
        if (!page) {
            export_server->send(503, "text/plain", "Low memory");
            return;
        }

        snprintf(page, PAGE_BUF_SIZE, EXPORT_PAGE_TEMPLATE,
            export_auth_user,           // %s  credentials user
            export_auth_pass,           // %s  credentials pass (mousedown)
            export_auth_pass,           // %s  credentials pass (touchstart)
            file_rows,                  // %s  file row HTML
            rm,                         // %u  minutes
            rs,                         // %02u seconds
            fill_pct,                   // %d  timer bar width
            VERSION_STRING,             // %s  footer version
            sig_loaded_from_sd ? "SD" : "BUILT-IN",  // %s  signature provenance
            (unsigned long)remaining_ms // %lu JS countdown seed
        );

        export_server->sendHeader("Connection", "close");
        export_server->send(200, "text/html", page);
        free(page);
    });

    auto serve_sd_file = [](const char* path, const char* mime) {
        if (!export_check_auth()) return;
        export_mode_started_at = millis();
        if (!sd_available) { export_server->sendHeader("Connection", "close"); export_server->send(503, "text/plain", "SD unavailable"); return; }

        // Phase 1: stat the file under mutex to get size and confirm existence.
        size_t total = 0;
        {
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                export_server->sendHeader("Connection", "close");
                export_server->send(503, "text/plain", "SD busy");
                return;
            }
            File f = SD.open(path, FILE_READ);
            if (!f) {
                xSemaphoreGive(sdMutex);
                export_server->sendHeader("Connection", "close");
                export_server->send(404, "text/plain", "Not found");
                return;
            }
            total = f.size();
            f.close();
            xSemaphoreGive(sdMutex);
        }

        if (total == 0) { export_server->sendHeader("Connection", "close"); export_server->send(200, mime, ""); return; }

        export_server->setContentLength(total);
        export_server->sendHeader("Connection", "close");
        export_server->sendHeader("Content-Disposition",
                                  String("attachment; filename=\"") + (path + 1) + "\"");
        export_server->send(200, mime, "");

        WiFiClient client = export_server->client();
        static uint8_t buf[512];
        size_t offset = 0;

        // Phase 2: stream in 1KB chunks, acquiring sdMutex only for each
        // individual read so flush_sd_buffer and PersistTask can interleave.
        // Re-open + seek each chunk — the Arduino SD File handle is not safe
        // to hold across mutex release boundaries.
        while (offset < total && client.connected()) {
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                Serial.println("[EXPORT] sdMutex timeout mid-transfer, aborting");
                break;
            }
            File f = SD.open(path, FILE_READ);
            if (!f) {
                xSemaphoreGive(sdMutex);
                Serial.println("[EXPORT] Re-open failed mid-transfer, aborting");
                break;
            }
            if (!f.seek(offset)) {
                f.close();
                xSemaphoreGive(sdMutex);
                Serial.println("[EXPORT] Seek failed mid-transfer, aborting");
                break;
            }
            size_t to_read = ((total - offset) < sizeof(buf)) ? (total - offset) : sizeof(buf);
            int n = f.read(buf, to_read);
            f.close();
            xSemaphoreGive(sdMutex);

            if (n <= 0) break;
            if (client.write(buf, n) == 0) break;
            offset += n;
            esp_task_wdt_reset();
            yield();
        }
    };

    export_server->on("/PlumeLog.csv", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/PLUME/logs/PlumeLog.csv", "text/csv");
    });
    export_server->on("/Threats.pcap", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/PLUME/captures/Threats.pcap", "application/vnd.tcpdump.pcap");
    });
    export_server->on("/BLE_Threats.pcap", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/PLUME/captures/BLE_Threats.pcap", "application/vnd.tcpdump.pcap");
    });

    export_server->onNotFound([]() {
        if (!export_check_auth()) return;
        export_server->sendHeader("Connection", "close");
        export_server->send(404, "text/plain", "Not found");
    });
}

// ── BLE pool — moved above export functions so export_mode_start and
// export_restore_promiscuous can reference ble_cb_singleton and the pool.
struct BleEventData {
    uint8_t  mac[6];
    uint8_t  addr_type;
    int8_t   rssi;
    int8_t   tx_power;
    bool     have_tx_power;
    char     dev_name[65];
    bool     have_name;
    uint8_t  mfg_data[64];
    uint8_t  mfg_data_len;
    bool     have_mfg;
    char     service_uuids[5][37];
    uint8_t  uuid_count;
    uint8_t  adv_channel;  // 37/38/39 if available, 0 if unknown
    volatile uint32_t in_use;  // pool slot occupancy flag (0 = free, 1 = occupied)
};

// Static pool — eliminates all malloc/free from the BLE advertisement
// hot path. 6 slots × ~380 bytes ≈ 2.3KB static cost, zero fragmentation.
#define BLE_POOL_SIZE 4
static BleEventData ble_pool[BLE_POOL_SIZE];
// Written only from NimBLE's scan callback (single FreeRTOS task context,
// never re-entrant). Atomic store ensures the cursor advance is visible
// to ble_worker_task on Core 1 after the pool slot's in_use flag is set.
static volatile uint32_t ble_pool_write = 0;

class AdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (!scanner_ready) return;
        // Claim a pool slot. If the slot is still being processed by the
        // worker task, drop this advertisement — better than heap-allocating.
        uint32_t slot = __atomic_load_n(&ble_pool_write, __ATOMIC_ACQUIRE);
        uint32_t next = (slot + 1) % BLE_POOL_SIZE;

        // Atomically claim this slot by advancing the write cursor.
        // If another callback already advanced it, CAS fails — drop this ad.
        if (!__atomic_compare_exchange_n(&ble_pool_write, &slot, next,
                                          false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return;
        }

        // We now own 'slot' exclusively. If the worker hasn't drained it yet,
        // drop the advertisement — don't revert the cursor.
        if (__atomic_load_n(&ble_pool[slot].in_use, __ATOMIC_ACQUIRE)) {
            return;
        }

        BleEventData* ev = &ble_pool[slot];
        memset(ev, 0, sizeof(BleEventData));

        NimBLEAddress addr = advertisedDevice->getAddress();
        // NimBLE stores addresses little-endian; we display big-endian.
        // Pulling the raw bytes avoids a heap-allocated std::string and a
        // sscanf round-trip on every advertisement (50–200/sec under
        // typical urban traffic — the steady alloc churn was the most
        // consistent heap-fragmentation source in the code).
        const uint8_t* native = addr.getVal();
        for (int i = 0; i < 6; i++) ev->mac[i] = native[5 - i];
        ev->addr_type = addr.getType();

        ev->rssi          = (int8_t)advertisedDevice->getRSSI();
        ev->have_tx_power = advertisedDevice->haveTXPower();
        ev->tx_power      = ev->have_tx_power ? (int8_t)advertisedDevice->getTXPower() : 0;

        ev->have_name = advertisedDevice->haveName();
        if (ev->have_name) {
            strncpy(ev->dev_name, advertisedDevice->getName().c_str(), 64);
            ev->dev_name[64] = '\0';
        } else {
            strcpy(ev->dev_name, "Unknown");
        }

        ev->have_mfg = advertisedDevice->haveManufacturerData();
        if (ev->have_mfg) {
            std::string mfg = advertisedDevice->getManufacturerData();
            ev->mfg_data_len = (uint8_t)(mfg.size() > 64 ? 64 : mfg.size());
            memcpy(ev->mfg_data, mfg.data(), ev->mfg_data_len);
        }

        if (advertisedDevice->haveServiceUUID()) {
            int count = advertisedDevice->getServiceUUIDCount();
            ev->uuid_count = (uint8_t)(count > 5 ? 5 : count);
            for (int i = 0; i < ev->uuid_count; i++) {
                std::string uuid = advertisedDevice->getServiceUUID(i).toString();
                strncpy(ev->service_uuids[i], uuid.c_str(), 36);
                ev->service_uuids[i][36] = '\0';
            }
        }

        // NimBLE does not reliably expose advertising channel on ESP32.
        ev->adv_channel = 0;

        // Mark slot as occupied — cursor was already advanced by the CAS above.
        __atomic_store_n(&ev->in_use, 1u, __ATOMIC_RELEASE);

        // Queue the slot index. If the queue is full, release the slot.
        uint8_t idx = (uint8_t)slot;
        if (xQueueSend(ble_event_queue, &idx, 0) != pdTRUE) {
            __atomic_store_n(&ev->in_use, 0u, __ATOMIC_RELEASE);
        }
    }
};

// Single shared instance — passed to setScanCallbacks at boot
// and on every periodic NimBLE restart. Avoids the slow heap leak that
// `new AdvertisedDeviceCallbacks()` produced every restart cycle.
static AdvertisedDeviceCallbacks ble_cb_singleton;

// Restore the promiscuous sniffer after export mode is finished or aborted.
static void export_restore_promiscuous() {
    // Fully release WiFi resources before NimBLE init — the WiFi station
    // + TCP stack fragments heap; turning WiFi OFF lets the allocator
    // coalesce free blocks so NimBLE can get its contiguous 20-30KB.
    //
    // Second argument is eraseap: this resolves to
    // WiFiSTAClass::disconnect(wifioff, eraseap, timeout), so the previous
    // single-arg form meant wifioff=true and left any NVS-cached AP config in
    // place. Erasing it here cleans up credentials cached by builds from before
    // WiFi.persistent(false). Deliberately NOT done in the promiscuous-mode
    // setup path or the BLE-restart path — both run repeatedly, and erasing a
    // config we never write is pointless flash churn.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(WIFI_MODE_SETTLE_LONG_MS);

    // Reinitialize NimBLE while WiFi is off (maximum heap available)
    NimBLEDevice::init("");
    NimBLEDevice::setPowerLevel(BLE_TX_POWER);
    pBLEScan = NimBLEDevice::getScan();
    if (pBLEScan) {
        pBLEScan->setScanCallbacks(&ble_cb_singleton, false);
        pBLEScan->setActiveScan(false);
        apply_ble_scan_params();
        pBLEScan->setMaxResults(0);
    } else {
        Serial.println("[BLE] getScan() returned null after reinit — BLE unavailable");
    }
    last_ble_restart_ms = millis();

    // Now restart WiFi for promiscuous scanning
    WiFi.mode(WIFI_STA);
    delay(WIFI_MODE_SETTLE_SHORT_MS);
    wifi_promiscuous_filter_t pf_restore;
    pf_restore.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&pf_restore);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);

    // Resume scanner task and re-subscribe to WDT
    if (ScannerTaskHandle) {
        vTaskResume(ScannerTaskHandle);
        esp_task_wdt_add(ScannerTaskHandle);
    }
    scanner_ready = true;
}

// Finish the connect sequence once WiFi.status() == WL_CONNECTED.
// Returns true on success, false if server allocation failed (in which case
// promiscuous has already been restored and a toast has been shown).
static bool export_finalize_connect() {
    IPAddress ip = WiFi.localIP();
    snprintf(export_ip_str, sizeof(export_ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    export_derive_password();

    export_server = new(std::nothrow) WebServer(80);
    if (!export_server) {
        set_toast_direct("EXPORT ALLOC FAIL", TOAST_WARNING, false);
        export_restore_promiscuous();
        return false;
    }
    export_server_setup_routes();
    export_server->begin();

    export_grid_needs_reset = true;
    export_mode_active = true;
    export_mode_started_at = millis();

    set_toast_direct("EXPORT ACTIVE", TOAST_SUCCESS);
    return true;
}

// Kick off the WiFi connect. Returns true if the attempt was started; the
// caller should not assume export_mode_active is set on return. The
// non-blocking poll in loop() (export_tick_connect) completes or aborts it.
bool export_mode_start() {
    flush_pending_deletes();
    if (export_mode_active || export_connecting) return true;
    if (strlen(export_ssid) == 0) {
        set_toast_direct("SET WIFI IN MENU", TOAST_WARNING, false);
        return false;
    }
    if (esp_get_free_heap_size() < 15000) {
        set_toast_direct("LOW MEMORY", TOAST_WARNING, false);
        return false;
    }

    // Shut down all scanning before joining a network
    scanner_ready = false;
    esp_wifi_set_promiscuous(false);
    if (ScannerTaskHandle) {
        esp_task_wdt_delete(ScannerTaskHandle);
        vTaskSuspend(ScannerTaskHandle);
    }
    if (pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        pBLEScan->clearResults();
    }

    // Free NimBLE heap (~20-30KB) for WiFi TCP stack
    xQueueReset(ble_event_queue);
    for (int i = 0; i < BLE_POOL_SIZE; i++) {
        __atomic_store_n(&ble_pool[i].in_use, 0u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&ble_pool_write, 0u, __ATOMIC_RELEASE);
    NimBLEDevice::deinit(true);
    pBLEScan = nullptr;

    WiFi.disconnect(true);
    delay(WIFI_MODE_SETTLE_MEDIUM_MS);
    WiFi.mode(WIFI_STA);
    // Re-asserted here as well as in setup(). This is the one call that would
    // write the password into NVS, so the guard sits directly against it rather
    // than relying on a setting made a thousand lines earlier surviving every
    // mode change in between.
    WiFi.persistent(false);
    WiFi.begin(export_ssid, export_pass);

    export_connecting = true;
    export_connect_start_ms = millis();
    set_toast_direct("CONNECTING...", TOAST_SUCCESS);
    return true;
}

// Called every loop() iteration while a connect attempt is pending.
// Transitions to active on success or restores sniffer + toast on timeout.
void export_tick_connect() {
    if (!export_connecting) return;
    if (WiFi.status() == WL_CONNECTED) {
        export_connecting = false;
        export_finalize_connect();
        return;
    }
    if (millis() - export_connect_start_ms >= EXPORT_CONNECT_TIMEOUT_MS) {
        export_connecting = false;
        set_toast_direct("WIFI CONNECT FAIL", TOAST_WARNING, false);
        export_restore_promiscuous();
    }
}

void export_mode_stop() {
    // Cancel a pending connect attempt.
    if (export_connecting) {
        export_connecting = false;
        export_restore_promiscuous();
        set_toast_direct("EXPORT CANCELLED", TOAST_NEUTRAL);
        return;
    }
    if (!export_mode_active) return;
    if (export_server) {
        export_server->stop();
        delete export_server;
        export_server = nullptr;
    }
    export_restore_promiscuous();
    export_mode_active = false;
    set_toast_direct("EXPORT MODE OFF", TOAST_NEUTRAL);
}

// ============================================================================
// PERSISTENCE & TRACKING
// ============================================================================

void save_detections_to_flash() {
    if (!littlefs_available) return;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    int cnt = capture_history_count;
    CaptureEntry local_hist[CAPTURE_HISTORY_SIZE];
    for (int i = 0; i < cnt; i++) local_hist[i] = capture_history[i];
    xSemaphoreGiveRecursive(dataMutex);

    littlefs_atomic_write(DETECT_FILE, [&](File& f) -> bool {
        for (int i = 0; i < cnt; i++) {
            int r = f.printf("%s|%s|%s|%d|%.6f|%.6f\n",
                local_hist[i].type, local_hist[i].mac, local_hist[i].name,
                local_hist[i].confidence, local_hist[i].lat, local_hist[i].lng);
            if (r <= 0) return false;
        }
        return true;
    });
}


static void perform_detection_delete(int idx) {
    if (!take_data_mutex()) return;

    // Snapshot the target MAC and timestamp before mutating sd_hist[]
    char target_mac[18] = "";
    unsigned long target_uptime_ms = 0;
    int  target_rssi = 0;

    if (sd_available && idx >= 0 && idx < sd_hist_count) {
        safe_copy(target_mac, sd_hist[idx].mac, sizeof(target_mac));
        target_uptime_ms = sd_hist[idx].uptime_ms;
        target_rssi = sd_hist[idx].rssi;
        Serial.printf("[DELDIAG] snapshot: idx=%d mac=%s uptime_ms=%lu (sd_hist_count=%d)\n",
                      idx, target_mac, target_uptime_ms, sd_hist_count);

        // Remove from in-memory sd_hist[] by shifting
        for (int i = idx; i < sd_hist_count - 1; i++) sd_hist[i] = sd_hist[i + 1];
        sd_hist_count--;

        // Also remove matching entry from capture_history[] (by MAC)
        for (int i = 0; i < capture_history_count; i++) {
            if (strncmp(capture_history[i].mac, target_mac, 17) == 0) {
                for (int j = i; j < capture_history_count - 1; j++) {
                    capture_history[j] = capture_history[j + 1];
                }
                capture_history_count--;
                break;
            }
        }

        if (history_selected_idx >= sd_hist_count)
            history_selected_idx = max(0, sd_hist_count - 1);
        if (history_scroll_offset > max(0, sd_hist_count - HIST_VISIBLE_ROWS))
            history_scroll_offset = max(0, sd_hist_count - HIST_VISIBLE_ROWS);

    } else if (!sd_available && idx >= 0 && idx < capture_history_count) {
        // No SD — only the in-memory list exists
        safe_copy(target_mac, capture_history[idx].mac, sizeof(target_mac));
        for (int i = idx; i < capture_history_count - 1; i++)
            capture_history[i] = capture_history[i + 1];
        capture_history_count--;
        if (history_selected_idx >= capture_history_count)
            history_selected_idx = max(0, capture_history_count - 1);
    }

    // Refresh seen_mac_table so the redetect window restarts
    if (target_mac[0] != '\0') add_seen_mac(target_mac);

    give_data_mutex();

    // Enqueue for batched rewrite — actual CSV rewrite deferred to
    // flush_pending_deletes() called on screen-leave or 5s idle.
    if (sd_available && target_mac[0] != '\0') {
        if (pending_delete_count < MAX_PENDING_DELETES) {
            safe_copy(pending_deletes[pending_delete_count].mac,
                      target_mac, sizeof(pending_deletes[0].mac));
            pending_deletes[pending_delete_count].uptime_ms = target_uptime_ms;
            pending_deletes[pending_delete_count].rssi = target_rssi;
            pending_delete_count++;
            pending_delete_dirty_ms = millis();
        }
    }

    set_toast_direct("DETECTION DELETED", TOAST_WARNING, false);
}

// Rewrite PlumeLog.csv once, removing every MAC in pending_deletes[].
// Called on screen-leave, 5s idle, export start, or stats clear.
static void flush_pending_deletes() {
    if (pending_delete_count == 0) return;
    Serial.printf("[DELDIAG] flush ENTER: pending=%d\n", pending_delete_count);
    for (int p = 0; p < pending_delete_count; p++) {
        Serial.printf("[DELDIAG]   pending[%d]: mac=%s uptime_ms=%lu\n",
                      p, pending_deletes[p].mac, pending_deletes[p].uptime_ms);
    }
    if (!sd_available) { pending_delete_count = 0; pending_delete_dirty_ms = 0; return; }
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) return;

    const char* tmp_path = "/PLUME/logs/PlumeLog.tmp";
    const char* bak_path = "/PLUME/logs/PlumeLog.bak";

    File src = SD.open(current_log_file, FILE_READ);
    if (!src) { xSemaphoreGive(sdMutex); return; }

    File dst = SD.open(tmp_path, FILE_WRITE);
    if (!dst) { src.close(); xSemaphoreGive(sdMutex); return; }

    char line[SD_LINE_LEN];
    int  skipped = 0;

    while (src.available()) {
        int len = 0;
        while (src.available() && len < (int)sizeof(line) - 1) {
            char c = src.read();
            if (c == '\n') break;
            line[len++] = c;
        }
        while (len > 0 && (line[len-1] == '\r')) len--;
        line[len] = '\0';
        if (len == 0) continue;

        // Always keep the header row
        if (strncmp(line, "Uptime_ms", 9) == 0) { dst.println(line); continue; }

        // Parse col 7 (MAC) and col 0 (Uptime_ms — raw integer)
        char row_mac[18] = "";
        unsigned long row_uptime_ms = 0;
        int  col = 0, start = 0;
        for (int i = 0; i <= len; i++) {
            if (line[i] == ',' || line[i] == '\0') {
                int flen = i - start;
                if (col == 0 && flen > 0) {
                    row_uptime_ms = strtoul(line + start, NULL, 10);
                } else if (col == 7 && flen == 17) {
                    memcpy(row_mac, line + start, 17); row_mac[17] = '\0';
                }
                col++; start = i + 1;
            }
        }

        // Check against every pending delete
        for (int p = 0; p < pending_delete_count; p++) {
            if (strncmp(row_mac, pending_deletes[p].mac, 17) == 0) {
                Serial.printf("[DELDIAG]   MAC match: row_uptime_ms=%lu vs pending=%lu -> %s\n",
                              row_uptime_ms, pending_deletes[p].uptime_ms,
                              (row_uptime_ms == pending_deletes[p].uptime_ms) ? "DROP" : "KEEP");
            }
        }
        bool drop = false;
        for (int p = 0; p < pending_delete_count && !drop; p++) {
            if (row_uptime_ms == pending_deletes[p].uptime_ms &&
                strncmp(row_mac, pending_deletes[p].mac, 17) == 0) {
                drop = true;
            }
        }
        if (drop) { skipped++; continue; }
        dst.println(line);
    }
    src.close();
    dst.close();

    if (skipped > 0) {
        if (SD.exists(bak_path)) SD.remove(bak_path);
        if (!SD.rename(current_log_file, bak_path)) {
            SD.remove(tmp_path); xSemaphoreGive(sdMutex); return;
        }
        if (!SD.rename(tmp_path, current_log_file)) {
            SD.rename(bak_path, current_log_file);
            xSemaphoreGive(sdMutex); return;
        }
        SD.remove(bak_path);
    } else {
        SD.remove(tmp_path);
    }

    Serial.printf("[DELDIAG] flush EXIT: dropped=%d\n", skipped);
    pending_delete_count  = 0;
    pending_delete_dirty_ms = 0;
    xSemaphoreGive(sdMutex);
}

void load_detections_from_flash() {
    if (!littlefs_available || !LittleFS.exists(DETECT_FILE)) return;
    File f = LittleFS.open(DETECT_FILE, "r");
    if (!f) return;
    capture_history_count = 0;
    while (f.available() && capture_history_count < CAPTURE_HISTORY_SIZE) {
        String line = f.readStringUntil('\n');
        if (line.length() < 5) continue;
        int idx = capture_history_count;
        int p0=0, p1=line.indexOf('|'), p2=line.indexOf('|',p1+1),
            p3=line.indexOf('|',p2+1), p4=line.indexOf('|',p3+1),
            p5=line.indexOf('|',p4+1);
        if (p4 < 0) continue;
        strncpy(capture_history[idx].type, line.substring(p0,p1).c_str(), 15);
        strncpy(capture_history[idx].mac,  line.substring(p1+1,p2).c_str(), 17);
        strncpy(capture_history[idx].name, line.substring(p2+1,p3).c_str(), 64);
        capture_history[idx].confidence = line.substring(p3+1,p4).toInt();
        capture_history[idx].lat = line.substring(p4+1, p5>0?p5:line.length()).toDouble();
        capture_history[idx].lng = p5>0 ? line.substring(p5+1).toDouble() : 0.0;
        capture_history[idx].rssi = -70;  
        safe_copy(capture_history[idx].time, "??:??:??", sizeof(capture_history[idx].time));
        capture_history_count++;
    }
    f.close();
}

// Load last SD_HIST_SIZE detections from SD CSV (most recent first in sd_hist[])
void load_sd_history() {
    Serial.printf("[DELDIAG] load_sd_history ENTER — heap=%u sd_hist_count=%d\n",
                  (unsigned)esp_get_free_heap_size(), sd_hist_count);
    // On no-PSRAM boards, runtime heap is ~6KB after NimBLE init.
    // The tail buffer needs 2-4KB contiguous. If heap is too low,
    // preserve existing sd_hist (populated at boot or by log_detection
    // in-memory pushes) rather than wiping it and failing the malloc.
    if (esp_get_free_heap_size() < 10000) {
        if (sd_hist_count > 0) {
            Serial.printf("[DELDIAG] load_sd_history SKIPPED — heap low (%u), keeping %d cached entries (POSSIBLE STALE DATA)\n",
                          (unsigned)esp_get_free_heap_size(), sd_hist_count);
            return;
        }
        // First load with no cached data — try with a minimal buffer below
    }
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    sd_hist_count = 0;
    xSemaphoreGiveRecursive(dataMutex);
    if (!sd_available) return;
    File f = SD.open(current_log_file, FILE_READ);
    if (!f) return;

    size_t file_size = f.size();
    if (file_size == 0) { f.close(); return; }

    // Read the last TAIL_BYTES of the file. 4KB covers ~20 typical CSV lines
    // (~200 bytes each), well above the 12 we need. Constant-time regardless
    // of file size — eliminates the 10s+ freeze on large log files.
    const size_t TAIL_BYTES_INITIAL = 1536;
    const size_t TAIL_BYTES_MAX     = 3072;

    size_t tail_bytes = TAIL_BYTES_INITIAL;
    if (tail_bytes > file_size) tail_bytes = file_size;

    // Prefer PSRAM for the tail buffer — keeps internal heap free for
    // WiFi/BLE stacks. Falls back to internal SRAM if PSRAM unavailable.
    char* tail_buf = (char*)heap_caps_malloc(tail_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tail_buf) tail_buf = (char*)heap_caps_malloc(tail_bytes + 1, MALLOC_CAP_8BIT);
    if (!tail_buf) {
        Serial.printf("[SD] load_sd_history: malloc failed (%u bytes, free heap: %u, free PSRAM: %u)\n",
                      (unsigned)(tail_bytes + 1),
                      (unsigned)esp_get_free_heap_size(),
                      (unsigned)ESP.getFreePsram());
        f.close();
        return;
    }

    size_t seek_pos = file_size - tail_bytes;
    f.seek(seek_pos);
    size_t bytes_read = f.read((uint8_t*)tail_buf, tail_bytes);
    tail_buf[bytes_read] = '\0';

    // Skip the first partial line (we seeked into the middle of it).
    // Find first '\n', then collect line-start positions after each '\n'.
    size_t first_newline = 0;
    while (first_newline < bytes_read && tail_buf[first_newline] != '\n') {
        first_newline++;
    }
    if (first_newline >= bytes_read) {
        // No newline — try a larger read once, then give up.
        if (tail_bytes < TAIL_BYTES_MAX && tail_bytes < file_size) {
            free(tail_buf);
            tail_bytes = TAIL_BYTES_MAX;
            if (tail_bytes > file_size) tail_bytes = file_size;
            tail_buf = (char*)heap_caps_malloc(tail_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tail_buf) tail_buf = (char*)heap_caps_malloc(tail_bytes + 1, MALLOC_CAP_8BIT);
            if (!tail_buf) {
                Serial.printf("[SD] load_sd_history: retry malloc failed (%u bytes)\n",
                              (unsigned)(tail_bytes + 1));
                f.close();
                return;
            }
            seek_pos = file_size - tail_bytes;
            f.seek(seek_pos);
            bytes_read = f.read((uint8_t*)tail_buf, tail_bytes);
            tail_buf[bytes_read] = '\0';
            first_newline = 0;
            while (first_newline < bytes_read && tail_buf[first_newline] != '\n') {
                first_newline++;
            }
        }
        if (first_newline >= bytes_read) {
            free(tail_buf);
            f.close();
            return;
        }
    }

    f.close();

    // Collect up to 64 line-start offsets (positions after each '\n').
    const int MAX_LINE_STARTS = 64;
    size_t line_starts[MAX_LINE_STARTS];
    int    line_start_count = 0;

    for (size_t i = first_newline + 1;
         i < bytes_read && line_start_count < MAX_LINE_STARTS; i++) {
        if (tail_buf[i - 1] == '\n') {
            if (i < bytes_read && tail_buf[i] != '\n' && tail_buf[i] != '\r') {
                line_starts[line_start_count++] = i;
            }
        }
    }

    if (line_start_count == 0) { free(tail_buf); return; }

    // Parse the last SD_HIST_SIZE complete lines (oldest → newest),
    // then reverse into sd_hist[] so index 0 = most recent.
    int first_line_idx = line_start_count - SD_HIST_SIZE;
    if (first_line_idx < 0) first_line_idx = 0;

    SDHistEntry parsed[SD_HIST_SIZE];
    int parsed_count = 0;

    for (int li = first_line_idx;
         li < line_start_count && parsed_count < SD_HIST_SIZE; li++) {
        char* line = &tail_buf[line_starts[li]];
        int len = 0;
        while (line_starts[li] + len < bytes_read &&
               line[len] != '\n' && line[len] != '\r') {
            len++;
        }
        if (len < 10) continue;
        line[len] = '\0';

        int fs[21]; int fc = 0;
        fs[0] = 0;
        for (int ci = 0; ci < len && fc < 20; ci++) {
            if (line[ci] == ',') fs[++fc] = ci + 1;
        }
        if (fc < 11) continue;
        if (strncmp(line, "Uptime_ms", 9) == 0) continue;  // skip CSV header

        auto copy_f = [&](int n, char* dest, int maxlen) {
            int start = fs[n];
            int end = start;
            while (end < len && line[end] != ',') end++;
            int flen = end - start;
            if (flen >= maxlen) flen = maxlen - 1;
            strncpy(dest, line + start, flen);
            dest[flen] = '\0';
        };

        SDHistEntry& e = parsed[parsed_count];
        copy_f(4,  e.type,   16);
        copy_f(7,  e.mac,    18);
        copy_f(8,  e.name,   32);
        copy_f(10, e.method, 24);
        e.rssi       = atoi(line + fs[6]);
        e.confidence = atoi(line + fs[11]);
        // DetID is column 20 — only present in newer log files
        e.id = (fc >= 20) ? atoi(line + fs[20]) : 0;
        {
            unsigned long uptime = (unsigned long)strtoul(line + fs[0], NULL, 10);
            e.uptime_ms = uptime;
            format_time_buf(uptime / 1000, e.timestamp, sizeof(e.timestamp));
        }
        // Parse epoch, lat, lng if columns are present
        e.epoch_utc = (fc >= 2) ? (uint32_t)strtoul(line + fs[1], NULL, 10) : 0;
        e.lat = (fc >= 16) ? atof(line + fs[15]) : 0.0;
        e.lng = (fc >= 17) ? atof(line + fs[16]) : 0.0;
        // Derive datestamp from epoch if available — apply timezone for local date
        if (e.epoch_utc > 0) {
            uint32_t local_ep = e.epoch_utc;
            if (auto_tz_valid) {
                local_ep = (uint32_t)((int64_t)local_ep + (int32_t)auto_tz_offset * 3600);
            }
            uint32_t y = 1970, m = 1, d = 1;
            uint32_t days = local_ep / 86400UL;
            while (true) {
                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t dy = leap ? 366 : 365;
                if (days < dy) break;
                days -= dy; y++;
            }
            static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            for (m = 1; m <= 12; m++) {
                bool leap2 = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t md = mdays[m-1] + (m == 2 && leap2 ? 1 : 0);
                if (days < md) { d = days + 1; break; }
                days -= md;
            }
            snprintf(e.datestamp, sizeof(e.datestamp), "%02u/%02u/%02u",
                     (unsigned)m, (unsigned)d, (unsigned)(y % 100));
        } else {
            safe_copy(e.datestamp, "--/--/--", sizeof(e.datestamp));
        }
        parsed_count++;
    }

    free(tail_buf);

    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    sd_hist_count = parsed_count;
    for (int i = 0; i < parsed_count; i++) {
        sd_hist[i] = parsed[parsed_count - 1 - i];
    }

    // Deduplicate by MAC — keep newest (lowest index) per device.
    // Re-detections after the seen_mac window expires append new CSV lines
    // for the same device; collapse those to one entry here.
    {
        int write = 0;
        for (int i = 0; i < sd_hist_count; i++) {
            bool dup = false;
            for (int j = 0; j < write; j++) {
                if (strncmp(sd_hist[i].mac, sd_hist[j].mac, 17) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                if (write != i) sd_hist[write] = sd_hist[i];
                write++;
            }
        }
        sd_hist_count = write;
    }

    Serial.printf("[DELDIAG] load_sd_history EXIT — sd_hist_count=%d\n", sd_hist_count);
    xSemaphoreGiveRecursive(dataMutex);
}

// Persist worker — runs save_session_to_flash on its own task so the main
// loop never blocks on LittleFS or SD writes. One-shot: spawned, runs, exits.
static volatile bool persist_in_flight = false;

static void save_session_to_flash();  // forward decl

static void PersistTask(void* pv) {
    esp_task_wdt_add(NULL);
    save_session_to_flash();
    esp_task_wdt_delete(NULL);
    persist_in_flight = false;
    vTaskDelete(NULL);
}

// Returns true if a persist is now in flight (either we just spawned one
// or one was already running). Returns false if the spawn failed (e.g.
// heap exhaustion) — caller should retry on the next loop iteration so
// a low-heap window doesn't lose a full PERSIST_INTERVAL of data.
static bool schedule_persist() {
    if (persist_in_flight) return true;
    persist_in_flight = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        PersistTask, "PersistTask", 4096, NULL, 1, NULL, 1);
    if (ok != pdPASS) {
        persist_in_flight = false;
        return false;
    }
    return true;
}

static void save_session_to_flash() {
    if (!littlefs_available) return;

    // Same guard as flush_sd_buffer — LittleFS internally mallocs cache pages
    // during open/write and will abort() on a NULL return. Skip this cycle
    // and let the next persist tick try again when heap recovers.
    if (esp_get_free_heap_size() < 6000) {
        Serial.println("[FS] Skipping persist — heap too low");
        last_persist_save = millis();
        return;
    }

    long l_wifi, l_ble, l_sec, l_flock, l_boots, l_writes, l_next_id;
    int l_vol, l_brightness;
    bool l_night, l_low_power, l_muted, l_turbo, l_c5;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    l_wifi = lifetime_wifi; l_ble = lifetime_ble; l_sec = lifetime_seconds;
    l_flock = lifetime_flock_total; l_vol = current_volume; l_boots = lifetime_boots;
    l_writes = lifetime_flash_writes + 1;
    l_next_id = next_detection_id;
    l_night      = night_mode;
    l_brightness = brightness_level;
    l_low_power  = low_power_mode;
    l_muted      = is_muted;
    l_turbo      = turbo_mode_active;
    l_c5         = c5_enabled;
    xSemaphoreGiveRecursive(dataMutex);

    // Minimum acceptable byte count for a complete write. Computed from all
    // 14 lines below: each contributes at least its key + '=' + minimum value
    // + '\n'. A real write runs ~120-140 bytes. Anything below this floor means
    // the write truncated mid-stream.
    //
    //   wifi=0\n       (7)   ble=0\n        (6)   seconds=0\n    (10)
    //   flock=0\n      (8)   volume=0\n     (9)   boots=0\n      (8)
    //   writes=0\n     (9)   next_id=0\n    (10)  night=0\n      (8)
    //   brightness=0\n (13)  low_power=0\n  (12)  muted=0\n      (8)
    //   turbo=0\n      (8)   c5=0\n         (5)
    //   Sum: 121 bytes (minimum-field floor)
    //
    // ssid= and pass= used to be written here and contributed 12 bytes to that
    // sum. They were removed because storing the WiFi password in plaintext
    // defeated the obfuscated /wifi_cred.dat entirely.
    //
    // The constant sits 13 bytes under the true floor as deliberate slack. The
    // tradeoff is unchanged from before: no legitimate write comes in below 121,
    // so 121 would be the tightest correct value, and 108 will still pass a
    // write that truncated after losing turbo (8) and c5 (5).
    //
    // Keep this list in sync with the printf block below. It has drifted twice:
    // turbo and c5 were both added without updating either the list or the
    // constant, which is how a stated sum of 130 came to describe a file layout
    // that had not existed for two releases.
    static const size_t PERSIST_MIN_BYTES = 108;

    bool write_ok = false;
    for (int attempt = 0; attempt < 3 && !write_ok; attempt++) {
        size_t written = 0;
        bool any_short = false;
        bool atomic_ok = littlefs_atomic_write(PERSIST_FILE, [&](File& f) -> bool {
            auto wp = [&](int r) {
                if (r <= 0) { any_short = true; return; }
                written += (size_t)r;
            };
            wp(f.printf("wifi=%ld\n",       l_wifi));
            wp(f.printf("ble=%ld\n",        l_ble));
            wp(f.printf("seconds=%lu\n",    l_sec));
            wp(f.printf("flock=%ld\n",      l_flock));
            wp(f.printf("volume=%d\n",      l_vol));
            wp(f.printf("boots=%ld\n",      l_boots));
            wp(f.printf("writes=%ld\n",     l_writes));
            wp(f.printf("next_id=%ld\n",    l_next_id));
            wp(f.printf("night=%d\n",       l_night ? 1 : 0));
            wp(f.printf("brightness=%d\n",  l_brightness));
            wp(f.printf("low_power=%d\n",   l_low_power ? 1 : 0));
            wp(f.printf("muted=%d\n",       l_muted ? 1 : 0));
            wp(f.printf("turbo=%d\n",       l_turbo ? 1 : 0));
            wp(f.printf("c5=%d\n",          l_c5 ? 1 : 0));
            return !any_short && written >= PERSIST_MIN_BYTES;
        });
        if (atomic_ok) {
            write_ok = true;
        } else {
            Serial.printf("[FS] Write failed: %u bytes (attempt %d, any_short=%d)\n",
                          (unsigned)written, attempt, any_short ? 1 : 0);
            delay(5);
        }
    }

    if (write_ok) {
        flash_write_fail_count = 0;
        last_persist_save = millis();
        save_detections_to_flash();
        save_stats_to_sd();
        xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
        lifetime_flash_writes = l_writes;
        xSemaphoreGiveRecursive(dataMutex);
        // Wear toasts: once at 80K (warning) and once at 100K (critical).
        // Equality checks fire exactly once per crossing.
        if (l_writes == 80000) {
            set_toast_direct("FLASH WEAR HIGH", TOAST_WARNING, false);
        } else if (l_writes == 100000) {
            set_toast_direct("FLASH WEAR CRIT", TOAST_WARNING, false);
        }
    } else {
        flash_write_fail_count++;
        if (flash_write_fail_count >= FLASH_FAIL_TOAST_THRESHOLD) {
            set_toast_direct("FLASH WRITE FAIL", TOAST_WARNING, false);
            last_persist_save = millis();
        }
    }
}

void save_stats_to_sd() {
    if (!sd_available) return;

    // Snapshot lifetime values under the mutex so the SD write doesn't see a
    // half-updated set (other tasks bump these counters concurrently).
    long          l_wifi, l_ble, l_flock, l_boots, l_writes;
    unsigned long l_sec;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    l_wifi   = lifetime_wifi;
    l_ble    = lifetime_ble;
    l_sec    = lifetime_seconds;
    l_flock  = lifetime_flock_total;
    l_boots  = lifetime_boots;
    l_writes = lifetime_flash_writes;
    xSemaphoreGiveRecursive(dataMutex);

    // Timed take — if the lock is held for too long, skip this cycle. The
    // next persist tick will retry. Keeps PersistTask's WDT alive even when
    // the main loop is mid-flush.
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return;
    }
    File f = SD.open("/PLUME/stats/lifetime.txt", FILE_WRITE);
    if (f) {
        f.printf("lifetime_wifi=%ld\n",          l_wifi);
        f.printf("lifetime_ble=%ld\n",           l_ble);
        f.printf("lifetime_seconds=%lu\n",       l_sec);
        f.printf("lifetime_flock_total=%ld\n",   l_flock);
        f.printf("lifetime_boots=%ld\n",         l_boots);
        f.printf("lifetime_flash_writes=%ld\n",  l_writes);
        f.close();
    }
    xSemaphoreGive(sdMutex);
}

// ── SD hot-plug: attempt remount when absent, detect silent removal ──────────
// Called every SD_CHECK_INTERVAL_MS from loop(). When no card is present, tries
// SD.begin() at 4 MHz then 20 MHz; on success, recreates the /PLUME
// directory tree, writes PCAP headers, reloads history, and toasts. When a
// card is present, probes a known file; if the open fails silently, treats it
// as a removal.
static void sd_check_hotplug() {
    unsigned long now = millis();
    if (now - last_sd_check_ms < current_sd_check_interval_ms()) return;
    last_sd_check_ms = now;

    // Short timed take — hot-plug is a 5-second poll and gladly retries.
    // Blocking the main loop here for seconds at a time would freeze the
    // UI and starve alarm processing while PersistTask is writing.
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    if (!sd_available) {
        // Remount via the same dedicated sdSPI bus setup() uses. No-arg
        // SD.begin or the default global SPI route to wrong pins on the
        // Cardputer. No frequency arg — matches the Launcher's default 4 MHz.
        bool mounted = false;
        if (SD.begin(SD_CS_PIN, sdSPI)) {
            if (SD.cardType() != CARD_NONE) {
                mounted = true;
            } else {
                SD.end();
            }
        }
        esp_task_wdt_reset();  // SD.begin can take several hundred ms

        if (mounted) {
            sd_mount_fail_streak = 0;   // re-arm fast polling for a future removal
            if (!SD.exists("/PLUME"))          SD.mkdir("/PLUME");
            if (!SD.exists("/PLUME/logs"))     SD.mkdir("/PLUME/logs");
            if (!SD.exists("/PLUME/captures")) SD.mkdir("/PLUME/captures");
            if (!SD.exists("/PLUME/stats"))    SD.mkdir("/PLUME/stats");
            esp_task_wdt_reset();

            if (!SD.exists(current_log_file)) {
                File f = SD.open(current_log_file, FILE_WRITE);
                if (f) {
                    f.println("Uptime_ms,EpochUTC,EpochIsGPS,Channel,Type,Proto,RSSI,MAC,Name,TXPower,Method,Conf,ConfLabel,Extra,SeqNum,Lat,Lon,SpeedMPH,HeadingDeg,AltM,DetID");
                    f.close();
                }
            }
            esp_task_wdt_reset();

            auto ensure_pcap = [](const char* path, uint32_t link_type) {
                bool need_hdr = !SD.exists(path);
                if (!need_hdr) {
                    File pcheck = SD.open(path, FILE_READ);
                    if (pcheck) { need_hdr = (pcheck.size() < 24); pcheck.close(); }
                }
                if (need_hdr) {
                    File pf = SD.open(path, FILE_WRITE);
                    if (pf) {
                        uint32_t hdr[6] = {0xa1b2c3d4, 0x00040002, 0, 0, 0x0000ffff, link_type};
                        pf.write((const uint8_t*)hdr, 24);
                        pf.close();
                    }
                }
            };
            ensure_pcap(current_pcap_file,     0x00000069);  // WiFi 802.11
            ensure_pcap(current_ble_pcap_file, 0x000000fb);  // Bluetooth LE
            esp_task_wdt_reset();

            // Phase 1 complete — release mutex so flush_sd_buffer and
            // PersistTask can run between phases.
            xSemaphoreGive(sdMutex);

            // Only now advertise the card as available — directories and
            // PCAP headers are confirmed written, so flush_sd_buffer and
            // PersistTask will find a fully initialized filesystem.
            sd_available = true;
            sd_full_warned = false;

            // Phase 2: history load + stats write. Re-acquire with a
            // longer timeout; if contention persists, skip (PersistTask
            // writes stats on the next 60s cycle).
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                load_sd_history();
                xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                sd_hist_dirty = false;
                long          l_wifi   = lifetime_wifi;
                long          l_ble    = lifetime_ble;
                unsigned long l_sec    = lifetime_seconds;
                long          l_flock  = lifetime_flock_total;
                long          l_boots  = lifetime_boots;
                long          l_writes = lifetime_flash_writes;
                xSemaphoreGiveRecursive(dataMutex);

                File sf = SD.open("/PLUME/stats/lifetime.txt", FILE_WRITE);
                if (sf) {
                    sf.printf("lifetime_wifi=%ld\n",          l_wifi);
                    sf.printf("lifetime_ble=%ld\n",           l_ble);
                    sf.printf("lifetime_seconds=%lu\n",       l_sec);
                    sf.printf("lifetime_flock_total=%ld\n",   l_flock);
                    sf.printf("lifetime_boots=%ld\n",         l_boots);
                    sf.printf("lifetime_flash_writes=%ld\n",  l_writes);
                    sf.close();
                }
                xSemaphoreGive(sdMutex);
            }

            sd_was_available = true;
            set_toast_direct("SD CARD MOUNTED", TOAST_SUCCESS);
            Serial.println("[SD] Hot-plug mount succeeded");
            return;
        }
        // Mount failed — lengthen the retry interval (see the backoff above).
        if (sd_mount_fail_streak < 200) sd_mount_fail_streak++;
    } else {
        // Probe at the controller level — independent of any file existing.
        // The previous file-probe falsely reported "removed" within the first
        // 60s of boot when /PLUME/stats/lifetime.txt had not yet been
        // written by the persist task.
        if (SD.cardType() == CARD_NONE && sd_was_available) {
            sd_available = false;
            SD.end();
            set_toast_direct("SD CARD REMOVED", TOAST_WARNING, false);
            Serial.println("[SD] Card removal detected");
        }
    }

    sd_was_available = sd_available;

    xSemaphoreGive(sdMutex);
}

void load_session_from_flash() {
    if (!LittleFS.exists(PERSIST_FILE)) return;
    File f = LittleFS.open(PERSIST_FILE, "r");
    if (!f) return;

    // Detect format: old positional files start with a digit (the wifi
    // count); new key=value files start with a letter. Peek at the first
    // character to choose the parser.
    int first_char = f.peek();
    if (first_char < 0) { f.close(); return; }

    if (first_char >= '0' && first_char <= '9') {
        // ── Legacy positional format — read once, then the next save
        //    will overwrite with key=value format automatically. ──
        String line;
        line = f.readStringUntil('\n'); if (line.length() > 0) lifetime_wifi = line.toInt();
        line = f.readStringUntil('\n'); if (line.length() > 0) lifetime_ble = line.toInt();
        line = f.readStringUntil('\n'); if (line.length() > 0) lifetime_seconds = line.toInt();
        line = f.readStringUntil('\n'); if (line.length() > 0) lifetime_flock_total = line.toInt();
        line = f.readStringUntil('\n'); if (line.length() > 0) current_volume = line.toInt();
        line = f.readStringUntil('\n'); if (line.length() > 0) lifetime_boots = line.toInt();
        line = f.readStringUntil('\n'); if (line.length() > 0) lifetime_flash_writes = line.toInt();
        line = f.readStringUntil('\n');
        if (line.length() > 0 && line.length() < sizeof(export_ssid)) {
            strncpy(export_ssid, line.c_str(), sizeof(export_ssid) - 1);
            export_ssid[sizeof(export_ssid) - 1] = '\0';
        }
        line = f.readStringUntil('\n');
        if (line.length() > 0 && line.length() < sizeof(export_pass)) {
            strncpy(export_pass, line.c_str(), sizeof(export_pass) - 1);
            export_pass[sizeof(export_pass) - 1] = '\0';
        }
        line = f.readStringUntil('\n');
        if (line.length() > 0) {
            long parsed = line.toInt();
            if (parsed >= 1) next_detection_id = parsed;
        }
        f.close();
        return;
    }

    // ── Key=value format — order independent, unknown keys ignored ──
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() < 3) continue;

        int eq = line.indexOf('=');
        if (eq <= 0 || eq >= (int)line.length() - 1) continue;

        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);

        if      (key == "wifi")       lifetime_wifi = val.toInt();
        else if (key == "ble")        lifetime_ble = val.toInt();
        else if (key == "seconds")    lifetime_seconds = val.toInt();
        else if (key == "flock")      lifetime_flock_total = val.toInt();
        else if (key == "volume")     current_volume = val.toInt();
        else if (key == "boots")      lifetime_boots = val.toInt();
        else if (key == "writes")     lifetime_flash_writes = val.toInt();
        else if (key == "next_id") {
            long parsed = val.toInt();
            if (parsed >= 1) next_detection_id = parsed;
        }
        else if (key == "night")      night_mode = (val.toInt() != 0);
        else if (key == "brightness") { int b = val.toInt(); if (b >= 0 && b <= 3) brightness_level = b; }
        else if (key == "low_power")  low_power_mode = (val.toInt() != 0);
        else if (key == "muted")      is_muted = (val.toInt() != 0);
        else if (key == "turbo")      turbo_mode_active = (val.toInt() != 0);
        else if (key == "c5")         c5_enabled = (val.toInt() != 0);
        // Unknown keys are silently ignored — forward compatibility
    }
    f.close();
}

// Derive a 16-byte AES key from the ESP32 eFuse MAC using HMAC-SHA256.
// The MAC is unique per physical device and lives in one-time-programmable
// eFuse — not in any user-accessible storage. HMAC with a fixed salt
// produces a key that cannot be reversed to the MAC without brute force.
static void derive_aes_key(uint8_t key_out[16]) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    // HMAC-SHA256(key=salt, message=mac) → 32 bytes; truncate to 16.
    static const uint8_t salt[16] = {
        0xF1, 0x0C, 0x4D, 0xE7, 0xA9, 0x3B, 0x58, 0x72,
        0x6E, 0xC0, 0x1F, 0x84, 0xD6, 0x2A, 0x95, 0x4B
    };
    uint8_t hmac_out[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, salt, sizeof(salt));
    mbedtls_md_hmac_update(&ctx, mac, sizeof(mac));
    mbedtls_md_hmac_finish(&ctx, hmac_out);
    mbedtls_md_free(&ctx);

    memcpy(key_out, hmac_out, 16);
}

// PKCS7 pad/unpad helpers for AES-CBC (block size = 16).
static size_t pkcs7_pad(uint8_t* buf, size_t data_len, size_t buf_size) {
    size_t pad_len = 16 - (data_len % 16);
    if (data_len + pad_len > buf_size) return 0;
    memset(buf + data_len, (uint8_t)pad_len, pad_len);
    return data_len + pad_len;
}

static size_t pkcs7_unpad(const uint8_t* buf, size_t padded_len) {
    if (padded_len == 0 || padded_len % 16 != 0) return 0;
    uint8_t pad_val = buf[padded_len - 1];
    if (pad_val == 0 || pad_val > 16) return 0;
    for (size_t i = 0; i < pad_val; i++) {
        if (buf[padded_len - 1 - i] != pad_val) return 0;
    }
    return padded_len - pad_val;
}

#define WIFI_CRED_FILE "/wifi_cred.dat"

static void save_wifi_credentials() {
    if (!littlefs_available) return;

    uint8_t key[16];
    derive_aes_key(key);

    // Generate a random IV for each save — ensures identical plaintext
    // produces different ciphertext across saves.
    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));

    // Prepare plaintext: [1 byte ssid_len] [ssid] [1 byte pass_len] [pass]
    size_t ssid_len = strlen(export_ssid);
    size_t pass_len = strlen(export_pass);
    size_t plain_len = 1 + ssid_len + 1 + pass_len;

    // Padded buffer — max plaintext is 1+32+1+64 = 98, padded to 112
    uint8_t plain[128] = {0};
    plain[0] = (uint8_t)ssid_len;
    memcpy(plain + 1, export_ssid, ssid_len);
    plain[1 + ssid_len] = (uint8_t)pass_len;
    memcpy(plain + 1 + ssid_len + 1, export_pass, pass_len);

    size_t padded_len = pkcs7_pad(plain, plain_len, sizeof(plain));
    if (padded_len == 0) { memset(key, 0, sizeof(key)); return; }

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);  // mbedtls modifies the IV buffer

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_len, iv_copy, plain, plain);
    mbedtls_aes_free(&aes);

    // Write: [16 bytes IV] [2 bytes padded_len LE] [ciphertext]
    uint8_t len_bytes[2] = { (uint8_t)(padded_len & 0xFF), (uint8_t)(padded_len >> 8) };
    littlefs_atomic_write(WIFI_CRED_FILE, [&](File& f) -> bool {
        if (f.write(iv, 16) != 16) return false;
        if (f.write(len_bytes, 2) != 2) return false;
        if (f.write(plain, padded_len) != padded_len) return false;
        return true;
    });

    memset(plain, 0, sizeof(plain));
    memset(key, 0, sizeof(key));
}

static void load_wifi_credentials() {
    if (!littlefs_available) return;
    if (!LittleFS.exists(WIFI_CRED_FILE)) return;
    File f = LittleFS.open(WIFI_CRED_FILE, "r");
    if (!f) return;

    size_t file_size = f.size();
    bool loaded = false;

    // ── Try AES-CBC format ──
    if (file_size >= 34) {
        uint8_t iv[16];
        if (f.read(iv, 16) == 16) {
            uint8_t len_bytes[2];
            if (f.read(len_bytes, 2) == 2) {
                size_t padded_len = len_bytes[0] | (len_bytes[1] << 8);
                if (padded_len >= 16 && padded_len <= 128 && padded_len % 16 == 0 &&
                    file_size >= 18 + padded_len) {
                    uint8_t cipher[128];
                    if (f.read(cipher, padded_len) == (int)padded_len) {
                        uint8_t key[16];
                        derive_aes_key(key);

                        mbedtls_aes_context aes;
                        mbedtls_aes_init(&aes);
                        mbedtls_aes_setkey_dec(&aes, key, 128);
                        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, padded_len, iv, cipher, cipher);
                        mbedtls_aes_free(&aes);
                        memset(key, 0, sizeof(key));

                        size_t plain_len = pkcs7_unpad(cipher, padded_len);
                        if (plain_len >= 2) {
                            uint8_t ssid_len = cipher[0];
                            if (ssid_len <= 32 && 1 + ssid_len + 1 <= plain_len) {
                                uint8_t pass_len = cipher[1 + ssid_len];
                                if (pass_len <= 64 && 1 + ssid_len + 1 + pass_len <= plain_len) {
                                    bool valid = true;
                                    for (int i = 0; i < (int)ssid_len && valid; i++) {
                                        if (cipher[1 + i] < 32 || cipher[1 + i] > 126) valid = false;
                                    }
                                    if (valid) {
                                        memcpy(export_ssid, cipher + 1, ssid_len);
                                        export_ssid[ssid_len] = '\0';
                                        memcpy(export_pass, cipher + 1 + ssid_len + 1, pass_len);
                                        export_pass[pass_len] = '\0';
                                        loaded = true;
                                    }
                                }
                            }
                        }
                        memset(cipher, 0, sizeof(cipher));
                    }
                }
            }
        }
    }

    // ── Fallback: old XOR format ──
    if (!loaded) {
        f.seek(0);
        int ssid_len_raw = f.read();
        int pass_len_raw = f.read();
        if (ssid_len_raw >= 0 && pass_len_raw >= 0) {
            uint8_t ssid_len = (uint8_t)ssid_len_raw;
            uint8_t pass_len = (uint8_t)pass_len_raw;
            if (ssid_len <= 32 && pass_len <= 64) {
                char enc_ssid[33] = {0}, enc_pass[65] = {0};
                f.readBytes(enc_ssid, ssid_len);
                f.readBytes(enc_pass, pass_len);

                // Reconstruct old XOR key
                uint8_t mac[6];
                esp_efuse_mac_get_default(mac);
                uint8_t old_key[16];
                for (int i = 0; i < 16; i++) {
                    old_key[i] = mac[i % 6] ^ (uint8_t)(i * 0x5A + 0x37);
                }
                for (size_t i = 0; i < ssid_len; i++) enc_ssid[i] ^= old_key[i % 16];
                for (size_t i = 0; i < pass_len; i++) enc_pass[i] ^= old_key[i % 16];

                bool valid = true;
                for (int i = 0; i < (int)ssid_len; i++) {
                    if ((unsigned char)enc_ssid[i] < 32 || (unsigned char)enc_ssid[i] > 126) {
                        valid = false; break;
                    }
                }
                if (valid) {
                    strncpy(export_ssid, enc_ssid, sizeof(export_ssid) - 1);
                    export_ssid[sizeof(export_ssid) - 1] = '\0';
                    strncpy(export_pass, enc_pass, sizeof(export_pass) - 1);
                    export_pass[sizeof(export_pass) - 1] = '\0';
                    loaded = true;
                }
            }
        }
    }

    f.close();

    // If loaded from old XOR format, re-save in AES format immediately.
    if (loaded && file_size < 34) {
        save_wifi_credentials();
    }
}

void rssi_track_update(const char* mac, int rssi) {
    unsigned long now = millis();
    if (!take_data_mutex()) return;
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, mac, 17) == 0) {
            if (rssi_tracker[i].sample_count < RSSI_TRACK_SAMPLES) {
                rssi_tracker[i].samples[rssi_tracker[i].sample_count++] = rssi;
            } else {
                for (int j = 0; j < RSSI_TRACK_SAMPLES - 1; j++) rssi_tracker[i].samples[j] = rssi_tracker[i].samples[j + 1];
                rssi_tracker[i].samples[RSSI_TRACK_SAMPLES - 1] = rssi;
            }
            rssi_tracker[i].last_seen = now;
            if (signal_active && strncmp(rssi_tracker[i].mac, signal_target_mac, 17) == 0) {
                signal_tracker_idx = i;
            }
            xSemaphoreGiveRecursive(dataMutex);
            return;
        }
    }
    if (rssi_tracker_count < RSSI_TRACK_MAX_DEVICES) {
        strncpy(rssi_tracker[rssi_tracker_count].mac, mac, 17);
        rssi_tracker[rssi_tracker_count].mac[17] = '\0';
        rssi_tracker[rssi_tracker_count].samples[0] = rssi;
        rssi_tracker[rssi_tracker_count].sample_count = 1;
        rssi_tracker[rssi_tracker_count].last_seen = now;
        rssi_tracker_count++;
    }
    give_data_mutex();
}



void rssi_track_expire() {
    if (!take_data_mutex()) return;
    unsigned long now = millis();
    for (int i = rssi_tracker_count - 1; i >= 0; i--) {
        if ((now - rssi_tracker[i].last_seen) > RSSI_TRACK_EXPIRY_MS) {
            for (int j = i; j < rssi_tracker_count - 1; j++) rssi_tracker[j] = rssi_tracker[j + 1];
            rssi_tracker_count--;
            // Keep signal_tracker_idx consistent across the array shift
            if (signal_tracker_idx == i)       signal_tracker_idx = -1;
            else if (signal_tracker_idx > i)   signal_tracker_idx--;
        }
    }
    // Re-validate after all shifts — defensive against combined adjustments
    if (signal_tracker_idx >= rssi_tracker_count) {
        signal_tracker_idx = -1;
    }
    give_data_mutex();
}

// ── SCAN viz device state ─────────────────────────────────────────────
// Each slot tracks a live device. Angle is derived from MAC hash for
// spatial stability; radial position maps RSSI → radius via anim_filter.
// Sweep brightness uses phosphor decay: illuminate on sweep pass, fade.
#define SCAN_MAX_DEVICES 10

struct ScanDevice {
    char     mac[18];
    float    angle;           // repulsion-adjusted target angle
    float    angle_smooth;    // eased display angle — prevents angular jumps
    float    dist;            // RSSI-derived target, 0=center 1=edge
    float    dist_smooth;     // eased distance for smooth radial motion
    float    sweep_bright;    // 0..1 brightness from sweep contact
    unsigned long last_sweep_ms;
    uint8_t  proto;
    bool     is_flock;
    int8_t   rssi;
    bool     occupied;
    unsigned long last_seen_ms;
    unsigned long appear_ms;  // timestamp of first appearance — drives fade-in
    bool     has_appeared;    // true once appear_ms is set (avoids millis()==0 sentinel bug)
};

static ScanDevice    scan_devs[SCAN_MAX_DEVICES] = {};
static unsigned long scan_last_refresh_ms = 0;
static unsigned long scan_last_frame_ms   = 0;
static float         scan_sweep_angle     = 0.0f;

// Byte order detection for direct sprite buffer access.
// Set once on first call to draw_scanner_viz_scan.
static bool g_buf_bytes_swapped      = false;
static bool g_buf_byte_order_detected = false;

static bool feed_recently_pushed(const char* mac) {
    unsigned long now = millis();
    for (int i = 0; i < feed_count; i++) {
        int idx = (feed_head - i + FEED_SIZE * 2) % FEED_SIZE;
        if (now - feed_entries[idx].timestamp > FEED_DEDUP_WINDOW_MS) break;
        if (strncmp(feed_entries[idx].mac, mac, 17) == 0) return true;
    }
    return false;
}

static void feed_push_candidate(const char* mac, const char* name, int rssi,
                                int proto, bool is_flock, bool is_5ghz = false) {
    if (!mac || mac[0] == '\0') return;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    if (feed_recently_pushed(mac)) { xSemaphoreGiveRecursive(dataMutex); return; }
    if (feed_pending_valid && rssi <= feed_pending.rssi) { xSemaphoreGiveRecursive(dataMutex); return; }

    strncpy(feed_pending.mac, mac, 17); feed_pending.mac[17] = '\0';
    if (name && name[0] != '\0') {
        strncpy(feed_pending.name, name, sizeof(feed_pending.name) - 1);
        feed_pending.name[sizeof(feed_pending.name) - 1] = '\0';
    } else {
        const char* tail = (strlen(mac) > 8) ? mac + 9 : mac;
        strncpy(feed_pending.name, tail, sizeof(feed_pending.name) - 1);
        feed_pending.name[sizeof(feed_pending.name) - 1] = '\0';
    }
    feed_pending.rssi      = (int8_t)rssi;
    feed_pending.proto     = (uint8_t)proto;
    feed_pending.is_flock  = is_flock;
    feed_pending.is_5ghz   = is_5ghz;
    // timestamp is set in feed_commit_pending() at the moment of commit;
    // setting it here would be dead-stored.
    feed_pending_valid     = true;
    xSemaphoreGiveRecursive(dataMutex);
}

static void feed_commit_pending() {
    unsigned long now = millis();
    unsigned long interval = show_feed_expanded
                           ? FEED_PUSH_INTERVAL_EXPANDED_MS
                           : FEED_MIN_PUSH_INTERVAL_MS;
    if (now - last_feed_push_ms < interval) return;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    if (!feed_pending_valid) { xSemaphoreGiveRecursive(dataMutex); return; }
    feed_head = (feed_head + 1) % FEED_SIZE;
    feed_entries[feed_head] = feed_pending;
    feed_entries[feed_head].timestamp = now;
    if (feed_count < FEED_SIZE) feed_count++;
    feed_pending_valid = false;
    last_feed_push_ms  = now;
    xSemaphoreGiveRecursive(dataMutex);
}

// Force a feed entry immediately, bypassing the strongest-pending throttle.
// Used for confirmed detections (esp. 5 GHz from the C5, which arrive once per
// 30 s and would otherwise lose the per-window candidate competition).
static void feed_force_push(const char* mac, const char* name, int rssi,
                            int proto, bool is_flock, bool is_5ghz = false) {
    if (!mac || mac[0] == '\0') return;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    if (!feed_recently_pushed(mac)) {              // honor the 30 s dedup
        feed_head = (feed_head + 1) % FEED_SIZE;
        FeedEntry& fe = feed_entries[feed_head];
        strncpy(fe.mac, mac, 17); fe.mac[17] = '\0';
        strncpy(fe.name, (name && name[0]) ? name : "Hidden", sizeof(fe.name) - 1);
        fe.name[sizeof(fe.name) - 1] = '\0';
        fe.rssi      = (int8_t)rssi;
        fe.proto     = (uint8_t)proto;
        fe.is_flock  = is_flock;
        fe.is_5ghz   = is_5ghz;
        fe.timestamp = millis();
        if (feed_count < FEED_SIZE) feed_count++;
    }
    xSemaphoreGiveRecursive(dataMutex);
}

void add_blip(uint16_t blip_color, int rssi) {
    (void)blip_color; (void)rssi;
}

// ============================================================================
// LOGGING ENGINE
// ============================================================================
void flush_sd_buffer() {
    static char local_log_buf[MAX_LOG_BUFFER][SD_LINE_LEN];
    static PcapQueueItem local_pcap_buf[MAX_PCAP_BUFFER];
    static PcapQueueItem local_ble_pcap_buf[MAX_PCAP_BUFFER];

    int log_count  = 0;
    int pcap_count = 0;
    int ble_pcap_count = 0;

    // Preconditions FIRST — if any fail, buffered data stays in place
    // for the next flush attempt rather than being silently discarded.
    if (!sd_available) return;

    // Skip flush if heap is critically low — the SD FAT driver mallocs
    // internally and will abort() if it gets NULL. Better to drop a flush
    // cycle than crash; the buffers will retry next interval.
    if (esp_get_free_heap_size() < 6000) {
        Serial.println("[SD] Skipping flush — heap too low");
        return;
    }

    // Short timed take — the service checks every 500 ms and retries full
    // buffers immediately or partial buffers on the normal flush interval.
    // Blocking for seconds here freezes the main loop and starves alarms /
    // WiFi event processing.
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    // Snapshot without clearing. Producers only append while we write, so the
    // snapshotted prefix remains stable. After each file write, remove only the
    // successfully appended prefix. A transient open/write failure therefore
    // retains unwritten records for the next eligible service pass instead of
    // silently discarding the entire batch.
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    log_count = sd_write_count;
    for (int i = 0; i < log_count; i++) {
        strncpy(local_log_buf[i], sd_write_buffer[i], SD_LINE_LEN - 1);
        local_log_buf[i][SD_LINE_LEN - 1] = '\0';
    }

    pcap_count = pcap_write_count;
    for (int i = 0; i < pcap_count; i++) {
        local_pcap_buf[i] = pcap_write_buffer[i];
    }
    ble_pcap_count = ble_pcap_write_count;
    for (int i = 0; i < ble_pcap_count; i++) {
        local_ble_pcap_buf[i] = ble_pcap_write_buffer[i];
    }
    xSemaphoreGiveRecursive(dataMutex);

    int log_written = 0;
    int pcap_written = 0;
    int ble_pcap_written = 0;

    if (log_count > 0) {
        File file = SD.open(current_log_file, FILE_APPEND);
        if (file) {
            for (int i = 0; i < log_count; i++) {
                if ((i & 0x0F) == 0) esp_task_wdt_reset();  // every 16 lines
                size_t written = file.println(local_log_buf[i]);
                if (written == 0) break;
                log_written++;
            }
            file.close();
            if (log_written < log_count && !sd_full_warned) {
                sd_full_warned = true;
                set_toast_direct("SD CARD FULL", TOAST_WARNING, false);
            }
        } else if (sd_available) {
            // Soft failure: controller may be doing GC or a brief voltage dip.
            // Don't tear down the SD connection — the 5s hot-plug probe owns the
            // real removal check, and this write will retry on the next flush.
            set_toast_direct("SD WRITE FAIL", TOAST_WARNING, false);
            Serial.println("[SD] Write failed — retrying next flush cycle");
        }
    }

    if (pcap_count > 0) {
        File pfile = SD.open(current_pcap_file, FILE_APPEND);
        if (pfile) {
            for (int i = 0; i < pcap_count; i++) {
                esp_task_wdt_reset();  // pcap writes can be large per packet
                pcap_packet_header pph;
                pph.ts_sec = local_pcap_buf[i].ts_sec;
                pph.ts_usec = local_pcap_buf[i].ts_usec;
                pph.incl_len = local_pcap_buf[i].incl_len;
                pph.orig_len = local_pcap_buf[i].orig_len;
                uint8_t record[sizeof(pph) + 256];
                memcpy(record, &pph, sizeof(pph));
                memcpy(record + sizeof(pph), local_pcap_buf[i].payload, local_pcap_buf[i].incl_len);
                size_t record_len = sizeof(pph) + local_pcap_buf[i].incl_len;
                if (pfile.write(record, record_len) != record_len) break;
                pcap_written++;
            }
            pfile.close();
        }
    }
    if (ble_pcap_count > 0) {
        File bfile = SD.open(current_ble_pcap_file, FILE_APPEND);
        if (bfile) {
            for (int i = 0; i < ble_pcap_count; i++) {
                esp_task_wdt_reset();
                pcap_packet_header pph;
                pph.ts_sec = local_ble_pcap_buf[i].ts_sec;
                pph.ts_usec = local_ble_pcap_buf[i].ts_usec;
                pph.incl_len = local_ble_pcap_buf[i].incl_len;
                pph.orig_len = local_ble_pcap_buf[i].orig_len;
                uint8_t record[sizeof(pph) + 256];
                memcpy(record, &pph, sizeof(pph));
                memcpy(record + sizeof(pph), local_ble_pcap_buf[i].payload, local_ble_pcap_buf[i].incl_len);
                size_t record_len = sizeof(pph) + local_ble_pcap_buf[i].incl_len;
                if (bfile.write(record, record_len) != record_len) break;
                ble_pcap_written++;
            }
            bfile.close();
        }
    }

    // Producers may have appended records after the snapshot. Remove only the
    // successfully written prefix and shift any old remainder plus new tail.
    if (log_written > 0 || pcap_written > 0 || ble_pcap_written > 0) {
        xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
        if (log_written > 0 && log_written <= sd_write_count) {
            for (int i = log_written; i < sd_write_count; i++) {
                memcpy(sd_write_buffer[i - log_written], sd_write_buffer[i], SD_LINE_LEN);
            }
            sd_write_count -= log_written;
        }
        if (pcap_written > 0 && pcap_written <= pcap_write_count) {
            for (int i = pcap_written; i < pcap_write_count; i++) {
                pcap_write_buffer[i - pcap_written] = pcap_write_buffer[i];
            }
            pcap_write_count -= pcap_written;
        }
        if (ble_pcap_written > 0 && ble_pcap_written <= ble_pcap_write_count) {
            for (int i = ble_pcap_written; i < ble_pcap_write_count; i++) {
                ble_pcap_write_buffer[i - ble_pcap_written] = ble_pcap_write_buffer[i];
            }
            ble_pcap_write_count -= ble_pcap_written;
        }
        xSemaphoreGiveRecursive(dataMutex);
    }

    xSemaphoreGive(sdMutex);

    if (log_written > 0 || pcap_written > 0 || ble_pcap_written > 0) {
        last_sd_flush = millis();
    }
}

// Thread-safe direct toast setter. Used for single-message notifications
// (battery warnings, flash errors, export-mode messages) that bypass the queue.
// Does NOT touch the queue — it only sets the currently-displayed toast.
static void set_toast_direct(const char* text, uint16_t accent, bool is_info) {
    if (!take_data_mutex()) return;
    ToastEntry& head = toast_queue[toast_queue_head];
    strncpy(head.text, text, sizeof(head.text) - 1);
    head.text[sizeof(head.text) - 1] = '\0';
    head.accent    = accent;
    head.is_action = is_info;
    if (!toast_active) {
        toast_queue_count = 1;
    }
    toast_start  = millis();
    toast_active = true;
    screen_dirty = true;
    give_data_mutex();
}

void trigger_toast(const char* type, const char* name, int confidence) {
    uint16_t accent;
    if (strcmp(type, "TARGET") == 0) accent = TOAST_SUCCESS;
    else if (confidence == 0)        accent = TOAST_NEUTRAL;
    else                             accent = TOAST_WARNING;

    const char* src = (name && name[0] != '\0' && strcmp(name, "Hidden") != 0) ? name : type;
    bool is_action = (confidence == 0);
    char full_text[TOAST_TEXT_LEN];
    if (is_action) {
        snprintf(full_text, sizeof(full_text), "%.*s",
                 (int)sizeof(full_text) - 1, src);
    } else {
        char pct_str[6];
        snprintf(pct_str, sizeof(pct_str), " %d%%", confidence);
        int src_budget = (int)sizeof(full_text) - 1 - (int)strlen(pct_str);
        if (src_budget < 1) src_budget = 1;
        snprintf(full_text, sizeof(full_text), "%.*s%s",
                 src_budget, src, pct_str);
    }

    if (!take_data_mutex()) return;

    // Enqueue; if full, drop oldest to make room
    if (toast_queue_count >= TOAST_QUEUE_SIZE) {
        toast_queue_head = (toast_queue_head + 1) % TOAST_QUEUE_SIZE;
        toast_queue_count--;
    }
    int slot = (toast_queue_head + toast_queue_count) % TOAST_QUEUE_SIZE;
    strncpy(toast_queue[slot].text, full_text, sizeof(toast_queue[slot].text) - 1);
    toast_queue[slot].text[sizeof(toast_queue[slot].text) - 1] = '\0';
    toast_queue[slot].accent = accent;
    toast_queue[slot].is_action = is_action;
    toast_queue_count++;

    // If nothing currently showing, activate immediately
    if (!toast_active) {
        toast_start  = millis();
        toast_active = true;
    }
    screen_dirty = true;

    give_data_mutex();
}

void add_to_capture_history(const char* type, const char* mac, const char* name, int rssi, int confidence) {
    for (int i = CAPTURE_HISTORY_SIZE - 1; i > 0; i--) capture_history[i] = capture_history[i - 1];
    strncpy(capture_history[0].type, type, sizeof(capture_history[0].type) - 1);
    capture_history[0].type[sizeof(capture_history[0].type) - 1] = '\0';
    strncpy(capture_history[0].mac, mac, sizeof(capture_history[0].mac) - 1);
    capture_history[0].mac[sizeof(capture_history[0].mac) - 1] = '\0';
    strncpy(capture_history[0].name, name, sizeof(capture_history[0].name) - 1);
    capture_history[0].name[sizeof(capture_history[0].name) - 1] = '\0';
    capture_history[0].rssi       = rssi;
    capture_history[0].confidence = confidence;
    format_time_buf((millis() - session_start_time) / 1000,
                    capture_history[0].time, sizeof(capture_history[0].time));

    // Caller (log_detection) already holds dataMutex — read GPS directly.
    // Without this, the new entry inherits stale lat/lng from the shifted
    // entry below it, then save_detections_to_flash persists wrong coords.
    if (gps.location.isValid() && gps.location.age() < 2000) {
        capture_history[0].lat = gps.location.lat();
        capture_history[0].lng = gps.location.lng();
    } else {
        capture_history[0].lat = 0.0;
        capture_history[0].lng = 0.0;
    }

    if (capture_history_count < CAPTURE_HISTORY_SIZE) capture_history_count++;
}

void log_detection(const char* type, const char* proto, int rssi, const char* mac,
                   const char* name, int channel, int tx_power,
                   const char* extra_data, const char* detection_method,
                   int confidence, int seq_num, uint32_t epoch_hint = 0) {
    unsigned long now_ms = millis();
    char current_time[9];
    format_time_buf((now_ms - session_start_time) / 1000, current_time, sizeof(current_time));

    // Brief window 1: GPS epoch computation
    uint32_t ts_epoch = 0;
    bool ts_is_gps = false;
    {
        if (xSemaphoreTakeRecursive(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2020 && gps.date.year() <= 2099) {
                uint32_t epoch = utc_to_epoch(
                    gps.date.year(), gps.date.month(), gps.date.day(),
                    gps.time.hour(), gps.time.minute(), gps.time.second());
                if (epoch > 0) { ts_epoch = epoch; ts_is_gps = true; }
            }
            xSemaphoreGiveRecursive(dataMutex);
        } else {
            Serial.println("[MUTEX] log_detection: GPS epoch window timeout");
        }
    }

    // 5 GHz hits arrive from the time-synced C5 with their own detection epoch.
    // Use it only when our own GPS epoch is unavailable — live GPS stays authoritative.
    if (!ts_is_gps && epoch_hint > 0) {
        ts_epoch  = epoch_hint;
        ts_is_gps = true;        // drives the datestamp derivation below
    }

    // Brief window 2: is_new check, counters, history, LED
    bool is_new = false;
    int assigned_det_id = 0;
    uint16_t blip_col = ACCENT_COLOR;
    if (xSemaphoreTakeRecursive(dataMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        Serial.println("[MUTEX] log_detection: main window timeout — skipping counters");
        return;
    }
    is_new = !is_mac_recently_seen(mac);

    if (is_new) {
        add_seen_mac(mac);
        bool is_raven = strstr(type, "RAVEN") != NULL;
        bool is_axon = strstr(type, "AXON") != NULL;
        if (strcmp(proto, "WIFI") == 0) {
            lifetime_wifi++;
            if (is_axon) session_axon++;
            else         session_flock_wifi++;
            blip_col = ACCENT_COLOR;   // condensed: all detections use accent
        } else {
            lifetime_ble++; blip_col = ACCENT_COLOR;
            if (is_raven)      session_raven++;
            else if (is_axon)  session_axon++;
            else               session_flock_ble++;
        }
        lifetime_flock_total++;
        add_to_capture_history(type, mac, name, rssi, confidence);
        // trigger_toast() is deferred until after we release dataMutex to minimize
        // critical section duration.
        // Flash LED: derive color from palette so night mode stays consistent
        {
            uint16_t fc = (strcmp(proto, "WIFI") == 0) ? HEADER_COLOR : PURPLE_COLOR;
            led_detect_r = ((fc >> 11) & 0x1F) << 3;
            led_detect_g = ((fc >> 5)  & 0x3F) << 2;
            led_detect_b = ( fc        & 0x1F) << 3;
        }
        led_detection_flash_until = millis() + 15000;
        led_detect_active = true;

        // Append to sd_hist in-memory so the Detections screen doesn't
        // need to re-scan the SD log file. We already hold dataMutex.
        // Without this, every new MAC would trigger a full PlumeLog.csv
        // re-read in loop() — which is megabytes after a day of use.

        // Remove any existing entry with the same MAC before inserting.
        // Keeps one entry per device when re-detected after the seen_mac window.
        for (int i = 0; i < sd_hist_count; i++) {
            if (strncmp(sd_hist[i].mac, mac, 17) == 0) {
                for (int j = i; j < sd_hist_count - 1; j++) sd_hist[j] = sd_hist[j + 1];
                sd_hist_count--;
                break;
            }
        }

        int new_count = (sd_hist_count < SD_HIST_SIZE) ? sd_hist_count + 1 : SD_HIST_SIZE;
        for (int i = new_count - 1; i > 0; i--) sd_hist[i] = sd_hist[i - 1];
        safe_copy(sd_hist[0].type,   type,             sizeof(sd_hist[0].type));
        safe_copy(sd_hist[0].mac,    mac,              sizeof(sd_hist[0].mac));
        safe_copy(sd_hist[0].name,   name,             sizeof(sd_hist[0].name));
        safe_copy(sd_hist[0].method, detection_method, sizeof(sd_hist[0].method));
        sd_hist[0].rssi       = rssi;
        sd_hist[0].confidence = confidence;
        sd_hist[0].uptime_ms  = now_ms;
        format_time_buf((millis() - session_start_time) / 1000,
                        sd_hist[0].timestamp, sizeof(sd_hist[0].timestamp));
        // Sequential ID — assigned to both the SD-history mirror and the
        // in-memory capture_history entry that add_to_capture_history just
        // pushed at index 0. Counter is persisted in PERSIST_FILE.
        sd_hist[0].id         = (int)next_detection_id;
        capture_history[0].id = (int)next_detection_id;
        assigned_det_id       = (int)next_detection_id;
        next_detection_id++;
        if (next_detection_id > 999999) {
            next_detection_id = 1;
        }
        if (sd_hist_count < SD_HIST_SIZE) sd_hist_count++;
        // New fields: datestamp (local time), GPS coordinates, epoch (always UTC)
        sd_hist[0].epoch_utc = ts_is_gps ? ts_epoch : 0;
        if (ts_is_gps && ts_epoch > 0 && auto_tz_valid) {
            // Derive datestamp from local epoch so 11pm Eastern shows today, not tomorrow
            uint32_t local_epoch = (uint32_t)((int64_t)ts_epoch + (int32_t)auto_tz_offset * 3600);
            uint32_t days = local_epoch / 86400UL;
            uint32_t y = 1970, m = 1, d = 1;
            while (true) {
                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t dy = leap ? 366 : 365;
                if (days < dy) break;
                days -= dy; y++;
            }
            static const uint8_t mdays_ld[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            for (m = 1; m <= 12; m++) {
                bool leap2 = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t md = mdays_ld[m-1] + (m == 2 && leap2 ? 1 : 0);
                if (days < md) { d = days + 1; break; }
                days -= md;
            }
            snprintf(sd_hist[0].datestamp, sizeof(sd_hist[0].datestamp),
                     "%02u/%02u/%02u", (unsigned)m, (unsigned)d, (unsigned)(y % 100));
        } else if (gps.date.isValid() && gps.date.year() >= 2020 && gps.date.year() <= 2099) {
            // Fallback: UTC date (no timezone computed yet)
            snprintf(sd_hist[0].datestamp, sizeof(sd_hist[0].datestamp),
                     "%02d/%02d/%02d",
                     gps.date.month(), gps.date.day(), gps.date.year() % 100);
        } else {
            safe_copy(sd_hist[0].datestamp, "--/--/--", sizeof(sd_hist[0].datestamp));
        }
        if (gps.location.isValid() && gps.location.age() < 2000) {
            sd_hist[0].lat = gps.location.lat();
            sd_hist[0].lng = gps.location.lng();
        } else {
            sd_hist[0].lat = 0.0;
            sd_hist[0].lng = 0.0;
        }

        // Leave sd_hist_dirty alone — the loop's was_dirty path becomes a
        // safety net rather than a normal-flow trigger. (Boot, screen
        // entry, and hot-plug remount still call load_sd_history directly.)
    }

    safe_copy(last_cap_type,       type,             sizeof(last_cap_type));
    safe_copy(last_cap_mac,        mac,              sizeof(last_cap_mac));
    safe_copy(last_cap_name,       name,             sizeof(last_cap_name));
    safe_copy(last_cap_time,       current_time,     sizeof(last_cap_time));
    safe_copy(last_cap_det_method, detection_method, sizeof(last_cap_det_method));
    xSemaphoreGiveRecursive(dataMutex);

    if (is_new) {
        trigger_toast(type, name, confidence);
        add_blip(blip_col, rssi);
        screen_dirty = true;

        // Scanner-screen reactive animations — consumed inside
        // draw_scanner_screen(). Tint colour mirrors the LED flash
        // colour pattern (WiFi=accent, BLE=purple). The spectrum bar
        // for the active channel highlights based on per-channel
        // packet counts, so no separate per-detection trigger is
        // needed for that.
        scanner_flash_ms    = millis();
        scanner_flash_proto = (strcmp(proto, "WIFI") == 0) ? 0 : 1;
    }

    // Heavy work outside mutex
    if (is_new && sd_available) {
        char clean_name[64];
        safe_copy(clean_name, name, sizeof(clean_name));
        for (char* p = clean_name; *p; p++) if (*p == ',') *p = ' ';

        char clean_extra[64];
        safe_copy(clean_extra, extra_data, sizeof(clean_extra));
        for (char* p = clean_extra; *p; p++) if (*p == ',') *p = ' ';

        // After the comma strip (which keeps the 21 columns aligned) and before
        // the row is assembled. Only these two fields: every other column is an
        // internally generated fixed string or a numeric, never attacker-controlled.
        csv_defuse_formula(clean_name,  sizeof(clean_name));
        csv_defuse_formula(clean_extra, sizeof(clean_extra));

        // Brief window: GPS snapshot
        char gps_fields[80];
        bool gps_fresh = false; double g_lat=0, g_lng=0; float g_spd=0, g_crs=0, g_alt=0;
        if (xSemaphoreTakeRecursive(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            gps_fresh = gps.location.isValid() && (gps.location.age() < 2000);
            if (gps_fresh) {
                g_lat = gps.location.lat(); g_lng = gps.location.lng();
                g_spd = gps.speed.isValid()    ? gps.speed.mph()       : 0.0f;
                g_crs = gps.course.isValid()   ? gps.course.deg()      : 0.0f;
                g_alt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0f;
            }
            xSemaphoreGiveRecursive(dataMutex);
        }

        if (gps_fresh)
            snprintf(gps_fields, sizeof(gps_fields), "%.6f,%.6f,%.1f,%.1f,%.1f", g_lat, g_lng, g_spd, g_crs, g_alt);
        else
            strncpy(gps_fields, "0.000000,0.000000,0.0,0.0,0.0", sizeof(gps_fields));

        char local_line[SD_LINE_LEN];
        snprintf(local_line, SD_LINE_LEN,
            "%lu,%lu,%d,%d,%s,%s,%d,%s,%s,%d,%s,%d,%s,%s,%d,%s,%d",
            now_ms, (unsigned long)ts_epoch, ts_is_gps ? 1 : 0, channel, type, proto, rssi, mac, clean_name,
            tx_power, detection_method, confidence,
            confidence_label(confidence), clean_extra, seq_num, gps_fields, assigned_det_id);

        // Brief window 3: commit line to sd_write_buffer
        if (xSemaphoreTakeRecursive(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (sd_write_count < MAX_LOG_BUFFER) {
                strncpy(sd_write_buffer[sd_write_count], local_line, SD_LINE_LEN - 1);
                sd_write_buffer[sd_write_count][SD_LINE_LEN - 1] = '\0';
                sd_write_count++;
            } else {
                sd_log_dropped++;
            }
            xSemaphoreGiveRecursive(dataMutex);
        } else {
            Serial.println("[MUTEX] log_detection: SD write buffer timeout — detection dropped from log");
        }
    }
}

// Called under dataMutex (caller already holds it recursively).
// Feeds an RSSI sample into the tracker, trace buffer, and peak bookmark.
static void signal_feed_rssi(const char* mac, int rssi) {
    rssi_track_update(mac, rssi);

    unsigned long now = millis();
    if (sig_trace_count == 0 || (now - sig_trace_last_sample) >= SIG_TRACE_INTERVAL_MS) {
        sig_trace[sig_trace_head].rssi = (int8_t)(rssi < -128 ? -128 : rssi > 127 ? 127 : rssi);
        sig_trace_head = (sig_trace_head + 1) % SIG_TRACE_SIZE;
        if (sig_trace_count < SIG_TRACE_SIZE) sig_trace_count++;
        sig_trace_last_sample = now;
        sig_trace_total++;
    }

    if (rssi > signal_peak_rssi) {
        signal_peak_rssi = rssi;
        // Reports arrive faster than the 2 s trace cadence, so pin the peak to
        // the most recently written sample. Worst case the tick sits one sample
        // (~4 px) off, which is inside the width of the tick itself.
        signal_peak_seq = sig_trace_total - 1;
    }

    signal_newest_sample_ms = now;
}

void signal_start(const char* mac, const char* name, const char* type = "", int id = 0) {
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);

    signal_active = false;

    sig_trace_head = 0; sig_trace_count = 0; sig_trace_last_sample = 0;
    sig_trace_last_frame_ms = 0;
    signal_bar_smooth = 0.0f;
    signal_bar_seeded = false;

    signal_peak_rssi = -120;
    signal_peak_seq  = -1;
    sig_trace_total  = 0;

    signal_tracker_idx = -1;
    signal_newest_sample_ms = 0;

    strncpy(signal_target_mac,  mac,  17); signal_target_mac[17]  = '\0';
    strncpy(signal_target_name, name, sizeof(signal_target_name) - 1);
    signal_target_name[sizeof(signal_target_name) - 1] = '\0';
    strncpy(signal_target_type, type, sizeof(signal_target_type) - 1);
    signal_target_type[sizeof(signal_target_type) - 1] = '\0';

    signal_active = true;

    xSemaphoreGiveRecursive(dataMutex);
    screen_dirty = true;
}

void signal_stop() {
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);

    signal_active = false;

    signal_target_mac[0]  = '\0';
    signal_target_name[0] = '\0';
    signal_target_type[0] = '\0';

    signal_peak_rssi = -120;
    signal_peak_seq  = -1;
    sig_trace_total  = 0;

    signal_tracker_idx = -1;
    signal_newest_sample_ms = 0;

    sig_trace_head = 0; sig_trace_count = 0; sig_trace_last_sample = 0;
    sig_trace_last_frame_ms = 0;
    signal_bar_smooth = 0.0f;
    signal_bar_seeded = false;

    xSemaphoreGiveRecursive(dataMutex);
    screen_dirty = true;
}

// ============================================================================
// WIFI PACKET HANDLER — LOCK-FREE DEFERRED PROCESSING
// ============================================================================
typedef struct {
    unsigned frame_ctrl:16; unsigned duration_id:16; uint8_t addr1[6];
    uint8_t addr2[6]; uint8_t addr3[6]; unsigned sequence_ctrl:16; uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct { wifi_ieee80211_mac_hdr_t hdr; uint8_t payload[0]; } wifi_ieee80211_packet_t;

#define WIFI_EVENT_QUEUE_SIZE 16

struct WifiEvent {
    uint8_t  mac[6];      // addr2 — transmitter
    uint8_t  addr1[6];    // addr1 — receiver/destination (sleeping-device check)
    int8_t   rssi;
    uint8_t  channel;
    uint16_t seq_num;
    char     ssid[33];
    bool     is_beacon;
    bool     is_probe_req;   // mgmt subtype 4 — required for the wildcard-probe signature
    uint8_t  payload_snap[128];
    uint16_t payload_snap_len;
    uint16_t orig_len;
    volatile uint32_t ready;
    uint8_t  vendor_ouis[4][3];
    uint8_t  vendor_oui_count;
};

static WifiEvent wifi_event_queue[WIFI_EVENT_QUEUE_SIZE];
// Written only from the WiFi promiscuous callback (single task context on Core 0).
static volatile uint32_t wifi_eq_write_idx = 0;
static uint8_t           wifi_eq_read_idx  = 0;
static volatile uint32_t wifi_pkt_enqueued = 0;
static volatile uint32_t wifi_pkt_dropped  = 0;

void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type) {
    if (!scanner_ready) return;
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buff;
    if (ppkt->rx_ctrl.sig_len < 24) return;
    __atomic_fetch_add(&ambient_packet_count, 1, __ATOMIC_RELAXED);

    // Per-channel histogram for the SPECTRUM viz on the scanner screen.
    {
        uint8_t pkt_ch = ppkt->rx_ctrl.channel;
        if (pkt_ch >= 1 && pkt_ch <= NUM_WIFI_CHANNELS) {
            __atomic_fetch_add(&channel_pkt_counts[pkt_ch - 1], 1, __ATOMIC_RELAXED);
        }
    }

    const wifi_ieee80211_packet_t* ipkt = (wifi_ieee80211_packet_t*)ppkt->payload;
    const wifi_ieee80211_mac_hdr_t* hdr = &ipkt->hdr;
    uint8_t frame_type    = (hdr->frame_ctrl & 0x0C) >> 2;
    uint8_t frame_subtype = (hdr->frame_ctrl & 0xF0) >> 4;
    if (frame_type != 0) return;
    bool is_beacon    = (frame_subtype == 8);
    bool is_probe_req = (frame_subtype == 4);
    if (!is_beacon && !is_probe_req) return;

    // Check the slot we're about to write rather than the next one — the
    // previous "next.ready" gate wasted a slot and capped capacity at 7
    // when the consumer was even one step behind.
    uint32_t cur_idx = __atomic_load_n(&wifi_eq_write_idx, __ATOMIC_RELAXED);
    if (__atomic_load_n(&wifi_event_queue[cur_idx].ready, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&wifi_pkt_dropped, 1u, __ATOMIC_RELAXED);
        return;
    }
    uint32_t next = (cur_idx + 1) % WIFI_EVENT_QUEUE_SIZE;

    WifiEvent* ev = &wifi_event_queue[cur_idx];

    // Copy only raw metadata and the frame snapshot.
    // SSID extraction, RSN parsing, and vendor OUI collection are
    // deferred to process_wifi_event_queue() via parse_wifi_event() —
    // keeps the callback fast and avoids back-pressure on the WiFi
    // driver's internal queue in dense RF environments.
    memcpy(ev->mac, hdr->addr2, 6);
    memcpy(ev->addr1, hdr->addr1, 6);
    ev->rssi             = (int8_t)ppkt->rx_ctrl.rssi;
    ev->channel          = (uint8_t)ppkt->rx_ctrl.channel;
    ev->seq_num          = (uint16_t)(hdr->sequence_ctrl >> 4);
    ev->is_beacon        = is_beacon;
    ev->is_probe_req     = is_probe_req;
    // sig_len is the OTA frame length. Sanity-cap before snapshotting:
    // values > 512 indicate DMA corruption or a malformed rx_ctrl struct.
    uint16_t sig_len = (uint16_t)ppkt->rx_ctrl.sig_len;
    if (sig_len < 24) {
        // Shouldn't reach here (checked above), but guard the snapshot path.
        __atomic_store_n(&ev->ready, 0u, __ATOMIC_RELEASE);
        return;
    }
    ev->orig_len         = sig_len;
    if (sig_len > 512) sig_len = 512;
    ev->payload_snap_len = (sig_len < sizeof(ev->payload_snap)) ? sig_len : (uint16_t)sizeof(ev->payload_snap);
    memcpy(ev->payload_snap, ppkt->payload, ev->payload_snap_len);

    // Clear parsed fields — they'll be populated by parse_wifi_event()
    memset(ev->ssid, 0, sizeof(ev->ssid));
    ev->vendor_oui_count = 0;

    __atomic_store_n(&ev->ready, 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&wifi_pkt_enqueued, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&wifi_eq_write_idx, next, __ATOMIC_RELAXED);
}

// ── IE fingerprinting ───────────────────────────────────────────────────────
// The set and ORDER of information elements a device emits is a property of its
// WiFi chipset and firmware, not of its OUI. A vendor buying a new OUI block
// invalidates every address-based signature; it does not change the silicon. So
// an IE-order hash is a signature that survives OUI rotation.
//
// What this actually sees, and why that matters: payload_snap is 128 bytes, and
// a beacon's IEs start at offset 36 (24-byte MAC header + 12 fixed params), so
// there are at most 92 bytes of IEs available — roughly the first six to ten
// elements. Real beacons run 200-400 bytes, so MOST are already truncated
// before this code runs. Early IE ordering is reasonably distinctive and is the
// part a vendor cannot casually reorder, but the vendor-specific IEs that carry
// the most discriminating power usually sit near the tail, out of reach.
// Raising payload_snap to 192 would capture ~156 bytes at a cost of 1,024 bytes
// of static RAM (16 slots x 64). Not spent yet: ship at 128, record whether each
// fingerprint was truncated, and let field data decide whether the kilobyte is
// worth it. Measure, then buy.
//
// These live in an out-param rather than in WifiEvent on purpose. parse_wifi_event
// runs on the consumer's single stack copy, so a field added to WifiEvent would
// exist in all 16 ring slots while only ever being written in one — 128 bytes of
// .bss for nothing. (The pre-existing ssid/vendor_ouis fields have exactly this
// problem and cost 736 bytes; not fixed here, but that is the cheapest reclaim
// in the struct if RAM gets tight.)
struct IeParse {
    uint32_t fp;         // FNV-1a over the IE tag sequence
    uint8_t  count;      // IEs successfully walked
    bool     truncated;  // snapshot cut the list short — fingerprint is partial
};

struct IeFingerprint {
    uint32_t    fp;
    uint8_t     min_ies;    // reject matches computed over fewer IEs than this
    uint8_t     weight;     // confidence points contributed
    const char* label;
};

// Deliberately empty. A fingerprint cannot come from a datasheet — it has to be
// captured from real devices. With DEBUG_KEYS on, every beacon from a known OUI
// prints its fp/count/truncated so candidates can be collected on a drive-by;
// a value only earns a place here once the same physical unit reproduces it
// across multiple passes. Until then this table makes the feature inert, which
// is the correct default for a scoring input nobody has validated.
static const IeFingerprint IE_FINGERPRINTS[] = {
    // { 0x9A3C21F0, 6, 25, "flock_ie_fp" },
    { 0, 0, 0, nullptr },   // sentinel — keeps the array non-empty for C++
};
static const int IE_FINGERPRINT_COUNT =
    (int)(sizeof(IE_FINGERPRINTS) / sizeof(IE_FINGERPRINTS[0])) - 1;   // minus sentinel

// Parse tagged parameters from a locally-copied WiFi event. Extracts SSID,
// RSN/WPA2-PSK status, vendor OUIs, and the IE-order fingerprint from the raw
// payload snapshot. Called from process_wifi_event_queue() after the event has
// been copied out of the ring buffer — never from ISR/callback context.
static void parse_wifi_event(WifiEvent* ev, IeParse* ie) {
    // Cleared unconditionally, OUTSIDE the is_beacon branch below. A probe
    // request never enters the IE walk, and leaving these uninitialized would
    // hand the caller whatever the last event left on the stack — a garbage fp
    // that could false-match a fingerprint if the is_beacon guard at the scoring
    // site were ever relaxed.
    ie->fp        = 0;
    ie->count     = 0;
    ie->truncated = false;

    // Locate the tagged parameters within the frame body.
    // Management frame: 24-byte MAC header, then frame body.
    // Beacon: 12 bytes of fixed fields (timestamp, interval, capability)
    //         before tagged parameters.
    // Probe Request: tagged parameters start immediately after MAC header.
    if (ev->payload_snap_len < 24) return;
    if (ev->payload_snap_len > sizeof(ev->payload_snap)) return;

    uint8_t* frame_body = ev->payload_snap + 24;
    uint8_t* tagged_params;
    int remaining;

    if (ev->is_beacon) {
        if (ev->payload_snap_len < 24 + 12 + 2) return;
        tagged_params = frame_body + 12;
        remaining = (int)ev->payload_snap_len - 24 - 12;
    } else {
        tagged_params = frame_body;
        remaining = (int)ev->payload_snap_len - 24;
    }
    // Subtract FCS (4 bytes) only when the full frame fits in the snapshot.
    // Truncated frames (payload_snap_len < orig_len) don't contain the FCS;
    // subtracting 4 would discard real tagged parameter data at the tail.
    if (ev->payload_snap_len >= ev->orig_len) {
        remaining -= 4;
        if (remaining < 0) remaining = 0;
    }

    // ── SSID extraction (tag 0) ──
    memset(ev->ssid, 0, sizeof(ev->ssid));
    if (remaining > 2 && tagged_params[0] == 0
        && tagged_params[1] <= 32 && tagged_params[1] <= remaining - 2) {
        memcpy(ev->ssid, &tagged_params[2], tagged_params[1]);
        ev->ssid[tagged_params[1]] = '\0';
    }

    // ── Vendor OUI parsing ──
    ev->vendor_oui_count = 0;

    if (ev->is_beacon) {
        uint8_t* p = tagged_params;
        int rem = remaining;
        uint32_t fp = 2166136261u;   // FNV-1a offset basis
        uint8_t  ie_n = 0;

        while (rem >= 2) {
            uint8_t tag_id = p[0];
            uint8_t tag_len = p[1];
            // A tag claiming more bytes than remain is what a mid-IE snapshot
            // cut looks like from here, so it marks the fingerprint partial
            // rather than just ending the walk.
            if (tag_len > rem - 2) { ie->truncated = true; break; }

            // Fold the tag id. Order matters — the ordering IS the fingerprint.
            fp ^= tag_id;  fp *= 16777619u;

            // Vendor-specific (221): fold OUI + vendor type too. These carry
            // most of the discriminating power on the rare frames where they
            // fit inside the snapshot.
            if (tag_id == 221 && tag_len >= 4) {
                for (int k = 0; k < 4; k++) { fp ^= p[2 + k]; fp *= 16777619u; }
            }

            ie_n++;

            // Vendor-specific IE (tag 221) — collect unique OUIs
            if (tag_id == 221 && tag_len >= 4 && ev->vendor_oui_count < 4) {
                bool seen = false;
                for (int k = 0; k < ev->vendor_oui_count; k++) {
                    if (memcmp(ev->vendor_ouis[k], p + 2, 3) == 0) { seen = true; break; }
                }
                if (!seen) {
                    memcpy(ev->vendor_ouis[ev->vendor_oui_count], p + 2, 3);
                    ev->vendor_oui_count++;
                }
            }

            p += 2 + tag_len;
            rem -= 2 + tag_len;
        }

        ie->fp    = fp;
        ie->count = ie_n;
        // Either condition means the fingerprint covers only part of the frame.
        if (ev->payload_snap_len < ev->orig_len) ie->truncated = true;
    }
}

void process_wifi_event_queue() {
    // Drain the full queue each call. parse_wifi_event is light (~0.1ms/event),
    // and loop() runs ~100Hz (10ms vTaskDelay), so capacity is roughly
    // WIFI_EVENT_QUEUE_SIZE per tick. 16 slots gives burst headroom in dense RF;
    // overflow increments wifi_pkt_dropped (surfaced on the stats screen).
    int budget = WIFI_EVENT_QUEUE_SIZE;
    while (budget-- > 0 &&
           __atomic_load_n(&wifi_event_queue[wifi_eq_read_idx].ready, __ATOMIC_ACQUIRE)) {
        WifiEvent* ev = &wifi_event_queue[wifi_eq_read_idx];

        WifiEvent local;
        memcpy(&local, ev, sizeof(WifiEvent));
        __atomic_store_n(&ev->ready, 0u, __ATOMIC_RELEASE);
        wifi_eq_read_idx = (wifi_eq_read_idx + 1) % WIFI_EVENT_QUEUE_SIZE;

        // Weak-signal filter, but NOT for known Flock silicon. This used to
        // discard below -80 dBm before any signature check ran, which capped
        // detection range blind — a camera at -85 was thrown away unexamined.
        // check_mac_prefix() is 54 three-byte memcmps against rt_oui[].bytes,
        // so consulting it here is essentially free. addr1 is checked too, since
        // the sleeping-device technique keys off the receiver address.
        if (local.rssi < IGNORE_WEAK_RSSI
            && check_mac_prefix(local.mac)   == OUI_NONE
            && check_mac_prefix(local.addr1) == OUI_NONE) continue;

        // Parse tagged parameters from the raw payload snapshot.
        // This was previously done in the sniffer callback; deferring it
        // here keeps the callback fast and prevents back-pressure on the
        // WiFi driver's internal queue in dense RF environments.
        IeParse ie;
        parse_wifi_event(&local, &ie);

        clean_device_name_char(local.ssid);

        // Skip locally-administered (randomized) MACs for OUI scoring — a
        // randomized MAC can coincidentally match a Flock OUI prefix. SSID
        // pattern matching still runs (it doesn't depend on OUI), so a real
        // Flock device with a randomized MAC can still be detected via SSID.
        // BLE handles this via addr_type; WiFi needs an explicit bit check.
        bool is_random_mac = (local.mac[0] & 0x02) != 0;
        int  mac_score     = is_random_mac ? 0 : check_mac_prefix(local.mac);

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
            local.mac[0], local.mac[1], local.mac[2],
            local.mac[3], local.mac[4], local.mac[5]);

        // Push to live feed (every observed device, preview flock status)
        {
            const char* feed_name = (strlen(local.ssid) > 0) ? local.ssid : "Hidden";
            bool preview_is_flock  = (strlen(local.ssid) > 0
                                      && (is_flock_ssid_format(local.ssid)
                                          || check_ssid_pattern(local.ssid)))
                                     || mac_score == 1;
            feed_push_candidate(mac_str, feed_name, local.rssi, 0, preview_is_flock);
        }

        // Feed RSSI to the Signal screen for any active target, regardless of confidence.
        if (signal_active) {
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            if (signal_active && strncmp(mac_str, signal_target_mac, 17) == 0)
                signal_feed_rssi(mac_str, local.rssi);
            xSemaphoreGiveRecursive(dataMutex);
        }

        // IEEE vendor attribution is exact, but an OUI cannot distinguish an
        // Axon body camera, vehicle system, dock, or another Axon product. Log
        // it as associated evidence without sounding an alarm or claiming a
        // model. Corroborated/high-confidence Axon classification is BLE-only
        // until field captures establish a safe Wi-Fi discriminator.
        if (!is_random_mac && memcmp(local.mac, AXON_OUI, sizeof(AXON_OUI)) == 0) {
            if (!is_mac_whitelisted(mac_str)) {
                const char* axon_name = strlen(local.ssid) > 0 ? local.ssid : "Axon-associated";
                log_detection("AXON_ASSOC", "WIFI", local.rssi, mac_str, axon_name,
                              local.channel, 0,
                              local.is_beacon ? "Beacon" : "ProbeReq",
                              "axon_oui", SCORE_STRONG, local.seq_num);
                write_threat_pcap(local.payload_snap, local.payload_snap_len);
            }
            continue;
        }

        int  confidence    = 0;
        char methods[64]   = {0};
        bool ssid_generic  = (strlen(local.ssid) > 0 && check_ssid_pattern(local.ssid));
        bool ssid_flock_fmt = (strlen(local.ssid) > 0 && is_flock_ssid_format(local.ssid));

        // ── test_flck: CVE-2025-59409 hardcoded SSID, always-on emission ──
        // Falcon/Sparrow firmware emits STA-mode probes for this SSID
        // continuously regardless of hotspot state. Unique to Flock devices.
        if (local.is_probe_req && strlen(local.ssid) > 0
            && strcmp(local.ssid, "test_flck") == 0) {
            confidence = SCORE_DEFINITIVE;
            strlcat(methods, "test_flck_cve ", sizeof(methods));
        }

        if (ssid_flock_fmt) {
            confidence = SCORE_DEFINITIVE; strlcat(methods, "ssid_fmt ", sizeof(methods));
        } else if (mac_score == 1) {
            confidence = SCORE_STRONG; strlcat(methods, "mac_t1 ", sizeof(methods));
            if (ssid_generic) { confidence = SCORE_DEFINITIVE; strlcat(methods, "ssid ", sizeof(methods)); }
        } else {
            if (mac_score == 2) { confidence += SCORE_WEAK; strlcat(methods, "mac_t2 ", sizeof(methods)); }
            if (ssid_generic)   { confidence += SCORE_WEAK; strlcat(methods, "ssid ", sizeof(methods)); }
        }

        // ── Wildcard probe request from a known OUI — DeFlockJoplin signature ──
        // Flock cameras hop channels and spam Probe Requests with empty SSID.
        // Strict subtype check — !is_beacon would also match probe responses,
        // action frames, etc., which can carry empty SSIDs and trigger false
        // positives. Only mgmt subtype 4 (Probe Request) qualifies.
        // Field-tested: 11/12 cameras caught with 2 false positives (Joplin).
        if (local.is_probe_req && mac_score > 0 && strlen(local.ssid) == 0) {
            if (mac_score == 1) {
                // Tier 1 OUI + wildcard probe = definitive Flock signature
                confidence = SCORE_DEFINITIVE;
                strlcat(methods, "wildcard_probe ", sizeof(methods));
            } else {
                // Tier 2 OUI + wildcard probe = strong signal, not definitive
                // Component-vendor OUIs are shared with non-Flock devices
                // that also do wildcard probing during normal WiFi scans
                confidence = SCORE_STRONG;
                strlcat(methods, "wildcard_probe_t2 ", sizeof(methods));
            }
        }

        // ── Cross-channel wildcard-probe behavioral tracker ──
        // Track every wildcard probe regardless of OUI — once a MAC has hopped
        // >= WILDCARD_MIN_CHANNELS distinct channels within WILDCARD_WINDOW_MS
        // it is flagged as behaviorally confirmed.  For unknown-OUI devices this
        // does NOT add confidence (avoids alarming on phones); it only emits a
        // serial candidate line so the operator can curate the OUI list later.
        // Known-OUI devices are already scored above; this only adds logging.
        // Skip randomized (locally-administered) MACs entirely: their OUI is
        // synthetic and worthless for curation, a real Flock unit advertises its
        // true OUI, and privacy-scanning phones would otherwise both flood this
        // log and evict genuine candidates from the 32-slot tracker.
        if (local.is_probe_req && strlen(local.ssid) == 0 && !(local.mac[0] & 0x02)) {
            bool wc_confirmed = wildcard_probe_observe(local.mac, local.channel);
            if (wc_confirmed && mac_score == 0) {
                Serial.printf("WILDCARD-CANDIDATE OUI %02x:%02x:%02x RSSI %d\n",
                              local.mac[0], local.mac[1], local.mac[2], local.rssi);
            }
        }

        // ── IE-order fingerprint match ──
        // Corroboration, never standalone proof. Weight is chosen so a match
        // alone cannot cross CONFIDENCE_ALARM_THRESHOLD (70): a device whose OUI
        // has rotated but whose fingerprint matches should read HIGH, not
        // CERTAIN, until the fingerprint has held across many samples. Halved on
        // a truncated frame, because a hash over six IEs out of twenty is a
        // weaker claim than one over the whole set.
        //
        // The ie_count >= 4 floor is separate from each entry's min_ies: it
        // rejects the degenerate case where a snapshot caught almost nothing, so
        // the collision space is too small for a match to mean anything.
        if (local.is_beacon && ie.count >= 4) {
            for (int i = 0; i < IE_FINGERPRINT_COUNT; i++) {
                if (IE_FINGERPRINTS[i].fp == ie.fp &&
                    ie.count >= IE_FINGERPRINTS[i].min_ies) {
                    int w = IE_FINGERPRINTS[i].weight;
                    if (ie.truncated) w /= 2;
                    confidence += w;
                    strlcat(methods, IE_FINGERPRINTS[i].label, sizeof(methods));
                    strlcat(methods, " ", sizeof(methods));
                    break;
                }
            }
        }

#if DEBUG_KEYS
        // Fingerprint collection. Prints only for beacons from an OUI already in
        // rt_oui[], so a drive-past of known units yields candidates without
        // burying them in ambient traffic. A value is only trustworthy once the
        // same physical unit reproduces it across passes — if it varies, check
        // ie_count first: a fingerprint that moves because the frame was cut at
        // a different point is a measurement artifact, not a rotating IE set.
        if (local.is_beacon && mac_score > 0 && ie.count > 0) {
            Serial.printf("IE-FP %s fp=%08lX ies=%u trunc=%d snap=%u/%u\n",
                          mac_str, (unsigned long)ie.fp, (unsigned)ie.count,
                          ie.truncated ? 1 : 0,
                          (unsigned)local.payload_snap_len, (unsigned)local.orig_len);
        }
#endif

        if (confidence > 0 && local.rssi > -50) confidence += SCORE_BONUS_RSSI;

        const char* name_str       = (strlen(local.ssid) > 0) ? local.ssid : "Hidden";
        const char* frame_type_str = local.is_beacon ? "Beacon" : "ProbeReq";

        // ── addr1 (receiver) OUI check — catches sleeping Flock devices ──
        // Flock cameras sleep most of their duty cycle but still appear as
        // the destination of probe responses and data frames from nearby APs.
        // Guards: skip broadcast/multicast addr1, skip randomized MACs.
        // Research: @NitekryDPaul promiscuous-mode addr1 technique.
        {
            bool addr1_multicast = (local.addr1[0] & 0x01);
            bool addr1_random    = (local.addr1[0] & 0x02);
            bool addr1_broadcast = (local.addr1[0] == 0xFF && local.addr1[1] == 0xFF);

            if (!addr1_multicast && !addr1_random && !addr1_broadcast) {
                int addr1_mac_score = check_mac_prefix(local.addr1);
                if (addr1_mac_score > 0 && mac_score == 0) {
                    // addr1 matched but addr2 didn't — sleeping-device hit.
                    if (addr1_mac_score == 1) {
                        // RAISE, never assign. A plain `= SCORE_STRONG` demoted an
                        // already-DEFINITIVE hit (test_flck_cve / ssid_fmt set 100)
                        // down to 60 whenever addr2's OUI was unknown and addr1
                        // matched Tier 1 — dropping it below CONFIDENCE_ALARM_
                        // THRESHOLD and silencing the buzzer on a real detection.
                        if (confidence < SCORE_STRONG) confidence = SCORE_STRONG;
                        strlcat(methods, "addr1_t1 ", sizeof(methods));
                    } else {
                        confidence += SCORE_WEAK;
                        strlcat(methods, "addr1_t2 ", sizeof(methods));
                    }

                    // Override mac_str so logging/tracking keys off the actual
                    // Flock device MAC (addr1) rather than the AP (addr2).
                    snprintf(mac_str, sizeof(mac_str),
                             "%02x:%02x:%02x:%02x:%02x:%02x",
                             local.addr1[0], local.addr1[1], local.addr1[2],
                             local.addr1[3], local.addr1[4], local.addr1[5]);
                    feed_push_candidate(mac_str, "Sleeping", local.rssi, 0, true);
                }
            }
        }

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            __atomic_store_n(&channel_lock_until, millis() + 10000, __ATOMIC_RELAXED);
            rssi_track_update(mac_str, local.rssi);
            if (confidence > 100) confidence = 100;

            int mlen = strlen(methods);
            if (mlen > 0 && methods[mlen - 1] == ' ') methods[mlen - 1] = '\0';

            char vendor_str[48] = "";
            if (local.vendor_oui_count > 0) {
                int off = 0;
                for (int k = 0; k < local.vendor_oui_count && off < (int)sizeof(vendor_str) - 12; k++) {
                    off += snprintf(vendor_str + off, sizeof(vendor_str) - off,
                                    "%sv%d:%02X-%02X-%02X",
                                    k == 0 ? "" : " ",
                                    k + 1,
                                    local.vendor_ouis[k][0],
                                    local.vendor_ouis[k][1],
                                    local.vendor_ouis[k][2]);
                }
            }
            char extra_combined[80];
            snprintf(extra_combined, sizeof(extra_combined), "%s %s", frame_type_str, vendor_str);

            if (!is_mac_whitelisted(mac_str)) {
                log_detection("FLOCK_WIFI", "WIFI", local.rssi, mac_str, name_str,
                              local.channel, 0, extra_combined, methods, confidence, local.seq_num);
                write_threat_pcap(local.payload_snap, local.payload_snap_len);

                xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                    trigger_alarm_confidence = confidence;
                    trigger_alarm_source = 0;  // WiFi
                    last_buzzer_time = millis();
                }
                xSemaphoreGiveRecursive(dataMutex);
            }
        }

        // ── Pepwave trailer infrastructure — correlated signal only ──
        // Flock Mobile Security Trailers use Pepwave cellular-bonded routers
        // (OUI A8:C0:EA, SSID "PEPWAVE_*"). Only log when the session already
        // has confirmed Flock detections — a Pepwave router alone is not
        // surveillance-indicative. Field Reference May 2026, §10.
        if (confidence < CONFIDENCE_ALARM_THRESHOLD) {
            bool is_pepwave_oui = (local.mac[0] == 0xa8
                                && local.mac[1] == 0xc0
                                && local.mac[2] == 0xea);
            bool is_pepwave_ssid = (strlen(local.ssid) >= 8
                                 && strncasecmp(local.ssid, "PEPWAVE_", 8) == 0);

            if ((is_pepwave_oui || is_pepwave_ssid)
                && (session_flock_wifi + session_flock_ble) > 0) {

                int pepwave_conf = SCORE_WEAK;
                char pepwave_methods[32] = "";
                if (is_pepwave_oui)  strlcat(pepwave_methods, "pepwave_oui ", sizeof(pepwave_methods));
                if (is_pepwave_ssid) strlcat(pepwave_methods, "pepwave_ssid ", sizeof(pepwave_methods));
                if (is_pepwave_oui && is_pepwave_ssid) pepwave_conf = SCORE_STRONG;

                int mlen = strlen(pepwave_methods);
                if (mlen > 0 && pepwave_methods[mlen - 1] == ' ')
                    pepwave_methods[mlen - 1] = '\0';

                if (!is_mac_whitelisted(mac_str)) {
                    log_detection("FLOCK_INFRA", "WIFI", local.rssi, mac_str,
                                  strlen(local.ssid) > 0 ? local.ssid : "Pepwave",
                                  local.channel, 0, "Pepwave trailer router",
                                  pepwave_methods, pepwave_conf, local.seq_num);
                }
            }
        }
    }
}

// ============================================================================
// BLE CALLBACK — STACK-SAFE DEFERRED PROCESSING
// ============================================================================
// (BleEventData struct, BLE_POOL_SIZE, ble_pool, ble_pool_write,
//  AdvertisedDeviceCallbacks, and ble_cb_singleton are declared above the
//  export server functions so export_mode_start can reference them.)

static void ble_worker_task(void* pvParameters) {
    bool wdt_subscribed = false;
    for (;;) {
        if (!wdt_subscribed) {
            esp_err_t err = esp_task_wdt_add(NULL);
            if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) wdt_subscribed = true;
        }
        if (wdt_subscribed) esp_task_wdt_reset();
        // 1s timeout so the WDT reset above can fire even when the queue is idle.
        uint8_t pool_idx;
        if (xQueueReceive(ble_event_queue, &pool_idx, pdMS_TO_TICKS(1000)) != pdTRUE) continue;

        BleEventData* ev = &ble_pool[pool_idx];

        // Same reasoning as the WiFi path: a known OUI overrides the weak-signal
        // cutoff so distant Flock hardware is still scored.
        if (ev->rssi < IGNORE_WEAK_RSSI && check_mac_prefix(ev->mac) == OUI_NONE) {
            __atomic_store_n(&ev->in_use, 0u, __ATOMIC_RELEASE); continue;
        }

        int  confidence   = 0;
        char methods[64]  = {0};
        char capture_type[16] = "FLOCK_BLE";
        bool got_name = false, got_mfg = false, got_raven = false;
        bool got_axon_mfg = false, got_axon_uuid = false;

        bool got_penguin_name = false;

        int mac_score = check_mac_prefix(ev->mac);

        char dev_name_char[65];
        strncpy(dev_name_char, ev->dev_name, 64);
        dev_name_char[64] = '\0';
        if (ev->have_name) {
            clean_device_name_char(dev_name_char);
            if (check_device_name_pattern(dev_name_char)) {
                strlcat(methods, "name ", sizeof(methods)); got_name = true;
            } else if (is_penguin_numeric_name(dev_name_char)) {
                strlcat(methods, "penguin_num ", sizeof(methods)); got_penguin_name = true;
            }
        }

        // Push to live feed (every observed BLE device)
        {
            char mac_str_feed[18];
            snprintf(mac_str_feed, sizeof(mac_str_feed),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     ev->mac[0], ev->mac[1], ev->mac[2],
                     ev->mac[3], ev->mac[4], ev->mac[5]);
            bool preview_is_flock = (mac_score == 1
                                     || check_device_name_pattern(dev_name_char)
                                     || is_penguin_numeric_name(dev_name_char));
            feed_push_candidate(mac_str_feed,
                                ev->have_name ? dev_name_char : "",
                                ev->rssi, 1, preview_is_flock);
            // Feed RSSI to the Signal screen for any active target, regardless of confidence.
            if (signal_active) {
                xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                if (signal_active && strncmp(mac_str_feed, signal_target_mac, 17) == 0)
                    signal_feed_rssi(mac_str_feed, ev->rssi);
                xSemaphoreGiveRecursive(dataMutex);
            }
        }

        if (ev->have_mfg) {
            if (check_manufacturer_id(ev->mfg_data, ev->mfg_data_len)) {
                strlcat(methods, "mfg_0x09C8 ", sizeof(methods)); got_mfg = true;
                if (has_tn_serial(ev->mfg_data, ev->mfg_data_len)) strlcat(methods, "tn_serial ", sizeof(methods));
            }
            got_axon_mfg = manufacturer_id_equals(ev->mfg_data, ev->mfg_data_len,
                                                  AXON_MFG_COMPANY_ID);
        }

        int raven_custom_count = 0;
        int raven_std_count = 0;
        for (int i = 0; i < ev->uuid_count; i++) {
            uint16_t code;
            if (!uuid_base_short_code(ev->service_uuids[i], &code)) continue;  // non-base UUID can't be Raven
            if (code == AXON_SERVICE_UUID) got_axon_uuid = true;
            for (int j = 0; j < NUM_RAVEN_CUSTOM_UUIDS; j++) {
                if (code == raven_custom_codes[j]) { raven_custom_count++; break; }
            }
            for (int j = 0; j < NUM_RAVEN_STANDARD_UUIDS; j++) {
                if (code == raven_standard_codes[j]) { raven_std_count++; break; }
            }
        }
        int raven_uuid_count = raven_custom_count + raven_std_count;

        if (raven_custom_count > 0 || raven_uuid_count >= 2) {
            strncpy(capture_type, "RAVEN_BLE", sizeof(capture_type)); got_raven = true;
            if (raven_uuid_count >= 3)        strlcat(methods, "raven_multi ", sizeof(methods));
            else if (raven_custom_count > 0)  strlcat(methods, "raven_custom ", sizeof(methods));
            else                              strlcat(methods, "raven_uuid ", sizeof(methods));
        }

        bool got_axon_oui = (ev->addr_type == 0 &&
                             memcmp(ev->mac, AXON_OUI, sizeof(AXON_OUI)) == 0);
        bool is_axon_classification = got_axon_mfg || got_axon_oui;
        // A public Axon OUI is useful supporting evidence, but it must never
        // become alarm-eligible solely because RSSI/TX-power bonuses push the
        // numeric score across the global threshold.
        bool axon_alarm_eligible = got_axon_mfg;

        if (is_axon_classification) {
            if (got_axon_mfg) strlcat(methods, "axon_mfg_0x034D ", sizeof(methods));
            if (got_axon_oui) strlcat(methods, "axon_oui ", sizeof(methods));
            if (got_axon_uuid) strlcat(methods, "axon_uuid_fc81 ", sizeof(methods));

            if (got_axon_mfg && (got_axon_oui || got_axon_uuid)) {
                confidence = SCORE_DEFINITIVE;
                strncpy(capture_type, "AXON_BLE", sizeof(capture_type));
            } else if (got_axon_mfg) {
                // A SIG company identifier is vendor-specific evidence, but
                // still does not justify an exact product/model label.
                confidence = CONFIDENCE_HIGH;
                strncpy(capture_type, "AXON_BLE", sizeof(capture_type));
            } else {
                confidence = SCORE_STRONG;
                strncpy(capture_type, "AXON_ASSOC", sizeof(capture_type));
            }
        } else {
            if (got_raven || got_name || got_mfg) {
                confidence = SCORE_DEFINITIVE;
                if (mac_score == 1) strlcat(methods, "mac_t1 ", sizeof(methods));
            } else if (mac_score == 1) {
                confidence = SCORE_STRONG;
                strlcat(methods, "mac_t1 ", sizeof(methods));
                if (got_penguin_name && got_mfg) { confidence = SCORE_DEFINITIVE; }
            } else {
                if (mac_score == 2) { confidence += SCORE_WEAK; strlcat(methods, "mac_t2 ", sizeof(methods)); }
                if (got_penguin_name && got_mfg) { confidence += SCORE_WEAK; }
                if ((mac_score == 2 || (got_penguin_name && got_mfg)) &&
                    (ev->addr_type == 0 || (ev->addr_type == 1 && (ev->mac[0] >> 6) == 0x03))) {
                    confidence += SCORE_WEAK; strlcat(methods, "static_addr ", sizeof(methods));
                }
            }
        }
        if (confidence > 0 && (!is_axon_classification || axon_alarm_eligible) &&
            ev->rssi > -50) {
            confidence += SCORE_BONUS_RSSI;
        }

        // Advertised TX power: fixed infrastructure often emits at higher TX
        // power than a phone. Corroborator only — never trips alarm alone.
        if (ev->have_tx_power && ev->tx_power >= TX_POWER_MIN_DBM &&
            (!is_axon_classification || axon_alarm_eligible)) {
            confidence += CONF_BONUS_TX_POWER;
            strlcat(methods, "tx_pwr ", sizeof(methods));
        }

        char mac_string[18];
        snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x",
            ev->mac[0], ev->mac[1], ev->mac[2],
            ev->mac[3], ev->mac[4], ev->mac[5]);

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD &&
            (!is_axon_classification || axon_alarm_eligible)) {
            __atomic_store_n(&channel_lock_until, millis() + 10000, __ATOMIC_RELAXED);
            rssi_track_update(mac_string, ev->rssi);
        }
        if (confidence > 100) confidence = 100;

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD || is_axon_classification) {
            char extra_data[96] = "";
            if (ev->have_mfg) {
                static const char hx[] = "0123456789ABCDEF";
                int out = 0;
                for (int i = 0; i < ev->mfg_data_len && out < (int)sizeof(extra_data) - 2; i++) {
                    extra_data[out++] = hx[ev->mfg_data[i] >> 4];
                    extra_data[out++] = hx[ev->mfg_data[i] & 0x0F];
                }
                extra_data[out] = '\0';
            }
            if (strcmp(capture_type, "RAVEN_BLE") == 0) {
                bool has_gps=false, has_power=false, has_net=false, has_up=false, has_err=false;
                bool has_legacy_health=false, has_legacy_loc=false;
                for (int i = 0; i < ev->uuid_count; i++) {
                    if (strcasestr(ev->service_uuids[i], "00003100")) has_gps   = true;
                    if (strcasestr(ev->service_uuids[i], "00003200")) has_power = true;
                    if (strcasestr(ev->service_uuids[i], "00003300")) has_net   = true;
                    if (strcasestr(ev->service_uuids[i], "00003400")) has_up    = true;
                    if (strcasestr(ev->service_uuids[i], "00003500")) has_err   = true;
                    if (strcasestr(ev->service_uuids[i], "00001809")) has_legacy_health = true;
                    if (strcasestr(ev->service_uuids[i], "00001819")) has_legacy_loc    = true;
                }
                const char* fw;
                if (has_legacy_health || has_legacy_loc) {
                    fw = "1.1.x-LEGACY";
                } else if (has_gps && has_power && has_net && has_up && has_err) {
                    fw = "1.3.x";
                } else if (has_gps && has_power && has_net) {
                    fw = "1.2.x";
                } else {
                    fw = "Unknown";
                }
                snprintf(extra_data, sizeof(extra_data), "FW:%s UUIDs:%d", fw, raven_uuid_count);
            }

            int mlen = strlen(methods);
            if (mlen > 0 && methods[mlen - 1] == ' ') methods[mlen - 1] = '\0';

            // Build minimal BLE LL advertising PDU for pcap
            uint8_t ble_pdu[64];
            int pdu_off = 0;
            ble_pdu[pdu_off++] = (ev->addr_type == 1) ? 0x40 : 0x00;
            int len_idx = pdu_off++;
            for (int i = 5; i >= 0; i--) ble_pdu[pdu_off++] = ev->mac[i];
            if (ev->have_name) {
                int nlen = strlen(ev->dev_name);
                if (nlen > 29) nlen = 29;
                if (pdu_off + nlen + 2 < (int)sizeof(ble_pdu)) {
                    ble_pdu[pdu_off++] = nlen + 1;
                    ble_pdu[pdu_off++] = 0x09;
                    memcpy(ble_pdu + pdu_off, ev->dev_name, nlen);
                    pdu_off += nlen;
                }
            }
            if (ev->have_mfg && ev->mfg_data_len > 0) {
                if (pdu_off + ev->mfg_data_len + 2 < (int)sizeof(ble_pdu)) {
                    ble_pdu[pdu_off++] = ev->mfg_data_len + 1;
                    ble_pdu[pdu_off++] = 0xFF;
                    memcpy(ble_pdu + pdu_off, ev->mfg_data, ev->mfg_data_len);
                    pdu_off += ev->mfg_data_len;
                }
            }
            ble_pdu[len_idx] = pdu_off - 2;
            write_ble_pcap(ble_pdu, pdu_off);

            if (!is_mac_whitelisted(mac_string)) {
                log_detection(capture_type, "BLE", ev->rssi, mac_string, dev_name_char,
                              ev->adv_channel, ev->have_tx_power ? ev->tx_power : 0,
                              extra_data, methods, confidence, -1);

                if (confidence >= CONFIDENCE_ALARM_THRESHOLD &&
                    (!is_axon_classification || axon_alarm_eligible)) {
                    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                    if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                        trigger_alarm_confidence = confidence;
                        trigger_alarm_source = 1;  // BLE
                        last_buzzer_time = millis();
                    }
                    xSemaphoreGiveRecursive(dataMutex);
                }
            }
        }

        __atomic_store_n(&ev->in_use, 0u, __ATOMIC_RELEASE);
    }
}

// ============================================================================
// DEDICATED TASKS (DUAL CORE)
// ============================================================================
void ScannerLoopTask(void* pvParameters) {
    bool wdt_subscribed = false;
    unsigned long scan_start_ms = 0;
    for (;;) {
        if (!wdt_subscribed) {
            esp_err_t err = esp_task_wdt_add(NULL);
            if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) wdt_subscribed = true;
        }
        if (wdt_subscribed) esp_task_wdt_reset();
        unsigned long now = millis();
        unsigned long lock_until = __atomic_load_n(&channel_lock_until, __ATOMIC_RELAXED);
        if ((long)(now - lock_until) > 0) {
            unsigned long dwell = current_channel_hop_interval();
            if (now - last_channel_hop > dwell) {
                current_channel++;
                if (current_channel > MAX_CHANNEL) current_channel = 1;
                esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
                last_channel_hop = now;
            }
        }

        bool scanning = pBLEScan ? pBLEScan->isScanning() : false;

        // Hang detection: if isScanning() has been true for more than 2x the
        // configured scan duration plus 3s headroom, the NimBLE stack has
        // wedged. Force-stop so the next iteration can restart cleanly.
        const unsigned long SCAN_HANG_LIMIT_MS =
            (unsigned long)(BLE_SCAN_DURATION * 2000UL + 3000UL);
        if (scanning && pBLEScan && scan_start_ms > 0 &&
            (now - scan_start_ms) > SCAN_HANG_LIMIT_MS) {
            Serial.println("[BLE] Scan hang detected — forcing stop");
            pBLEScan->stop();
            scanning = false;
            scan_start_ms = 0;
        }

        // BLE: keep sessions running back-to-back. Each session resets the
        // duplicate filter → every device is re-reported every ~BLE_SCAN_DURATION s
        // (preserves RSSI-trend resampling) with near-zero idle gap.
        if (pBLEScan && !scanning) {
            bool active = low_power_mode ? false : (ble_scan_cycle % 3 == 0);
            pBLEScan->setActiveScan(active);
            pBLEScan->start(BLE_SCAN_DURATION * 1000, false);   // 2.x: duration in ms (was seconds)
            scan_start_ms = millis();
            ble_scan_cycle++;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void GPSLoopTask(void* pvParameters) {
    bool wdt_subscribed = false;
    for (;;) {
        if (!wdt_subscribed) {
            esp_err_t err = esp_task_wdt_add(NULL);
            if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) wdt_subscribed = true;
        }
        if (wdt_subscribed) esp_task_wdt_reset();
        int avail = SerialGPS.available();
        if (avail > 0) {
            uint8_t buf[128];
            int bytes_read = SerialGPS.readBytes(buf, min(avail, 128));
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            for(int i = 0; i < bytes_read; i++) {
                gps.encode(buf[i]);
            }
            xSemaphoreGiveRecursive(dataMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void AlarmTask(void* pvParameters) {
    esp_task_wdt_add(NULL);

    // Flush any stale I2S DMA state before playing — prevents an assertion
    // inside the M5Unified speaker driver when tone() is called after the
    // peripheral has been idle for extended periods (e.g. ambient mode).
    M5Cardputer.Speaker.stop();
    vTaskDelay(10 / portTICK_PERIOD_MS);

    // Pack: low 16 bits = confidence, bit 16 = source (0=WiFi, 1=BLE)
    intptr_t param = (intptr_t)pvParameters;
    int conf    = (int)(param & 0xFFFF);
    bool is_ble = (bool)((param >> 16) & 0x1);

    if (conf >= CONFIDENCE_CERTAIN) {
        if (is_ble) {
            // BLE: descending three-note (G → E → C) — soft "ping-down" feel
            M5Cardputer.Speaker.tone(784, 90);  vTaskDelay(120 / portTICK_PERIOD_MS);
            M5Cardputer.Speaker.tone(659, 90);  vTaskDelay(120 / portTICK_PERIOD_MS);
            M5Cardputer.Speaker.tone(523, 140); vTaskDelay(160 / portTICK_PERIOD_MS);
        } else {
            // WiFi: ascending three-note (C → E → A)
            M5Cardputer.Speaker.tone(523, 90);  vTaskDelay(120 / portTICK_PERIOD_MS);
            M5Cardputer.Speaker.tone(659, 90);  vTaskDelay(120 / portTICK_PERIOD_MS);
            M5Cardputer.Speaker.tone(880, 140); vTaskDelay(160 / portTICK_PERIOD_MS);
        }
    } else if (conf >= CONFIDENCE_HIGH) {
        int freq = is_ble ? 698 : 740;  // BLE: F, WiFi: F#
        for (int i = 0; i < 2; i++) {
            M5Cardputer.Speaker.tone(freq, 110);
            vTaskDelay(180 / portTICK_PERIOD_MS);
        }
    } else {
        int freq = is_ble ? 587 : 620;  // BLE: D, WiFi: Eb
        M5Cardputer.Speaker.tone(freq, 90);
        vTaskDelay(140 / portTICK_PERIOD_MS);
    }

    is_alarming = false;
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

void play_escalated_alarm(int confidence, int source) {
    if (is_muted || is_alarming) return;
    is_alarming = true;
    intptr_t param = ((intptr_t)confidence & 0xFFFF) | ((intptr_t)(source & 0x1) << 16);
    if (xTaskCreate(AlarmTask, "AlarmTask", 2048, (void*)param, 2, NULL) != pdPASS) {
        // Spawn failed (heap pressure); reset the gate so future alarms
        // can still fire instead of being permanently suppressed.
        is_alarming = false;
        Serial.println("[ALARM] xTaskCreate failed — alarm skipped");
    }
}

// ============================================================================
// UI RENDERING - BASE COMPONENTS
// ============================================================================
void draw_toast_spr() {
    // Snapshot all toast state under mutex before rendering.
    bool          active_snap;
    char          text_snap[TOAST_TEXT_LEN];
    uint16_t      accent_snap      = 0;
    bool          is_info_snap     = true;
    unsigned long start_snap       = 0;
    int           queue_count_snap = 0;

    if (!take_data_mutex()) return;
    active_snap = toast_active;
    if (active_snap) {
        const ToastEntry& head = toast_queue[toast_queue_head];
        strncpy(text_snap, head.text, sizeof(text_snap) - 1);
        text_snap[sizeof(text_snap) - 1] = '\0';
        accent_snap      = head.accent;
        is_info_snap     = head.is_action;
        start_snap       = toast_start;
        queue_count_snap = toast_queue_count;
    }
    give_data_mutex();

    if (!active_snap) return;

    unsigned long elapsed = millis() - start_snap;

    // Expiration — advance queue or clear under mutex
    if (elapsed > TOAST_DURATION_MS) {
        if (!take_data_mutex()) return;
        if (toast_queue_count > 0) {
            toast_queue_head = (toast_queue_head + 1) % TOAST_QUEUE_SIZE;
            toast_queue_count--;
        }
        if (toast_queue_count > 0) {
            toast_start  = millis();
            toast_active = true;
        } else {
            toast_active = false;
        }
        give_data_mutex();
        return;
    }

    // Fade alpha
    float toast_alpha;
    if (elapsed < UI_FADE_IN_MS) {
        toast_alpha = ui_ease((float)elapsed / (float)UI_FADE_IN_MS);
    } else if (elapsed > TOAST_DURATION_MS - UI_FADE_OUT_MS) {
        float fade_t = (float)(elapsed - (TOAST_DURATION_MS - UI_FADE_OUT_MS)) / (float)UI_FADE_OUT_MS;
        toast_alpha = 1.0f - ui_ease(fade_t);
    } else {
        toast_alpha = 1.0f;
    }
    if (toast_alpha < 0.02f) return;

    auto ta = [&](uint16_t c) -> uint16_t { return lerp_col16(BG_COLOR, c, toast_alpha); };

    uint16_t accent = accent_snap ? accent_snap : CAUTION_COLOR;

    // ── Centered pip+pill layout ──
    int text_len = (int)strlen(text_snap);
    int char_w   = ts_char_w(TS_BODY);
    int text_w   = text_len * char_w;
    int pip_w    = 7;
    int gap      = 3;
    int pad_lr   = 9;
    int th       = 17;

    char queue_str[6] = "";
    int queue_w = 0;
    if (queue_count_snap > 1) {
        snprintf(queue_str, sizeof(queue_str), " +%d", queue_count_snap - 1);
        queue_w = (int)strlen(queue_str) * char_w;
    }

    int content_w = pip_w + gap + text_w + queue_w;
    int tw = content_w + pad_lr * 2;
    int tx = (DISP_W - tw) / 2;
    int ty = SPR_H - 26;
    int tr = th / 2;

    uint16_t pill_bg = ta(lerp_col16(BG_COLOR, accent, 0.15f));
    spr.fillRoundRect(tx, ty, tw, th, tr, pill_bg);
    spr.drawRoundRect(tx, ty, tw, th, tr, ta(accent));

    // Pip
    int content_start = tx + pad_lr;
    int pip_cx = content_start + pip_w / 2;
    int pip_cy = ty + th / 2;

    if (!is_info_snap) {
        // Warning: filled triangle pointing up
        spr.fillTriangle(
            pip_cx,      pip_cy - 3,
            pip_cx + 3,  pip_cy + 2,
            pip_cx - 3,  pip_cy + 2,
            ta(accent));
    } else {
        // Info/success: filled circle
        spr.fillCircle(pip_cx, pip_cy, 2, ta(accent));
    }

    // Text
    spr.setTextColor(ta(TEXT_COLOR), pill_bg);
    spr.setTextSize(TS_BODY);
    int text_x = content_start + pip_w + gap;
    spr.setCursor(text_x, ty + 4);
    spr.print(text_snap);

    // Queue suffix
    if (queue_w > 0) {
        spr.setTextColor(ta(DIM_COLOR), pill_bg);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(text_x + text_w + 2, ty + 5);
        spr.print(queue_str);
    }
}

void draw_vol_overlay() {
    if (!show_vol_overlay) return;
    unsigned long elapsed = millis() - vol_overlay_start;
    const unsigned long SHOW_MS = 2200;
    if (elapsed > SHOW_MS) { show_vol_overlay = false; return; }

    float alpha;
    if (elapsed < UI_FADE_IN_MS) {
        alpha = ui_ease((float)elapsed / (float)UI_FADE_IN_MS);
    } else if (elapsed > SHOW_MS - UI_FADE_OUT_MS) {
        float t = (float)(elapsed - (SHOW_MS - UI_FADE_OUT_MS)) / (float)UI_FADE_OUT_MS;
        alpha = 1.0f - ui_ease(t);
    } else {
        alpha = 1.0f;
    }
    if (alpha < 0.02f) return;

    auto va = [&](uint16_t c) -> uint16_t { return lerp_col16(BG_COLOR, c, alpha); };

    detect_buffer_byte_order(spr);

    // ── Dimmed backdrop below header ──
    {
        float dim_strength = 0.45f * alpha;
        int dim_alpha_i = (int)(dim_strength * 256.0f);
        uint16_t* sbuf = (uint16_t*)spr.getBuffer();
        if (sbuf) {
            for (int py = 0; py < SPR_H; py++) {
                int row = py * DISP_W;
                for (int px = 0; px < DISP_W; px++) {
                    int idx = row + px;
                    uint16_t existing = read_pixel_logical(sbuf, idx);
                    uint16_t dimmed   = lerp_col16_i(existing, BG_COLOR, dim_alpha_i);
                    write_pixel_logical(sbuf, idx, dimmed);
                }
            }
        }
    }

    // ── Pill layout ──
    uint16_t accent = is_muted ? DIM_COLOR : HEADER_COLOR;
    int vol_pct = is_muted ? 0 : (int)map(current_volume, 0, 255, 0, 100);

    char label[12];
    if (is_muted) {
        snprintf(label, sizeof(label), "MUTED");
    } else {
        snprintf(label, sizeof(label), "VOL %d%%", vol_pct);
    }

    int char_w   = ts_char_w(TS_BODY);
    int label_w  = (int)strlen(label) * char_w;
    int pad_lr   = 9;
    int th       = 17;
    int bar_gap  = 5;
    int bar_w    = 50;
    int bar_h    = 4;
    bool show_bar = !is_muted;

    int tw = pad_lr + label_w + (show_bar ? bar_gap + bar_w : 0) + pad_lr;
    int tx = (DISP_W - tw) / 2;
    int ty = SPR_H / 2 - 1;
    int tr = th / 2;

    uint16_t pill_bg = va(lerp_col16(BG_COLOR, accent, 0.15f));
    spr.fillRoundRect(tx, ty, tw, th, tr, pill_bg);
    spr.drawRoundRect(tx, ty, tw, th, tr, va(accent));

    int label_x = tx + pad_lr;
    spr.setTextColor(va(is_muted ? DIM_COLOR : TEXT_COLOR), pill_bg);
    spr.setTextSize(TS_BODY);
    spr.setCursor(label_x, ty + 4);
    spr.print(label);

    if (show_bar) {
        int bar_x = tx + pad_lr + label_w + bar_gap;
        int bar_y = ty + (th - bar_h) / 2;
        int fill_w = (vol_pct * bar_w) / 100;
        spr.fillRoundRect(bar_x, bar_y, bar_w, bar_h, 2, va(CARD_BORDER));
        if (fill_w > 0) {
            if (fill_w < 4) {
                spr.fillRect(bar_x, bar_y, fill_w, bar_h, va(accent));
            } else {
                spr.fillRoundRect(bar_x, bar_y, fill_w, bar_h, 2, va(accent));
            }
        }
    }
}

// Draw a header-bar status pill: rounded rect with text inside.
// bg_accent_pct: how much of accent_col to mix into BG (0.0–1.0).
// filled: if true, the pill is solid accent_col with BG-colored text.
static void drawPill(int x, int y, const char* text, uint16_t accent_col,
                     float bg_accent_pct, bool filled) {
    int tw = (int)strlen(text) * ts_char_w(TS_MICRO) + 7;
    int th = 12;
    uint16_t bg = filled ? accent_col : lerp_col16(BG_COLOR, accent_col, bg_accent_pct);
    uint16_t fg = filled ? BG_COLOR : TEXT_COLOR;
    uint16_t border = filled ? accent_col : lerp_col16(BG_COLOR, accent_col, 0.4f);
    spr.fillRoundRect(x, y, tw, th, 3, bg);
    spr.drawRoundRect(x, y, tw, th, 3, border);
    spr.setTextColor(fg, bg);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(x + 3, y + 2);
    spr.print(text);
}

// ── LCD-direct rendering helpers (Phase 1 of sprite-split) ──────────
// These mirror kprint / drawPill but target M5Cardputer.Display instead
// of the sprite. Used by draw_header_lcd(). Nothing else changes yet.

static void kprint_lcd(const char* text, int extra = 1) {
    auto& lcd = M5Cardputer.Display;
    int cx = lcd.getCursorX(), cy = lcd.getCursorY();
    for (const char* p = text; *p; p++) {
        char ch[2] = {*p, '\0'};
        lcd.setCursor(cx, cy);
        lcd.print(ch);
        cx += 6 + extra;
    }
}

static void drawPill_lcd(int x, int y, const char* text, uint16_t accent_col,
                         float bg_accent_pct = 0.18f, bool filled = false) {
    auto& lcd = M5Cardputer.Display;
    int tw = (int)strlen(text) * ts_char_w(TS_MICRO) + 7;
    int th = 12;
    uint16_t bg = filled ? accent_col : lerp_col16(BG_COLOR, accent_col, bg_accent_pct);
    uint16_t fg = filled ? BG_COLOR : TEXT_COLOR;
    uint16_t border = filled ? accent_col : lerp_col16(BG_COLOR, accent_col, 0.4f);
    lcd.fillRoundRect(x, y, tw, th, 3, bg);
    lcd.drawRoundRect(x, y, tw, th, 3, border);
    lcd.setTextColor(fg, bg);
    lcd.setTextSize(TS_MICRO);
    lcd.setCursor(x + 3, y + 2);
    lcd.print(text);
}

void draw_header_lcd(int screen_num, const char* name_override) {
    auto& lcd = M5Cardputer.Display;
    static const char* screen_names[NUM_SCREENS] = {
        "SCANNER", "SIGNAL", "DETECTIONS", "GPS", "STATS"
    };
    if (screen_num < 0 || screen_num >= NUM_SCREENS) screen_num = 0;

    lcd.fillRect(0, 0, DISP_W, CONTENT_Y, BG_COLOR);
    lcd.drawFastHLine(0, 18, DISP_W, CARD_BORDER);
    lcd.setTextColor(HEADER_COLOR, BG_COLOR);
    lcd.setTextSize(TS_BODY);
    const char* display_name = name_override ? name_override : screen_names[screen_num];
    lcd.setCursor(TEXT_LEFT, 5);
    kprint_lcd(display_name);

    // ── Status pill row (mirrors draw_header_spr exactly) ──
    bool gps_lock_now;
    if (!take_data_mutex()) return;
    gps_lock_now = gps.satellites.isValid() && gps.satellites.value() >= 1;
    long pill_det  = lifetime_flock_total;
    long pill_wifi = session_flock_wifi;
    long pill_ble  = session_flock_ble;
    give_data_mutex();
    bool muted_now = is_muted;

    // Right margin for the status pill row. Was 4px, which ran the rightmost
    // pill into the bezel; TEXT_LEFT keeps it symmetric with the title margin.
    int icon_right = DISP_W - TEXT_LEFT;
    int icon_y = 4;

    // Mode badges
    {
        // A (ambient) and N (night) were dropped: each announces a state that
        // is already unmistakable on screen — ambient visibly dims the
        // backlight, night swaps the entire palette — so the badge was
        // spending scarce header width to say what the display had
        // already said. What remains is state you cannot otherwise see.
        // Nothing here uses DIM_COLOR any more; at TS_MICRO it was too faint.
        struct ModeBadge { bool active; const char* letter; uint16_t color; };
        ModeBadge badges[4] = {
            { signal_active,    "L", CAUTION_COLOR },
            { low_power_mode,    "P", ACCENT_COLOR },
            { turbo_mode_active, "T", CAUTION_COLOR },
            { c5_is_present() && !low_power_mode, "5G", HEADER_COLOR },
        };
        for (int i = 0; i < 4; i++) {
            if (!badges[i].active) continue;
            int pw = (int)strlen(badges[i].letter) * ts_char_w(TS_MICRO) + 6;
            drawPill_lcd(icon_right - pw, icon_y, badges[i].letter, badges[i].color);
            icon_right -= pw + 2;
        }
    }

    // Muted indicator
    if (muted_now) {
        drawPill_lcd(icon_right - 13, icon_y, "M", TEXT_COLOR);
        icon_right -= 15;
    }

    // SD missing
    if (system_fully_booted && !sd_available) {
        uint16_t sd_warn = lgfx::color565(180, 40, 40);
        drawPill_lcd(icon_right - 20, icon_y, "SD", sd_warn);
        icon_right -= 22;
    }

    // GPS missing
    if (!gps_lock_now) {
        drawPill_lcd(icon_right - 27, icon_y, "GPS", CAUTION_COLOR);
        icon_right -= 29;
    }

    // Detection count pill
    {
        char det_str[8];
        snprintf(det_str, sizeof(det_str), "D%lu", (unsigned long)pill_det);
        int dw = (int)strlen(det_str) * ts_char_w(TS_MICRO) + 6;
        drawPill_lcd(icon_right - dw, icon_y, det_str, ACCENT_COLOR, 0.0f, true);
        icon_right -= dw + 2;
    }

    // WiFi + BLE session count pills
    {
        // Colour-coded by protocol instead of DIM_COLOR — these two counts are
        // the most-glanced values in the header and were the least legible.
        char b_str[8];
        snprintf(b_str, sizeof(b_str), "B%ld", pill_ble);
        int bw = (int)strlen(b_str) * ts_char_w(TS_MICRO) + 6;
        drawPill_lcd(icon_right - bw, icon_y, b_str, PURPLE_COLOR);
        icon_right -= bw + 2;

        char w_str[8];
        snprintf(w_str, sizeof(w_str), "W%ld", pill_wifi);
        int ww = (int)strlen(w_str) * ts_char_w(TS_MICRO) + 6;
        drawPill_lcd(icon_right - ww, icon_y, w_str, TEAL_COLOR);
        icon_right -= ww + 2;
    }

    // Battery percentage pill
    {
        int32_t bat_mv = get_filtered_voltage();
        int bat_pct = voltage_to_percent(bat_mv);
        bool charging = M5Cardputer.Power.isCharging();

        char bat_str[10];
        if (charging) {
            snprintf(bat_str, sizeof(bat_str), "%d%%+", bat_pct);
        } else {
            snprintf(bat_str, sizeof(bat_str), "%d%%", bat_pct);
        }
        int pw = (int)strlen(bat_str) * ts_char_w(TS_MICRO) + 6;
        uint16_t bat_col = charging       ? GPS_COLOR
                         : (bat_pct <= 10) ? CAUTION_COLOR
                         : (bat_pct <= 25) ? lerp_col16(DIM_COLOR, CAUTION_COLOR, 0.5f)
                         :                   DIM_COLOR;
        drawPill_lcd(icon_right - pw, icon_y, bat_str, bat_col);
    }
}

static void draw_overlay_header_lcd(const char* label) {
    auto& lcd = M5Cardputer.Display;
    lcd.fillRect(0, 0, DISP_W, CONTENT_Y, BG_COLOR);
    lcd.drawFastHLine(0, 18, DISP_W, CARD_BORDER);
    lcd.setTextColor(HEADER_COLOR, BG_COLOR);
    lcd.setTextSize(TS_BODY);
    lcd.setCursor(TEXT_LEFT, 5);
    kprint_lcd(label);
}

static void render_frame() {
    auto& lcd = M5Cardputer.Display;
    lcd.startWrite();

    // ── Dirty-track header state — only redraw when something changes ──
    static bool     first_call          = true;
    static int      last_screen         = -1;
    static long     last_det            = -1;
    static long     last_wifi           = -1;
    static long     last_ble            = -1;
    static int      last_bat_pct        = -1;
    static bool     last_charging       = false;
    static bool     last_gps_lock       = false;
    static bool     last_muted          = false;
    static bool     last_ambient        = false;
    static bool     last_night          = false;
    static bool     last_signal_active  = false;
    static bool     last_low_power      = false;
    static bool     last_turbo          = false;
    static bool     last_sd             = true;
    static bool     last_export         = false;
    static bool     last_menu           = false;
    static bool     last_help           = false;
    static bool     last_wifi_cfg       = false;
    static bool     last_feed_exp       = false;

    // Sample current state under mutex
    long cur_det = 0, cur_wifi = 0, cur_ble = 0;
    bool cur_gps = false;
    if (take_data_mutex()) {
        cur_det  = lifetime_flock_total;
        cur_wifi = session_flock_wifi;
        cur_ble  = session_flock_ble;
        cur_gps  = gps.satellites.isValid() && gps.satellites.value() >= 1;
        give_data_mutex();
    }
    int  cur_bat      = voltage_to_percent(get_filtered_voltage());
    bool cur_charging = M5Cardputer.Power.isCharging();

    bool header_dirty = first_call
        || (current_screen     != last_screen)
        || (cur_det            != last_det)
        || (cur_wifi           != last_wifi)
        || (cur_ble            != last_ble)
        || (cur_bat            != last_bat_pct)
        || (cur_charging       != last_charging)
        || (cur_gps            != last_gps_lock)
        || (is_muted           != last_muted)
        || (ambient_mode       != last_ambient)
        || (night_mode         != last_night)
        || (signal_active      != last_signal_active)
        || (low_power_mode     != last_low_power)
        || (turbo_mode_active  != last_turbo)
        || (sd_available       != last_sd)
        || (export_mode_active != last_export)
        || (menu_open          != last_menu)
        || (show_help_overlay  != last_help)
        || (wifi_config_open   != last_wifi_cfg)
        || (show_feed_expanded != last_feed_exp);

    if (header_dirty) {
        first_call     = false;
        last_screen    = current_screen;
        last_det       = cur_det;
        last_wifi      = cur_wifi;
        last_ble       = cur_ble;
        last_bat_pct   = cur_bat;
        last_charging  = cur_charging;
        last_gps_lock  = cur_gps;
        last_muted     = is_muted;
        last_ambient   = ambient_mode;
        last_night     = night_mode;
        last_signal_active = signal_active;
        last_low_power = low_power_mode;
        last_turbo     = turbo_mode_active;
        last_sd        = sd_available;
        last_export    = export_mode_active;
        last_menu      = menu_open;
        last_help      = show_help_overlay;
        last_wifi_cfg  = wifi_config_open;
        last_feed_exp  = show_feed_expanded;

        if (menu_open) {
            draw_overlay_header_lcd("MENU");
        } else if (show_help_overlay) {
            draw_overlay_header_lcd("HELP");
        } else if (wifi_config_open) {
            draw_overlay_header_lcd("Export Mode Configuration");
        } else if (export_mode_active) {
            draw_header_lcd(current_screen, "EXPORT");
        } else {
            draw_header_lcd(current_screen);
            if (show_feed_expanded) {
                lcd.setTextColor(lerp_col16(HEADER_COLOR, ACCENT_COLOR, 0.4f), BG_COLOR);
                lcd.setTextSize(TS_BODY);
                lcd.setCursor(56, 5);
                kprint_lcd("/ FEED");
            }
        }
    }

    // Push content rows via DMA — buffer already in native swap565 byte order,
    // lgfx::swap565_t* cast bypasses per-pixel conversion and goes direct DMA.
    uint16_t* buf = (uint16_t*)spr.getBuffer();
    if (buf) {
        lcd.pushImageDMA(0, CONTENT_Y, DISP_W, SPR_H,
                         (lgfx::swap565_t*)buf);
    }

    lcd.endWrite();
}

void draw_help_overlay() {
    float alpha = ui_progress(help_ease_start, UI_FADE_IN_MS);
    if (alpha < 0.02f) return;

    bool fully_faded = (alpha >= 1.0f);
    auto ea = [&](uint16_t c) -> uint16_t { return fully_faded ? c : lerp_col16(BG_COLOR, c, alpha); };

    spr.fillSprite(BG_COLOR);

    struct HelpKey { const char* key; const char* desc; };
    const HelpKey* keys;
    int key_count;

    static const HelpKey global_keys[] = {
        {"</>",  "screens"},
        {"-/+",  "volume"},
        {"`",    "mute"},
        {"n",    "night"},
        {"DEL",  "back"},
        {"ESC",  "home"},
        {"f",    "feed"},
    };

    static const HelpKey scanner_keys[] = {
        {"v",   "cycle viz"},
#if DEBUG_KEYS
        {"x",   "simulate"},
#endif
        {"f",   "expand feed"},
        {"t",   "locate"},
        {"m",   "dock"},
    };
    static const HelpKey signal_keys[] = {
        {"f",   "feed / select target"},
        {"t",   "target device"},
        {"l",   "stop tracking"},
    };
    static const HelpKey detections_keys[] = {
        {"^/v", "navigate"},
        {"ENT", "detail"},
        {"d",   "delete"},
    };
    static const HelpKey gps_keys[] = {
        {"",    "no keys"},
    };
    static const HelpKey devinfo_keys[] = {
        {"^/v", "scroll"},
        {"m",   "menu (clear)"},
    };

    switch (current_screen) {
        case 0: keys = scanner_keys;    key_count = sizeof(scanner_keys)/sizeof(scanner_keys[0]);       break;
        case 1: keys = signal_keys;     key_count = sizeof(signal_keys)/sizeof(signal_keys[0]);         break;
        case 2: keys = detections_keys; key_count = sizeof(detections_keys)/sizeof(detections_keys[0]); break;
        case 3: keys = gps_keys;        key_count = sizeof(gps_keys)/sizeof(gps_keys[0]);               break;
        case 4: keys = devinfo_keys;    key_count = sizeof(devinfo_keys)/sizeof(devinfo_keys[0]);       break;
        default: keys = scanner_keys; key_count = 0; break;
    }

    // Screen 4 swaps the standard THIS SCREEN / GLOBAL layout for a
    // dedicated stat guide — there are 13 cards to explain and the
    // standard layout has no room for descriptions.
    if (current_screen == 4) {
        const int col_lx = UI_PAD_SM;
        const int col_rx = DISP_W / 2 + 4;
        int y = 4;

        // Compact key list at top
        spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
        spr.setTextSize(TS_BODY);
        spr.setCursor(col_lx, y);
        kprint(spr, "KEYS");
        y += 11;
        for (int i = 0; i < key_count; i++) {
            spr.setTextSize(TS_BODY);
            spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
            spr.setCursor(col_lx, y);
            spr.print(keys[i].key);
            spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
            spr.setCursor(col_lx + 28, y);
            spr.print(keys[i].desc);
            y += 10;
        }
        y += 2;

        // Stat guide — two columns, 8 rows × 2 = 15 cells (last cell blank).
        struct Desc { const char* name; const char* desc; };
        static const Desc descs[] = {
            {"SESS DET", "session"},
            {"LIFE DET", "lifetime"},
            {"WIFI",     "wifi"},
            {"BLE",      "ble"},
            {"RAVEN",    "raven"},
            {"SESSION",  "uptime"},
            {"LIFETIME", "total up"},
            {"BATTERY",  "voltage"},
            {"HEAP",     "free mem"},
            {"PACKETS",  "scanned"},
            {"SD CARD",  "capacity"},
            {"BOOTS",    "boots"},
            {"FLASH",    "writes"},
            {"VERSION",  "firmware"},
            {"VOLTAGE",  "raw mV"},
        };
        const int n_desc = sizeof(descs) / sizeof(descs[0]);
        const int rows   = (n_desc + 1) / 2;  // 7

        spr.setTextSize(TS_MICRO);
        for (int r = 0; r < rows && y < SPR_H - 11; r++) {
            // Left column entry
            int li = r;
            spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
            spr.setCursor(col_lx, y);
            spr.print(descs[li].name);
            spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
            spr.setCursor(col_lx + 54, y);
            spr.print(descs[li].desc);

            // Right column entry (skip if past end)
            int ri = r + rows;
            if (ri < n_desc) {
                spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
                spr.setCursor(col_rx, y);
                spr.print(descs[ri].name);
                spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
                spr.setCursor(col_rx + 54, y);
                spr.print(descs[ri].desc);
            }
            y += 8;
        }

        // Footer
        spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(UI_PAD_SM, SPR_H - 10);
        spr.print("TAB close  M=clear stats");
        spr.setCursor(DISP_W - 30, SPR_H - 10);
        spr.print(VERSION_SHORT);
        return;
    }

    const int ROW_H = 10;
    const int col_left_x  = UI_PAD_SM;
    const int col_right_x = DISP_W / 2 + 4;
    int row_y;

    // ── Left column: screen-specific keys ──
    row_y = 4;
    spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setCursor(col_left_x, row_y);
    kprint(spr, "THIS SCREEN");
    row_y += ROW_H + 2;

    for (int i = 0; i < key_count && row_y < SPR_H - 12; i++) {
        spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
        spr.setCursor(col_left_x, row_y);
        spr.print(keys[i].key);
        spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
        spr.setCursor(col_left_x + 28, row_y);
        spr.print(keys[i].desc);
        row_y += ROW_H;
    }

    // ── Right column: global keys ──
    row_y = 4;
    spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setCursor(col_right_x, row_y);
    kprint(spr, "GLOBAL");
    row_y += ROW_H + 2;

    int global_count = sizeof(global_keys) / sizeof(global_keys[0]);
    for (int i = 0; i < global_count && row_y < SPR_H - 12; i++) {
        spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
        spr.setCursor(col_right_x, row_y);
        spr.print(global_keys[i].key);
        spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
        spr.setCursor(col_right_x + 28, row_y);
        spr.print(global_keys[i].desc);
        row_y += ROW_H;
    }

    // Footer
    spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(UI_PAD_SM, SPR_H - 10);
    spr.print("TAB to close");
    // Version tag, right-aligned
    spr.setCursor(DISP_W - 30, SPR_H - 10);
    spr.print(VERSION_SHORT);
}

// Soft UI click — brief tone that reads as a tactile tick.
static void menu_click() {
    if (is_muted) return;
    M5Cardputer.Speaker.tone(660, 5);  // soft mechanical-keyboard tick
}

// ── Menu icon primitives (10px cell, drawn at ix, iy) ───────────────

static void menu_icon_scanner(int x, int y, uint16_t col) {
    spr.drawTriangle(x, y+9, x+9, y+9, x+4, y, col);
}

static void menu_icon_signal(int x, int y, uint16_t col) {
    spr.drawFastHLine(x, y+4, 10, col);
    spr.drawFastVLine(x+4, y, 10, col);
    spr.drawPixel(x+1, y+1, col); spr.drawPixel(x+7, y+1, col);
    spr.drawPixel(x+1, y+7, col); spr.drawPixel(x+7, y+7, col);
}

static void menu_icon_detections(int x, int y, uint16_t col) {
    spr.fillRect(x,   y+6, 3, 4,  col);
    spr.fillRect(x+3, y+3, 3, 7,  col);
    spr.fillRect(x+7, y,   3, 10, col);
}

static void menu_icon_gps(int x, int y, uint16_t col) {
    spr.drawCircle(x+4, y+3, 3, col);
    spr.drawLine(x+1, y+6, x+4, y+9, col);
    spr.drawLine(x+7, y+6, x+4, y+9, col);
}

static void menu_icon_stats(int x, int y, uint16_t col) {
    spr.fillRect(x,   y,   4, 4, col);
    spr.fillRect(x+5, y,   4, 4, col);
    spr.fillRect(x,   y+5, 4, 4, col);
    spr.fillRect(x+5, y+5, 4, 4, col);
}

static void menu_icon_night(int x, int y, uint16_t col) {
    spr.drawCircle(x+4, y+4, 4, col);
    spr.fillCircle(x+6, y+3, 4, BG_COLOR);
}

static void menu_icon_power(int x, int y, uint16_t col) {
    spr.drawCircle(x+4, y+4, 4, col);
    spr.fillRect(x+3, y, 3, 3, BG_COLOR);
    spr.drawFastVLine(x+4, y, 5, col);
}

static void menu_icon_mute(int x, int y, uint16_t col) {
    spr.fillRect(x, y+3, 3, 4, col);
    spr.drawLine(x+3, y+1, x+3, y+8, col);
    spr.drawLine(x+5, y+1, x+8, y+7, col);
    spr.drawLine(x+5, y+7, x+8, y+1, col);
}

static void menu_icon_wifi(int x, int y, uint16_t col) {
    spr.drawPixel(x+4, y+9, col);
    spr.drawPixel(x+2, y+7, col); spr.drawPixel(x+6, y+7, col);
    spr.drawPixel(x+1, y+4, col); spr.drawPixel(x+7, y+4, col);
    spr.drawPixel(x,   y+1, col); spr.drawPixel(x+8, y+1, col);
}

static void menu_icon_export(int x, int y, uint16_t col) {
    spr.drawLine(x+4, y, x, y+4, col);
    spr.drawLine(x+4, y, x+8, y+4, col);
    spr.drawFastVLine(x+4, y+1, 8, col);
}

static void menu_icon_trash(int x, int y, uint16_t col) {
    spr.drawFastHLine(x, y, 10, col);
    spr.fillRect(x+1, y+2, 8, 7, col);
    spr.drawFastVLine(x+3, y+3, 5, BG_COLOR);
    spr.drawFastVLine(x+6, y+3, 5, BG_COLOR);
}

static void menu_icon_charge(int x, int y, uint16_t col) {
    spr.drawRect(x, y+2, 8, 6, col);
    spr.drawFastVLine(x+8, y+4, 2, col);
    spr.drawLine(x+4, y+3, x+2, y+5, col);
    spr.drawLine(x+5, y+4, x+3, y+6, col);
}

static void menu_draw_icon(int flat_idx, int x, int y, uint16_t col) {
    switch (flat_idx) {
        case 0:  menu_icon_scanner(x, y, col);    break;
        case 2:  menu_icon_detections(x, y, col); break;
        case 3:  menu_icon_gps(x, y, col);        break;
        case 4:  menu_icon_stats(x, y, col);      break;
        case 5:  menu_icon_night(x, y, col);      break;
        case 6:  menu_icon_power(x, y, col);      break;
        case 7:  menu_icon_mute(x, y, col);       break;
        case 8:  menu_icon_signal(x, y, col);     break;
        case 12: menu_icon_signal(x, y, col);     break;
        case 13: menu_icon_charge(x, y, col);     break;
        case 9:  menu_icon_wifi(x, y, col);       break;
        case 10: menu_icon_export(x, y, col);     break;
        case 11: menu_icon_trash(x, y, col);      break;
    }
}

// Animated title-card grid. Shared by the content sprite and the boot-time
// header strip: the spacing (20) equals CONTENT_Y, so the same off_x/off_y
// values produce one continuous grid across the two canvases.
static void draw_title_grid(lgfx::LovyanGFX& g, int h, float alpha) {
    uint16_t grid_col = lerp_col16(BG_COLOR, HEADER_COLOR, alpha * 0.16f);
    const int spacing = 20;

    static float grid_dx = 0.0f, grid_dy = 0.0f;
    static bool grid_dir_set = false;
    if (!grid_dir_set) {
        // Random diagonal with both components guaranteed visible: random
        // sign, 12-22 px/s per axis — a slow glide, roughly one grid cell
        // per second. (cos/sin of a random angle could land near an axis
        // and read as static.)
        grid_dx = (random(0, 2) ? 1.0f : -1.0f) * (12.0f + (float)random(0, 11));
        grid_dy = (random(0, 2) ? 1.0f : -1.0f) * (12.0f + (float)random(0, 11));
        grid_dir_set = true;
    }

    // Offsets are recomputed at most every 25ms and cached, so the two
    // canvases drawn within one frame always share identical offsets — no
    // 1px seam at the strip boundary. At <=22 px/s the position never moves
    // more than 1px per 25ms window, so caching costs no smoothness.
    static unsigned long grid_calc_ms = 0;
    static int off_x = 0, off_y = 0;
    unsigned long now = millis();
    if (grid_calc_ms == 0 || now - grid_calc_ms >= 25) {
        float t = (float)now / 1000.0f;
        off_x = ((int)(t * grid_dx) % spacing + spacing) % spacing;
        off_y = ((int)(t * grid_dy) % spacing + spacing) % spacing;
        grid_calc_ms = now;
    }

    for (int y = off_y - spacing; y < h; y += spacing)
        g.drawFastHLine(0, y, DISP_W, grid_col);
    for (int x = off_x - spacing; x < DISP_W; x += spacing)
        g.drawFastVLine(x, 0, h, grid_col);
}

static void draw_title_card_impl(lgfx::LovyanGFX& g, float alpha, int h) {
    if (alpha <= 0.01f) return;

    draw_title_grid(g, h, alpha);

    // ── Pill with title ──
    g.setTextSize(2);
    int kern = 2;
    int title_w = g.textWidth("PLUME") + kern * (strlen("PLUME") - 1);
    int title_h = g.fontHeight();

    int pad_x = 26, pad_y = 18;
    int pill_w = title_w + pad_x;
    int pill_h = title_h + pad_y;
    int pill_r = pill_h / 2;
    int pill_x = (DISP_W - pill_w) / 2;
    // Centered in FULL-SCREEN coordinates (the boot-time strip canvas
    // extends the card to y=0), then shifted into the content sprite's
    // local space. Centering within the sprite's own height sat the card
    // ~11px below screen center once the grid covered the whole screen.
    int pill_y = (DISP_H - pill_h) / 2 - 6 - CONTENT_Y;

    uint16_t pill_fill  = lerp_col16(BG_COLOR, lerp_col16(BG_COLOR, HEADER_COLOR, 0.15f), alpha);
    uint16_t border_col = lerp_col16(BG_COLOR, HEADER_COLOR, alpha);
    uint16_t title_col  = lerp_col16(BG_COLOR, HEADER_COLOR, alpha);
    uint16_t ver_col    = lerp_col16(BG_COLOR, TEXT_COLOR, alpha);

    g.fillRoundRect(pill_x, pill_y, pill_w, pill_h, pill_r, pill_fill);
    g.drawRoundRect(pill_x, pill_y, pill_w, pill_h, pill_r, border_col);

    g.setTextColor(title_col, pill_fill);
    g.setTextDatum(TL_DATUM);
    {
        const char* title = "PLUME";
        int cx = pill_x + pill_w / 2 - title_w / 2;
        int cy = pill_y + pill_h / 2 - title_h / 2;
        for (int i = 0; title[i]; i++) {
            char tmp[2] = { title[i], 0 };
            g.drawString(tmp, cx, cy);
            cx += g.textWidth(tmp) + kern;
        }
    }

    g.setTextSize(TS_MICRO);
    g.setTextColor(ver_col, BG_COLOR);
    g.setTextDatum(TC_DATUM);
    g.drawString(VERSION_SHORT, DISP_W / 2, pill_y + pill_h + 6);
    g.setTextDatum(TL_DATUM);
}
static void draw_title_card_overlay(float alpha) { draw_title_card_impl(spr, alpha, SPR_H); }

// ── Fullscreen single-column scrollable menu ────────────────────────
static void draw_menu_overlay() {
    float alpha = ui_progress(menu_open_ms, UI_FADE_IN_MS);
    if (alpha < 0.02f) return;

    bool fully_faded = (alpha >= 1.0f);
    auto ea = [&](uint16_t c) -> uint16_t {
        return fully_faded ? c : lerp_col16(BG_COLOR, c, alpha);
    };

    spr.fillSprite(BG_COLOR);

    // Layout
    const int ROW_H    = 17;
    const int VIEW_TOP = 0;
    const int FOOTER_H = 12;
    const int VIEW_H   = SPR_H - VIEW_TOP - FOOTER_H;
    const int ICON_X   = UI_PAD_SM + 2;
    const int LABEL_X  = ICON_X + 16;
    const int ROW_LEFT = UI_PAD_SM - 2;
    const int ROW_W    = DISP_W - UI_PAD_SM * 2 + 4;
    const int SECT_H   = 15;
    const int GAP_H    = 5;

    // Layout + navigation share one table (MENU_ROWS, file scope).
    const MRow* mrows = MENU_ROWS;
    const int   NROWS = MENU_ROW_COUNT;

    // Compute virtual y for each row
    int virt_y[MENU_ROW_COUNT];
    int cy = 0;
    for (int i = 0; i < NROWS; i++) {
        virt_y[i] = cy;
        if      (mrows[i].type == 0) cy += SECT_H;
        else if (mrows[i].type == 2) cy += GAP_H;
        else                         cy += ROW_H;
    }
    int total_h = cy;

    // Smooth scroll easing + eased selection highlight
    {
        unsigned long now = millis();
        float dt = (menu_last_frame_ms == 0) ? 16.0f
                 : (float)(now - menu_last_frame_ms);
        if (dt > 100.0f) dt = 100.0f;
        menu_last_frame_ms = now;
        menu_scroll_y_f = anim_filter(menu_scroll_y_f,
                                      (float)menu_scroll_offset, 80.0f, dt);
        // Find selected item's virtual Y and ease highlight toward it
        int sel_virt_y = 0;
        for (int i = 0; i < NROWS; i++) {
            if (mrows[i].type == 1 && mrows[i].idx == menu_selected) {
                sel_virt_y = virt_y[i];
                break;
            }
        }
        int cur_scroll_y = (int)(menu_scroll_y_f + 0.5f);
        float sel_target_y = (float)(VIEW_TOP + sel_virt_y - cur_scroll_y);
        menu_sel_y_f = anim_filter_seed(menu_sel_y_f, sel_target_y, 80.0f, dt, &menu_sel_y_seeded);
    }
    int scroll_y = (int)(menu_scroll_y_f + 0.5f);

    // Auto-scroll to keep selection visible
    {
        int sel_y = 0;
        for (int i = 0; i < NROWS; i++) {
            if (mrows[i].type == 1 && mrows[i].idx == menu_selected) {
                sel_y = virt_y[i]; break;
            }
        }
        if (sel_y < menu_scroll_offset)
            menu_scroll_offset = sel_y;
        if (sel_y + ROW_H > menu_scroll_offset + VIEW_H)
            menu_scroll_offset = sel_y + ROW_H - VIEW_H;
        int max_scroll = total_h - VIEW_H;
        if (max_scroll < 0) max_scroll = 0;
        if (menu_scroll_offset < 0) menu_scroll_offset = 0;
        if (menu_scroll_offset > max_scroll) menu_scroll_offset = max_scroll;
    }

    // Render rows (clipped to view area)
    spr.setClipRect(0, VIEW_TOP, DISP_W, VIEW_H);

    for (int i = 0; i < NROWS; i++) {
        int ry = VIEW_TOP + virt_y[i] - scroll_y;
        int rh = (mrows[i].type == 0) ? SECT_H
               : (mrows[i].type == 2) ? GAP_H : ROW_H;

        if (ry + rh < VIEW_TOP || ry > VIEW_TOP + VIEW_H) continue;
        if (mrows[i].type == 2) continue;

        if (mrows[i].type == 0) {
            spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
            spr.setTextSize(TS_BODY);
            spr.setCursor(UI_PAD_SM, ry + 1);
            kprint(spr, mrows[i].text);
            spr.drawFastHLine(UI_PAD_SM, ry + SECT_H - 2,
                              DISP_W - UI_PAD_SM * 2, ea(CARD_BORDER));
        } else {
            int idx = mrows[i].idx;
            bool sel = (menu_selected == idx);
            bool danger = (idx == 11);
            bool export_active_row = (idx == 10 && (export_mode_active || export_connecting));

            // Icon
            uint16_t icon_col = ea(export_active_row ? CAUTION_COLOR
                               : danger             ? CAUTION_COLOR
                               : sel                ? HEADER_COLOR : DIM_COLOR);
            menu_draw_icon(idx, ICON_X, ry + 3, icon_col);

            // Label — dynamic for export toggle
            const char* label_text = mrows[i].text;
            if (export_active_row) label_text = "Stop Export";
            uint16_t text_col = ea(export_active_row ? CAUTION_COLOR
                               : danger             ? CAUTION_COLOR
                               : sel                ? TEXT_COLOR : DIM_COLOR);
            spr.setTextColor(text_col, BG_COLOR);
            spr.setTextSize(TS_STRONG);
            spr.setCursor(LABEL_X, ry + 2);
            spr.print(label_text);

            // Current screen indicator — small filled circle after label
            if (idx >= 0 && idx <= 4 && idx == current_screen) {
                int label_len = (int)strlen(label_text);
                int dot_x = LABEL_X + label_len * ts_char_w(TS_STRONG) + 6;
                spr.fillCircle(dot_x, ry + ROW_H / 2, 2, ea(HEADER_COLOR));
            }

            // Toggle pills for settings (idx 5-8, plus 5GHz radio = 12)
            if ((idx >= 5 && idx <= 8) || idx == 12) {
                bool on = (idx == 5) ? night_mode
                        : (idx == 6) ? low_power_mode
                        : (idx == 7) ? is_muted
                        : (idx == 8) ? turbo_mode_active
                        : c5_scanning_now();   // effective, not raw c5_enabled
                if (on) {
                    int pw = 14, ph = 9;
                    int px = DISP_W - pw - UI_PAD_SM - 4;
                    spr.fillRoundRect(px, ry + 4, pw, ph, 2, ea(HEADER_COLOR));
                    spr.setTextColor(BG_COLOR, ea(HEADER_COLOR));
                    spr.setTextSize(TS_MICRO);
                    spr.setCursor(px + 2, ry + 5);
                    spr.print("ON");
                } else {
                    int pw = 18, ph = 9;
                    int px = DISP_W - pw - UI_PAD_SM - 4;
                    spr.drawRoundRect(px, ry + 4, pw, ph, 2, ea(DIM_COLOR));
                    spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
                    spr.setTextSize(TS_MICRO);
                    spr.setCursor(px + 2, ry + 5);
                    spr.print("OFF");
                }
            }
        }
    }

    spr.clearClipRect();

    // ── Eased selection highlight (drawn on top of all rows) ──
    {
        int sel_draw_y = (int)(menu_sel_y_f + 0.5f);
        if (sel_draw_y + ROW_H > VIEW_TOP && sel_draw_y < VIEW_TOP + VIEW_H) {
            spr.setClipRect(0, VIEW_TOP, DISP_W, VIEW_H);
            bool danger = (menu_selected == 11);
            bool export_active_sel = (menu_selected == 10 && (export_mode_active || export_connecting));
            uint16_t accent = (danger || export_active_sel) ? CAUTION_COLOR : HEADER_COLOR;
            spr.drawRect(ROW_LEFT, sel_draw_y, ROW_W, ROW_H, ea(accent));
            spr.fillRect(ROW_LEFT, sel_draw_y, 2, ROW_H, ea(accent));
            spr.clearClipRect();
        }
    }

    // Scrollbar (matches stats screen style)
    if (total_h > VIEW_H) {
        int sb_x = DISP_W - 4;
        spr.drawFastVLine(sb_x, VIEW_TOP, VIEW_H, ea(CARD_BORDER));
        int thumb_h = (VIEW_H * VIEW_H) / total_h;
        if (thumb_h < 8) thumb_h = 8;
        int max_scr = total_h - VIEW_H;
        if (max_scr < 1) max_scr = 1;
        int thumb_y = VIEW_TOP + (scroll_y * (VIEW_H - thumb_h)) / max_scr;
        spr.fillRect(sb_x - 1, thumb_y, 3, thumb_h, ea(DIM_COLOR));
    }

    // Footer
    spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(UI_PAD_SM, SPR_H - 10);
    spr.print("arrows  ENT select  M close  TAB help");
}
void draw_wifi_config_overlay() {
    float alpha = ui_progress(wifi_config_open_ms, UI_FADE_IN_MS);
    bool fully_faded = (alpha >= 1.0f);
    auto ea = [&](uint16_t c) -> uint16_t { return fully_faded ? c : lerp_col16(BG_COLOR, c, alpha); };

    spr.fillSprite(BG_COLOR);

    // No outer card. The form sits directly on BG; the header strip already
    // frames the screen, so a second outline just boxed in an already-bounded
    // panel. Selected fields/buttons are distinguished by border color, never
    // by fill — that still holds and is what makes the selection readable
    // without the card.
    //
    // cx/cy/cw/ch stay: every field, label and button below positions off them.
    int cx = 4, cy = UI_PAD_XS, cw = DISP_W - 8, ch = SPR_H - UI_PAD_SM;

    unsigned long now_ms = millis();
    bool cursor_visible = ((now_ms / 500) % 2 == 0);

    // ── SSID field ──
    int field_y = cy + 6;
    bool ssid_selected = (wifi_config_field == 0);
    bool ssid_editing = ssid_selected && wifi_config_editing;

    spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(cx + 6, field_y);
    kprint(spr, "SSID");

    int input_y = field_y + 12;
    uint16_t ssid_border = ssid_selected ? ea(HEADER_COLOR) : ea(CARD_BORDER);
    spr.drawRect(cx + 6, input_y, cw - 12, 16, ssid_border);

    const char* ssid_display = wifi_config_ssid_buf;
    bool ssid_empty = (strlen(ssid_display) == 0);

    // Placeholder is NOT dimmed: "(not set)" is the most actionable thing on
    // the screen for a user who has not configured WiFi yet, and dimming it
    // read as disabled. TEXT_COLOR not a literal white — it is #FFD0D0 in the
    // red night palette, and hardcoding white would blow out night vision.
    spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(cx + 9, input_y + 4);

    if (ssid_empty && !ssid_editing) {
        spr.print("(not set)");
    } else {
        char display[34];
        strncpy(display, ssid_display, 32);
        display[32] = '\0';
        spr.print(display);
        if (ssid_editing && cursor_visible) {
            int cursor_x = cx + 9 + wifi_config_cursor * ts_char_w(TS_MICRO);
            spr.drawFastVLine(cursor_x, input_y + 3, 10, ea(HEADER_COLOR));
        }
    }

    // ── Password field ──
    field_y = input_y + 22;
    bool pass_selected = (wifi_config_field == 1);
    bool pass_editing = pass_selected && wifi_config_editing;

    spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(cx + 6, field_y);
    kprint(spr, "PASSWORD");

    input_y = field_y + 12;
    uint16_t pass_border = pass_selected ? ea(HEADER_COLOR) : ea(CARD_BORDER);
    spr.drawRect(cx + 6, input_y, cw - 12, 16, pass_border);

    const char* pass_src = wifi_config_pass_buf;
    bool pass_empty = (strlen(pass_src) == 0);

    spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);   // see the SSID note above
    spr.setTextSize(TS_MICRO);
    spr.setCursor(cx + 9, input_y + 4);

    if (pass_empty && !pass_editing) {
        spr.print("(not set)");
    } else {
        // Show plaintext when (a) actively editing, or (b) the user has
        // toggled reveal mode with 's'. Otherwise mask with asterisks.
        bool show_plain = pass_editing || wifi_config_show_pass;
        if (show_plain) {
            char display[66];
            strncpy(display, pass_src, 64);
            display[64] = '\0';
            spr.print(display);
        } else {
            int plen = strlen(pass_src);
            for (int i = 0; i < plen && i < 32; i++) spr.print("*");
        }
        if (pass_editing && cursor_visible) {
            int cursor_x = cx + 9 + wifi_config_cursor * ts_char_w(TS_MICRO);
            spr.drawFastVLine(cursor_x, input_y + 3, 10, ea(HEADER_COLOR));
        }
    }

    // ── Action buttons: Save / Clear (outline only) ──
    int btn_y = input_y + 24;
    int btn_w = 70;
    int btn_h = 16;
    int btn_gap = 10;
    int btn_x1 = cx + (cw - btn_w * 2 - btn_gap) / 2;
    int btn_x2 = btn_x1 + btn_w + btn_gap;

    bool save_sel = (wifi_config_field == 2);
    uint16_t save_border = save_sel ? ea(ACCENT_COLOR) : ea(CARD_BORDER);
    spr.drawRoundRect(btn_x1, btn_y, btn_w, btn_h, 3, save_border);
    spr.setTextColor(save_sel ? ea(ACCENT_COLOR) : ea(TEXT_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(btn_x1 + 18, btn_y + 4);
    spr.print("SAVE");

    bool clear_sel = (wifi_config_field == 3);
    uint16_t clear_border = clear_sel ? ea(CAUTION_COLOR) : ea(CARD_BORDER);
    spr.drawRoundRect(btn_x2, btn_y, btn_w, btn_h, 3, clear_border);
    spr.setTextColor(clear_sel ? ea(CAUTION_COLOR) : ea(TEXT_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(btn_x2 + 14, btn_y + 4);
    spr.print("CLEAR");

    // Footer hint
    spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(cx + 6, cy + ch - 11);
    if (wifi_config_editing) {
        spr.print("type  <> move  ENT done  DEL bksp");
    } else if (wifi_config_field == 1) {
        // Password field selected — surface the show/hide affordance.
        spr.print(wifi_config_show_pass ? "ENT edit  s hide  ESC close"
                                        : "ENT edit  s show  ESC close");
    } else {
        spr.print("ENT edit  ^/v field  ESC close");
    }
}

static void set_turbo_mode(bool on) {
    if (on == turbo_mode_active) return;
    turbo_mode_active = on;
    if (on) {
        if (low_power_mode) {
            low_power_mode = false;
            apply_ble_scan_params();
            gps_standby(false);   // turbo cancels low power: wake the GPS too
            // Low-power dimmed the backlight; turbo means full performance, so
            // bring it back up (unless ambient owns it).
            if (!ambient_mode)
                M5Cardputer.Display.setBrightness(effective_brightness());
        }
        setCpuFrequencyMhz(240);
        set_toast_direct("TURBO ON", TOAST_SUCCESS);
    } else {
        setCpuFrequencyMhz(160);
        set_toast_direct("TURBO OFF", TOAST_NEUTRAL);
    }
    schedule_persist();
    screen_dirty = true;
}

// Charge Mode: reboot into the radios-off charging screen. A reboot is
// required to actually shed the WiFi/BLE load; the RTC_NOINIT flag carries
// the request across esp_restart() to the boot gate. Draw the confirmation
// directly (the render loop is about to stop, so a toast would never paint).
// Never returns.
static void enter_charge_mode_reboot() {
    charge_mode_request = CHARGE_MODE_MAGIC;
    auto& lcd = M5Cardputer.Display;
    lcd.fillScreen(lgfx::color565(0, 0, 0));
    lcd.setTextDatum(MC_DATUM);
    lcd.setTextColor(lgfx::color565(60, 210, 120), lgfx::color565(0, 0, 0));
    lcd.setTextSize(2);
    lcd.drawString("CHARGE MODE", DISP_W / 2, DISP_H / 2);
    delay(400);
    ESP.restart();
}

void handle_menu_select() {
    // Consume the clear-stats arm on EVERY selection, so choosing any other
    // item disarms it. Only case 11 reads it back.
    const bool clear_stats_armed = stats_clear_confirming;
    stats_clear_confirming = false;

    // Selecting an item closes the menu — unchanged behaviour, just moved here
    // from the ENTER handler so the clear-stats confirm can re-open it below.
    menu_open = false;

    switch (menu_selected) {
        // No case 1: the Signal screen is reachable only by targeting a device
        // from the scanner feed, so idx 1 is no longer a menu row. It is also
        // already out of the ,// carousel via screen_in_carousel().
        case 0: case 2: case 3: case 4: {
            if (export_mode_active) break;
            int target = menu_selected;
            transition_screen(target, (target >= current_screen) ? 1 : -1);
            break;
        }
        case 5:
            night_mode = !night_mode;
            apply_color_palette();
            schedule_persist();
            screen_dirty = true;
            break;
        case 6:
            low_power_mode = !low_power_mode;
            if (low_power_mode) {
                if (turbo_mode_active) turbo_mode_active = false;
                setCpuFrequencyMhz(80);
                set_toast_direct("LOW POWER ON", TOAST_SUCCESS);
            } else {
                setCpuFrequencyMhz(160);
                set_toast_direct("LOW POWER OFF", TOAST_NEUTRAL);
            }
            // Apply the backlight change now (dim on, restore on off). Skip when
            // ambient owns the backlight so we don't fight it.
            if (!ambient_mode)
                M5Cardputer.Display.setBrightness(effective_brightness());
            apply_ble_scan_params();
            gps_standby(low_power_mode);   // GPS receiver to standby / wake
            schedule_persist();
            screen_dirty = true;
            break;
        case 7:
            is_muted = !is_muted;
            if (!is_muted) beep(600, 50);
            schedule_persist();
            screen_dirty = true;
            break;
        case 8:
            set_turbo_mode(!turbo_mode_active);
            break;
        case 12:
            if (low_power_mode) {
                // Display already shows OFF (c5_scanning_now), so toggling here
                // would either be a no-op the user cannot see or would quietly
                // change a preference that stays suppressed. Say why instead —
                // same pattern as LOW POWER LOCKS DIM for brightness.
                set_toast_direct("LOW POWER IDLES 5GHZ", TOAST_WARNING);
                screen_dirty = true;
                break;
            }
            c5_enabled = !c5_enabled;
            if (c5_enabled) { c5_link_begin(); set_toast_direct("5GHz RADIO ON",  TOAST_SUCCESS); }
            else            { c5_link_end();   set_toast_direct("5GHz RADIO OFF", TOAST_NEUTRAL); }
            schedule_persist();
            screen_dirty = true;
            break;
        case 13:
            // Flush queued SD deletes synchronously before the reboot (same as
            // export teardown). No schedule_persist(): its background task would
            // race the restart, session stats reset on any reboot anyway, and
            // lifetime stats have their own 60s cadence.
            flush_pending_deletes();
            enter_charge_mode_reboot();  // never returns
            break;
        case 9:
            show_feed_expanded = false;
            wifi_config_open = true;
            wifi_config_open_ms = millis();
            wifi_config_field = 0;
            wifi_config_editing = false;
            strncpy(wifi_config_ssid_buf, export_ssid, sizeof(wifi_config_ssid_buf) - 1);
            wifi_config_ssid_buf[sizeof(wifi_config_ssid_buf) - 1] = '\0';
            strncpy(wifi_config_pass_buf, export_pass, sizeof(wifi_config_pass_buf) - 1);
            wifi_config_pass_buf[sizeof(wifi_config_pass_buf) - 1] = '\0';
            wifi_config_cursor = 0;
            draw_current_screen(); render_frame();
            break;
        case 10:
            if (export_mode_active || export_connecting) {
                export_mode_stop();
            } else {
                export_mode_start();
            }
            screen_dirty = true;
            break;
        case 11: {
            // Clear all stats — session and lifetime. Irreversible, so require
            // a second ENTER. The first press only arms and warns.
            if (!clear_stats_armed) {
                stats_clear_confirming = true;
                menu_open = true;   // hold the menu open so both presses are adjacent
                set_toast_direct("CLEAR ALL STATS? ENT AGAIN", TOAST_WARNING, false);
                screen_dirty = true;
                break;
            }
            flush_pending_deletes();
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            session_flock_wifi = 0; session_flock_ble = 0;
            session_raven = 0; session_axon = 0;
            lifetime_wifi = 0; lifetime_ble = 0;
            lifetime_flock_total = 0;
            lifetime_seconds = 0;
            lifetime_boots = 0;
            lifetime_flash_writes = 0;
            session_start_time = millis();
            xSemaphoreGiveRecursive(dataMutex);
            // Hand the write off to PersistTask on Core 1 so the main
            // loop doesn't freeze for the LittleFS + SD round-trip
            // (~200-800ms, longer on a slow card). schedule_persist
            // already no-ops if a write is in flight.
            schedule_persist();
            set_toast_direct("STATS CLEARED", TOAST_WARNING, false);
            screen_dirty = true;
            break;
        }
    }
}

// ============================================================================
// UI RENDERING - SCREENS 
// ============================================================================
// ── Layout constants for the scanner screen ──
// Viz panel fills the entire left side; W/B counts moved to header pills.
// Feed runs full-height down the right side of the divider.
// ── Scanner screen layout ───────────────────────────────────────────
// Every value derived from the spacing system. No magic numbers.
//
//   Screen edge margins:  UI_PAD_SM (6px) both sides
//   Divider gutters:      UI_PAD_XS (2px) symmetric
//   Label row inset:      UI_PAD_XS * 2 (4px) from CONTENT_Y
//
// Horizontal map (pixels):
//   0  [6px margin]  6..137 viz  [2px] 140 div [2px] 143..233 feed  [6px margin] 240
//
static const int DIVIDER_X     = 140;
static const int DIVIDER_GAP   = UI_PAD_XS;                          // 2 — gutter each side of divider
static const int VIZ_X         = UI_PAD_SM;                           // 6 — left screen margin
static const int VIZ_W         = DIVIDER_X - VIZ_X - DIVIDER_GAP;    // 132
static const int VIZ_RIGHT     = VIZ_X + VIZ_W;                       // 138
static const int FEED_X        = DIVIDER_X + 1 + DIVIDER_GAP;         // 143 — 1px line + gutter
static const int FEED_RIGHT    = DISP_W - UI_PAD_SM;                  // 234 — right screen margin
// The "SCAN"/"LINE" and "FEED" label row was removed: the titles were static
// text that never changed and the panels below them are self-evident, so the
// 16px it occupied is given back to the two content panes instead. VIZ_Y = 0
// makes both the viz and the feed 16px taller (99 -> 115), and max_feed_rows
// is computed from the available height, so the feed simply gains rows.
static const int VIZ_Y         = 0;                                    // viz/feed content top (sprite-space)
static const int VIZ_H         = SPR_H - VIZ_Y;                        // 115
static const int VIZ_BOTTOM    = VIZ_Y + VIZ_H;                        // 115 (== SPR_H)
static const int FEED_FIRST_Y  = VIZ_Y;                                // 16 — feed rows start here

// ── SCAN viz angle LUT ─────────────────────────────────────────────
// One-time precomputed angle (in radians, quantized to 0..255) from
// each 2×2 block center to the scan-viz center. Replaces the per-frame
// fast_atan2f calls in the phosphor trail loop. ~3.2KB in PSRAM.
//
// Index: row * SCAN_LUT_COLS + col
//   row = (py - VIZ_Y) / 2
//   col = (px - VIZ_X) / 2
// Value: angle * 256 / (2π), so multiply back by (2π/256) for radians.
static const int SCAN_LUT_COLS = (VIZ_W + 1) / 2;
static const int SCAN_LUT_ROWS = (VIZ_H + 1) / 2;
static const int SCAN_LUT_SIZE = SCAN_LUT_COLS * SCAN_LUT_ROWS;
static const float SCAN_LUT_INV_SCALE = (2.0f * (float)M_PI) / 256.0f;

static void scan_angle_lut_build() {
    if (scan_angle_lut_ready) return;

    scan_angle_lut = (uint8_t*)heap_caps_malloc(SCAN_LUT_SIZE,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scan_angle_lut) {
        scan_angle_lut = (uint8_t*)heap_caps_malloc(SCAN_LUT_SIZE, MALLOC_CAP_8BIT);
    }
    if (!scan_angle_lut) {
        Serial.println("[SCAN] angle LUT alloc failed — falling back to atan2 path");
        return;
    }

    const int CX = VIZ_X + VIZ_W / 2;
    const int CY = VIZ_Y + VIZ_H / 2;
    const float PI2f  = 2.0f * (float)M_PI;
    const float SCALE = 256.0f / PI2f;

    for (int row = 0; row < SCAN_LUT_ROWS; row++) {
        int py = VIZ_Y + row * 2;
        for (int col = 0; col < SCAN_LUT_COLS; col++) {
            int px = VIZ_X + col * 2;
            float dx = (float)(px - CX);
            float dy = (float)(py - CY);
            float ang = atan2f(dy, dx);
            if (ang < 0.0f) ang += PI2f;
            int quant = (int)(ang * SCALE + 0.5f);
            if (quant < 0)   quant = 0;
            if (quant > 255) quant = 255;
            scan_angle_lut[row * SCAN_LUT_COLS + col] = (uint8_t)quant;
        }
    }
    scan_angle_lut_ready = true;
    Serial.printf("[SCAN] angle LUT built (%d entries, %u bytes)\n",
                  SCAN_LUT_SIZE, (unsigned)SCAN_LUT_SIZE);
}

// Compute sine and cosine of the same angle. Currently just calls sinf
// and cosf separately — the name reflects what it does, not a promise
// of optimization. If this becomes a hot path, consider a sin LUT plus
// cos derived from sqrtf(1 - sin²) with sign tracking.
static inline void compute_sincos(float angle, float* s, float* c) {
    *s = sinf(angle);
    *c = cosf(angle);
}

// Polynomial atan2 approximation — ~0.002 rad error, ~5× faster than atan2f.
static inline float fast_atan2f(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    float mn = (ax < ay) ? ax : ay;
    float mx = (ax < ay) ? ay : ax;
    float a = mn / (mx + 1e-10f);
    float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.5707963f - r;
    if (x < 0.0f) r = 3.14159265f - r;
    if (y < 0.0f) r = -r;
    return r;
}



// ── Export mode info display — replaces scanner content while active ──
static void draw_export_info() {
    spr.fillSprite(BG_COLOR);

    // ── Animated grid background (matches title card) ──
    {
        uint16_t grid_col = lerp_col16(BG_COLOR, HEADER_COLOR, 0.16f);
        int spacing = 20;

        static float export_grid_dx = 0.0f, export_grid_dy = 0.0f;
        static bool export_grid_dir_set = false;
        if (export_grid_needs_reset) {
            export_grid_dir_set = false;
            export_grid_needs_reset = false;
        }
        if (!export_grid_dir_set) {
            float angle = (float)random(0, 628) / 100.0f;
            export_grid_dx = cosf(angle) * 14.0f;
            export_grid_dy = sinf(angle) * 14.0f;
            export_grid_dir_set = true;
        }

        float t = (float)millis() / 1000.0f;
        int off_x = ((int)(t * export_grid_dx) % spacing + spacing) % spacing;
        int off_y = ((int)(t * export_grid_dy) % spacing + spacing) % spacing;

        for (int y = off_y % spacing; y < SPR_H; y += spacing) {
            spr.drawFastHLine(0, y, DISP_W, grid_col);
        }
        for (int x = off_x % spacing; x < DISP_W; x += spacing) {
            spr.drawFastVLine(x, 0, SPR_H, grid_col);
        }
    }

    const int LBL_X = UI_PAD_SM + 2;
    int ry = UI_PAD_SM + 2;

    // Row 1: PASSWORD label (left) + EXPORT ACTIVE badge (right)
    spr.setTextColor(HEADER_COLOR, BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(LBL_X, ry);
    kprint(spr, "PASSWORD");

    {
        const char* badge_str = "EXPORT ACTIVE";
        int bw = (int)strlen(badge_str) * (ts_char_w(TS_MICRO) + 1) + 13;
        int bx = DISP_W - 4 - bw;
        uint16_t sfill = lerp_col16(BG_COLOR, CAUTION_COLOR, 0.22f);
        spr.fillRoundRect(bx, ry - 2, bw, 16, 5, sfill);
        spr.drawRoundRect(bx, ry - 2, bw, 16, 5, CAUTION_COLOR);
        spr.setTextColor(CAUTION_COLOR, sfill);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(bx + 6, ry + 2);
        kprint(spr, badge_str);
    }

    // Password value — large and prominent
    ry += 14;
    spr.setTextColor(TEXT_COLOR, BG_COLOR);
    spr.setTextSize(TS_STRONG);
    spr.setCursor(LBL_X, ry);
    spr.print(export_auth_pass);

    // Row 2: URL
    ry += 24;
    spr.setTextColor(HEADER_COLOR, BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(LBL_X, ry);
    kprint(spr, "URL");

    ry += 14;
    spr.setTextColor(TEXT_COLOR, BG_COLOR);
    spr.setTextSize(TS_STRONG);
    spr.setCursor(LBL_X, ry);
    {
        char url[32];
        snprintf(url, sizeof(url), "http://%s", export_ip_str);
        spr.print(url);
    }

    // Row 3: Time remaining — white, not gray
    ry += 22;
    spr.setTextColor(HEADER_COLOR, BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(LBL_X, ry);
    kprint(spr, "TIME");

    {
        unsigned long elapsed = millis() - export_mode_started_at;
        unsigned long remaining_ms = (elapsed < EXPORT_MODE_MAX_MS)
                                   ? (EXPORT_MODE_MAX_MS - elapsed) : 0;
        unsigned int rm = (unsigned int)(remaining_ms / 60000UL);
        unsigned int rs = (unsigned int)((remaining_ms / 1000UL) % 60);
        char time_buf[12];
        snprintf(time_buf, sizeof(time_buf), "%um %02us", rm, rs);
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setTextSize(TS_STRONG);
        spr.setCursor(LBL_X + 42, ry);
        spr.print(time_buf);
    }

    // Footer
    spr.setTextColor(DIM_COLOR, BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(UI_PAD_SM, SPR_H - 10);
    spr.print("ESC stop export  M menu");
}

void draw_scanner_screen() {
    // Show connection details instead of scanner while export is active
    if (export_mode_active) {
        draw_export_info();
        return;
    }

    unsigned long frame_ms = millis();

    // Step 1: clear
    spr.fillSprite(BG_COLOR);

    // Step 2: vertical divider
    spr.drawFastVLine(DIVIDER_X, 0, SPR_H, CARD_BORDER);

    // Step 3: (removed) the viz/FEED label row — see VIZ_Y above. Both panes
    // now start at y=0 and use the reclaimed 16px. Note this also drops the
    // "N/M" viz-mode indicator; the active viz is identified by its own shape.

    // Step 4: viz panel
    spr.setClipRect(VIZ_X, VIZ_Y, VIZ_W, VIZ_H);
    update_channel_histogram();

    // Keep spectrum smooth data warm across all modes so LINE starts live.
    {
        float sdt = (spectrum_last_frame == 0) ? 16.0f
                  : (float)(frame_ms - spectrum_last_frame);
        if (sdt > 100.0f) sdt = 100.0f;
        spectrum_last_frame = frame_ms;
        scan_line_last_frame = frame_ms;  // keep scan line timestamp warm across viz modes
        for (int i = 0; i < NUM_WIFI_CHANNELS; i++) {
            float target = (channel_peak > 0 && channel_pkt_display[i] > 0)
                          ? (float)channel_pkt_display[i] / (float)channel_peak
                          : 0.0f;
            spectrum_smooth[i] = anim_filter(spectrum_smooth[i], target, 300.0f, sdt);
        }
    }

    switch (scanner_viz_mode) {
        case 0: draw_scanner_viz_scan(frame_ms);         break;
        case 1: draw_scanner_viz_spectrum(frame_ms);     break;
    }
    spr.clearClipRect();

    if (!show_feed_expanded && (frame_ms - scan_feed_last_snapshot >= 500 || scan_feed_last_snapshot == 0)) {
        if (take_data_mutex()) {
            scan_local_count = feed_count;
            scan_local_head  = feed_head;
            for (int i = 0; i < FEED_SIZE; i++) scan_local_feed[i] = feed_entries[i];
            give_data_mutex();
            scan_feed_last_snapshot = frame_ms;
        }
    }

    const int feed_row_h    = 14;
    const int feed_last_y   = SPR_H - 1;
    const int max_feed_rows = (feed_last_y - FEED_FIRST_Y) / feed_row_h;
    unsigned long feed_now  = frame_ms;

    // Detect a new top entry → trigger a slide-down. Skip the very
    // first frame after the screen opens (prev_head == -1) so we
    // don't slide on the initial render.
    if (scan_local_head != feed_anim_prev_head && feed_anim_prev_head != -1) {
        feed_anim_shift_ms = frame_ms;
    }
    feed_anim_prev_head = scan_local_head;

    float slide_t = 1.0f;
    if (feed_anim_shift_ms > 0 && (frame_ms - feed_anim_shift_ms) < FEED_SHIFT_ANIM_MS) {
        slide_t = ui_ease((float)(frame_ms - feed_anim_shift_ms)
                          / (float)FEED_SHIFT_ANIM_MS);
    }
    int slide_offset = (int)((1.0f - slide_t) * (float)feed_row_h);

    int feed_clip_x = FEED_X + DIVIDER_GAP + 1;
    spr.setClipRect(feed_clip_x, FEED_FIRST_Y,
                    FEED_RIGHT - feed_clip_x, SPR_H - FEED_FIRST_Y);

    if (scan_local_count == 0) {
        char dots[4];
        anim_ellipsis(dots, sizeof(dots));
        char scanning[20];
        snprintf(scanning, sizeof(scanning), "Scanning%s", dots);
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(TS_BODY);
        spr.setCursor(FEED_X + DIVIDER_GAP + 1, FEED_FIRST_Y + UI_PAD_SM + UI_PAD_XS);
        spr.print(scanning);
    }

    for (int i = 0; i < scan_local_count && i < max_feed_rows; i++) {
        int idx = (scan_local_head - i + FEED_SIZE * 2) % FEED_SIZE;
        FeedEntry& e = scan_local_feed[idx];
        int ry = FEED_FIRST_Y + i * feed_row_h - slide_offset;

        unsigned long age = feed_now - e.timestamp;
        float af;
        if (age < FEED_AGE_FULL_MS) {
            af = 1.0f;
        } else if (age < FEED_AGE_GONE_MS) {
            af = 1.0f - (float)(age - FEED_AGE_FULL_MS)
                       / (float)(FEED_AGE_GONE_MS - FEED_AGE_FULL_MS);
        } else {
            af = 0.0f;
        }
        if (af < 0.1f) continue;

        // Symbol — shape=protocol, color=threat status (amber if flock)
        int sym_x = FEED_X + DIVIDER_GAP + 1;
        int sym_y = ry + 3;
        uint16_t base_proto_col = e.is_flock  ? CAUTION_COLOR
                                : (e.proto == 0) ? HEADER_COLOR
                                                 : PURPLE_COLOR;
        uint16_t proto_col = lerp_col16(BG_COLOR, base_proto_col, af);

        if (e.proto == 0) {
            spr.drawTriangle(sym_x,     sym_y + 7,
                             sym_x + 8, sym_y + 7,
                             sym_x + 4, sym_y,
                             proto_col);
        } else {
            int ecx = sym_x + 4, ecy = sym_y + 4, ehr = 4;
            spr.drawLine(ecx,       ecy - ehr, ecx + ehr, ecy,       proto_col);
            spr.drawLine(ecx + ehr, ecy,       ecx,       ecy + ehr, proto_col);
            spr.drawLine(ecx,       ecy + ehr, ecx - ehr, ecy,       proto_col);
            spr.drawLine(ecx - ehr, ecy,       ecx,       ecy - ehr, proto_col);
        }

        int name_x = sym_x + UI_PAD_MD;

        spr.setTextColor(lerp_col16(BG_COLOR, TEXT_COLOR, af), BG_COLOR);
        spr.setTextSize(TS_BODY);

        int max_chars = (FEED_RIGHT - name_x - 2) / ts_char_w(TS_BODY);
        if (max_chars > 14) max_chars = 14;
        if (max_chars < 1)  max_chars = 1;
        char nd[16];
        strncpy(nd, e.name, max_chars);
        nd[max_chars] = '\0';
        spr.setCursor(name_x, ry + 3);
        spr.print(nd);

        // Signal strength dot after the name — color maps to RSSI proximity
        {
            uint16_t dot_base;
            if      (e.rssi > -55) dot_base = HEADER_COLOR;
            else if (e.rssi > -75) dot_base = DIM_COLOR;
            else                   dot_base = CARD_BORDER;

            int dot_x = name_x + (int)strlen(nd) * ts_char_w(TS_BODY) + UI_PAD_XS * 2;
            if (dot_x + 2 < FEED_RIGHT) {
                spr.fillCircle(dot_x, ry + 6, 1, lerp_col16(BG_COLOR, dot_base, af));
            }
        }

        // Row separator — fades with row age
        {
            uint16_t sep_col = lerp_col16(BG_COLOR, CARD_BORDER, af * 0.25f);
            int sep_y2 = ry + feed_row_h - 1;
            if (sep_y2 < SPR_H) {
                int sep_x = FEED_X + DIVIDER_GAP + 1;
                spr.drawFastHLine(sep_x, sep_y2,
                                  FEED_RIGHT - sep_x, sep_col);
            }
        }

        // ── Radar glow sync: if this device is lit on the scan radar,
        // draw a faint accent bar on the left edge of the feed row ──
        if (scanner_viz_mode == 0) {
            for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
                if (!scan_devs[pi].occupied) continue;
                if (scan_devs[pi].sweep_bright < 0.10f) continue;
                if (strncmp(scan_devs[pi].mac, e.mac, 17) != 0) continue;
                float sync_alpha = scan_devs[pi].sweep_bright * af * 0.6f;
                if (sync_alpha > 0.05f) {
                    uint16_t sync_col = lerp_col16(BG_COLOR, base_proto_col, sync_alpha);
                    spr.fillRect(FEED_X + UI_PAD_SM, ry, 2, feed_row_h - 1, sync_col);
                }
                break;
            }
        }
    }

    spr.clearClipRect();
    feed_commit_pending();
}

// ── SCAN viz: device-list refresh (also used by ambient radar) ────────────
static void prox_radar_refresh(unsigned long frame_ms) {
    if (frame_ms - scan_last_refresh_ms < 200 && scan_last_refresh_ms != 0) return;
    scan_last_refresh_ms = frame_ms;

    const float PI2f = 2.0f * (float)M_PI;
    bool matched[SCAN_MAX_DEVICES] = {false};

    for (int fi = 0; fi < scan_local_count && fi < FEED_SIZE; fi++) {
        int idx = (scan_local_head - fi + FEED_SIZE * 2) % FEED_SIZE;
        FeedEntry& e = scan_local_feed[idx];
        if (e.mac[0] == '\0') continue;
        if ((frame_ms - e.timestamp) > 60000UL) continue;

        bool dup = false;
        for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
            if (scan_devs[pi].occupied && matched[pi] &&
                strncmp(scan_devs[pi].mac, e.mac, 17) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;

        int slot = -1;
        for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
            if (scan_devs[pi].occupied &&
                strncmp(scan_devs[pi].mac, e.mac, 17) == 0) {
                slot = pi; matched[pi] = true; break;
            }
        }

        float rssi_norm = (float)(e.rssi - (-95)) / (float)((-25) - (-95));
        if (rssi_norm < 0.0f) rssi_norm = 0.0f;
        if (rssi_norm > 1.0f) rssi_norm = 1.0f;
        // Compress radial range so icons cluster toward center.
        // sqrt curve means weak signals still spread out but strong
        // signals group tightly near the center dot.
        float target_dist = (1.0f - rssi_norm) * 0.90f;

        if (slot >= 0) {
            scan_devs[slot].dist       = target_dist;
            scan_devs[slot].proto      = e.proto;
            scan_devs[slot].is_flock   = e.is_flock;
            scan_devs[slot].last_seen_ms = frame_ms;
            scan_devs[slot].rssi       = e.rssi;
        } else {
            for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
                if (!scan_devs[pi].occupied) { slot = pi; break; }
            }
            if (slot < 0) {
                int weakest_idx = 0;
                int weakest_rssi = 0;
                for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
                    if (!matched[pi] && scan_devs[pi].occupied) {
                        if (weakest_rssi == 0 || scan_devs[pi].rssi < weakest_rssi) {
                            weakest_rssi = scan_devs[pi].rssi;
                            weakest_idx = pi;
                        }
                    }
                }
                if (e.rssi > weakest_rssi) {
                    slot = weakest_idx;
                } else {
                    continue;
                }
            }
            ScanDevice& d = scan_devs[slot];
            d.occupied     = true;
            strncpy(d.mac, e.mac, 17); d.mac[17] = '\0';
            d.proto        = e.proto;
            d.is_flock     = e.is_flock;
            d.rssi         = e.rssi;
            d.last_seen_ms = frame_ms;
            d.dist         = target_dist;
            d.dist_smooth  = target_dist;
            d.sweep_bright = 0.0f;

            d.last_sweep_ms = 0;
            uint32_t h = hash_mac(e.mac);
            d.angle        = ((float)(h % 10000) / 10000.0f) * PI2f;
            d.angle_smooth = d.angle;
            d.appear_ms    = frame_ms;
            d.has_appeared = true;
            matched[slot] = true;
        }
    }

    for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
        if (scan_devs[pi].occupied && !matched[pi]) {
            unsigned long expire_ms = 22000UL;
            {
                int occupied_count = 0;
                for (int k = 0; k < SCAN_MAX_DEVICES; k++) {
                    if (scan_devs[k].occupied) occupied_count++;
                }
                if (occupied_count >= SCAN_MAX_DEVICES - 1) expire_ms = 10000UL;
            }
            if ((frame_ms - scan_devs[pi].last_seen_ms) > expire_ms) {
                scan_devs[pi].occupied = false;
            }
        }
    }

    // Angular repulsion — push overlapping icons apart.
    // Single pass: for each pair within MIN_ANGLE_SEP, split the difference.
    // Stable because angles are hash-derived (don't drift) and the nudge
    // is capped at MAX_NUDGE so icons can't migrate to the wrong quadrant.
    {
        const float MIN_ANGLE_SEP = 0.40f;  // ~23° — minimum angular gap
        const float MAX_NUDGE     = 0.12f;  // ~7° — max displacement per frame
        const float PI2f = 2.0f * (float)M_PI;

        for (int i = 0; i < SCAN_MAX_DEVICES; i++) {
            if (!scan_devs[i].occupied) continue;
            for (int j = i + 1; j < SCAN_MAX_DEVICES; j++) {
                if (!scan_devs[j].occupied) continue;

                float dist_diff = fabsf(scan_devs[i].dist_smooth - scan_devs[j].dist_smooth);
                if (dist_diff > 0.35f) continue;

                float diff = scan_devs[i].angle - scan_devs[j].angle;
                if (diff >  (float)M_PI) diff -= PI2f;
                if (diff < -(float)M_PI) diff += PI2f;
                float abs_diff = fabsf(diff);

                if (abs_diff < MIN_ANGLE_SEP) {
                    float push = (MIN_ANGLE_SEP - abs_diff) * 0.5f;
                    if (push > MAX_NUDGE) push = MAX_NUDGE;
                    float sign = (diff >= 0.0f) ? 1.0f : -1.0f;
                    scan_devs[i].angle += sign * push;
                    scan_devs[j].angle -= sign * push;

                    if (scan_devs[i].angle < 0.0f)  scan_devs[i].angle += PI2f;
                    if (scan_devs[i].angle >= PI2f)  scan_devs[i].angle -= PI2f;
                    if (scan_devs[j].angle < 0.0f)  scan_devs[j].angle += PI2f;
                    if (scan_devs[j].angle >= PI2f)  scan_devs[j].angle -= PI2f;
                }
            }
        }
    }
}

// Detect whether the sprite buffer stores RGB565 byte-swapped.
// Writes a known logical color via drawPixel, reads back the raw buffer.
// If the raw value doesn't match what we wrote, bytes are swapped.
static inline void detect_buffer_byte_order(M5Canvas& sprite) {
    if (g_buf_byte_order_detected) return;
    uint16_t* buf = (uint16_t*)sprite.getBuffer();
    if (!buf) return;
    uint16_t saved = buf[0];
    sprite.drawPixel(0, 0, 0xF800);  // pure logical red
    g_buf_bytes_swapped = (buf[0] != 0xF800);
    buf[0] = saved;
    g_buf_byte_order_detected = true;
    Serial.printf("[GFX] Buffer bytes_swapped=%d\n", (int)g_buf_bytes_swapped);
}

// Read a pixel from the sprite buffer in LOGICAL RGB565 format.
static inline uint16_t read_pixel_logical(const uint16_t* buf, int idx) {
    uint16_t raw = buf[idx];
    return g_buf_bytes_swapped ? (uint16_t)((raw >> 8) | (raw << 8)) : raw;
}

// Write a pixel to the sprite buffer from a LOGICAL RGB565 value.
static inline void write_pixel_logical(uint16_t* buf, int idx, uint16_t logical) {
    buf[idx] = g_buf_bytes_swapped
             ? (uint16_t)((logical >> 8) | (logical << 8))
             : logical;
}

// ── Viz mode 0: SCAN (proximity radar) ────────────────────────────────────
static void draw_scanner_viz_scan(unsigned long frame_ms) {
    detect_buffer_byte_order(spr);

    const float PI2f = 2.0f * (float)M_PI;

    const int CX = VIZ_X + VIZ_W / 2;
    const int CY = VIZ_Y + VIZ_H / 2;
    const int R  = (int)((float)VIZ_H * 1.40f);

    float dt = (scan_last_frame_ms == 0) ? 16.0f
             : (float)(frame_ms - scan_last_frame_ms);
    if (dt > 100.0f) dt = 100.0f;
    scan_last_frame_ms = frame_ms;

    // Two overlapping sine waves: primary at 0.7× rotation, secondary at 1.3×.
    // The secondary adds asymmetry — the sweep doesn't just speed up and slow
    // down symmetrically, it lingers in some quadrants more than others.
    // Stronger swing: primary ±25%, secondary ±12%.
    // The sweep visibly pauses in some quadrants and accelerates
    // through others. Total range: ±37% at rare beat peaks.
    float swing = 1.0f
                + 0.25f * sinf(scan_sweep_angle * 0.7f)
                + 0.12f * sinf(scan_sweep_angle * 1.3f);
    scan_sweep_angle += 0.0015f * dt * swing;
    if (scan_sweep_angle >= PI2f) scan_sweep_angle -= PI2f;

    // ── 1. Phosphor trail (lowest layer) ─────────────────────────────────
    // Per-pixel alpha blend in 2×2 blocks — composites over existing
    // content (rings, background) instead of overwriting with flat blocks.
    // Quadratic falloff: bright near the line, smooth monotonic decay to
    // invisible at the tail. Angle-to-center lookups use scan_angle_lut[]
    // built once at first render; falls back to fast_atan2f if alloc failed.
    {
        scan_angle_lut_build();  // no-op after first call

        const float TRAIL_ARC = 1.8f;   // radians behind sweep (~103°)
        const float PEAK      = 0.28f;  // max blend alpha right behind line
        const float GAP       = 0.04f;  // skip the line itself
        const float R2_max    = (float)(R * R) * 1.10f;
        const float inv_range = 1.0f / (TRAIL_ARC - GAP);

        uint16_t* sbuf = (uint16_t*)spr.getBuffer();
        const int sbuf_w = DISP_W;

        // Normalize sweep angle to [0, 2π) once per frame.
        float sweep_norm = scan_sweep_angle;
        if (sweep_norm < 0.0f)           sweep_norm += 2.0f * (float)M_PI;
        if (sweep_norm >= 2.0f * (float)M_PI) sweep_norm -= 2.0f * (float)M_PI;

        int row_idx = 0;
        for (int py = VIZ_Y; py < VIZ_Y + VIZ_H; py += 2, row_idx++) {
            int col_idx = 0;
            for (int px = VIZ_X; px < VIZ_X + VIZ_W; px += 2, col_idx++) {
                float dx = (float)(px - CX);
                float dy = (float)(py - CY);
                if (dx * dx + dy * dy > R2_max) continue;

                float ang;
                if (scan_angle_lut_ready) {
                    uint8_t q = scan_angle_lut[row_idx * SCAN_LUT_COLS + col_idx];
                    ang = (float)q * SCAN_LUT_INV_SCALE;
                } else {
                    ang = fast_atan2f(dy, dx);
                    if (ang < 0.0f) ang += 2.0f * (float)M_PI;
                }

                float diff = sweep_norm - ang;
                if (diff < 0.0f) diff += 2.0f * (float)M_PI;
                // diff is now in [0, 2π) — angular distance behind the sweep.

                if (diff < GAP)       continue;    // within the line
                if (diff > TRAIL_ARC) continue;    // past the tail (ahead-of-sweep wraps here)

                float t = (diff - GAP) * inv_range; // 0..1 (0 = near line, 1 = tail)
                float fade = 1.0f - t;
                float alpha = PEAK * fade * fade;   // quadratic falloff
                if (alpha < 0.008f) continue;

                // Blend 2×2 block — reads existing pixel, mixes, writes back
                for (int by = 0; by < 2; by++) {
                    int ry = py + by;
                    if (ry >= VIZ_Y + VIZ_H) break;
                    int row = ry * sbuf_w;
                    for (int bx = 0; bx < 2; bx++) {
                        int rx = px + bx;
                        if (rx >= VIZ_X + VIZ_W) break;
                        int idx = row + rx;
                        uint16_t existing = read_pixel_logical(sbuf, idx);
                        uint16_t blended  = lerp_col16(existing, HEADER_COLOR, alpha);
                        write_pixel_logical(sbuf, idx, blended);
                    }
                }
            }
        }
    }

    // ── 2. Range rings (on top of glow) ──────────────────────────────────
    // Ring radii sized to fit the panel — outermost ring is a complete circle.
    // R stays large for sweep/glow/device math; rings are visual guides only.
    uint16_t ring_col = lerp_col16(BG_COLOR, HEADER_COLOR, 0.30f);
    for (int i = 1; i <= 4; i++) {
        spr.drawCircle(CX, CY, R * i / 4, ring_col);
    }

    // ── 3. Sweep line (on top of rings) ──────────────────────────────────
    {
        int ex = CX + (int)((float)R * cosf(scan_sweep_angle));
        int ey = CY + (int)((float)R * sinf(scan_sweep_angle));
        spr.drawLine(CX, CY, ex, ey, lerp_col16(BG_COLOR, HEADER_COLOR, 0.70f));
    }

    // ── 4. Device icons ───────────────────────────────────────────────────
    prox_radar_refresh(frame_ms);

    for (int pi = 0; pi < SCAN_MAX_DEVICES; pi++) {
        if (!scan_devs[pi].occupied) continue;
        ScanDevice& d = scan_devs[pi];

        // Smooth radial + angular motion
        d.dist_smooth = anim_filter(d.dist_smooth, d.dist, 800.0f, dt);

        // Ease the display angle toward the repulsion-adjusted target.
        // Wraps correctly around 0/2π so icons don't spin the long way.
        {
            float diff = d.angle - d.angle_smooth;
            if (diff >  (float)M_PI) diff -= PI2f;
            if (diff < -(float)M_PI) diff += PI2f;
            d.angle_smooth += diff * (1.0f - expf(-dt / 400.0f));
            if (d.angle_smooth < 0.0f)  d.angle_smooth += PI2f;
            if (d.angle_smooth >= PI2f) d.angle_smooth -= PI2f;
        }

        // Sweep contact — signed angular diff; trigger on trailing side only
        float diff = scan_sweep_angle - d.angle;
        if (diff >  (float)M_PI) diff -= PI2f;
        if (diff < -(float)M_PI) diff += PI2f;
        float behind = (diff >= 0.0f) ? diff : (PI2f + diff);
        const float sweep_zone = PI2f * 0.12f;

        if (behind < sweep_zone) {
            float t      = behind / sweep_zone;
            float target = (1.0f - t) * (1.0f - t) * (1.0f - t);
            if (target > d.sweep_bright) d.sweep_bright = target;
            d.last_sweep_ms = frame_ms;
        } else if (frame_ms - d.last_sweep_ms > 1000) {
            d.sweep_bright *= powf(0.9975f, (float)dt);
            if (d.sweep_bright < 0.0f) d.sweep_bright = 0.0f;
        }

        // Position — uses eased angle for smooth angular motion
        float draw_dist = 0.05f + d.dist_smooth * 0.70f;
        float ds, dc;
        compute_sincos(d.angle_smooth, &ds, &dc);
        int dpx = CX + (int)(draw_dist * (float)R * dc);
        int dpy = CY + (int)(draw_dist * (float)R * ds);

        // Clamp to panel so icons never disappear off-screen
        const int MARGIN = 2;
        if (dpx < VIZ_X + MARGIN)          dpx = VIZ_X + MARGIN;
        if (dpx > VIZ_X + VIZ_W - MARGIN)  dpx = VIZ_X + VIZ_W - MARGIN;
        if (dpy < VIZ_Y + MARGIN)          dpy = VIZ_Y + MARGIN;
        if (dpy > VIZ_Y + VIZ_H - MARGIN)  dpy = VIZ_Y + VIZ_H - MARGIN;

        // Master opacity — handles appear fade-in and exit fade-out.
        // Target is 1.0 while alive, decays through exit phases.
        float opacity_target = 1.0f;
        unsigned long age = frame_ms - d.last_seen_ms;
        if (age > 15000UL) {
            // Phase 3 (15–22s): flicker — rapid oscillation on fading alpha
            float exit_t = (float)(age - 15000UL) / 7000.0f;
            if (exit_t > 1.0f) exit_t = 1.0f;
            float fade = 0.3f * (1.0f - exit_t);
            float flicker = 0.5f + 0.5f * sinf((float)frame_ms * 0.02f);
            opacity_target = fade * flicker;
        } else if (age > 8000UL) {
            // Phase 2 (8–15s): smooth fade from 1.0 → 0.3
            float fade_t = (float)(age - 8000UL) / 7000.0f;
            opacity_target = 1.0f - fade_t * 0.7f;
        }

        // Appear: ease opacity from 0→target over 400ms
        float appear_t = d.has_appeared
            ? (float)(frame_ms - d.appear_ms) / 400.0f
            : 1.0f;
        if (appear_t > 1.0f) appear_t = 1.0f;
        float appear_ease = appear_t * appear_t * (3.0f - 2.0f * appear_t);
        opacity_target *= appear_ease;

        float age_af = opacity_target;
        if (age_af < 0.02f) continue;  // skip drawing invisible icons

        // Color — on sweep contact, outline shifts toward white
        uint16_t base_col = d.is_flock ? CAUTION_COLOR
                          : (d.proto == 0 ? HEADER_COLOR : PURPLE_COLOR);
        uint16_t icon_col;
        if (d.sweep_bright > 0.05f) {
            uint16_t bright_col = lerp_col16(base_col, TEXT_COLOR, d.sweep_bright * 0.40f);
            icon_col = (age_af >= 1.0f) ? bright_col
                     : lerp_col16(BG_COLOR, bright_col, age_af);
        } else {
            icon_col = (age_af >= 1.0f) ? base_col
                     : lerp_col16(BG_COLOR, base_col, age_af);
        }

        // Size — flock icons pulse continuously, non-flock static.
        // Appear animation scales from 0 → full over the first 400ms.
        int base_sz = d.is_flock ? 7 : 6;
        float pulse_factor = 0.0f;
        if (d.is_flock) {
            float breath = 0.5f + 0.5f * sinf((float)frame_ms * 2.0f * (float)M_PI / 1200.0f);
            pulse_factor = (breath - 0.5f) * 0.16f;
        }
        float appear_scale = appear_ease;  // 0→1 smoothstep from opacity block
        int sz = (int)((float)base_sz * (1.0f + pulse_factor) * appear_scale);
        if (sz < 1) sz = 1;

        // ── Sweep glow: per-pixel alpha blend, byte-order-corrected ──
        // Reads pixel from buffer, swaps to logical format if needed,
        // blends in logical RGB565 (where lerp_col16 works correctly),
        // swaps back, writes to buffer. Result: correct colors guaranteed.
        if (d.sweep_bright > 0.08f) {
            float glow_t = d.sweep_bright * d.sweep_bright * (3.0f - 2.0f * d.sweep_bright);

            uint16_t* sbuf = (uint16_t*)spr.getBuffer();
            const int sbuf_w = DISP_W;

            int   glow_r    = sz + 11;
            float glow_r2   = (float)(glow_r * glow_r);
            float glow_peak = glow_t * 0.38f;

            int gx0 = dpx - glow_r; if (gx0 < VIZ_X) gx0 = VIZ_X;
            int gx1 = dpx + glow_r; if (gx1 >= VIZ_X + VIZ_W) gx1 = VIZ_X + VIZ_W - 1;
            int gy0 = dpy - glow_r; if (gy0 < VIZ_Y) gy0 = VIZ_Y;
            int gy1 = dpy + glow_r; if (gy1 >= VIZ_Y + VIZ_H) gy1 = VIZ_Y + VIZ_H - 1;

            for (int gy = gy0; gy <= gy1; gy++) {
                int row_off = gy * sbuf_w;
                for (int gx = gx0; gx <= gx1; gx++) {
                    float dx = (float)(gx - dpx);
                    float dy = (float)(gy - dpy);
                    float dist2 = dx * dx + dy * dy;
                    if (dist2 >= glow_r2) continue;

                    float falloff = 1.0f - dist2 / glow_r2;
                    float alpha = glow_peak * falloff * falloff;
                    if (alpha < 0.015f) continue;

                    int idx = row_off + gx;
                    uint16_t existing_logical = read_pixel_logical(sbuf, idx);
                    uint16_t blended_logical  = lerp_col16(existing_logical,
                                                           base_col, alpha);
                    write_pixel_logical(sbuf, idx, blended_logical);
                }
            }
        }

        // Flock icons get a semi-transparent fill — threat stands out from ambient traffic
        if (d.is_flock) {
            uint16_t fill_col = lerp_col16(BG_COLOR, icon_col, 0.45f);
            if (d.proto == 0) {
                spr.fillTriangle(
                    dpx,                       dpy - sz,
                    dpx - (int)(sz * 0.85f),  dpy + (int)(sz * 0.65f),
                    dpx + (int)(sz * 0.85f),  dpy + (int)(sz * 0.65f),
                    fill_col);
            } else {
                spr.fillTriangle(dpx, dpy - sz, dpx + sz, dpy, dpx, dpy + sz, fill_col);
                spr.fillTriangle(dpx, dpy - sz, dpx - sz, dpy, dpx, dpy + sz, fill_col);
            }
        }

        // Shape outline (drawn on top of fill for all devices)
        if (d.proto == 0) {
            // WiFi: outlined triangle, point up
            spr.drawTriangle(
                dpx,                       dpy - sz,
                dpx - (int)(sz * 0.85f),  dpy + (int)(sz * 0.65f),
                dpx + (int)(sz * 0.85f),  dpy + (int)(sz * 0.65f),
                icon_col);
        } else {
            // BLE: outlined diamond
            spr.drawLine(dpx,      dpy - sz, dpx + sz, dpy,      icon_col);
            spr.drawLine(dpx + sz, dpy,      dpx,      dpy + sz, icon_col);
            spr.drawLine(dpx,      dpy + sz, dpx - sz, dpy,      icon_col);
            spr.drawLine(dpx - sz, dpy,      dpx,      dpy - sz, icon_col);
        }

    }

    // ── 5. Center dot in box frame (topmost) ─────────────────────────────
    {
        const int box_sz = 7;
        spr.drawRect(CX - box_sz, CY - box_sz, box_sz * 2, box_sz * 2,
                     lerp_col16(BG_COLOR, HEADER_COLOR, 0.35f));
        spr.drawRect(CX - box_sz + 1, CY - box_sz + 1, box_sz * 2 - 2, box_sz * 2 - 2,
                     lerp_col16(BG_COLOR, HEADER_COLOR, 0.25f));
        spr.fillCircle(CX, CY, 2, lerp_col16(BG_COLOR, HEADER_COLOR, 0.50f));
    }
}

// ── Viz mode 1: SPECTRUM ───────────────────────────────────────────────────
// 13-bar 2.4 GHz channel histogram driven by channel_pkt_counts[],
// incremented in the WiFi sniffer ISR. Display values refresh every 2 s
// with a 2/3 decay so the bars represent a rolling window. Current
// Maintain the per-channel packet histogram used by the SPECTRUM viz.
// Called regularly regardless of which viz mode is active so counts don't
// accumulate unbounded while SPECTRUM is off-screen.
static void update_channel_histogram() {
    unsigned long now = millis();
    if (now - channel_display_last_update < 5000) return;

    channel_peak = 1;
    for (int i = 0; i < NUM_WIFI_CHANNELS; i++) {
        channel_pkt_display[i] = channel_pkt_counts[i];
        channel_pkt_counts[i]  = channel_pkt_counts[i] / 2;
        if (channel_pkt_display[i] > channel_peak)
            channel_peak = channel_pkt_display[i];
    }
    channel_display_last_update = now;
}

// hopping channel renders bright with a triangle marker; flock
// detections briefly flash the peak bar in CAUTION_COLOR.
static void draw_scanner_viz_spectrum(unsigned long frame_ms) {
    // Smoothing is now done in draw_scanner_screen() warm-keep block.

    // Ambient waviness — two overlapping sines per channel at different
    // frequencies and per-channel phase offsets so the curve undulates
    // organically when data is flat. Amplitudes (0.04 + 0.025) are
    // small enough not to distort real packet ratios.
    float spectrum_display[NUM_WIFI_CHANNELS];
    for (int i = 0; i < NUM_WIFI_CHANNELS; i++) {
        float wave_phase  = (float)i * 0.8f + (float)frame_ms * 0.002f;
        float wave2_phase = (float)i * 1.3f + (float)frame_ms * 0.0013f;
        float ambient_wave = sinf(wave_phase) * 0.04f + sinf(wave2_phase) * 0.025f;
        float v = spectrum_smooth[i] + ambient_wave;
        if (v < 0.02f) v = 0.02f;  // never fully flatlines
        if (v > 1.0f)  v = 1.0f;
        spectrum_display[i] = v;
    }

    // BLE presence — checked once per second; drives baseline indicator,
    // channel label tinting, and curve color blending.
    static bool ble_active = false;
    {
        static unsigned long ble_check_ms = 0;
        if (frame_ms - ble_check_ms > 1000) {
            ble_check_ms = frame_ms;
            ble_active = false;
            for (int i = 0; i < scan_local_count && i < FEED_SIZE; i++) {
                int idx2 = (scan_local_head - i + FEED_SIZE * 2) % FEED_SIZE;
                if (scan_local_feed[idx2].proto == 1) { ble_active = true; break; }
            }
        }
    }

    // Ease the curve color toward purple when BLE is scanning.
    // Uses the live BLE scan state, not the 1s-polled ble_active flag,
    // so the transition tracks the actual radio activity.
    {
        bool ble_scanning = (pBLEScan && pBLEScan->isScanning());
        float ble_target = ble_scanning ? 1.0f : 0.0f;
        static unsigned long ble_blend_last_frame = 0;
        float sdt = (ble_blend_last_frame == 0) ? 16.0f
                  : (float)(frame_ms - ble_blend_last_frame);
        if (sdt > 100.0f) sdt = 100.0f;
        ble_blend_last_frame = frame_ms;
        spectrum_ble_blend = anim_filter(spectrum_ble_blend, ble_target, 250.0f, sdt);
    }

    // Plot area — 8px reserved at bottom for channel labels (1, 6, 11, 13).
    const int plot_x      = VIZ_X + 2;
    const int plot_w      = VIZ_W - 4;
    const int plot_y      = VIZ_Y + 2;
    const int plot_h      = VIZ_H - 12;
    const int plot_bottom = plot_y + plot_h;

    // ── Diagonal hatch background ──
    // Bold 2px diagonal stripes at 8px spacing — deliberate tactical-grid
    // texture, easy to read on the LCD. Tightened clip keeps the hatch
    // inside the panel; we restore the outer scanner clip afterwards so
    // the rest of the spectrum renders against the same bounds.
    {
        uint16_t hatch_col = HATCH_COLOR;
        // Clip hatch to plot area only — channel labels below sit on clean BG
        spr.setClipRect(VIZ_X, VIZ_Y, VIZ_W, plot_bottom - VIZ_Y);
        for (int d = -VIZ_H; d < VIZ_W + VIZ_H; d += 8) {
            int x0 = VIZ_X + d;
            int y0 = VIZ_Y;
            int x1 = x0 + VIZ_H;
            int y1 = VIZ_Y + VIZ_H;
            spr.drawLine(x0,     y0, x1,     y1, hatch_col);
            spr.drawLine(x0 + 1, y0, x1 + 1, y1, hatch_col);
        }
        spr.setClipRect(VIZ_X, VIZ_Y, VIZ_W, VIZ_H);
    }

    spr.drawFastHLine(plot_x, plot_bottom, plot_w, GRID_LINE_DIM);

    // BLE band baseline indicator — purple line under channels 1–11
    if (ble_active) {
        int ble_x0 = plot_x;
        int ble_x1 = plot_x + (10 * plot_w) / 12;  // ch 11 = index 10
        spr.drawFastHLine(ble_x0, plot_bottom, ble_x1 - ble_x0, PURPLE_COLOR);
    }

    // Channel labels below baseline: 1, 6, 11, 13
    {
        spr.setTextSize(TS_MICRO);
        const int label_chs[] = {1, 6, 11, 13};
        for (int li = 0; li < 4; li++) {
            int ch_idx = label_chs[li] - 1;
            int lx = plot_x + (ch_idx * plot_w) / 12;
            char ch_label[4];
            snprintf(ch_label, sizeof(ch_label), "%d", label_chs[li]);
            int label_w = (int)strlen(ch_label) * ts_char_w(TS_MICRO);
            bool in_ble = (label_chs[li] <= 11);
            spr.setTextColor((ble_active && in_ble) ? PURPLE_COLOR : DIM_COLOR, BG_COLOR);
            spr.setCursor(lx - label_w / 2, plot_bottom + 2);
            spr.print(ch_label);
        }
    }

    const float MAX_HEIGHT = (float)plot_h * 0.58f;
    auto val_to_y = [&](float val) -> int {
        return plot_bottom - (int)(val * MAX_HEIGHT);
    };

    // If a flock detection landed recently, mark the busiest channel as
    // the "flock" channel so curve and fill near it tint CAUTION.
    bool recent_detection = (scanner_flash_ms > 0 && (frame_ms - scanner_flash_ms) < 3000);
    int  flock_ch_idx = -1;
    if (recent_detection) {
        uint32_t max_val = 0;
        for (int i = 0; i < NUM_WIFI_CHANNELS; i++) {
            if (channel_pkt_display[i] > max_val) {
                max_val = channel_pkt_display[i];
                flock_ch_idx = i;
            }
        }
    }

    // Inline Catmull-Rom evaluator — used by both the fill and the
    // curve passes so they trace exactly the same path.
    auto eval_curve = [&](int px_col) -> float {
        float ch_f = (float)px_col / (float)plot_w * 12.0f;
        int ch_i = (int)ch_f;
        float t = ch_f - (float)ch_i;
        int i0 = max(0, ch_i - 1);
        int i1 = min(12, ch_i);
        int i2 = min(12, ch_i + 1);
        int i3 = min(12, ch_i + 2);
        float p0 = spectrum_display[i0];
        float p1 = spectrum_display[i1];
        float p2 = spectrum_display[i2];
        float p3 = spectrum_display[i3];
        float t2 = t * t, t3 = t2 * t;
        float val = 0.5f * ((-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3
                          + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2
                          + (-p0 + p2) * t
                          + 2.0f*p1);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        return val;
    };

    // Pre-evaluate the spline into a cache so fill, line, and dot passes
    // read from memory instead of recomputing 8 FP muls per pixel.
    float curve_cache[VIZ_W + 2];
    for (int px_col = 0; px_col <= plot_w; px_col++) {
        curve_cache[px_col] = eval_curve(px_col);
    }


    // Gradient fill under the curve. Channels 1-11 blend toward PURPLE_COLOR
    // at the bottom when BLE devices are active, indicating band overlap.
    for (int px_col = 0; px_col <= plot_w; px_col++) {
        float ch_f = (float)px_col / (float)plot_w * 12.0f;
        int ch_i = (int)ch_f;
        bool flock_fill = (flock_ch_idx >= 0 && abs(ch_i - flock_ch_idx) <= 1);
        uint16_t fill_base = flock_fill ? CAUTION_COLOR
                           : lerp_col16(HEADER_COLOR, PURPLE_COLOR, spectrum_ble_blend);

        float val = curve_cache[px_col];
        int cx = plot_x + px_col;
        int cy = val_to_y(val);


        {
            int fy = cy + 1;
            while (fy < plot_bottom) {
                float fill_t = (float)(fy - cy) / (float)(plot_bottom - cy);
                float fill_alpha = (1.0f - fill_t * fill_t) * 0.35f;
                if (fill_alpha < 0.02f) break;
                uint16_t col = lerp_col16(BG_COLOR, fill_base, fill_alpha);
                int run_end = fy + 1;
                while (run_end < plot_bottom) {
                    float ft2 = (float)(run_end - cy) / (float)(plot_bottom - cy);
                    float fa2 = (1.0f - ft2 * ft2) * 0.30f;
                    if (fa2 < 0.01f) break;
                    if (lerp_col16(BG_COLOR, fill_base, fa2) != col) break;
                    run_end++;
                }
                spr.fillRect(cx, fy, 1, run_end - fy, col);
                fy = run_end;
            }
        }
    }


    // Smooth scan line — eased x slides between channel positions
    // instead of snapping when the hopper advances. On wrap from a high
    // channel back to ch1, snap the eased position to just past the left
    // edge so the line resets and sweeps in from the left rather than
    // easing backwards across the chart.
    static int scan_prev_channel = 0;
    bool channel_wrapped = (current_channel == 1 && scan_prev_channel > 10);
    scan_prev_channel = current_channel;

    float scan_target_x = (float)plot_x +
        (float)(current_channel - 1) / 12.0f * (float)plot_w;
    if (scan_line_last_frame == 0) scan_line_x_f = scan_target_x;
    if (channel_wrapped)           scan_line_x_f = (float)(plot_x - 4);
    float scan_dt = (scan_line_last_frame == 0) ? 16.0f
                  : (float)(frame_ms - scan_line_last_frame);
    if (scan_dt > 100.0f) scan_dt = 100.0f;
    // scan_line_last_frame updated in draw_scanner_screen() warm-keep block
    scan_line_x_f = anim_filter(scan_line_x_f, scan_target_x, 120.0f, scan_dt);
    int scan_x = (int)(scan_line_x_f + 0.5f);

    // Trailing wake — 8 vertical pixels left of the scan line, fading
    // from 25% to 0% so the scanning direction reads as "→". Drawn
    // before the main line so the line sits brightest on top.
    const int WAKE_LENGTH = 8;
    for (int wi = 1; wi <= WAKE_LENGTH; wi++) {
        int wake_x = scan_x - wi;
        if (wake_x < plot_x || wake_x > plot_x + plot_w) continue;
        float wake_t = (float)wi / (float)WAKE_LENGTH;
        float wake_alpha = (1.0f - wake_t) * 0.25f;
        spr.drawFastVLine(wake_x, plot_y, plot_h,
                          lerp_col16(BG_COLOR, HEADER_COLOR, wake_alpha));
    }
    spr.drawFastVLine(scan_x, plot_y, plot_h, SWEEP_LINE_COLOR);

    // Catmull-Rom curve, walked pixel by pixel. Single 1px drawLine per
    // segment in full HEADER_COLOR (CAUTION_COLOR near flock detection).
    int prev_px = -1, prev_py = -1;
    for (int px_col = 0; px_col <= plot_w; px_col++) {
        float ch_f = (float)px_col / (float)plot_w * 12.0f;
        int ch_i = (int)ch_f;
        float val = curve_cache[px_col];
        int cx = plot_x + px_col;
        int cy = val_to_y(val);

        if (prev_px >= 0) {
            bool near_flock = (flock_ch_idx >= 0 && abs(ch_i - flock_ch_idx) <= 1);
            uint16_t base_line = near_flock ? CAUTION_COLOR
                               : lerp_col16(HEADER_COLOR, PURPLE_COLOR, spectrum_ble_blend);
            spr.drawLine(prev_px, prev_py, cx, cy, base_line);
        }
        prev_px = cx;
        prev_py = cy;
    }

    // Intersection dot
    {
        int dot_col_px = scan_x - plot_x;
        if (dot_col_px < 0)      dot_col_px = 0;
        if (dot_col_px > plot_w) dot_col_px = plot_w;
        float dot_val = curve_cache[dot_col_px];
        int dot_y = val_to_y(dot_val);
        uint16_t dot_col = lerp_col16(HEADER_COLOR, PURPLE_COLOR, spectrum_ble_blend);
        spr.fillCircle(scan_x, dot_y, 1, dot_col);
        spr.drawCircle(scan_x, dot_y, 2, dot_col);
    }

    // Band pill — below LINE title, dark background over hatch
    {
        const char* band_label = "2.4GHz";
        int pill_w = (int)strlen(band_label) * ts_char_w(TS_MICRO) + UI_PAD_SM;
        int pill_x = TEXT_LEFT + UI_PAD_XS;
        int pill_y = VIZ_Y + UI_PAD_SM;
        int pill_h = 11;
        spr.fillRoundRect(pill_x, pill_y, pill_w, pill_h, 3, BG_COLOR);
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(pill_x + 3, pill_y + 2);
        spr.print(band_label);
    }
}

// ============================================================================
// EXPANDED FEED OVERLAY — fullscreen activity feed view
// ============================================================================
void draw_feed_expanded_overlay() {
    // Read directly from the scanner's frozen snapshot — scan_local_feed is not
    // updated while show_feed_expanded is true, so it matches exactly what the
    // 't' key targeting code reads. No mutex needed; no per-frame copy.
    int local_count = scan_local_count;
    int local_head  = scan_local_head;
    unsigned long local_now = millis();

    // Slide-in animation state — tracks new entries arriving while overlay is open
    static int           expand_prev_head   = -1;
    static unsigned long expand_shift_ms    = 0;
    static unsigned long expand_open_ms_last = 0;
    static float         feed_sel_y_f       = 0.0f;
    static unsigned long feed_sel_last_frame = 0;

    if (feed_expand_ms != expand_open_ms_last) {
        expand_open_ms_last = feed_expand_ms;
        expand_prev_head    = local_head;
        expand_shift_ms     = 0;
        feed_sel_last_frame = 0;
    }
    if (local_count > 0 && expand_prev_head != -1 && local_head != expand_prev_head) {
        expand_shift_ms  = local_now;
        expand_prev_head = local_head;
    } else if (expand_prev_head == -1 && local_count > 0) {
        expand_prev_head = local_head;
    }

    float expand_alpha = ui_progress(feed_expand_ms, UI_FADE_OUT_MS);
    bool fully_faded = (expand_alpha >= 1.0f);
    auto ea = [&](uint16_t c) -> uint16_t { return fully_faded ? c : lerp_col16(BG_COLOR, c, expand_alpha); };

    spr.fillSprite(BG_COLOR);

    // Gate content rendering until fade-in is underway
    if (expand_alpha < 0.3f) return;

    // Empty state
    if (local_count == 0) {
        spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
        spr.setTextSize(TS_BODY);
        const char* msg = "no activity yet";
        int mw = (int)strlen(msg) * ts_char_w(TS_BODY);
        spr.setCursor((DISP_W - mw) / 2, SPR_H / 2 - 4);
        kprint(spr, msg);
    } else {
        // Column layout (240px wide, 4px side padding = 232 usable):
        //   x=4    type symbol (9px) + flock star (6px) = ~18px
        //   x=26   DEVICE name (12 chars = 84px)
        //   x=114  RSSI ("-XXdBm" = 6 chars = 42px)
        //   x=162  SIGNAL (STRONG/MEDIUM/WEAK = up to 6 chars = 42px)
        //   x=210  AGE (short form like "1m" or "45s" = 20px)

        const int col_sym    = UI_PAD_SM;
        const int col_rssi   = 140;
        const int col_sig    = 195;

        // Column headers (faded in)
        const int hdr_y = 3;
        spr.setTextSize(TS_BODY);
        spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
        spr.setCursor(col_sym, hdr_y); kprint(spr, "DEVICE");
        spr.setCursor(col_rssi, hdr_y); kprint(spr, "TYPE");
        spr.setCursor(col_sig,  hdr_y); kprint(spr, "SIGNAL");

        // Render rows
        const int row_top    = hdr_y + 12;
        const int avail_h    = SPR_H - row_top;
        const int max_rows   = FEED_EXPANDED_ROWS;
        const int row_h      = avail_h / max_rows;
        const int row_pad    = avail_h - (max_rows * row_h);
        const int row_top_adj = row_top + row_pad / 2;

        // Clamp selection to the whole feed, then pull the scroll window onto it.
        // Handles the feed shrinking while the overlay is open.
        {
            if (feed_expanded_selected > local_count - 1) feed_expanded_selected = local_count - 1;
            if (feed_expanded_selected < 0)               feed_expanded_selected = 0;
            int max_off = local_count - max_rows;
            if (max_off < 0) max_off = 0;
            if (feed_expanded_offset > max_off) feed_expanded_offset = max_off;
            if (feed_expanded_offset < 0)       feed_expanded_offset = 0;
            if (feed_expanded_selected < feed_expanded_offset)
                feed_expanded_offset = feed_expanded_selected;
            if (feed_expanded_selected >= feed_expanded_offset + max_rows)
                feed_expanded_offset = feed_expanded_selected - max_rows + 1;
        }

        // Eased selection highlight Y — position within the scroll window.
        float sel_target_y = (float)(row_top_adj
                             + (feed_expanded_selected - feed_expanded_offset) * row_h);
        {
            float sdt = (feed_sel_last_frame == 0) ? 0.0f
                      : (float)(local_now - feed_sel_last_frame);
            if (sdt > 100.0f) sdt = 100.0f;
            feed_sel_last_frame = local_now;
            feed_sel_y_f = (sdt == 0.0f) ? sel_target_y
                         : anim_filter(feed_sel_y_f, sel_target_y, 80.0f, sdt);
        }

        // Shared slide offset — when a new entry arrives every row shifts
        // down together, mirroring the scanner-feed-preview animation.
        float expand_slide_t = 1.0f;
        if (expand_shift_ms > 0 && (local_now - expand_shift_ms) < 250) {
            expand_slide_t = ui_ease((float)(local_now - expand_shift_ms) / 250.0f);
        }
        int expand_slide_offset = (int)((1.0f - expand_slide_t) * (float)row_h);

        // Clip to row area to prevent slide animation from bleeding into headers
        spr.setClipRect(0, row_top_adj, DISP_W, SPR_H - row_top_adj);
        int rendered = 0;
        for (int i = feed_expanded_offset; i < local_count && rendered < max_rows; i++) {
            int idx = (local_head - i + FEED_SIZE * 2) % FEED_SIZE;
            FeedEntry& e = scan_local_feed[idx];
            int row_y = row_top_adj + rendered * row_h - expand_slide_offset;

            // Compare absolute feed index, not row position, now that the
            // window can start partway down the feed.
            bool is_sel = (i == feed_expanded_selected);
            uint16_t row_bg = is_sel ? lerp_col16(BG_COLOR, CARD_COLOR, 0.5f) : BG_COLOR;

            // Selection highlight
            if (is_sel) {
                int sel_y = (int)feed_sel_y_f - expand_slide_offset;
                spr.fillRect(0, sel_y, DISP_W, row_h, ea(row_bg));
                spr.drawFastHLine(0, sel_y,            DISP_W, ea(ACCENT_COLOR));
                spr.drawFastHLine(0, sel_y + row_h - 1, DISP_W, ea(ACCENT_COLOR));
            }

            // Type symbol color (flock entries tinted toward amber), faded in
            uint16_t proto_col = e.is_flock  ? CAUTION_COLOR
                               : (e.proto == 0) ? HEADER_COLOR
                                                : PURPLE_COLOR;
            proto_col = ea(proto_col);

            // Small stats-style symbols: outline-only triangle (WiFi) or diamond (BLE)
            int sym_x = col_sym;
            int sym_y = row_y + 3;
            if (e.proto == 0) {
                spr.drawTriangle(sym_x,     sym_y + 7,
                                 sym_x + 8, sym_y + 7,
                                 sym_x + 4, sym_y,
                                 proto_col);
            } else {
                int ecx = sym_x + 4, ecy = sym_y + 4, ehr = 4;
                spr.drawLine(ecx,       ecy - ehr, ecx + ehr, ecy,       proto_col);
                spr.drawLine(ecx + ehr, ecy,       ecx,       ecy + ehr, proto_col);
                spr.drawLine(ecx,       ecy + ehr, ecx - ehr, ecy,       proto_col);
                spr.drawLine(ecx - ehr, ecy,       ecx,       ecy - ehr, proto_col);
            }
            // DEVICE name — matches scanner feed preview spacing
            int name_start_x = sym_x + UI_PAD_MD;
            int name_max_chars = (col_rssi - name_start_x - 4) / ts_char_w(TS_BODY);
            if (name_max_chars > 14) name_max_chars = 14;
            if (name_max_chars < 1) name_max_chars = 1;
            if (name_max_chars > (int)sizeof(e.name) - 1) name_max_chars = sizeof(e.name) - 1;
            char name_disp[20];
            strncpy(name_disp, e.name, name_max_chars);
            name_disp[name_max_chars] = '\0';
            spr.setTextColor(ea(TEXT_COLOR), ea(row_bg));
            spr.setTextSize(TS_BODY);
            spr.setCursor(name_start_x, row_y + 3);
            spr.print(name_disp);

            // Protocol + band, replacing the old dBm readout: the SIGNAL column
            // beside it is a strictly lossy function of the same e.rssi, so the
            // number added nothing at a glance. Deliberately TEXT_COLOR and not
            // a per-band hue — the word already says which band it is, and a
            // fourth protocol colour would compete with amber, the one signal
            // that has to stay unmistakable (amber == flock).
            const char* type_str = e.is_5ghz  ? "5GHz"
                                 : (e.proto == 1) ? "BLE"
                                                  : "2.4GHz";
            spr.setTextColor(ea(TEXT_COLOR), ea(row_bg));
            spr.setCursor(col_rssi, row_y + 3);
            spr.print(type_str);

            // SIGNAL (spelled out)
            const char* strength_str;
            uint16_t strength_col;
            if (e.rssi > -60)      { strength_str = "STRONG"; strength_col = ACCENT_COLOR; }
            else if (e.rssi > -80) { strength_str = "MEDIUM"; strength_col = CAUTION_COLOR; }
            else                   { strength_str = "WEAK";   strength_col = DIM_COLOR; }
            spr.setTextColor(ea(strength_col), ea(row_bg));
            spr.setCursor(col_sig, row_y + 3);
            spr.print(strength_str);

            rendered++;
        }
        spr.clearClipRect();
    }

    // Footer hint
    spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(UI_PAD_SM, SPR_H - 10);
    spr.print("arrows select  t target  f close");

}

// ============================================================================
// DETECTIONS SCREEN (screen 2)
// ============================================================================
void draw_capture_history_screen() {
    unsigned long now = millis();

    // Smooth selection cursor
    float dt_f = (hist_last_frame_ms == 0) ? 16.0f : (float)(now - hist_last_frame_ms);
    if (dt_f > 200.0f) dt_f = 200.0f;
    hist_last_frame_ms = now;

    int hist_total = sd_available ? sd_hist_count : capture_history_count;

    int target_y = (history_selected_idx - history_scroll_offset) * HIST_ROW_H;
    hist_sel_y_f = anim_filter(hist_sel_y_f, (float)target_y, HIST_SEL_TC, dt_f);

    spr.fillSprite(BG_COLOR);

    if (hist_detail_open && hist_total > 0) {
        // ── Detail overlay — refined layout ───────────────────────────────
        int idx = history_selected_idx;

        // Pull fields from sd_hist or capture_history
        const char* d_type      = "";
        const char* d_mac       = "";
        const char* d_name      = "";
        const char* d_method    = "";
        const char* d_timestamp = "";
        const char* d_datestamp = "--/--/--";
        int          d_rssi     = 0;
        int          d_conf     = 0;
        int          d_id       = 0;
        double       d_lat      = 0.0, d_lng = 0.0;

        if (sd_available && idx < sd_hist_count) {
            SDHistEntry& e = sd_hist[idx];
            d_type = e.type; d_mac = e.mac; d_name = e.name;
            d_method = e.method; d_timestamp = e.timestamp;
            d_datestamp = e.datestamp;
            d_rssi = e.rssi; d_conf = e.confidence; d_id = e.id;
            d_lat = e.lat; d_lng = e.lng;
        } else if (idx < capture_history_count) {
            CaptureEntry& e = capture_history[idx];
            d_type = e.type; d_mac = e.mac; d_name = e.name;
            d_timestamp = e.time;
            d_rssi = e.rssi; d_conf = e.confidence; d_id = e.id;
            d_lat = e.lat; d_lng = e.lng;
        }

        bool is_wifi = (strstr(d_type, "WIFI") != NULL || strstr(d_type, "WiFi") != NULL);
        bool is_flock = (strstr(d_type, "FLOCK") != NULL || strstr(d_type, "RAVEN") != NULL);

        // Protocol symbol color: amber for confirmed flock, protocol color otherwise
        uint16_t proto_col = is_flock ? CAUTION_COLOR
                           : (is_wifi ? HEADER_COLOR : PURPLE_COLOR);

        // Confidence color and label
        uint16_t conf_col;
        const char* conf_label_str;
        if      (d_conf >= CONFIDENCE_CERTAIN)         { conf_col = HEADER_COLOR;  conf_label_str = "CERTAIN"; }
        else if (d_conf >= CONFIDENCE_HIGH)            { conf_col = CAUTION_COLOR; conf_label_str = "HIGH"; }
        else if (d_conf >= CONFIDENCE_ALARM_THRESHOLD) { conf_col = CAUTION_COLOR; conf_label_str = "MEDIUM"; }
        else                                           { conf_col = DIM_COLOR;     conf_label_str = "LOW"; }

        const int LBL_X  = 8;     // label column x
        const int VAL_X  = 50;    // value column x — aligns all data
        const int ROW_H  = 14;    // matches scanner feed row height

        // ── Row 1: [symbol] Device Name ........... [conf bar] LABEL ──
        int ry = 3;

        // Protocol symbol: triangle (WiFi) or diamond (BLE)
        if (is_wifi) {
            spr.drawTriangle(12, ry, 12 + 8, ry + 7, 12 + 4, ry - 1, proto_col);
        } else {
            int dcx = 12 + 4, dcy = ry + 4, dhr = 4;
            spr.drawLine(dcx,       dcy - dhr, dcx + dhr, dcy,       proto_col);
            spr.drawLine(dcx + dhr, dcy,       dcx,       dcy + dhr, proto_col);
            spr.drawLine(dcx,       dcy + dhr, dcx - dhr, dcy,       proto_col);
            spr.drawLine(dcx - dhr, dcy,       dcx,       dcy - dhr, proto_col);
        }

        // Device name — hero, white, right after symbol
        {
            bool name_ok = (d_name[0] != '\0'
                            && strcmp(d_name, "Hidden") != 0
                            && strcmp(d_name, "Unknown") != 0);
            char hero[20];
            if (name_ok) {
                safe_copy(hero, d_name, sizeof(hero));
            } else {
                // No useful name — show MAC tail or "Hidden"
                if (strlen(d_mac) > 8) {
                    safe_copy(hero, d_mac + 9, sizeof(hero));
                } else {
                    safe_copy(hero, d_name[0] ? d_name : d_mac, sizeof(hero));
                }
            }
            // Truncate to fit before the confidence bar
            int max_hero_chars = 18;  // leaves room for conf bar on right
            if ((int)strlen(hero) > max_hero_chars) hero[max_hero_chars] = '\0';

            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setTextSize(TS_BODY);
            spr.setCursor(22, ry + 1);
            spr.print(hero);
        }

        // Confidence bar + label — right-aligned
        {
            int conf_label_w = (int)strlen(conf_label_str) * ts_char_w(TS_MICRO);
            int conf_label_x = DISP_W - 6 - conf_label_w;
            int conf_bar_w   = 44;
            int conf_bar_x   = conf_label_x - conf_bar_w - 4;
            int conf_bar_y   = ry + 2;

            // Bar background
            spr.fillRect(conf_bar_x, conf_bar_y, conf_bar_w, 5, CARD_COLOR);
            // Bar fill
            int fill_w = (int)((float)conf_bar_w * (float)d_conf / 100.0f);
            if (fill_w > conf_bar_w) fill_w = conf_bar_w;
            spr.fillRect(conf_bar_x, conf_bar_y, fill_w, 5, conf_col);

            // Label
            spr.setTextColor(conf_col, BG_COLOR);
            spr.setTextSize(TS_MICRO);
            spr.setCursor(conf_label_x, ry + 1);
            spr.print(conf_label_str);
        }

        // ── Separator ──
        ry += 15;
        spr.drawFastHLine(LBL_X, ry, DISP_W - LBL_X * 2, CARD_BORDER);
        ry += 7;

        // ── Row 2: MAC (left, white) .................. #ID (right, white) ──
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(LBL_X, ry);
        spr.print(d_mac);

        if (d_id > 0) {
            char id_str[8];
            snprintf(id_str, sizeof(id_str), "#%03d", d_id);
            int id_w = (int)strlen(id_str) * ts_char_w(TS_MICRO);
            spr.setCursor(DISP_W - LBL_X - id_w, ry);
            spr.print(id_str);
        }
        ry += ROW_H;

        // ── Row 3: SIG — dBm + strength label ──
        spr.setTextColor(HEADER_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(LBL_X, ry);
        kprint(spr, "SIG");

        {
            char rssi_str[10];
            snprintf(rssi_str, sizeof(rssi_str), "%ddBm", d_rssi);
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setCursor(VAL_X, ry);
            spr.print(rssi_str);

            // Strength label — colored by tier
            const char* sig_str;
            uint16_t sig_col;
            if      (d_rssi > -60) { sig_str = "STRONG"; sig_col = HEADER_COLOR; }
            else if (d_rssi > -80) { sig_str = "MED";    sig_col = CAUTION_COLOR; }
            else                   { sig_str = "WEAK";   sig_col = TEXT_COLOR; }

            int rssi_w = (int)strlen(rssi_str) * ts_char_w(TS_MICRO);
            spr.setTextColor(sig_col, BG_COLOR);
            spr.setCursor(VAL_X + rssi_w + 6, ry);
            spr.print(sig_str);
        }
        ry += ROW_H;

        // ── Row 4: MATCH — detection method ──
        spr.setTextColor(HEADER_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(LBL_X, ry);
        kprint(spr, "MATCH");

        {
            char human[48];
            methods_to_human(d_method, human, sizeof(human));
            // Truncate to fit screen width
            int max_match = (DISP_W - VAL_X - LBL_X) / ts_char_w(TS_MICRO);
            if ((int)strlen(human) > max_match) human[max_match] = '\0';

            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setCursor(VAL_X, ry);
            spr.print(human);
        }
        ry += ROW_H;

        // ── Row 5: TIME — timestamp + date ──
        spr.setTextColor(HEADER_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(LBL_X, ry);
        kprint(spr, "TIME");

        {
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setCursor(VAL_X, ry);
            spr.print(d_timestamp);

            // Date after timestamp with a gap
            int ts_w = (int)strlen(d_timestamp) * ts_char_w(TS_MICRO);
            spr.setCursor(VAL_X + ts_w + 8, ry);
            spr.print(d_datestamp);
        }
        ry += ROW_H;

        // ── Row 6: GPS — coordinates ──
        spr.setTextColor(HEADER_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(LBL_X, ry);
        kprint(spr, "GPS");

        {
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setCursor(VAL_X, ry);
            if (d_lat != 0.0 || d_lng != 0.0) {
                char gps_str[24];
                snprintf(gps_str, sizeof(gps_str), "%.4f, %.4f", d_lat, d_lng);
                spr.print(gps_str);
            } else {
                spr.print("No GPS fix");
            }
        }

        // ── Footer ──
        spr.setTextSize(TS_MICRO);
        spr.setCursor(LBL_X, SPR_H - 10);
        if (hist_delete_confirming) {
            spr.setTextColor(CAUTION_COLOR, BG_COLOR);
            spr.print("DELETE? ENT/d yes  DEL cancel");
        } else {
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.print("d del  t track  w wl  DEL close");
        }

    } else {
        // ── List view ─────────────────────────────────────────────────────
        if (hist_total == 0) {
            // Centred both axes, app typescale, white — measured with
            // textWidth/fontHeight rather than estimated from char counts.
            const char* msg = "No detections yet";
            spr.setTextSize(TS_BODY);
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            int mw = spr.textWidth(msg);
            int mh = spr.fontHeight();
            spr.setCursor((DISP_W - mw) / 2, (SPR_H - mh) / 2);
            spr.print(msg);
            return;
        }

        spr.setClipRect(0, 0, DISP_W, SPR_H);

        for (int i = 0; i < HIST_VISIBLE_ROWS + 1 && i + history_scroll_offset < hist_total; i++) {
            int real_idx = i + history_scroll_offset;
            int row_y = (real_idx - history_scroll_offset) * HIST_ROW_H;

            // Row background
            bool selected = (real_idx == history_selected_idx);
            uint16_t row_bg = selected ? lerp_col16(BG_COLOR, CARD_COLOR, 0.5f) : BG_COLOR;
            spr.fillRect(0, row_y, DISP_W, HIST_ROW_H - 1, row_bg);
            if (selected) {
                // Rows stay fixed; highlight bars use eased Y for smooth glide
                int sel_draw_y = (int)(hist_sel_y_f + 0.5f);
                spr.drawFastHLine(0, sel_draw_y,                  DISP_W, ACCENT_COLOR);
                spr.drawFastHLine(0, sel_draw_y + HIST_ROW_H - 2, DISP_W, ACCENT_COLOR);
            }

            const char* e_type = ""; const char* e_mac = ""; const char* e_name = "";
            int e_rssi = 0, e_conf = 0; bool e_is_flock = false;

            if (sd_available && real_idx < sd_hist_count) {
                SDHistEntry& e = sd_hist[real_idx];
                e_type = e.type; e_mac = e.mac; e_name = e.name;
                e_rssi = e.rssi; e_conf = e.confidence;
                e_is_flock = (strstr(e.type, "FLOCK") != NULL || strstr(e.type, "RAVEN") != NULL);
            } else if (real_idx < capture_history_count) {
                CaptureEntry& e = capture_history[real_idx];
                e_type = e.type; e_mac = e.mac; e_name = e.name;
                e_rssi = e.rssi; e_conf = e.confidence;
                e_is_flock = (strstr(e.type, "FLOCK") != NULL || strstr(e.type, "RAVEN") != NULL);
            }

            bool is_w = (strstr(e_type, "WIFI") != NULL || strstr(e_type, "WiFi") != NULL);
            uint16_t proto_col_r = is_w ? HEADER_COLOR : PURPLE_COLOR;
            if (e_is_flock) proto_col_r = CAUTION_COLOR;

            // Confidence bar (left edge)
            int bar_h = (int)((float)(HIST_ROW_H - 4) * (float)e_conf / 100.0f);
            spr.fillRect(0, row_y + 2, 3, bar_h, proto_col_r);

            // Name
            spr.setTextColor(proto_col_r, row_bg);
            spr.setTextSize(TS_BODY);
            spr.setCursor(6, row_y + 5);
            char nd[20]; safe_copy(nd, e_name[0] ? e_name : e_mac, sizeof(nd));
            spr.print(nd);

            // RSSI right-aligned
            char rssi_str[8];
            snprintf(rssi_str, sizeof(rssi_str), "%d", e_rssi);
            int rssi_w = (int)strlen(rssi_str) * ts_char_w(TS_MICRO);
            spr.setTextColor(DIM_COLOR, row_bg);
            spr.setTextSize(TS_MICRO);
            spr.setCursor(DISP_W - rssi_w - 4, row_y + 7);
            spr.print(rssi_str);

            // Type label
            spr.setTextColor(DIM_COLOR, row_bg);
            spr.setTextSize(TS_MICRO);
            spr.setCursor(6, row_y + 17);
            // Shorten type string: strip "FLOCK_" prefix if present
            const char* type_disp = e_type;
            if (strncmp(type_disp, "FLOCK_", 6) == 0) type_disp += 6;
            char td[14]; safe_copy(td, type_disp, sizeof(td));
            spr.print(td);
        }

        spr.clearClipRect();

        // Scroll indicator
        if (hist_total > HIST_VISIBLE_ROWS) {
            int bar_total = SPR_H - 4;
            int bar_h = bar_total * HIST_VISIBLE_ROWS / hist_total;
            if (bar_h < 6) bar_h = 6;
            int bar_y = 2 + (bar_total - bar_h)
                        * history_scroll_offset / max(1, hist_total - HIST_VISIBLE_ROWS);
            spr.fillRect(DISP_W - 2, bar_y, 2, bar_h, DIM_COLOR);
        }
    }
}


// ── Signal screen ──────────────────────────────────────────────────────────
// Answers one question: is this thing getting stronger or weaker, and how close
// have I ever gotten? Deliberately no distance readout — RSSI is relative, so
// any foot/metre figure would be a lie. Everything here is dBm, percent of
// scale, or elapsed time.
//
// Coordinates below are sprite-local. The sprite starts at CONTENT_Y, so a
// screen-space y from the design spec maps to (y - CONTENT_Y) here.
void draw_signal_screen() {
    unsigned long frame_ms = millis();

    // ── Snapshot state under mutex ──
    bool  active;
    char  target_mac[18], target_name[65], target_type[16];
    int   peak_rssi, tracker_idx;
    unsigned long newest_ms;
    int   target_rssi = 0;
    bool  has_rssi    = false;
    static SigTraceEntry trace_snap[SIG_TRACE_SIZE];
    int  trace_head, trace_count;
    long peak_seq, trace_total;

    if (!take_data_mutex()) return;
    active       = signal_active;
    safe_copy(target_mac,  signal_target_mac,  sizeof(target_mac));
    safe_copy(target_name, signal_target_name, sizeof(target_name));
    safe_copy(target_type, signal_target_type, sizeof(target_type));
    peak_rssi    = signal_peak_rssi;
    tracker_idx  = signal_tracker_idx;
    newest_ms    = signal_newest_sample_ms;
    if (tracker_idx >= 0 && tracker_idx < rssi_tracker_count
        && rssi_tracker[tracker_idx].sample_count > 0
        && strncmp(rssi_tracker[tracker_idx].mac, target_mac, 17) == 0) {
        int sc = rssi_tracker[tracker_idx].sample_count;
        target_rssi = rssi_tracker[tracker_idx].samples[sc - 1];
        has_rssi    = true;
    }
    memcpy(trace_snap, sig_trace, sizeof(sig_trace));
    trace_head  = sig_trace_head;
    trace_count = sig_trace_count;
    peak_seq    = signal_peak_seq;
    trace_total = sig_trace_total;
    give_data_mutex();

    // ── Per-frame dt for the meter ease ──
    float frame_dt = (sig_trace_last_frame_ms == 0) ? 16.0f
                   : (float)(frame_ms - sig_trace_last_frame_ms);
    if (frame_dt > 100.0f) frame_dt = 100.0f;
    sig_trace_last_frame_ms = frame_ms;

    spr.fillSprite(BG_COLOR);

    // ── Geometry, spec screen-space converted to sprite-space ──
    const int SX          = 6;                    // left margin — fuller bleed than TEXT_LEFT
    const int RX          = DISP_W - 6;           // right edge for right-anchored runs
    const int PLOT_W      = RX - SX;              // 228
    const int PLOT_TOP    = 106 - CONTENT_Y;      // 86
    const int PLOT_BOTTOM = 130 - CONTENT_Y;      // 110
    const int PLOT_H      = PLOT_BOTTOM - PLOT_TOP;

    // Size-aware letter-spaced text. kprint() hardcodes a 6px advance, which is
    // only correct at size 1; this screen mixes five sizes.
    auto text_w = [](const char* t, float sz, int extra) -> int {
        int n = (int)strlen(t);
        return n > 0 ? n * (ts_char_w(sz) + extra) - extra : 0;
    };
    auto text_at = [&](int x, int y, const char* t, float sz, int extra) {
        spr.setTextSize(sz);
        int adv = ts_char_w(sz) + extra;
        for (const char* p = t; *p; p++) {
            char ch[2] = {*p, '\0'};
            spr.setCursor(x, y);
            spr.print(ch);
            x += adv;
        }
    };

    // ── No-target state ──
    if (!active) {
        const char* head = "NEED TARGET";
        const char* hint = "press F for feed, pick a device, then T";
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        text_at((DISP_W - text_w(head, 2.0f, 1)) / 2, 52 - CONTENT_Y, head, 2.0f, 1);
        // Plain advance, not letter-spaced: at 38 characters the hint only just
        // fits the panel as it is.
        text_at((DISP_W - text_w(hint, TS_MICRO, 0)) / 2, 74 - CONTENT_Y, hint, TS_MICRO, 0);
        spr.drawFastHLine(SX, PLOT_BOTTOM, PLOT_W, CARD_BORDER);
        return;
    }

    // ── Freshness ──
    // LOST is the shorter gate: the live elements go quiet fast, the labelling
    // waits longer before it starts shouting.
    bool ever_heard = (newest_ms != 0);
    int  age_s      = ever_heard ? (int)((frame_ms - newest_ms) / 1000UL) : 0;
    bool is_lost    = !ever_heard || age_s >= SIG_LOST_AFTER_S;
    bool is_stale   = !ever_heard || age_s >= SIG_STALE_AFTER_S;

    bool is_ble   = (strstr(target_type, "BLE") != nullptr);
    bool is_flock = (strncmp(target_type, "FLOCK", 5) == 0
                  || strncmp(target_type, "RAVEN", 5) == 0);

    uint16_t accent = is_stale ? CAUTION_COLOR : HEADER_COLOR;

    // ── Status pill (right-anchored, drawn first so the name can be bounded) ──
    const char* pill_text = is_stale ? "STALE" : (is_flock ? "HUNTING" : "TRACKING");
    uint16_t    pill_col  = (is_stale || is_flock) ? CAUTION_COLOR : HEADER_COLOR;
    int pill_w = text_w(pill_text, TS_MICRO, 1) + 10;
    int pill_x = RX - pill_w;
    int pill_y = 29 - CONTENT_Y;
    spr.fillRoundRect(pill_x, pill_y, pill_w, 11, 3, pill_col);
    spr.setTextColor(BG_COLOR, pill_col);
    text_at(pill_x + 5, pill_y + 2, pill_text, TS_MICRO, 1);

    // ── Protocol glyph — identity, so it never takes the stale colour ──
    const int gx = SX, gy = 28 - CONTENT_Y;
    {
        uint16_t gc = is_ble ? PURPLE_COLOR : HEADER_COLOR;
        if (is_ble) {
            const int ox[4] = {5, 10, 5, 0}, oy[4] = {0, 5, 10, 5};
            const int ix[4] = {5,  9, 5, 1}, iy[4] = {1, 5,  9, 5};
            for (int i = 0; i < 4; i++) {
                int j = (i + 1) & 3;
                spr.drawLine(gx + ox[i], gy + oy[i], gx + ox[j], gy + oy[j], gc);
                spr.drawLine(gx + ix[i], gy + iy[i], gx + ix[j], gy + iy[j], gc);
            }
        } else {
            const int ox[3] = {5, 10, 0}, oy[3] = {0, 9, 9};
            const int ix[3] = {5,  8, 2}, iy[3] = {2, 8, 8};
            for (int i = 0; i < 3; i++) {
                int j = (i + 1) % 3;
                spr.drawLine(gx + ox[i], gy + oy[i], gx + ox[j], gy + oy[j], gc);
                spr.drawLine(gx + ix[i], gy + iy[i], gx + ix[j], gy + iy[j], gc);
            }
        }
    }

    // ── Device name — truncated, no ellipsis, stops 6px short of the pill ──
    {
        bool nok = target_name[0] != '\0'
                   && strcmp(target_name, "Hidden")  != 0
                   && strcmp(target_name, "Unknown") != 0;
        const char* raw = nok ? target_name
                        : ((strlen(target_mac) > 8) ? target_mac + 9 : target_mac);
        int name_x = gx + 15;
        int avail  = (pill_x - 6) - name_x;
        int maxc   = (avail > 0) ? avail / ts_char_w(2.0f) : 0;
        if (maxc > 64) maxc = 64;
        char disp[65];
        strncpy(disp, raw, maxc);
        disp[maxc] = '\0';
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setTextSize(2.0f);
        spr.setCursor(name_x, gy - 3);   // optical centre against the 10px glyph
        spr.print(disp);
    }

    // ── Last heard (right-anchored) — computed before the hero so the "(peak)"
    //    tag can bow out rather than collide with it. ──
    char seen[20];
    if (!ever_heard)      safe_copy(seen, "NOT HEARD", sizeof(seen));
    else if (age_s <= 99) snprintf(seen, sizeof(seen), "SEEN %ds AGO", age_s);
    else {
        int m = age_s / 60;
        if (m > 99) m = 99;
        snprintf(seen, sizeof(seen), "SEEN %dm AGO", m);
    }
    int seen_x = RX - text_w(seen, TS_BODY, 0);
    spr.setTextColor(is_stale ? CAUTION_COLOR : TEXT_COLOR, BG_COLOR);
    text_at(seen_x, 58 - CONTENT_Y, seen, TS_BODY, 0);

    // ── Peak hero — the reading, so white in every state ──
    {
        const int hero_y = 50 - CONTENT_Y;
        const int hero_b = hero_y + 24;          // size-3 cell height
        char pk[8];
        if (peak_rssi > -120) snprintf(pk, sizeof(pk), "%d", peak_rssi);
        else                  safe_copy(pk, "--", sizeof(pk));

        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setTextSize(3.0f);
        spr.setCursor(SX, hero_y);
        spr.print(pk);

        int x = SX + (int)strlen(pk) * ts_char_w(3.0f) + 5;
        spr.setTextSize(TS_BODY);
        spr.setCursor(x, hero_b - 10);
        spr.print("dBm");

        x += ts_char_w(TS_BODY) * 3 + 7;
        if (x + text_w("(peak)", TS_MICRO, 1) <= seen_x - 6)
            text_at(x, hero_b - 8, "(peak)", TS_MICRO, 1);
    }

    // ── Peak-hold meter ──
    {
        const int mt_y = 83 - CONTENT_Y;
        spr.fillRoundRect(SX, mt_y, PLOT_W, 6, 3, CARD_COLOR);

        if (!is_lost && has_rssi) {
            float lv = (float)(target_rssi - RSSI_VIS_FLOOR) / (float)RSSI_METER_RANGE;
            if (lv < 0.0f) lv = 0.0f;
            if (lv > 1.0f) lv = 1.0f;
            // 140ms constant ≈ the spec's 320ms settle at ~90%.
            signal_bar_smooth = anim_filter_seed(signal_bar_smooth, lv, 140.0f,
                                                 frame_dt, &signal_bar_seeded);
            int fw = (int)(signal_bar_smooth * (float)PLOT_W + 0.5f);
            if (fw > 0) {
                int fr = (fw / 2 < 3) ? fw / 2 : 3;
                spr.fillRoundRect(SX, mt_y, fw, 6, fr, accent);
            }
        } else {
            signal_bar_smooth = 0.0f;
            signal_bar_seeded = false;
        }

        // Peak hold: snaps on contact and never falls back while the target is
        // held — smoothing this would misrepresent what peak detection means.
        if (peak_rssi > -120) {
            float pv = (float)(peak_rssi - RSSI_VIS_FLOOR) / (float)RSSI_METER_RANGE;
            if (pv < 0.0f) pv = 0.0f;
            if (pv > 1.0f) pv = 1.0f;
            int hx = SX + (int)(pv * (float)PLOT_W + 0.5f);
            if (hx > RX - 2) hx = RX - 2;
            spr.fillRect(hx, 79 - CONTENT_Y, 2, 14, TEXT_COLOR);
        }
    }

    // ── Trace ──
    spr.drawFastHLine(SX, PLOT_BOTTOM, PLOT_W, CARD_BORDER);

    if (trace_count >= 1) {
        static int xs[SIG_TRACE_SIZE], ys[SIG_TRACE_SIZE];
        const int n = trace_count;

        // The x axis scales to the samples we actually have and pins at two
        // minutes once the ring is full, so a fresh target plots full width
        // instead of a stub in an empty box.
        for (int i = 0; i < n; i++) {
            int slot = (trace_head - n + i + SIG_TRACE_SIZE * 2) % SIG_TRACE_SIZE;
            int dbm  = (int)trace_snap[slot].rssi;
            int y    = PLOT_BOTTOM - (int)((float)PLOT_H * (float)(dbm - RSSI_VIS_FLOOR)
                                           / (float)RSSI_TRACE_RANGE + 0.5f);
            if (y < PLOT_TOP)    y = PLOT_TOP;
            if (y > PLOT_BOTTOM) y = PLOT_BOTTOM;
            ys[i] = y;
            xs[i] = (n == 1) ? RX : SX + (PLOT_W * i) / (n - 1);
        }

        // Clip top reaches above the plot band to admit the peak tick, and the
        // sides run 3px wide of the plot so the dots sitting on the first and
        // last vertices are not sliced in half.
        spr.setClipRect(SX - 3, 104 - CONTENT_Y, PLOT_W + 7,
                        PLOT_BOTTOM - (104 - CONTENT_Y) + 1);

        if (n >= 2) {
            uint16_t fill_col = lerp_col16(BG_COLOR, accent, 0.14f);
            for (int px = 0; px <= PLOT_W; px++) {
                float t  = (float)px * (float)(n - 1) / (float)PLOT_W;
                int   i0 = (int)t;
                if (i0 > n - 2) i0 = n - 2;
                int y = (int)((float)ys[i0]
                              + (float)(ys[i0 + 1] - ys[i0]) * (t - (float)i0) + 0.5f);
                if (y < PLOT_BOTTOM)
                    spr.drawFastVLine(SX + px, y + 1, PLOT_BOTTOM - y - 1, fill_col);
            }
            for (int i = 0; i < n - 1; i++) {
                spr.drawLine(xs[i], ys[i],     xs[i + 1], ys[i + 1],     accent);
                spr.drawLine(xs[i], ys[i] + 1, xs[i + 1], ys[i + 1] + 1, accent);
            }
        }

        // Cold start: make a five-point plot read as deliberate, not broken.
        if (n < 8)
            for (int i = 0; i < n; i++) spr.fillCircle(xs[i], ys[i], 2, accent);

        // Peak tick. Dropped once the peak scrolls out of the window rather than
        // re-pointed at the in-window maximum — that would contradict the hero.
        if (peak_seq >= 0 && peak_seq >= trace_total - n) {
            int pi = (int)(peak_seq - (trace_total - n));
            if (pi >= 0 && pi < n)
                spr.drawFastVLine(xs[pi], 104 - CONTENT_Y,
                                  PLOT_BOTTOM - (104 - CONTENT_Y) + 1, TEXT_COLOR);
        }

        // No current dot once the signal is lost — the trace is on the floor and
        // there is nothing current to mark.
        if (!is_lost) spr.fillCircle(xs[n - 1], ys[n - 1], 2, TEXT_COLOR);

        spr.clearClipRect();
    }
}

void draw_gps_screen() {
    spr.fillSprite(BG_COLOR);
    unsigned long frame_ms = millis();

    bool has_loc, stale;
    int sats;
    double d_lat, d_lng;
    float f_hdop;
    bool hdop_valid, time_valid;
    int gps_hour, gps_min, gps_sec;

    if (!take_data_mutex()) return;
    has_loc     = gps.location.isValid();
    stale       = has_loc && (gps.location.age() > 2000);
    sats        = gps.satellites.isValid() ? gps.satellites.value() : 0;
    d_lat       = gps.location.lat();
    d_lng       = gps.location.lng();
    hdop_valid  = gps.hdop.isValid();
    f_hdop      = hdop_valid ? gps.hdop.hdop() : 0.0f;
    time_valid  = gps.time.isValid();
    gps_hour    = time_valid ? gps.time.hour()   : 0;
    gps_min     = time_valid ? gps.time.minute() : 0;
    gps_sec     = time_valid ? gps.time.second() : 0;
    give_data_mutex();

    // ── Off-axis 3D wireframe globe ──────────────────────────────────────────
    // Solid BG fill, diagonal axis tilt like a real globe on a stand
    const int gx = 55, gy = 52, gr = 30;  // gy=52 in sprite-space (screen Y=72)

    // TILT: X-axis — north pole backward; ROLL: Z-axis — axis diagonal upper-left→lower-right
    const float TILT  = -0.30f;
    const float ROLL  =  0.45f;
    float rot = fmodf((float)frame_ms / 8000.0f, 1.0f) * 2.0f * (float)M_PI;

    float sr = sinf(rot),  cr = cosf(rot);
    float st = sinf(TILT), ct = cosf(TILT);
    float sroll = sinf(ROLL), croll = cosf(ROLL);

    // Project a sphere point → screen (px, py); return z-depth [-1..1]
    // Takes longitude as a precomputed cos/sin pair rather than an angle. The
    // meridian loop calls this 48 times with the same longitude, so computing
    // the trig here meant 1,152 sinf/cosf per frame producing 24 distinct
    // values. Callers now hoist it to whatever scope the longitude is constant
    // over.
    auto proj = [&](float clat, float slat, float clon, float slon, int* px, int* py) -> float {
        float sx = clat * clon;
        float sy = slat;
        float sz = clat * slon;
        float rx =  sx * cr - sz * sr;   // Y-spin
        float ry =  sy;
        float rz =  sx * sr + sz * cr;
        float tx =  rx;
        float ty =  ry * ct - rz * st;   // X-tilt
        float tz =  ry * st + rz * ct;
        float ux = tx * croll - ty * sroll;  // Z-roll (diagonal axis)
        float uy = tx * sroll + ty * croll;
        *px = gx + (int)(gr * ux);
        *py = gy - (int)(gr * uy);
        return tz;
    };

    // ─ Background starfield ──────────────────────────────────────────────
    // Single-pixel dots with a gentle per-star twinkle. Static positions,
    // generated once with UI_PAD_MD clearance from the globe rim and bound
    // to the left panel so they never reach the right-side text column.
    {
        #define NUM_STARS 18
        static int  star_x[NUM_STARS], star_y[NUM_STARS];
        static bool stars_init = false;
        if (!stars_init) {
            const int min_x = UI_PAD_SM / 2;
            const int max_x = gx * 2 - UI_PAD_MD;  // stop well before right panel
            const int min_y = UI_PAD_SM;
            const int max_y = SPR_H - UI_PAD_SM;
            const int clear = gr + UI_PAD_MD;
            for (int i = 0; i < NUM_STARS; i++) {
                int sx, sy;
                do {
                    sx = random(min_x, max_x);
                    sy = random(min_y, max_y);
                } while ((sx - gx) * (sx - gx) + (sy - gy) * (sy - gy) <
                         clear * clear);
                star_x[i] = sx;
                star_y[i] = sy;
            }
            stars_init = true;
        }
        for (int i = 0; i < NUM_STARS; i++) {
            float twinkle = anim_pulse(1600 + i * 280, (float)i / (float)NUM_STARS);
            uint8_t b = (uint8_t)(30 + twinkle * 225);
            uint16_t star_col = lgfx::color565(b, b, b);
            spr.drawPixel(star_x[i], star_y[i], star_col);
        }
    }

    // Solid BG fill so globe interior matches screen background
    // (also erases any starfield pixel that would land inside the rim).
    spr.fillCircle(gx, gy, gr, BG_COLOR);

    // ─ Latitude circles (every 30°, 5 lines) ─
    const float lats[] = { -60.0f, -30.0f, 0.0f, 30.0f, 60.0f };
    for (int li = 0; li < 5; li++) {
        float lat_r = radians(lats[li]);
        float clat = cosf(lat_r), slat = sinf(lat_r);
        bool  is_eq = (li == 2);
        const int STEPS = 72;
        int px0, py0, px1, py1;
        float pz0 = proj(clat, slat, 1.0f, 0.0f, &px0, &py0);   // cos 0, sin 0
        for (int s = 1; s <= STEPS; s++) {
            float lon = (float)s / STEPS * 2.0f * (float)M_PI;
            // Longitude genuinely varies per step here, so the trig stays in the
            // loop — it just moved out of proj() to the call site.
            float clon = cosf(lon), slon = sinf(lon);
            float pz1 = proj(clat, slat, clon, slon, &px1, &py1);
            float avg_z = (pz0 + pz1) * 0.5f;
            if (avg_z > 0.0f) {  // cull back-facing segments
                float brt = avg_z * 0.45f + 0.55f;
                uint16_t base = is_eq ? GPS_COLOR : HEADER_COLOR;
                spr.drawLine(px0, py0, px1, py1, lerp_col16(DIM_COLOR, base, brt));
            }
            px0 = px1; py0 = py1; pz0 = pz1;
        }
    }

    // ─ Longitude lines (every 30°, 12 meridians) ─
    const int N_MER = 12, M_STEPS = 48;
    // Each meridian starts at the south pole, so this is the same latitude every
    // time — it was evaluating trig on a literal 12 times per frame.
    const float POLE_CLAT = cosf(-1.5707f);
    const float POLE_SLAT = sinf(-1.5707f);
    for (int m = 0; m < N_MER; m++) {
        float lon = (float)m / N_MER * 2.0f * (float)M_PI;
        // Longitude is constant down a meridian — once per meridian, not per step.
        float clon = cosf(lon), slon = sinf(lon);
        int px0, py0, px1, py1;
        float clat = POLE_CLAT, slat = POLE_SLAT;
        float pz0 = proj(clat, slat, clon, slon, &px0, &py0);
        for (int s = 1; s <= M_STEPS; s++) {
            float lat_r = -1.5707f + (float)s / M_STEPS * (float)M_PI;
            clat = cosf(lat_r); slat = sinf(lat_r);
            float pz1 = proj(clat, slat, clon, slon, &px1, &py1);
            float avg_z = (pz0 + pz1) * 0.5f;
            if (avg_z > 0.0f) {  // cull back-facing segments
                float brt = avg_z * 0.40f + 0.50f;
                spr.drawLine(px0, py0, px1, py1, lerp_col16(DIM_COLOR, HEADER_COLOR, brt));
            }
            px0 = px1; py0 = py1; pz0 = pz1;
        }
    }

    // Globe rim
    uint16_t rim_col = stale ? CAUTION_COLOR
                     : (has_loc ? GPS_COLOR : DIM_COLOR);
    spr.drawCircle(gx, gy, gr,     rim_col);
    spr.drawCircle(gx, gy, gr + 1, CARD_BORDER);

    // ── Multi-plane orbital satellite animation ──────────────────────────────
    // Skipped entirely when no GPS lock — the spinning globe alone reads as
    // "searching" without the visual noise of orbiting placeholders.
    if (sats > 0)
    {
        // 3 orbital planes: inclinations, radii, speeds, satellite counts
        struct OrbPlane {
            float inc;       // inclination (radians)
            float radius;    // screen pixels from globe center
            float speed;     // angular speed factor (applied to millis)
            int   n_sats;    // satellites in this plane
            float phase_off; // phase offset for plane
        };
        // Fewer satellites (5 total instead of 7), spread across 2 planes.
        // Cleaner visual, bigger triangles have room to breathe.
        const OrbPlane planes[2] = {
            { radians(55.0f), (float)(gr + 14), 0.00015f, 3, 0.0f },
            { radians(80.0f), (float)(gr + 10), 0.0002f,  2, 1.05f },
        };

        // Per-plane projection lambda — applies globe tilt/roll then inclination
        for (int pi = 0; pi < 2; pi++) {
            const OrbPlane& pl = planes[pi];
            float ci = cosf(pl.inc), si2 = sinf(pl.inc);

            auto orb_proj_p = [&](float ang, int* px, int* py) -> float {
                // Orbit in tilted plane
                float ox = cosf(ang);
                float oy = sinf(ang) * ci;
                float oz = sinf(ang) * si2;
                // Apply globe rotation + tilt + roll (same transforms as globe wireframe)
                float rx2 = ox * cr - oz * sr;
                float ry2 = oy;
                float rz2 = ox * sr + oz * cr;
                float tx2 = rx2;
                float ty2 = ry2 * ct - rz2 * st;
                float tz2 = ry2 * st + rz2 * ct;
                float ux  = tx2 * croll - ty2 * sroll;
                float uy  = tx2 * sroll + ty2 * croll;
                *px = gx + (int)(ux * pl.radius);
                *py = gy - (int)(uy * pl.radius);
                return tz2;
            };

            // Draw satellites as wireframe triangles tumbling around their
            // own center. No trails — the tumble + depth scaling carries
            // the motion. Acquired sats are brighter, unacquired dimmer.
            float orbit_t = (float)frame_ms * pl.speed + pl.phase_off;
            for (int si = 0; si < pl.n_sats; si++) {
                float base_ang = orbit_t + (float)si / (float)pl.n_sats * 2.0f * (float)M_PI;

                int dpx, dpy;
                float tz2 = orb_proj_p(base_ang, &dpx, &dpy);
                if (tz2 > -0.3f) {
                    float depth_fade = (tz2 > 0.3f) ? 1.0f : (tz2 + 0.3f) / 0.6f;
                    if (depth_fade < 0.0f) depth_fade = 0.0f;
                    if (depth_fade > 1.0f) depth_fade = 1.0f;
                    if (depth_fade < 0.05f) continue;

                    bool acquired = (si < sats);
                    uint16_t sat_col = acquired
                        ? lerp_col16(BG_COLOR, GPS_COLOR, depth_fade)
                        : lerp_col16(BG_COLOR, DIM_COLOR, depth_fade * 0.5f);

                    // Depth-based size scaling — front-facing sats larger.
                    float depth_scale = 0.5f + 0.5f * ((tz2 + 0.3f) / 1.3f);
                    if (depth_scale < 0.4f) depth_scale = 0.4f;
                    if (depth_scale > 1.0f) depth_scale = 1.0f;

                    // Wireframe tetrahedron (4 verts, 6 edges) tumbling on
                    // two axes with phases derived from base_ang so each
                    // satellite spins independently. Reads as a real 3D
                    // body in flight rather than a flat icon.
                    const float sz = 5.0f * depth_scale;
                    float tet_v[4][3] = {
                        {  0.0f,    -sz * 1.2f,  0.0f      },  // top
                        { -sz,       sz * 0.6f, -sz * 0.5f },  // base L-back
                        {  sz,       sz * 0.6f, -sz * 0.5f },  // base R-back
                        {  0.0f,     sz * 0.6f,  sz * 0.8f },  // base front
                    };

                    const float tumble_speed = 0.004f;
                    float t1 = (float)frame_ms * tumble_speed       + base_ang * 3.0f;
                    float t2 = (float)frame_ms * tumble_speed * 0.7f + base_ang * 2.0f;
                    float c1 = cosf(t1), s1 = sinf(t1);
                    float c2 = cosf(t2), s2 = sinf(t2);

                    int sx[4], sy[4];
                    for (int vi = 0; vi < 4; vi++) {
                        float vx = tet_v[vi][0];
                        float vy = tet_v[vi][1];
                        float vz = tet_v[vi][2];
                        // Y-axis roll
                        float rx = vx * c1 - vz * s1;
                        float ry = vy;
                        float rz = vx * s1 + vz * c1;
                        // X-axis pitch
                        float px = rx;
                        float py = ry * c2 - rz * s2;
                        sx[vi] = dpx + (int)px;
                        sy[vi] = dpy + (int)py;
                    }

                    // 6 edges: 0-1, 0-2, 0-3, 1-2, 2-3, 3-1
                    spr.drawLine(sx[0], sy[0], sx[1], sy[1], sat_col);
                    spr.drawLine(sx[0], sy[0], sx[2], sy[2], sat_col);
                    spr.drawLine(sx[0], sy[0], sx[3], sy[3], sat_col);
                    spr.drawLine(sx[1], sy[1], sx[2], sy[2], sat_col);
                    spr.drawLine(sx[2], sy[2], sx[3], sy[3], sat_col);
                    spr.drawLine(sx[3], sy[3], sx[1], sy[1], sat_col);
                }
            }
        }
    }

    // ── Right panel: STATUS badge + key-value readouts ──────────────────────
    const int RX = 122;
    const int RW = DISP_W - RX - 4;  // right margin 4px
    int ry = 5;

    // STATUS badge
    {
        const char* status_base;
        uint16_t    status_col;
        bool        status_anim = false;
        if      (low_power_mode)    { status_base = "DISABLED";      status_col = DIM_COLOR; }  // receiver in standby
        else if (has_loc && !stale) { status_base = "GPS LOCKED";    status_col = GPS_COLOR; }
        else if (stale)             { status_base = "REATTEMPTING";  status_col = CAUTION_COLOR; status_anim = true; }
        else                        { status_base = "Searching";     status_col = GPS_COLOR; status_anim = true; }
        char status_str[22];
        if (status_anim) {
            char dots[4];
            anim_ellipsis(dots, sizeof(dots));
            snprintf(status_str, sizeof(status_str), "%s%s", status_base, dots);
        } else {
            strncpy(status_str, status_base, sizeof(status_str) - 1);
            status_str[sizeof(status_str) - 1] = '\0';
        }
        int bw = (int)strlen(status_base) * ts_char_w(TS_BODY) + (status_anim ? 3 * ts_char_w(TS_BODY) : 0) + 12;
        if (bw > RW) bw = RW;
        uint16_t sfill = lerp_col16(BG_COLOR, status_col, 0.22f);
        spr.fillRoundRect(RX, ry, bw, 16, 5, sfill);
        spr.drawRoundRect(RX, ry, bw, 16, 5, status_col);
        spr.setTextColor(status_col, sfill); spr.setTextSize(TS_MICRO);
        spr.setClipRect(RX + 1, ry + 1, bw - 2, 14);
        spr.setCursor(RX + 6, ry + 4); kprint(spr, status_str);
        spr.clearClipRect();
    }

    // Key-value readout rows — label left, value right-justified
    ry += 22 + UI_PAD_SM;  // 6px standard gap between badge and data
    const int KV_RIGHT = DISP_W - 6;  // right edge for value text

    auto kv_row = [&](const char* label, const char* value, uint16_t val_col = TEXT_COLOR) {
        // Label (accent, left-aligned)
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setTextSize(TS_BODY);
        spr.setCursor(RX, ry);
        kprint(spr, label);

        // Value (right-aligned)
        int val_w = (int)strlen(value) * ts_char_w(TS_BODY);
        spr.setTextColor(val_col, BG_COLOR);
        spr.setTextSize(TS_BODY);
        spr.setCursor(KV_RIGHT - val_w, ry);
        spr.print(value);

        ry += 14;  // row height: 10px text + 4px gap
    };

    // LAT
    {
        char lat_buf[14];
        snprintf(lat_buf, sizeof(lat_buf), "%.4f", has_loc ? d_lat : 0.0);
        kv_row("LAT", lat_buf);
    }

    // LNG
    {
        char lng_buf[14];
        snprintf(lng_buf, sizeof(lng_buf), "%.4f", has_loc ? d_lng : 0.0);
        kv_row("LNG", lng_buf);
    }

    // SAT — count only (NEO-6M tracks up to 12 but the cap is implicit)
    {
        char sat_buf[8];
        snprintf(sat_buf, sizeof(sat_buf), "%d", sats);
        kv_row("SAT", sat_buf);
    }

    // HDOP — horizontal dilution of precision
    {
        char hdop_buf[8];
        snprintf(hdop_buf, sizeof(hdop_buf), "%.1f", f_hdop);
        uint16_t hdop_col = (hdop_valid && f_hdop <= 2.0f) ? TEXT_COLOR
                          : (hdop_valid && f_hdop <= 5.0f) ? CAUTION_COLOR
                          : DIM_COLOR;
        kv_row("HDOP", hdop_buf, hdop_col);
    }

    // TIME — local if timezone available, UTC otherwise
    {
        char time_buf[12];
        if (time_valid) {
            if (auto_tz_valid) {
                int local_hour = (gps_hour + auto_tz_offset + 24) % 24;
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                         local_hour, gps_min, gps_sec);
            } else {
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                         gps_hour, gps_min, gps_sec);
            }
        } else {
            strncpy(time_buf, "--:--:--", sizeof(time_buf));
        }
        kv_row(auto_tz_valid ? "LOCAL" : "UTC", time_buf, TEXT_COLOR);
    }
}

// Draw a single stat card. Outlined only (no fill).
// Label: TS_MICRO, DIM_COLOR, kprint with extra=2 letter-spacing — hugs top.
// Value: TS_STRONG by default, TEXT_COLOR — hugs bottom.
// prev_str / char_anim_ms (size STAT_MAX_CHARS each): per-character
//   change tracking. The function compares value[ci] against prev_str[ci]
//   and bumps char_anim_ms[ci] whenever a column's glyph changes, then
//   draws each glyph at its own y so only the digits that flipped roll
//   up — the rest stay perfectly still. Pass nullptr/nullptr to draw a
//   static card with no roll.
static void draw_stat_card(int x, int y, int w, int h,
                           const char* label, const char* value,
                           float value_size = TS_STRONG,
                           char* prev_str = nullptr,
                           unsigned long* char_anim_ms = nullptr) {
    spr.drawRoundRect(x, y, w, h, 4, HEADER_COLOR);

    // 8px inner inset (UI_PAD_SM + UI_PAD_XS) so glyphs aren't crowding
    // the 1px outline. Same value used for label and value.
    const int card_inset = UI_PAD_SM + UI_PAD_XS;

    spr.setTextColor(DIM_COLOR, BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(x + card_inset, y + UI_PAD_SM);
    kprint(spr, label, 2);

    int value_glyph_h  = (int)(value_size * 8.0f);
    int char_w         = ts_char_w(value_size);
    int value_target_y = y + h - value_glyph_h - UI_PAD_SM;
    int value_x        = x + card_inset;

    int len = (int)strlen(value);
    if (len > STAT_MAX_CHARS) len = STAT_MAX_CHARS;

    unsigned long now = millis();

    // Per-character change detection. Trailing slots compare against
    // prev_str's matching position so a shrinking value (e.g. "10" → "9")
    // also triggers a roll on the column that just dropped a digit.
    if (prev_str && char_anim_ms) {
        int prev_len = (int)strlen(prev_str);
        for (int ci = 0; ci < STAT_MAX_CHARS; ci++) {
            char curc  = (ci < len)      ? value[ci]    : '\0';
            char prevc = (ci < prev_len) ? prev_str[ci] : '\0';
            if (curc != prevc) {
                char_anim_ms[ci] = now;
            }
        }
        safe_copy(prev_str, value, STAT_MAX_CHARS);
    }

    int clip_x = x + 1;
    int clip_y = y + 1;
    int clip_right  = x + w - 1;
    int clip_bottom = (y + h - 1 < STATS_VIEW_H) ? (y + h - 1) : STATS_VIEW_H;
    int clip_w = clip_right - clip_x;
    int clip_h = clip_bottom - clip_y;
    if (clip_w > 0 && clip_h > 0) {
        spr.setClipRect(clip_x, clip_y, clip_w, clip_h);
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setTextSize(value_size);
        for (int ci = 0; ci < len; ci++) {
            int cx     = value_x + ci * char_w;
            int draw_y = value_target_y;
            if (char_anim_ms && char_anim_ms[ci] != 0) {
                unsigned long elapsed = now - char_anim_ms[ci];
                if (elapsed < UI_ANIM_QUICK) {
                    draw_y = anim_slide_in(value_target_y, value_glyph_h,
                                           char_anim_ms[ci], UI_ANIM_QUICK);
                }
            }
            char ch[2] = { value[ci], '\0' };
            spr.setCursor(cx, draw_y);
            spr.print(ch);
        }
        spr.clearClipRect();
    }
}


void draw_device_info_screen() {
    unsigned long frame_ms = millis();

    // Snapshot stats under mutex
    long lt, sr, sa, sw, sb, lb, lfw;
    unsigned long l_sec;
    if (!take_data_mutex()) return;
    lt    = lifetime_flock_total;
    sr    = session_raven;
    sa    = session_axon;
    sw    = session_flock_wifi;
    sb    = session_flock_ble;
    lb    = lifetime_boots;
    lfw   = lifetime_flash_writes;
    l_sec = lifetime_seconds;
    give_data_mutex();

    long session_det_total = sw + sb + sr + sa;

    int32_t  bat_mv      = get_filtered_voltage();
    size_t   free_heap   = esp_get_free_heap_size();
    uint64_t sd_bytes    = sd_available ? SD.cardSize() : 0;
    unsigned long sess_s = (frame_ms - session_start_time) / 1000;

    // PACKETS throttle — ambient_packet_count churns every frame, which would
    // strobe the roll-up animation. Only refresh the displayed value once per
    // STATS_PKT_REFRESH_MS. The first sample populates immediately so the
    // card isn't blank.
    if (stats_pkt_last_update == 0 ||
        frame_ms - stats_pkt_last_update >= STATS_PKT_REFRESH_MS) {
        stats_pkt_display     = ambient_packet_count;
        stats_pkt_last_update = frame_ms;
    }

    spr.fillSprite(BG_COLOR);

    // ── Format value strings ──
    char det_sess_str[10]; snprintf(det_sess_str, sizeof(det_sess_str), "%ld", session_det_total);
    char det_life_str[10]; snprintf(det_life_str, sizeof(det_life_str), "%ld", lt);
    char wifi_str[10];  snprintf(wifi_str,  sizeof(wifi_str),  "%ld", sw);
    char ble_str[10];   snprintf(ble_str,   sizeof(ble_str),   "%ld", sb);
    char raven_str[10]; snprintf(raven_str, sizeof(raven_str), "%ld", sr);
    char axon_str[10];  snprintf(axon_str,  sizeof(axon_str),  "%ld", sa);
    char sess_str[10];  format_time_buf(sess_s, sess_str, sizeof(sess_str));
    char life_str[10];  format_time_buf(l_sec, life_str, sizeof(life_str));
    int bat_pct = voltage_to_percent(bat_mv);
    // isCharging() returns 2 (unknown) on this hardware — no charge-status
    // line — so only a definite 1 may show the "+" (see get_filtered_voltage).
    // Truthy-testing it painted a permanent "+" next to a falling percent.
    bool charging = (M5Cardputer.Power.isCharging() == 1);
    char volt_str[10];
    if (charging) {
        snprintf(volt_str, sizeof(volt_str), "%d%%+", bat_pct);
    } else {
        snprintf(volt_str, sizeof(volt_str), "%d%%", bat_pct);
    }
    char heap_str[10];  snprintf(heap_str,  sizeof(heap_str),  "%uKB", (unsigned)(free_heap / 1024));
    char pkt_str[12];   snprintf(pkt_str,   sizeof(pkt_str),   "%lu",  (unsigned long)stats_pkt_display);
    char sd_str[10];
    if (sd_available && sd_bytes > 0) {
        snprintf(sd_str, sizeof(sd_str), "%.1fGB", (double)sd_bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        strncpy(sd_str, "--", sizeof(sd_str));
    }
    char boots_str[10];   snprintf(boots_str,   sizeof(boots_str),   "%ld",   lb);
    char flash_str[10];   snprintf(flash_str,   sizeof(flash_str),   "%ld",   lfw);
    char voltage_str[10]; snprintf(voltage_str, sizeof(voltage_str), "%.2fV", bat_mv / 1000.0f);
    // V CHANGE: cumulative drift since boot (negative = net loss). Falls back
    // to mV until the magnitude crosses a volt, which never fits the cell's
    // real range anyway.
    char vdelta_str[10];
    int32_t dv = (session_start_mv > 0) ? (get_unsagged_voltage() - session_start_mv) : 0;
    if (dv <= -1000 || dv >= 1000)
        snprintf(vdelta_str, sizeof(vdelta_str), "%+.2fV", dv / 1000.0f);
    else
        snprintf(vdelta_str, sizeof(vdelta_str), "%+dmV", (int)dv);

    // First-frame seed for per-character roll animation. Copy the freshly
    // formatted strings into stats_prev_strings without stamping any
    // char_anim slots, so the initial render doesn't fire 13 simultaneous
    // rolls. Subsequent frames let draw_stat_card do the per-glyph
    // comparison and timestamp bumping.
    if (!stats_values_initialized) {
        const char* seed[STATS_CARD_COUNT];
        seed[SC_DET_SESSION] = det_sess_str;
        seed[SC_DET_LIFETIME] = det_life_str;
        seed[SC_WIFI]       = wifi_str;
        seed[SC_BLE]        = ble_str;
        seed[SC_RAVEN]      = raven_str;
        seed[SC_AXON]       = axon_str;
        seed[SC_SESSION]    = sess_str;
        seed[SC_LIFETIME]   = life_str;
        seed[SC_BATTERY]    = volt_str;
        seed[SC_HEAP]       = heap_str;
        seed[SC_PACKETS]    = pkt_str;
        seed[SC_SD]         = sd_str;
        seed[SC_BOOTS]      = boots_str;
        seed[SC_FLASH]      = flash_str;
        seed[SC_VERSION]    = VERSION_SHORT;
        seed[SC_VOLTAGE]    = voltage_str;
        seed[SC_VDELTA]     = vdelta_str;
        for (int i = 0; i < STATS_CARD_COUNT; i++) {
            strncpy(stats_prev_strings[i], seed[i], STAT_MAX_CHARS - 1);
            stats_prev_strings[i][STAT_MAX_CHARS - 1] = '\0';
            for (int ci = 0; ci < STAT_MAX_CHARS; ci++) {
                stats_char_anim[i][ci] = 0;
            }
        }
        stats_values_initialized = true;
    }

    // ── Smooth scroll easing ──
    // Renderer eases stats_scroll_y_f toward stats_scroll_target each frame.
    // Frame-rate independent via anim_filter; clamp dt after long gaps.
    {
        unsigned long now = millis();
        float dt = (stats_last_frame_ms == 0) ? 16.0f
                                              : (float)(now - stats_last_frame_ms);
        if (dt > 100.0f) dt = 100.0f;
        stats_last_frame_ms = now;
        stats_scroll_y_f = anim_filter(stats_scroll_y_f, (float)stats_scroll_target,
                                        STATS_SCROLL_TC, dt);
    }
    int scroll_y = (int)(stats_scroll_y_f + 0.5f);

    // ── Layout: scroll-aware card grid ──
    // Virtual coordinates (pre-scroll) → screen coordinates via:
    //   screen_y = CONTENT_Y + virtual_y - scroll_y
    // Cards entirely outside the viewport are skipped.
    const int x_full   = 4;        // full-width card x
    const int w_full   = 224;      // 240 - 8 (right edge at 228, scrollbar gutter 6 + track at 234)
    const int x_q1     = 4;        // four-card row x positions
    const int x_q2     = 61;
    const int x_q3     = 118;
    const int x_q4     = 175;
    const int w_q      = 51;
    const int w_q_last = 53;       // absorbs the rounding
    const int x_h1     = 4;        // half-card x positions
    const int x_h2     = 119;      // x_h1 + 109 + 6 gap
    const int w_half   = 109;
    const int H_HERO   = 50;       // 6+8+14+16+6 — generous gap (unused, kept for future)
    const int H_NORMAL = 36;       // 6+8+4+12+6 — TS_STRONG fits in tighter card

    auto card = [&](int vx, int vy, int w, int h, const char* label, const char* value,
                    int idx, float val_size = TS_STRONG) {
        int sy = vy - scroll_y;
        if (sy + h <= 0) return;             // entirely above viewport
        if (sy >= STATS_VIEW_H) return;      // entirely below viewport
        draw_stat_card(vx, sy, w, h, label, value, val_size,
                       stats_prev_strings[idx], stats_char_anim[idx]);
        // The helper's internal clip rect replaces the outer scroll clip;
        // restore it so the next card's outline still gets viewport-clipped.
        spr.setClipRect(0, 0, DISP_W, STATS_VIEW_H);
    };

    // Clip drawing to the viewport so partially-visible cards don't bleed
    // off the bottom edge.
    spr.setClipRect(0, 0, DISP_W, STATS_VIEW_H);

    // Row 1 (vy = 0):   SESS DET | LIFE DET
    card(x_h1, 0,   w_half, H_NORMAL, "SESS DET", det_sess_str, SC_DET_SESSION);
    card(x_h2, 0,   w_half, H_NORMAL, "LIFE DET", det_life_str, SC_DET_LIFETIME);

    // Row 2 (vy = 42):  WIFI | BLE | RAVEN | AXON
    card(x_q1, 42, w_q,      H_NORMAL, "WIFI",  wifi_str,  SC_WIFI);
    card(x_q2, 42, w_q,      H_NORMAL, "BLE",   ble_str,   SC_BLE);
    card(x_q3, 42, w_q,      H_NORMAL, "RAVEN", raven_str, SC_RAVEN);
    card(x_q4, 42, w_q_last, H_NORMAL, "AXON",  axon_str,  SC_AXON);

    // Row 3 (vy = 84):  SESSION | LIFETIME
    card(x_h1, 84,  w_half, H_NORMAL, "SESSION",  sess_str, SC_SESSION);
    card(x_h2, 84,  w_half, H_NORMAL, "LIFETIME", life_str, SC_LIFETIME);

    // Row 4 (vy = 126): BATTERY | HEAP
    card(x_h1, 126, w_half, H_NORMAL, "BATTERY", volt_str, SC_BATTERY);
    card(x_h2, 126, w_half, H_NORMAL, "HEAP",    heap_str, SC_HEAP);

    // Row 5 (vy = 168): PACKETS | SD CARD
    card(x_h1, 168, w_half, H_NORMAL, "PACKETS", pkt_str, SC_PACKETS);
    card(x_h2, 168, w_half, H_NORMAL, "SD CARD", sd_str,  SC_SD);

    // Row 6 (vy = 210): BOOTS | FLASH
    card(x_h1, 210, w_half, H_NORMAL, "BOOTS", boots_str, SC_BOOTS);
    card(x_h2, 210, w_half, H_NORMAL, "FLASH", flash_str, SC_FLASH);

    // Row 7 (vy = 252): VERSION | VOLTAGE
    card(x_h1, 252, w_half, H_NORMAL, "VERSION", VERSION_SHORT, SC_VERSION);
    card(x_h2, 252, w_half, H_NORMAL, "VOLTAGE", voltage_str,   SC_VOLTAGE);

    // Row 8 (vy = 294): V CHANGE — cumulative voltage drift since boot
    card(x_full, 294, w_full, H_NORMAL, "V CHANGE", vdelta_str, SC_VDELTA);

    spr.clearClipRect();


    // ── Scrollbar — only when content overflows. Tracks the eased value. ──
    if (STATS_CONTENT_H > STATS_VIEW_H) {
        spr.drawFastVLine(DISP_W - 4, 0, STATS_VIEW_H, CARD_BORDER);
        int thumb_h = (STATS_VIEW_H * STATS_VIEW_H) / STATS_CONTENT_H;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = (scroll_y * (STATS_VIEW_H - thumb_h)) / STATS_MAX_SCROLL;
        spr.fillRect(DISP_W - 5, thumb_y, 3, thumb_h, DIM_COLOR);
    }
}

// ============================================================================
// MAIN UI CONTROLLER
// ============================================================================
void draw_current_screen() {
    // Fullscreen opaque overlays completely occlude the screen beneath.
    // Skip the expensive underlying render when one is active.
    bool fullscreen_overlay = menu_open || show_help_overlay || wifi_config_open;

    if (!fullscreen_overlay) {
        switch (current_screen) {
            case 0:
                draw_scanner_screen();
                break;
            case 1: draw_signal_screen();           break;
            case 2: draw_capture_history_screen(); break;
            case 3: draw_gps_screen();             break;
            case 4: draw_device_info_screen();     break;
        }
        if (show_feed_expanded) draw_feed_expanded_overlay();
    }

    if (show_vol_overlay) draw_vol_overlay();
    if (show_help_overlay) draw_help_overlay();
    if (wifi_config_open) draw_wifi_config_overlay();
    if (menu_open) draw_menu_overlay();
    draw_toast_spr();
}

void transition_screen(int new_screen, int dir) {
    screen_dirty = true;
    if (!is_muted) M5Cardputer.Speaker.tone(660, 5);

    // Flush queued deletes whenever we leave the history screen
    if (current_screen == 2 && new_screen != 2 && pending_delete_count > 0)
        flush_pending_deletes();

    // Screen-specific setup (same as before)
    if (new_screen == 0) {
        feed_anim_prev_head = -1;
        feed_anim_shift_ms  = 0;
    }
    if (new_screen == 2) {
        history_scroll_offset  = 0;
        history_selected_idx   = 0;
        hist_detail_open       = false;
        hist_detail_open_ms    = 0;
        hist_delete_confirming = false;
        hist_sel_y_f           = 0.0f;
        hist_last_frame_ms     = 0;
        if (sd_available) {
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                Serial.println("[DELDIAG] === transition_screen(2) — reloading sd_hist with filters ===");
                load_sd_history();
                xSemaphoreGive(sdMutex);
                sd_hist_dirty = false;
            }
        }
    }
    if (new_screen == 4) {
        stats_scroll_target      = 0;
        stats_scroll_y_f         = 0.0f;
        stats_last_frame_ms      = millis();
        stats_values_initialized = false;
        for (int i = 0; i < STATS_CARD_COUNT; i++)
            for (int ci = 0; ci < STAT_MAX_CHARS; ci++)
                stats_char_anim[i][ci] = 0;
    }
    if (show_feed_expanded && new_screen != 0 && new_screen != 1) show_feed_expanded = false;

    current_screen = new_screen;
    draw_current_screen();
    render_frame();
}

// ============================================================================
// SYSTEM SETUP
// ============================================================================

// Boot progress screen — drawn directly to Display (not sprite).
// Uses incremental redraws to avoid full-screen flicker between milestones.
static int boot_prev_pct = -1;
static float boot_eased_fill = 0.0f;
static bool boot_first_draw = true;
static int boot_prev_fill_w = 0;
static char  boot_drawn_digits[8]          = "";   // glyph actually on the LCD, per cell
static int   boot_drawn_x[8]               = {0};  // x it was drawn at (layout can reflow)
static int   boot_drawn_y[8]               = {0};  // y it was drawn at (slides during rolls)
static unsigned long boot_digit_anim_ms[8] = {0};

void draw_boot_screen(int pct, const char* status_text = nullptr) {
    auto& lcd = M5Cardputer.Display;

    uint16_t bg     = lgfx::color565(  5,  10,  20);   // matches BG_COLOR (day)
    uint16_t blue   = lgfx::color565( 77, 219, 194);   // matches HEADER_COLOR (teal)
    uint16_t white  = lgfx::color565(232, 239, 255);   // matches TEXT_COLOR
    uint16_t dim    = lgfx::color565(149, 165, 184);   // matches DIM_COLOR (bright steel)

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    // Bar geometry — 130×20, positioned 13px below the percentage number.
    const int bar_w = 130;
    const int bar_h = 20;
    const int bar_x = (DISP_W - bar_w) / 2;
    const int bar_y = (DISP_H - 75) / 2 + 16 + 13;   // num_y + num_h + gap = 59
    const int bar_r = bar_h / 2;

    // Centered layout: (DISP_H - total_h) / 2 where total_h = num(16) + gap(13) + bar(20) + gap(18) + status(8) = 75
    const int num_y = (DISP_H - 75) / 2;          // = 30
    const int num_w = 80;
    const int num_h = 16;

    const int status_y = num_y + num_h + 13 + 20 + 18;  // = 97
    const int status_h = 8;

    // ── Staggered intro — each element appears at its own offset ──
    // Sequence (offsets relative to first call):
    //   0ms            → screen filled, layout starts blank
    //   UI_ANIM_QUICK  → bar outline reveals left-to-right over UI_ANIM_NORMAL
    //   UI_ANIM_SLOW   → percentage + status text + bar fill allowed to render
    static unsigned long boot_intro_start_ms = 0;
    static int           boot_outline_drawn_to = 0;  // pixels of outline drawn so far

    if (boot_first_draw) {
        lcd.fillScreen(bg);
        boot_first_draw      = false;
        boot_eased_fill      = 0.0f;
        boot_prev_fill_w     = 0;
        boot_intro_start_ms  = millis();
        boot_outline_drawn_to = 0;
    }

    unsigned long intro_elapsed = millis() - boot_intro_start_ms;

    // ── Bar outline: draws across 350ms starting at +220ms ──
    // Implemented as a left-to-right reveal — each frame extends the drawn
    // outline rightward to match the eased progress. The completed outline
    // stays drawn afterward.
    {
        // Outline reveal: lead-in matches UI_ANIM_QUICK, reveal duration matches
        // the title's UI_ANIM_NORMAL — outline finishes drawing as title settles.
        const unsigned long OUTLINE_DELAY = UI_ANIM_QUICK;

        if (intro_elapsed >= OUTLINE_DELAY && boot_outline_drawn_to < bar_w) {
            unsigned long oe = intro_elapsed - OUTLINE_DELAY;
            float t = (oe < UI_ANIM_NORMAL) ? (float)oe / (float)UI_ANIM_NORMAL : 1.0f;
            float ease = ui_ease(t);
            int target = (int)(ease * (float)bar_w);
            if (target > bar_w) target = bar_w;

            // Draw the full outline once we cross any progress threshold,
            // but clip the rendering to the revealed portion only. Single
            // 1px stroke — looks cleaner at the smaller bar size.
            lcd.startWrite();
            lcd.setClipRect(bar_x, bar_y, target, bar_h);
            lcd.drawRoundRect(bar_x, bar_y, bar_w, bar_h, bar_r, blue);
            lcd.clearClipRect();
            lcd.endWrite();
            boot_outline_drawn_to = target;
        }
    }

    // Gate everything else (percentage, status text, bar fill) until the
    // intro's UI_ANIM_SLOW slot opens — title + outline have settled by then.
    if (intro_elapsed < UI_ANIM_SLOW) {
        return;
    }

    // ── Percentage — digits roll individually, % symbol stays static ──
    // Each digit gets its own clipped redraw window. The % symbol is only
    // redrawn when the digit count reflows its position — otherwise it is
    // never touched: no flicker, no movement.
    //
    // Each digit's redraw is its own startWrite/endWrite transaction so the
    // SPI burst is small and atomic.
    {
        char pct_str[8];
        snprintf(pct_str, sizeof(pct_str), "%d%%", pct);
        int n_chars = (int)strlen(pct_str);

        // Layout: digits left-justified inside the number area. The % symbol
        // sits to the right of the digits and never moves — only the digits
        // animate. We compute total width based on (n_chars - 1) digits + the
        // %, then center the whole block.
        const int char_w = 12;  // textSize 2 char width
        int total_w = n_chars * char_w;
        int start_x = (DISP_W - total_w) / 2;

        // On every pct change, kick off all digit rolls simultaneously.
        // The previous left-to-right stagger looked broken at the new
        // 100ms roll duration — too short to read as a cascade.
        unsigned long now_t = millis();
        if (pct != boot_prev_pct) {
            for (int di = 0; di < n_chars - 1 && di < 8; di++) {
                boot_digit_anim_ms[di] = now_t;
            }
            boot_prev_pct = pct;
        }

        // Fast roll — must complete within boot_animate's 120ms animation
        // window so digits are never caught mid-animation by a milestone
        // transition.
        const unsigned long ROLL_MS = 100;

        // Draw the % symbol whenever its position needs to change (which
        // happens on first appearance and when the digit count changes:
        // 9% → 10% shifts everything left, 99% → 100% shifts again).
        // Tracking by position, not a one-shot flag, prevents the symbol
        // from being orphaned when the layout reflows.
        static int pct_symbol_x = -1;
        int new_pct_x = start_x + (n_chars - 1) * char_w;
        if (new_pct_x != pct_symbol_x) {
            lcd.startWrite();
            // Clear the old position (if any) so the previous % is erased.
            if (pct_symbol_x >= 0) {
                lcd.fillRect(pct_symbol_x, num_y, char_w, num_h, bg);
            }
            lcd.setTextColor(white, bg);
            lcd.setTextSize(2);
            lcd.setTextDatum(TL_DATUM);
            lcd.drawString("%", new_pct_x, num_y);
            lcd.endWrite();
            pct_symbol_x = new_pct_x;
        }

        // Redraw a digit only when what would land on the LCD differs from
        // what is already there (glyph, x after a layout reflow, or slide y).
        // Settled digits are no-ops — the old draw-every-frame loop wiped and
        // repainted each cell at ~60fps, and the panel's async refresh kept
        // catching the wiped state (the boot-screen shimmer). Tracking the
        // actually-drawn state also preserves the "hung digit" fix the
        // every-frame loop existed for: a digit whose roll was cut short
        // still mismatches its settled position, so it gets one final
        // corrective draw instead of freezing mid-roll.
        for (int di = 0; di < n_chars - 1 && di < 8; di++) {
            int dx = start_x + di * char_w;
            int draw_y = num_y;  // default: settled position

            if (boot_digit_anim_ms[di] != 0 &&
                (long)(now_t - boot_digit_anim_ms[di]) >= 0) {
                unsigned long elapsed = now_t - boot_digit_anim_ms[di];
                if (elapsed < ROLL_MS) {
                    // Half-cell slide: a full num_h offset started the glyph
                    // entirely outside the clip rect, so every roll opened
                    // with a blank cell — read as the digit blinking out.
                    draw_y = anim_slide_in(num_y, num_h / 2, boot_digit_anim_ms[di], ROLL_MS);
                }
                // else: animation complete — draw_y stays at num_y
            }

            if (boot_drawn_digits[di] == pct_str[di] &&
                boot_drawn_x[di] == dx && boot_drawn_y[di] == draw_y) {
                continue;
            }

            // Every pixel in the cell is written exactly once: bg fill for
            // the strip above the glyph (old digit remnant during a roll),
            // opaque glyph for everything below. Never wipe-then-repaint —
            // the panel can't catch an empty cell mid-frame.
            lcd.startWrite();
            lcd.setClipRect(dx, num_y, char_w, num_h);
            if (draw_y > num_y) {
                lcd.fillRect(dx, num_y, char_w, draw_y - num_y, bg);
            }
            lcd.setTextColor(white, bg);
            lcd.setTextSize(2);
            lcd.setTextDatum(TL_DATUM);
            char ch[2] = { pct_str[di], '\0' };
            lcd.drawString(ch, dx, draw_y);
            lcd.clearClipRect();
            lcd.endWrite();

            boot_drawn_digits[di] = pct_str[di];
            boot_drawn_x[di]      = dx;
            boot_drawn_y[di]      = draw_y;
        }
    }

    // ── Update bar fill — time-based ease toward target ──
    // Each pct change kicks off a new UI_ANIM_NORMAL animation from the
    // current fill width to the new target. Frame-rate independent.
    {
        // Inset by bar_h/2 on each side so the fill never enters the rounded
        // corner radius region. Maximum reachable width is bar_w - bar_h.
        const int fill_max_w = bar_w - bar_h;
        int target_fill = (pct * fill_max_w) / 100;

        static int           fill_anim_from   = 0;
        static int           fill_anim_to     = 0;
        static unsigned long fill_anim_start  = 0;

        if (target_fill != fill_anim_to) {
            fill_anim_from  = (int)(boot_eased_fill + 0.5f);
            fill_anim_to    = target_fill;
            fill_anim_start = millis();
        }

        // Boot bar fill matches the digit roll duration (100ms) so both
        // animations land together. Must be <= boot_animate's 120ms window
        // so the fill fully resolves before the next milestone.
        static const unsigned long BOOT_BAR_FILL_MS = 100;
        float ease = ui_progress(fill_anim_start, BOOT_BAR_FILL_MS);
        boot_eased_fill = (float)fill_anim_from + (float)(fill_anim_to - fill_anim_from) * ease;

        int fill_w = (int)(boot_eased_fill + 0.5f);
        if (fill_w < 0) fill_w = 0;

        // Horizontal inset matches the corner radius so the pill-shaped fill
        // sits flush inside the rounded outline. Vertical centering keeps the
        // fill line balanced inside the 20px-tall outline.
        const int fill_x = bar_x + bar_h / 2;
        const int fill_h = 4;                                // 4px tall
        const int fill_y = bar_y + (bar_h - fill_h) / 2;     // vertically centered

        // Bar only grows during boot — never clear, just redraw from the origin so
        // the rounded ends stay crisp as new pixels are added to the right.
        if (fill_w > boot_prev_fill_w) {
            int fill_r = fill_h / 2;
            if (fill_r < 1) fill_r = 1;
            if (fill_w < fill_r * 2) {
                lcd.fillRect(fill_x, fill_y, fill_w, fill_h, blue);
            } else {
                lcd.fillRoundRect(fill_x, fill_y, fill_w, fill_h, fill_r, blue);
            }
            boot_prev_fill_w = fill_w;
        }
    }

    // ── Status text: 200ms roll-up. Anti-flicker — fillRect runs ONLY on
    // the first frame after a string change (to wipe leftover chars from a
    // wider previous status). Subsequent slide frames just redraw glyphs
    // with bg as text background, so each glyph self-clears in place.
    static char boot_cur_status[32] = "";
    static unsigned long boot_status_anim_start = 0;
    static bool boot_status_settled_drawn = false;
    static bool boot_status_needs_clear   = false;

    if (status_text && strcmp(status_text, boot_cur_status) != 0) {
        strncpy(boot_cur_status, status_text, sizeof(boot_cur_status) - 1);
        boot_cur_status[sizeof(boot_cur_status) - 1] = '\0';
        boot_status_anim_start = millis();
        boot_status_settled_drawn = false;
        boot_status_needs_clear = true;
    }

    if (boot_cur_status[0] != '\0' && !boot_status_settled_drawn) {
        char cur_buf[32];
        int cur_len = 0;
        for (const char* p = boot_cur_status; *p && cur_len < 31; p++) {
            cur_buf[cur_len++] = (char)toupper((unsigned char)*p);
        }
        cur_buf[cur_len] = '\0';

        int cur_w = (cur_len > 0) ? (cur_len * ts_char_w(TS_BODY) - 1) : 0;
        int cur_x = (DISP_W - cur_w) / 2;

        const unsigned long ROLL_MS = 120;
        const int SLIDE_PX = 10;  // slides up 10px
        int draw_y = anim_slide_in(status_y, SLIDE_PX, boot_status_anim_start, ROLL_MS);
        int y_offset = draw_y - status_y;
        float t = ui_progress(boot_status_anim_start, ROLL_MS);

        lcd.startWrite();
        lcd.setClipRect(0, status_y, DISP_W, status_h);
        if (boot_status_needs_clear) {
            lcd.fillRect(0, status_y, DISP_W, status_h, bg);
            boot_status_needs_clear = false;
        }
        lcd.setTextColor(white, bg);
        lcd.setTextSize(1);
        lcd.setTextDatum(TL_DATUM);
        for (int i = 0; i < cur_len; i++) {
            char ch[2] = { cur_buf[i], '\0' };
            lcd.drawString(ch, cur_x + i * 7, status_y + y_offset);
        }
        lcd.clearClipRect();
        lcd.endWrite();

        if (t >= 1.0f) boot_status_settled_drawn = true;
    }

    lcd.setTextDatum(TL_DATUM);
}

// Drive the boot screen to the target percentage. Renders ~60fps frames
// just long enough for the renderer's internal eased animations (bar fill
// 100ms, digit roll 100ms, status slide 120ms) to land, then returns so
// real boot work continues immediately. The old "boot personality" system
// (random overshoots, stalls, post-target dwells) added several seconds of
// pure pacing theater per boot and is gone.
//
// The third parameter is retained as `int = 0` so existing call sites that
// passed a frame count keep compiling; the value is ignored.
static void boot_animate(int pct, const char* status, int /*unused*/ = 0) {
    const unsigned long ANIM_WINDOW_MS = 120;  // longest internal animation
    unsigned long start = millis();
    bool first_frame = true;
    while (millis() - start < ANIM_WINDOW_MS) {
        esp_task_wdt_reset();   // no-op if this task isn't subscribed
        draw_boot_screen(pct, first_frame ? status : nullptr);
        first_frame = false;
        delay(16);
    }
}

// Configure BLE scan parameters based on current power mode.
// Called at boot and after every periodic BLE stack restart.
static void apply_ble_scan_params() {
    if (!pBLEScan) return;
    if (low_power_mode) {
        // 50% duty cycle: 125ms interval, 62.5ms window.
        // Flock devices advertise every 100-200ms — still caught reliably.
        pBLEScan->setInterval(125);   // ms — NimBLE 2.x takes ms directly (was 0.625ms units)
        pBLEScan->setWindow(63);      // ms (~62.5)
    } else {
        // Window MUST be < interval or coexistence can never schedule WiFi RX.
        // 60 ms listen per 100 ms = ~60% BLE duty, leaving ~40% for promiscuous.
        // Flock/Raven advertise often enough to still be caught within 1-2 intervals.
        pBLEScan->setInterval(100);   // ms — NimBLE 2.x takes ms directly (was 0.625ms units)
        pBLEScan->setWindow(60);      // ms
    }
}

SET_LOOP_TASK_STACK_SIZE(7168);

// ═══════════════════════════════════════════════════════════════════════════
// ESP32-C5 5 GHz CO-PROCESSOR LINK
//
// A separate ESP32-C5 board wired to the Cardputer ADV's Grove port acts as a
// 5 GHz radio ear (the S3 only hears 2.4 GHz). The C5 sniffs the 5 GHz Wi-Fi
// channels, matches the same Flock/Raven signatures, and reports each hit as
// one newline-terminated, '|'-delimited line over UART. We parse those lines
// and push them straight through log_detection() — so a 5 GHz hit is logged,
// counted, GPS-tagged, alarmed and shown exactly like a 2.4 GHz hit.
//
// Wire protocol (C5 -> Cardputer):
//   D|mac|name|rssi|ch|conf|methods    a detection
//   H|fw|ver                           hello / heartbeat (drives the 5G badge)
// Example:  D|aa:bb:cc:dd:ee:ff|flock-1a2b|-67|149|85|ssid_match oui_match
//
// Link: UART1 on Grove G1/G2 (GPIO1 = S3 TX, GPIO2 = S3 RX). GPS owns UART2.
// 3.3 V logic both ends — no level shifter. Harmless with no C5 attached:
// nothing arrives, c5_is_present() stays false, the UI is unchanged.
// ═══════════════════════════════════════════════════════════════════════════
#define C5_RX_PIN              2        // Grove G2 (yellow) — S3 RX <- C5 TXD
#define C5_TX_PIN              1        // Grove G1 (white)  — S3 TX -> C5 RXD
#define C5_BAUD                115200
#define C5_PRESENT_TIMEOUT_MS  8000     // C5 deemed "gone" after this much silence

static HardwareSerial SerialC5(1);      // UART1 (SerialGPS owns UART2)
static bool           c5_link_started = false;
static unsigned long  c5_last_msg_ms  = 0;
static char           c5_line_buf[160];
static uint8_t        c5_line_len     = 0;
static bool           c5_discarding_line = false;

// ── TX/RX orientation probe ─────────────────────────────────────────────────
// Swapping yellow and white on the Grove cable is the most common wiring
// mistake with this link, and the only symptom used to be a dark 5G badge with
// no explanation. So instead of assuming the canonical orientation forever, try
// both and latch whichever one actually carries protocol traffic.
//
// SAFETY: probe RX-only, with the TX pin passed as -1 so no pin is driven. On a
// reversed cable the S3's TX line is already wired into the C5's TX output and
// two push-pull drivers are fighting each other. That contention exists in
// hardware whatever the firmware does — but driving our side makes it worse, so
// a TX pin is only configured once the orientation is known to be right.
#define C5_PROBE_DWELL_MS   4000   // > the C5's ~3 s heartbeat interval
#define C5_PROBE_MAX_CYCLES 3      // full A/B passes before warning the user

static int8_t        c5_probe_rx        = C5_RX_PIN;
static bool          c5_orient_locked   = false;
static uint8_t       c5_probe_cycles    = 0;
static bool          c5_probe_warned    = false;
static unsigned long c5_probe_switch_ms = 0;

// The two Grove data pins are a pair: whichever one we are listening on, the
// other is the one we would transmit from.
static inline int8_t c5_tx_for_rx(int8_t rx) {
    return (rx == C5_RX_PIN) ? C5_TX_PIN : C5_RX_PIN;
}

// True only when a C5 has actually reported in recently — a user with no C5
// never sees the 5G badge light.
bool c5_is_present() {
    return c5_link_started
        && c5_last_msg_ms != 0
        && (millis() - c5_last_msg_ms) < C5_PRESENT_TIMEOUT_MS;
}

void c5_link_begin() {
    if (c5_link_started) return;
    SerialC5.setRxBufferSize(512);                       // must precede begin()
    // TX = -1: listen only until the orientation is proven. See the safety note
    // at the probe state above.
    SerialC5.begin(C5_BAUD, SERIAL_8N1, C5_RX_PIN, -1);
    c5_line_len        = 0;
    c5_discarding_line = false;
    c5_link_started    = true;
    c5_probe_rx        = C5_RX_PIN;
    c5_orient_locked   = false;
    c5_probe_cycles    = 0;
    c5_probe_warned    = false;
    c5_probe_switch_ms = millis();
    Serial.printf("[C5] probing for link on UART1, listening on GPIO%d (RX-only)\n",
                  (int)C5_RX_PIN);
}

void c5_link_end() {
    if (!c5_link_started) return;
    // Idle the C5's radio BEFORE closing the port. Once SerialC5 is down we can
    // never reach that board again, and it would otherwise keep sniffing 5 GHz at
    // full power indefinitely, transmitting into a dead line — which is why
    // "5GHz RADIO OFF" previously saved nothing at all.
    //
    // Only reachable if the orientation locked: while probing there is no TX pin
    // configured, so this write would go nowhere at best. Skipping it costs
    // nothing — a C5 we never established contact with was never told to scan.
    if (c5_orient_locked) {
        SerialC5.printf("P|1\n");
        SerialC5.flush();
    }
    SerialC5.end();
    c5_link_started = false;
    c5_last_msg_ms  = 0;
    // Reset the probe so the menu toggle re-detects from scratch — the user may
    // well be toggling the radio *because* they just reseated the cable.
    c5_probe_rx        = C5_RX_PIN;
    c5_orient_locked   = false;
    c5_probe_cycles    = 0;
    c5_probe_warned    = false;
    c5_probe_switch_ms = 0;
    Serial.println("[C5] link down");
}

static unsigned long c5_last_time_push_ms     = 0;
static bool          c5_was_present_for_sync  = false;
static const unsigned long C5_TIME_PUSH_INTERVAL_MS = 60000UL;

static void c5_push_time() {
    if (!c5_link_started) return;
    uint32_t epoch = 0;
    if (take_data_mutex()) {
        if (gps.date.isValid() && gps.time.isValid() &&
            gps.date.year() >= 2020 && gps.date.year() <= 2099) {
            epoch = utc_to_epoch(gps.date.year(), gps.date.month(), gps.date.day(),
                                 gps.time.hour(), gps.time.minute(), gps.time.second());
        }
        give_data_mutex();
    }
    if (epoch > 0) SerialC5.printf("T|%lu\n", (unsigned long)epoch);
}

static void c5_push_signatures() {
    if (!c5_link_started) return;
    SerialC5.printf("SB\n");
    int oui_sent = 0, ssid_sent = 0;
    for (int i = 0; i < rt_oui_count; i++) {
        SerialC5.printf("SO|%s|%d\n", rt_oui[i].prefix, (int)rt_oui[i].tier);
        oui_sent++;
    }
    for (int i = 0; i < rt_ssid_count; i++) { SerialC5.printf("SS|%s\n", rt_ssid[i]); ssid_sent++; }
    SerialC5.printf("SE|%d|%d\n", oui_sent, ssid_sent);
    Serial.printf("[C5] pushed signatures: OUI=%d SSID=%d\n", oui_sent, ssid_sent);
}

// Mirror this device's own LED state onto the C5 so the pair glows as one
// instrument. The gating expression is lifted from LedTask deliberately — the
// C5 must go dark in exactly the cases the Cardputer's LED does, so the two
// read as one instrument rather than two devices with opinions. A C5 breathing
// away on the Grove port beside a Cardputer that has deliberately gone dark —
// night mode, low power, or a brightness level its LED cannot physically reach
// — reads as a malfunction, and gives away a device the user dimmed on purpose.
// Sent only on change (the C5 latches the last value), and unknown tags are
// ignored in both directions, so this needs no PROTOCOL_VERSION bump.
static void c5_push_led_state(bool force) {
    if (!c5_link_started) return;
    static int last_on = -1, last_r = -1, last_g = -1, last_b = -1;

    bool export_on = (export_mode_active || export_connecting);
    // Condition is copied from LedTask verbatim, and must stay that way — the
    // whole point is that the pair reads as one instrument. The brightness gate
    // was dropped from both together: the C5's LED has its own supply and could
    // always light at any brightness, and the Cardputer's dependency on the
    // backlight boost rail enforces itself in hardware without our help. If a
    // gate is ever reintroduced on one side, it has to be reintroduced here too,
    // or the C5 glows beside a dark Cardputer (or the reverse).
    bool on = !night_mode
              && (export_on || (led_breathing_on && !low_power_mode));
    int r = export_on ? 255 : (int)led_r;
    int g = export_on ? 130 : (int)led_g;
    int b = export_on ?   0 : (int)led_b;

    if (!force && last_on == (int)on && last_r == r && last_g == g && last_b == b) return;
    last_on = (int)on; last_r = r; last_g = g; last_b = b;
    SerialC5.printf("L|%d|%d|%d|%d\n", on ? 1 : 0, r, g, b);
}

// Push the C5's radio power state. Nothing on this side had ever told the C5
// anything about power: low_power_mode never reached it, and c5_link_end() only
// closes OUR uart — the C5 kept sniffing at full duty and transmitting into a
// dead line. So "5GHz off" and low power both saved exactly nothing on that
// board, which is the largest single load in the system when it is Grove-powered.
// P|1 idles its radio, P|0 resumes. Sent on change; unknown tags are ignored in
// both directions so this needs no PROTOCOL_VERSION bump.
static void c5_push_power_state(bool force) {
    if (!c5_link_started) return;
    static int last_idle = -1;
    int idle = low_power_mode ? 1 : 0;
    if (!force && last_idle == idle) return;
    last_idle = idle;
    SerialC5.printf("P|%d\n", idle);
    Serial.printf("[C5] pushed power state: %s\n", idle ? "idle" : "full");
}

// Alternate the RX pin until protocol traffic appears, then latch and enable TX.
// Runs before service_c5_link() so the orientation is settled before the
// presence-edge logic decides to push signatures.
static void service_c5_probe() {
    if (!c5_enabled || !c5_link_started || c5_orient_locked) return;

    // c5_last_msg_ms is set ONLY by c5_handle_line(), and only for a recognized
    // tag (H/F/D). That makes it exactly the signal wanted here — "valid
    // protocol traffic arrived on this pin" — with no separate validity check to
    // get out of sync with the parser.
    if (c5_last_msg_ms != 0) {
        int8_t tx = c5_tx_for_rx(c5_probe_rx);
        SerialC5.end();
        SerialC5.setRxBufferSize(512);                   // must precede begin()
        SerialC5.begin(C5_BAUD, SERIAL_8N1, c5_probe_rx, tx);
        c5_line_len      = 0;
        c5_discarding_line = false;
        c5_orient_locked = true;
        bool reversed = (c5_probe_rx != C5_RX_PIN);
        Serial.printf("[C5] orientation locked: RX=GPIO%d TX=GPIO%d%s\n",
                      (int)c5_probe_rx, (int)tx,
                      reversed ? "  (Grove cable is REVERSED - yellow/white swapped)"
                               : "  (standard wiring)");
        return;
    }

    if (millis() - c5_probe_switch_ms < C5_PROBE_DWELL_MS) return;

    // Nothing heard on this pin within the dwell — try the other one, still
    // RX-only.
    c5_probe_rx = (c5_probe_rx == C5_RX_PIN) ? C5_TX_PIN : C5_RX_PIN;
    SerialC5.end();
    SerialC5.setRxBufferSize(512);                       // must precede begin()
    SerialC5.begin(C5_BAUD, SERIAL_8N1, c5_probe_rx, -1);
    c5_line_len        = 0;
    c5_discarding_line = false;
    c5_probe_switch_ms = millis();
    if (c5_probe_rx == C5_RX_PIN) c5_probe_cycles++;     // wrapped = one full A/B pass

    // Warn once, then keep probing forever: a C5 powered up later, or a cable
    // reseated mid-session, must still be found. Repeating the toast would just
    // train the user to ignore it.
    if (c5_probe_cycles >= C5_PROBE_MAX_CYCLES && !c5_probe_warned) {
        c5_probe_warned = true;
        set_toast_direct("NO C5 - CHECK WIRING", TOAST_WARNING, false);
        Serial.println("[C5] no protocol traffic on either pin - check the Grove cable");
    }
}

static void service_c5_link() {
    if (!c5_enabled || !c5_link_started) { c5_was_present_for_sync = false; return; }
    bool present = c5_is_present();
    // Cheap change-detect every tick; the full push happens on link-up below.
    c5_push_led_state(false);
    c5_push_power_state(false);
    if (present && !c5_was_present_for_sync) {
        c5_push_signatures();
        c5_push_time();
        c5_push_led_state(true);   // re-assert after a C5 reboot/reconnect
        c5_push_power_state(true);
        c5_last_time_push_ms = millis();
    }
    c5_was_present_for_sync = present;
    if (present && (millis() - c5_last_time_push_ms) >= C5_TIME_PUSH_INTERVAL_MS) {
        c5_push_time();
        c5_last_time_push_ms = millis();
    }
}

// Split a '|'-delimited line in place; fields[] point into buf. The final
// field (methods) may legitimately contain spaces.
static int c5_split(char* buf, char* fields[], int max_fields) {
    int n = 0;
    fields[n++] = buf;
    for (char* p = buf; *p && n < max_fields; p++) {
        if (*p == '|') { *p = '\0'; fields[n++] = p + 1; }
    }
    return n;
}

static void c5_handle_line(char* line) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
    if (len == 0) return;

    char* f[8];
    int nf = c5_split(line, f, 8);
    if (nf < 1 || f[0][0] == '\0') return;

    if (f[0][0] == 'H') {                 // H|fw|ver — hello / heartbeat
        c5_last_msg_ms = millis();
        return;
    }

    if (f[0][0] == 'F' && nf >= 5) {     // F|mac|name|rssi|ch — ambient 5 GHz
        c5_last_msg_ms = millis();
        const char* mac  = f[1];
        clean_device_name_char(f[2]);   // C5 names were reaching the UI unscrubbed
        const char* name = f[2];
        int         rssi = atoi(f[3]);
        if (mac[0] && !is_mac_whitelisted(mac))
            feed_push_candidate(mac, name[0] ? name : "Hidden", rssi, 0, false, true);
        return;
    }

    if (f[0][0] == 'D' && nf >= 7) {      // D|mac|name|rssi|ch|conf|methods
        c5_last_msg_ms = millis();

        const char* mac     = f[1];
        clean_device_name_char(f[2]);   // as above, for the detection path
        const char* name    = f[2];
        int         rssi    = atoi(f[3]);
        int         channel = atoi(f[4]);
        int         conf    = atoi(f[5]);
        const char* methods = f[6];
        uint32_t    c5_epoch = (nf >= 8) ? (uint32_t)strtoul(f[7], NULL, 10) : 0;

        if (conf < 0)   conf = 0;
        if (conf > 100) conf = 100;
        if (mac[0] == '\0')          return;
        if (is_mac_whitelisted(mac)) return;       // honor the user's whitelist

        feed_force_push(mac, name, rssi, 0, true, true);   // 5 GHz hit into the live feed

        // Same pipeline as a 2.4 GHz hit. proto "WIFI" keeps the alarm/visual
        // routing identical; the 5 GHz channel (36-165) marks the band, and
        // extra_data flags the source. GPS tagging happens inside log_detection.
        log_detection("FLOCK_5G", "WIFI", rssi, mac, name,
                      channel, 0, "5GHz radio", methods, conf, 0, c5_epoch);

        // The one thing log_detection() doesn't do itself: arm the buzzer.
        // Mirror the 2.4 GHz call site so a strong 5 GHz hit sounds the alarm.
        if (conf >= CONFIDENCE_ALARM_THRESHOLD) {
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                trigger_alarm_confidence = conf;
                trigger_alarm_source     = 0;       // WiFi-class ascending tone
                last_buzzer_time         = millis();
            }
            xSemaphoreGiveRecursive(dataMutex);
        }
    }
    // Unknown tags ignored — forward-compatible with future C5 message types.
}

// Call once per loop(). Drains all waiting bytes and assembles whole lines.
void process_c5_serial() {
    if (!c5_link_started) return;
    while (SerialC5.available() > 0) {
        char c = (char)SerialC5.read();
        if (c == '\n') {
            if (!c5_discarding_line) {
                c5_line_buf[c5_line_len] = '\0';
                c5_handle_line(c5_line_buf);
            }
            c5_line_len = 0;
            c5_discarding_line = false;
        } else if (c5_discarding_line) {
            continue;
        } else if (c5_line_len < sizeof(c5_line_buf) - 1) {
            c5_line_buf[c5_line_len++] = c;
        } else {
            c5_line_len = 0;
            c5_discarding_line = true;  // consume through newline; never parse a suffix
        }
    }
}

void setup() {
    // ── Safe WDT reconfiguration ────────────────────────────────────
    // esp_task_wdt_deinit() is NOT safe on IDF 5.x — it wraps IDLE
    // task unsubscription in ESP_ERROR_CHECK, which aborts if the
    // tasks aren't subscribed (common after warm reboots / crash
    // resets). esp_task_wdt_reconfigure() uses ESP_GOTO_ON_ERROR
    // instead — safe to call in any WDT state.
    {
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms     = 30000,
            .idle_core_mask = 0,
            .trigger_panic  = true,
        };
        esp_err_t err = esp_task_wdt_reconfigure(&wdt_cfg);
        if (err != ESP_OK) {
            // WDT wasn't initialized at all — fresh init.
            esp_task_wdt_init(&wdt_cfg);
        }
    }

    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);   // USB-CDC: drop instead of blocking when the host isn't draining
    delay(500);

    auto cfg = M5.config();
    cfg.internal_imu = false;   // IMU is unused app-wide; don't power or poll it
    M5Cardputer.begin(cfg);

    // Resolve board identity now that M5Unified has auto-detected the panel.
    // Everything ADV-only (the GNSS receiver) is gated on this so the same
    // binary runs on a regular Cardputer with GPS simply absent.
    plume_is_adv = (M5.getBoard() == m5::board_t::board_M5CardputerADV);
    Serial.printf("[BOOT] board = %s\n",
                  plume_is_adv ? "Cardputer ADV" : "Cardputer (regular, no GPS)");

    // ── Keyboard health check (ADV only) ────────────────────────────────────
    // Adafruit_TCA8418::begin() returns only the result of its LAST register
    // write, and TCA8418KeyboardReader::begin() takes an early return when that
    // is false — skipping matrix(), flush(), attachInterruptArg() AND
    // enableInterrupts(). The result is a keyboard that is silently dead for the
    // entire session: no ISR fires, so _isr_flag never sets, so update() always
    // returns early and isPressed() is permanently 0. An I2C write is most
    // likely to fail on a marginal rail — which is exactly when Charge Mode
    // runs, and why Charge Mode could become impossible to wake from.
    //
    // enableInterrupts() is the LAST call in that chain and sets
    // GPI_IEN|KE_IEN (0x03) in CFG (0x01), so those bits being set proves the
    // whole init completed. Read them back and retry if not.
    //
    // detachInterrupt FIRST: Keyboard::begin() destroys the old reader while its
    // ISR is still registered with that object as its argument, so an interrupt
    // landing in that window would write to freed memory.
    if (plume_is_adv) {
        const uint8_t  KB_ADDR = 0x34;   // TCA8418_DEFAULT_ADDR
        const uint8_t  KB_CFG  = 0x01;   // TCA8418_REG_CFG
        const uint8_t  KB_IEN  = 0x03;   // GPI_IEN | KE_IEN
        const gpio_num_t KB_INT = GPIO_NUM_11;
        for (int attempt = 0; attempt < 3; attempt++) {
            uint8_t cfg = M5.In_I2C.readRegister8(KB_ADDR, KB_CFG, 400000);
            if ((cfg & KB_IEN) == KB_IEN) {
                if (attempt) Serial.printf("[KB] recovered on attempt %d (cfg=0x%02X)\n",
                                           attempt + 1, cfg);
                break;
            }
            Serial.printf("[KB] TCA8418 not configured (cfg=0x%02X) - re-init %d/3\n",
                          cfg, attempt + 1);
            detachInterrupt(digitalPinToInterrupt(KB_INT));
            delay(50);                       // let the boot current surge pass
            M5Cardputer.Keyboard.begin();
        }
    }

    // Drop to the lowest clock for the brown-out-prone early boot. The ESP32-S3
    // powers on at the board's full configured clock (typ. 240 MHz), so without
    // this the CPU draws peak current through the whole pre-radio window (sprite,
    // SD, GPS, LittleFS) — exactly where a depleted cell collapses (reset_reason
    // 9 / BROWNOUT). 80 MHz is plenty for display+SD+GPS; the steady-state clock
    // is set later, right before radio init.
    setCpuFrequencyMhz(80);

    // PSRAM availability + heap snapshot. Total PSRAM = 0 means PSRAM
    // isn't enabled in the board config; setPsram(true) will silently fall
    // through to internal RAM and the sprite-fallback path will kick in.
    Serial.printf("[MEM] Total PSRAM: %u, Free PSRAM: %u, Free internal: %u\n",
                  (unsigned)ESP.getPsramSize(),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)esp_get_free_heap_size());

    // Reset-reason — distinguishes the reboot class across power cycles.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* rs = "OTHER";
        switch (rr) {
            case ESP_RST_POWERON:   rs = "POWERON";                 break;
            case ESP_RST_SW:        rs = "SW_RESTART (ESP.restart)"; break;
            case ESP_RST_PANIC:     rs = "PANIC (crash/abort)";      break;
            case ESP_RST_INT_WDT:   rs = "INT_WDT";                  break;
            case ESP_RST_TASK_WDT:  rs = "TASK_WDT";                 break;
            case ESP_RST_WDT:       rs = "WDT";                      break;
            case ESP_RST_BROWNOUT:  rs = "BROWNOUT";                 break;
            case ESP_RST_DEEPSLEEP: rs = "DEEPSLEEP";                break;
            case ESP_RST_EXT:       rs = "EXT_RESET";                break;
            default: break;
        }
        Serial.printf("[BOOT] reset_reason=%d (%s)\n", (int)rr, rs);
    }

    // ── Charge Mode gate ────────────────────────────────────────────────────
    // Before touching the sprite or radios (the brown-out-prone loads), decide
    // whether the cell can survive a normal boot. If it's below the entry floor,
    // or the user requested charge mode via 'c' (which reboots with this flag),
    // hold in the radios-off charging screen until it recovers or a key is
    // pressed. This is what breaks the low-battery boot loop: we never reach
    // radio init on a cell that can't power it.
    {
        int32_t boot_mv   = charge_mode_read_mv();
        // An implausible reading means the battery ADC failed to initialize, NOT
        // an empty cell (see BATT_ADC_MIN_PLAUSIBLE_MV). Gating on it would send
        // every boot into Charge Mode, which then has no voltage to decide with
        // — a loop that presents as a bricked device. Skip the voltage gate and
        // boot the app; an explicit 'c' request or a brownout still enters (and
        // Charge Mode's own ADC health check bails straight back out).
        bool    adc_ok    = (boot_mv > BATT_ADC_MIN_PLAUSIBLE_MV);
        if (!adc_ok)
            Serial.printf("[BOOT] battery ADC implausible (%dmV) - skipping voltage gate\n",
                          (int)boot_mv);
        // Honor the 'c'-key request only after a software restart (esp_restart);
        // on power-on the NOINIT value is indeterminate. Consume it immediately
        // so a stale value can't re-trigger on a later boot.
        bool    requested = (esp_reset_reason() == ESP_RST_SW)
                            && (charge_mode_request == CHARGE_MODE_MAGIC);
        charge_mode_request = 0;
        // A brownout reset means the last boot's radio-init surge collapsed the
        // rail. The resting reading here reads deceptively high (no load yet), so
        // it alone won't stop the loop — but the reset reason will. Hold and
        // charge instead of surging into the same brownout again.
        bool    brownout  = (esp_reset_reason() == ESP_RST_BROWNOUT);
        Serial.printf("[BOOT] battery=%dmV, charge_mode_request=%s, brownout=%s\n",
                      (int)boot_mv, requested ? "yes" : "no", brownout ? "yes" : "no");
        if (requested || brownout || (adc_ok && boot_mv < CHARGE_MODE_ENTER_MV)) {
            const char* why = requested ? "user request"
                            : brownout  ? "recovering from brownout"
                                        : "battery below entry floor";
            Serial.printf("[BOOT] entering Charge Mode (%s)\n", why);
            // Only a deliberate 'c'-request tops up to full; auto entries resume
            // at the safe-to-run floor so a low cell isn't held longer than needed.
            run_charge_mode(requested, brownout);
            // Charge Mode drops to 40 MHz; the 80 MHz set for the brown-out-prone
            // boot window happens ABOVE this gate, so without this the whole
            // post-charge boot ran at half clock.
            setCpuFrequencyMhz(80);
            Serial.println("[BOOT] leaving Charge Mode -> normal boot");
        }
    }

    // Shrink speaker DMA buffers so I2S allocation doesn't eat the DMA pool.
    // The end()+begin() also restarts the amp if Charge Mode powered it down on
    // the resume path (Speaker.end() there), and is what makes the shrunk config
    // actually take effect — a live config change only applies on the next begin.
    {
        auto spk_cfg = M5Cardputer.Speaker.config();
        spk_cfg.dma_buf_count = 3;     // default 8
        spk_cfg.dma_buf_len   = 128;   // default 512; simple sine tones don't need more
        M5Cardputer.Speaker.config(spk_cfg);
        M5Cardputer.Speaker.end();     // no-op if already stopped (e.g. by Charge Mode)
        M5Cardputer.Speaker.begin();   // (re)start with the shrunk buffers
    }

    // dataMutex MUST be created before any task that uses it is spawned.
    dataMutex = xSemaphoreCreateRecursiveMutex();
    sdMutex   = xSemaphoreCreateMutex();

    // Create the 54KB draw sprite FIRST, on the clean heap. It is a single
    // contiguous block — the hardest allocation to satisfy on this no-PSRAM
    // ESP32-S3FN8 (Total PSRAM: 0) — so it must go before WiFi/BLE/LittleFS
    // fragment internal RAM. (The BLE controller, by contrast, allocates in
    // smaller pieces and fits fine in the fragmented remainder — verified: it
    // inits at ~91KB free.) createSprite tries PSRAM first (a no-op here), then
    // internal. Hard-fail visibly if even this can't fit.
    spr.setColorDepth(16);
    spr.setPsram(true);
    void* sprite_buf = spr.createSprite(DISP_W, SPR_H);
    if (!sprite_buf) {
        Serial.println("[GFX] PSRAM sprite failed, falling back to internal");
        spr.setPsram(false);
        sprite_buf = spr.createSprite(DISP_W, SPR_H);
    }
    if (!sprite_buf) {
        M5Cardputer.Display.fillScreen(lgfx::color565(255, 0, 0));
        M5Cardputer.Display.setCursor(10, 10);
        M5Cardputer.Display.print("SPRITE ALLOC FAIL");
        while (1) delay(1000);
    }
    Serial.printf("[GFX] Sprite allocated in %s, free heap: %u\n",
                  (ESP.getPsramSize() > 0 && (uint32_t)sprite_buf >= 0x3C000000) ? "PSRAM" : "internal",
                  (unsigned)esp_get_free_heap_size());

    // LED: start dark. The breathing task is spawned at the very end of setup()
    // to avoid RMT/radio contention during WiFi + BLE init on core 0.
    set_cardputer_led(0, 0, 0);

    M5Cardputer.Speaker.setVolume(0);
    M5Cardputer.Display.setRotation(1);
    brightness_level = 3;
    apply_color_palette();

    // Ease the screen in: brightness ramps from 0 → target over UI_ANIM_NORMAL
    // while the title intro animation runs simultaneously. Reads as a "wakeup"
    // — screen and title come alive together rather than the layout popping in.
    M5Cardputer.Display.setBrightness(0);
    {
        const int FADE_STEPS   = 16;
        const int FADE_STEP_MS = (int)UI_ANIM_NORMAL / FADE_STEPS;
        // Keep the backlight dim through the whole risky boot phase (SD, GPS,
        // LittleFS, radio init) — the backlight is the largest early load and
        // this is where a depleted cell browns out. The title-card reveal at
        // the end of setup() ramps to full brightness, after the surge is over.
        const int target_b     = AMBIENT_BRIGHTNESS;
        for (int i = 0; i <= FADE_STEPS; i++) {
            int b = (target_b * i) / FADE_STEPS;
            M5Cardputer.Display.setBrightness(b);
            // First call kicks off the staggered intro animation; subsequent
            // calls advance it. Status only set on first draw.
            draw_boot_screen(0, (i == 0) ? "waking up" : nullptr);
            delay(FADE_STEP_MS);
        }
    }

    // Route a dedicated SPI3 instance to the Cardputer's SD pins. 15 MHz is
    // the FAT32 sweet spot — slow enough for marginal cards, fast enough for
    // pcap flushes.
    //
    // ESP32-S3 boots GPIO 40/39/14/12 in their JTAG alternate function. If we
    // jump straight into SPI.begin() they stay configured for JTAG and the
    // SD mount silently fails. gpio_reset_pin returns them to default GPIO so
    // the SPI peripheral can claim them cleanly — this is what the launcher
    // apps do implicitly via their init path. Then idle CS high and wait
    // 100ms for the card's internal controller to settle before clocking.
    Serial.println("[SD] === Initializing FSPI for SD Card ===");

    gpio_reset_pin((gpio_num_t)SD_SPI_SCK_PIN);   // GPIO40 — clear JTAG MTDO
    gpio_reset_pin((gpio_num_t)SD_SPI_MISO_PIN);  // GPIO39 — clear JTAG MTCK
    gpio_reset_pin((gpio_num_t)SD_SPI_MOSI_PIN);  // GPIO14
    gpio_reset_pin((gpio_num_t)SD_CS_PIN);        // GPIO12

    // GPIO5 enables the SD card slot on the Cardputer — the slot is physically
    // disabled until this line is driven HIGH. Missed this because it lives in
    // the Launcher's board init (_setup_gpio), not its SD code.
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);

    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    delay(100);

    sdSPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN);

    // Launcher uses SD.begin with no frequency arg (default 4 MHz) on this hardware.
    if (SD.begin(SD_CS_PIN, sdSPI)) {
        sd_available = true;
        Serial.printf("[SD] OK! Type=%d Size=%lluMB\n",
                      SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
        Serial.printf("[BOOT] Free heap after SD init: %u\n",
                      (unsigned)esp_get_free_heap_size());

        if (!SD.exists("/PLUME"))           SD.mkdir("/PLUME");
        if (!SD.exists("/PLUME/logs"))      SD.mkdir("/PLUME/logs");
        if (!SD.exists("/PLUME/captures"))  SD.mkdir("/PLUME/captures");
        if (!SD.exists("/PLUME/stats"))     SD.mkdir("/PLUME/stats");
        Serial.println("[SD] Directory structure OK");

        // Recover from a power loss mid-delete: if .bak exists but the main file
        // is missing, restore the backup. If both exist, the rename completed —
        // just clean up the orphan.
        {
            const char* main_path = "/PLUME/logs/PlumeLog.csv";
            const char* bak_path  = "/PLUME/logs/PlumeLog.bak";
            if (SD.exists(bak_path)) {
                if (!SD.exists(main_path)) {
                    SD.rename(bak_path, main_path);
                    Serial.println("[SD] Recovered PlumeLog.csv from .bak");
                } else {
                    SD.remove(bak_path);
                    Serial.println("[SD] Cleaned orphan PlumeLog.bak");
                }
            }
        }

        if (!SD.exists(current_log_file)) {
            File file = SD.open(current_log_file, FILE_WRITE);
            if (file) { file.println("Uptime_ms,EpochUTC,EpochIsGPS,Channel,Type,Proto,RSSI,MAC,Name,TXPower,Method,Conf,ConfLabel,Extra,SeqNum,Lat,Lon,SpeedMPH,HeadingDeg,AltM,DetID"); file.close(); }
        }
        {
            bool need_pcap_header = !SD.exists(current_pcap_file);
            if (!need_pcap_header) {
                File pcheck = SD.open(current_pcap_file, FILE_READ);
                if (pcheck) { if (pcheck.size() < 24) need_pcap_header = true; pcheck.close(); }
            }
            if (need_pcap_header) {
                File pfile = SD.open(current_pcap_file, FILE_WRITE);
                if (pfile) {
                    uint32_t pcap_header[6] = {0xa1b2c3d4, 0x00040002, 0x00000000, 0x00000000, 0x0000ffff, 0x00000069};
                    pfile.write((const uint8_t*)pcap_header, 24); pfile.close();
                }
            }
        }
        {
            bool need_ble_header = !SD.exists(current_ble_pcap_file);
            if (!need_ble_header) {
                File bcheck = SD.open(current_ble_pcap_file, FILE_READ);
                if (bcheck) { if (bcheck.size() < 24) need_ble_header = true; bcheck.close(); }
            }
            if (need_ble_header) {
                File bfile = SD.open(current_ble_pcap_file, FILE_WRITE);
                if (bfile) {
                    uint32_t ble_pcap_header[6] = {0xa1b2c3d4, 0x00040002, 0x00000000, 0x00000000, 0x0000ffff, 0x000000fb};
                    bfile.write((const uint8_t*)ble_pcap_header, 24); bfile.close();
                }
            }
        }
    } else {
        sd_available = false;
        Serial.println("[SD] Mount failed. Verify card is FAT32 and fully inserted.");
    }
    boot_animate(12, sd_available ? "checking SD card" : "no SD card");
    // Seed hot-plug state so the first poll doesn't fire a spurious "mounted" toast
    sd_was_available = sd_available;
    last_sd_check_ms = millis();

    // Load detection history from SD so the Detections screen has data
    // immediately. No mutex needed — tasks haven't been spawned yet.
    if (sd_available) {
        Serial.println("[DELDIAG] === EARLY load_sd_history ===");
        load_sd_history();
        if (sd_hist_count > 0) {
            Serial.printf("[SD] Loaded %d detections from history\n", sd_hist_count);
        }
    }

    signatures_seed_defaults();
    signatures_load_from_sd();
    Serial.print(F("[SIG] MAC:")); Serial.print(rt_oui_count);
    Serial.print(F(" SSID:"));    Serial.print(rt_ssid_count);
    Serial.print(F(" BLE:"));     Serial.println(rt_name_count);

    delay(100);
    // Label this BEFORE the probe below: gps_detect_baud() can take ~7s (2 sweeps
    // x 3 bauds x 1.2s) and posting the message afterwards left the boot screen
    // frozen on the previous step's label for the whole wait, which reads as a
    // hang. The bar parks here, but at least it says what it is waiting for.
    boot_animate(25, plume_is_adv ? "finding GPS" : "no GPS module");

    // Auto-detect the GNSS baud. On success the port is left open at the
    // detected rate; on failure we open at the module default and let the
    // GPS task keep trying (covers a slow-to-emit cold start).
    if (plume_is_adv) {
        uint32_t detected = gps_detect_baud();
        if (detected == 0) {
            Serial.printf("[gps] auto-detect failed; defaulting to %u\n", (unsigned)GPS_BAUD);
            SerialGPS.end();
            delay(20);
            SerialGPS.setRxBufferSize(256);
            SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
        }
    } else {
        Serial.println("[gps] skipped: no GNSS on regular Cardputer");
    }
    delay(WIFI_MODE_SETTLE_MEDIUM_MS);
    // Before the first WiFi call, so it governs every later operation. The
    // arduino-esp32 default is WIFI_STORAGE_FLASH (WiFiGenericClass::_persistent
    // is initialised to true), which makes WiFi.begin() write the SSID and
    // password into the nvs.net80211 namespace in plaintext, under keys
    // ap.ssid / ap.passwd. That is a third copy of the password on the device,
    // outside our control and unaffected by Forget WiFi. We hold the credentials
    // ourselves and re-supply them on every connect, so the driver's cache buys
    // nothing.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA); delay(WIFI_MODE_SETTLE_SHORT_MS);
    boot_animate(50, "waking radios");

    // Sprite was already created at the top of setup().

    // Prime the EMA filter to eliminate startup ADC noise before taking the baseline
    for (int i = 0; i < 20; i++) {
        update_load_sag();
        get_filtered_voltage();
        delay(2);
    }

    // Stay at the low boot clock — the BLE worker only blocks on a queue until
    // the radio is brought up later, so 240 MHz here would just stack a CPU
    // surge onto a charging cell and risk a boot-time brownout.
    ble_event_queue = xQueueCreate(BLE_POOL_SIZE, sizeof(uint8_t));
    // Stack must cover the matched-device path: scoring + BLE-pcap build +
    // log_detection() (dataMutex, CSV formatting, and SD/FatFS writes, which
    // alone can burn 1-2KB). 2752 overflowed there — only a *matched* device
    // dives this deep, which is why unmatched feed traffic never crashed.
    xTaskCreatePinnedToCore(ble_worker_task, "BLEWorker", 6144, NULL, 1, &BLEWorkerHandle, 1);

    memset(seen_mac_table, 0, sizeof(seen_mac_table));

    // Robust LittleFS mount with verbose recovery logging. begin(true)
    // auto-formats on corruption; if it still fails (known ESP32-S3 issue
    // where block 0x0 reports bad), erase the spiffs partition manually
    // and retry. Every branch logs an actionable line.
    {
        Serial.println("[FS] Attempting LittleFS.begin(true)...");
        bool ok = LittleFS.begin(true);
        if (!ok) {
            Serial.println("[FS] First begin(true) failed — trying partition erase...");
            const esp_partition_t* part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
            if (!part) {
                Serial.println("[FS] spiffs partition NOT FOUND in partition table!");
                littlefs_available = false;
            } else {
                Serial.printf("[FS] Found partition '%s' addr=0x%lx size=%lu\n",
                              part->label, (unsigned long)part->address,
                              (unsigned long)part->size);
                esp_err_t err = esp_partition_erase_range(part, 0, part->size);
                Serial.printf("[FS] erase_range returned: %d (%s)\n",
                              err, esp_err_to_name(err));
                if (err == ESP_OK) {
                    bool ok2 = LittleFS.begin(true);
                    Serial.printf("[FS] Retry begin(true): %s\n", ok2 ? "OK" : "FAIL");
                    littlefs_available = ok2;
                } else {
                    littlefs_available = false;
                }
            }
        } else {
            littlefs_available = true;
            Serial.printf("[FS] Mounted. total=%u used=%u\n",
                          (unsigned)LittleFS.totalBytes(),
                          (unsigned)LittleFS.usedBytes());
        }
    }

    if (littlefs_available) {
        load_session_from_flash();
        load_wifi_credentials();
        load_detections_from_flash();
        load_whitelist();
    }
    // Apply persisted settings that require hardware calls after load
    if (night_mode)     apply_color_palette();
    if (is_muted)       M5Cardputer.Speaker.setVolume(0);
    if (c5_enabled)        c5_link_begin();   // bring up 5 GHz C5 link if enabled
    Serial.printf("[BOOT] Free heap after LittleFS: %u\n",
                  (unsigned)esp_get_free_heap_size());
    boot_animate(62, "loading session");

    // First-boot WiFi credential initialization from #defines if flash is empty
    if (strlen(export_ssid) == 0 && strlen(EXPORT_WIFI_SSID) > 0) {
        strncpy(export_ssid, EXPORT_WIFI_SSID, sizeof(export_ssid) - 1);
        export_ssid[sizeof(export_ssid) - 1] = '\0';
    }
    if (strlen(export_pass) == 0 && strlen(EXPORT_WIFI_PASS) > 0) {
        strncpy(export_pass, EXPORT_WIFI_PASS, sizeof(export_pass) - 1);
        export_pass[sizeof(export_pass) - 1] = '\0';
    }

    lifetime_boots++;
    if (littlefs_available) {
        save_session_to_flash();
    }
    session_start_time = millis();

    // Set steady-state CPU clock before radio bring-up so WiFi and BLE init
    // don't stack their current spikes on top of a 240 MHz draw.
    if (turbo_mode_active)
        setCpuFrequencyMhz(240);
    else if (low_power_mode)
        setCpuFrequencyMhz(80);
    else
        setCpuFrequencyMhz(160);
    Serial.printf("[BOOT] CPU %lu MHz before radio init\n",
                  (unsigned long)getCpuFrequencyMhz());

    // WiFi promiscuous mode — complete before scanner screen appears
    WiFi.mode(WIFI_STA);          // re-assert STA HERE so the driver is guaranteed
    WiFi.disconnect();            // started at the moment we enable promiscuous
    delay(WIFI_MODE_SETTLE_MEDIUM_MS);
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_promiscuous_filter_t filt;
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;

    esp_err_t pf = esp_wifi_set_promiscuous_filter(&filt);
    esp_err_t pr = esp_wifi_set_promiscuous(true);
    esp_err_t cb = esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_err_t ch = esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[WIFI] filter=%d promisc=%d cb=%d channel=%d (0=OK)\n",
                  (int)pf, (int)pr, (int)cb, (int)ch);

    delay(WIFI_MODE_SETTLE_MEDIUM_MS);
    Serial.printf("[BOOT] Free heap after WiFi promisc: %u\n",
                  (unsigned)esp_get_free_heap_size());
    boot_animate(80, "starting 2.4GHz WiFi");

    // NimBLE — complete before scanner screen appears
    NimBLEDevice::init(""); NimBLEDevice::setMTU(23); NimBLEDevice::setPowerLevel(BLE_TX_POWER);
    Serial.printf("[BOOT] Free heap after NimBLE init: %u\n",
                  (unsigned)esp_get_free_heap_size());
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(&ble_cb_singleton, false);
    pBLEScan->setActiveScan(false);
    apply_ble_scan_params();
    // Don't store results internally — every advertisement is already handled
    // via the callback -> ble_event_queue -> ble_worker_task pipeline. The
    // default cache can hit 10–20 KB in a busy RF environment for no benefit.
    pBLEScan->setMaxResults(0);
    last_ble_restart_ms = millis();
    boot_animate(95, "starting Bluetooth");

    // Tasks
    last_channel_hop = millis(); last_sd_flush = millis(); last_persist_save = millis();
    xTaskCreatePinnedToCore(ScannerLoopTask, "ScannerTask", 2048, NULL, 1, &ScannerTaskHandle, 0);
    if (plume_is_adv)   // no GPS UART on the regular Cardputer -> don't spawn the reader
        xTaskCreatePinnedToCore(GPSLoopTask, "GPSTask", 2048, NULL, 1, &GPSTaskHandle, 0);
    last_user_input_ms = millis();
    system_fully_booted = true;

    // V CHANGE baseline — taken HERE, not earlier. It has to sit after the EMA
    // prime loop above (whose comment says "before taking the baseline", which
    // the old capture site 169 lines earlier defeated) and after the sag model
    // reaches steady state. Unsagged, so it stays comparable to the card's
    // current reading no matter what the sag terms do afterwards.
    session_start_mv = get_unsagged_voltage();

    // Spawn the LED task at priority 1 on Core 1 — must come after WiFi+BLE
    // are up so RMT/radio contention is avoided.
    xTaskCreatePinnedToCore(LedTask, "LedTask", 1536, NULL, 1, &LedTaskHandle, 1);

    // Persisted low power: put the GPS receiver into standby now that its
    // UART is up (the toggle sites handle the live transitions).
    if (low_power_mode) gps_standby(true);

    // WDT was already initialized early in setup(); each watched task
    // self-subscribes via esp_task_wdt_add(NULL) inside its loop.

    // Ungate the radios NOW, before the feed gate + title card, so the
    // WiFi/BLE callbacks capture in the background while the title card is up.
    // The scanner is then already populated and live the instant it's revealed.
    scanner_ready = true;

    // Gate: wait for the WiFi sniffer to confirm radios are up (~15 packets) or
    // 4 seconds, whichever comes first. Pump event queues during the wait so the
    // live feed buffer pre-populates before the scanner screen appears. The
    // 800ms feed-push throttle is bypassed here — boot wants maximum population
    // speed, not smooth UI animation.
    {
        unsigned long feed_gate_start = millis();
        const unsigned long FEED_GATE_MAX_MS = 1500;
        last_feed_push_ms = 0;
        while ((millis() - feed_gate_start) < FEED_GATE_MAX_MS) {
            if (ambient_packet_count >= 5) break;
            draw_boot_screen(100, "listening for signals");
            delay(30);
            M5Cardputer.update();
            process_wifi_event_queue();
            last_feed_push_ms = 0;
            feed_commit_pending();
        }
        // Final drain — anything that arrived during the last delay(30).
        process_wifi_event_queue();
        last_feed_push_ms = 0;
        feed_commit_pending();
        // Reset so a later boot-screen draw (shouldn't happen) would repaint cleanly
        boot_prev_fill_w = 0;
    }

    // Pre-populate the scanner feed snapshot so the first frame of
    // draw_scanner_screen() already has rows to display.
    {
        xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
        scan_local_count = feed_count;
        scan_local_head  = feed_head;
        for (int i = 0; i < FEED_SIZE; i++) scan_local_feed[i] = feed_entries[i];
        xSemaphoreGiveRecursive(dataMutex);
        scan_feed_last_snapshot = millis();
    }

    // ── Title card boot sequence ──
    {
        int start_brightness = effective_brightness();

        // Phase 1: Fade out boot screen
        {
            int steps = 12;
            for (int i = 1; i <= steps; i++) {
                float t = (float)i / (float)steps;
                M5Cardputer.Display.setBrightness((uint8_t)(start_brightness * (1.0f - t)));
                delay(25);
            }
        }

        // The content sprite only covers y=CONTENT_Y..DISP_H, so during the
        // title card the top 20px would sit empty — a visible dead band where
        // the grid lines stop. A small throwaway canvas carries the grid
        // across that strip; grid spacing == CONTENT_Y, so the two canvases
        // tile into one seamless full-screen grid. Freed before the phase-4
        // dissolve, where the real header takes the strip over.
        M5Canvas title_strip(&M5Cardputer.Display);
        title_strip.setColorDepth(16);
        bool have_strip = (title_strip.createSprite(DISP_W, CONTENT_Y) != nullptr);

        // Pushes the composed title-card frame without render_frame() so the
        // header is not drawn during the card — it first appears with the
        // scanner during the phase-4 dissolve. Same DMA push as
        // render_frame, minus the header drawing.
        auto push_title_frame = [&]() {
            auto& lcd = M5Cardputer.Display;
            if (have_strip) {
                title_strip.fillSprite(BG_COLOR);
                draw_title_grid(title_strip, CONTENT_Y, 1.0f);
            }
            lcd.startWrite();
            if (have_strip) {
                uint16_t* sbuf = (uint16_t*)title_strip.getBuffer();
                if (sbuf) {
                    lcd.pushImageDMA(0, 0, DISP_W, CONTENT_Y,
                                     (lgfx::swap565_t*)sbuf);
                }
            }
            uint16_t* buf = (uint16_t*)spr.getBuffer();
            if (buf) {
                lcd.pushImageDMA(0, CONTENT_Y, DISP_W, SPR_H,
                                 (lgfx::swap565_t*)buf);
            }
            lcd.endWrite();
        };

        // Phase 2: Compose the title card through the sprite pipeline and
        // fade the backlight in. Same geometry as the dissolve in phase 4 —
        // previously this drew direct to the LCD centered in the full 135px
        // screen while phase 4 drew via the 115px content sprite, so the
        // card jumped ~10px at the phase boundary.
        {
            M5Cardputer.Display.fillScreen(BG_COLOR);

            int steps = 12;
            for (int i = 1; i <= steps; i++) {
                float t = (float)i / (float)steps;
                spr.fillSprite(BG_COLOR);
                draw_title_card_overlay(1.0f);
                push_title_frame();
                M5Cardputer.Display.setBrightness((uint8_t)(start_brightness * t));
                delay(25);
            }
        }

        // Boot chime — plays over the title card, not after the reveal.
        // stop() flushes stale I2S DMA (same trick AlarmTask uses) instead of
        // the end()/begin() cycle that caused the DC-transient "pop". Higher
        // pitch + volume so the small speaker actually reproduces it.
        M5Cardputer.Speaker.stop();
        delay(10);
        M5Cardputer.Speaker.setVolume(150);
        M5Cardputer.Speaker.tone(640, 160); delay(190);
        M5Cardputer.Speaker.tone(480, 160); delay(190);
        M5Cardputer.Speaker.tone(540, 220); delay(260);
        M5Cardputer.Speaker.stop();
        M5Cardputer.Speaker.setVolume(is_muted ? 0 : current_volume);

        // The dissolve renders through render_frame(), which owns the header
        // strip from here on — the grid strip canvas is no longer needed.
        title_strip.deleteSprite();

        // Phase 4: Dissolve dark → scanner over 1 second
        {
            unsigned long dissolve_start = millis();
            unsigned long dissolve_ms = 1000;

            // The fade rewrites every pixel of the sprite on all ~62 frames.
            // readPixel/drawPixel would be ~1.7 million calls each across the
            // dissolve, with bounds checks and format dispatch per pixel; the
            // sprite is one flat DISP_W-stride array, so walk it directly. Same
            // accessors draw_scanner_viz_scan() uses, which carry the RGB565
            // byte-order fixup.
            //
            // Detect byte order explicitly rather than inheriting it. Today it
            // happens as a side effect of draw_scanner_viz_scan() running inside
            // draw_current_screen() — true only because scanner_viz_mode
            // defaults to 0, which is not a fact this loop should depend on. The
            // call self-guards on g_buf_byte_order_detected, so it is free.
            detect_buffer_byte_order(spr);
            uint16_t* dissolve_buf   = (uint16_t*)spr.getBuffer();
            const int dissolve_px    = DISP_W * SPR_H;

            while (millis() - dissolve_start < dissolve_ms) {
                M5Cardputer.update();
                process_wifi_event_queue();
                feed_commit_pending();
                float alpha = 1.0f - (float)(millis() - dissolve_start) / (float)dissolve_ms;

                draw_current_screen();

                if (dissolve_buf) {
                    for (int i = 0; i < dissolve_px; i++) {
                        uint16_t px = read_pixel_logical(dissolve_buf, i);
                        write_pixel_logical(dissolve_buf, i,
                                            lerp_col16(px, BG_COLOR, alpha));
                    }
                } else {
                    // getBuffer() can return null. Fade through the slow path
                    // rather than skipping it, so the boot still looks right.
                    for (int y = 0; y < SPR_H; y++) {
                        for (int x = 0; x < DISP_W; x++) {
                            uint16_t px = spr.readPixel(x, y);
                            spr.drawPixel(x, y, lerp_col16(px, BG_COLOR, alpha));
                        }
                    }
                }

                draw_title_card_overlay(alpha);
                render_frame();
                delay(16);
            }
        }

        // Phase 5: Clean transition to normal operation
        draw_current_screen();
        render_frame();
    }

    // Ungate callbacks — both WiFi and BLE were discarding packets until now.
    scanner_ready = true;
    Serial.println("[BOOT] Scanner ready — promiscuous callbacks enabled");

    // Signature provenance, raised here rather than at load time. The loader runs
    // hundreds of lines above, before the boot reveal, and the reveal repaints
    // the whole screen — a toast set there is wiped before anyone sees it.
    // TOAST_WARNING because a replaced signature database is "something changed",
    // not a success: the file could just as easily have removed signatures as
    // added them.
    if (sig_loaded_from_sd) {
        char sig_msg[32];
        snprintf(sig_msg, sizeof(sig_msg), "SIGS FROM SD: %d/%d/%d",
                 sig_sd_oui, sig_sd_ssid, sig_sd_name);
        set_toast_direct(sig_msg, TOAST_WARNING, false);
    }
}

// ============================================================================
// PERIODIC SERVICES — lifted verbatim out of loop() for readability.
// Behavior-identical: same logic, same static state, same call order. The only
// change is that loop-locals they relied on (e.g. loop_mv) are now parameters.
// ============================================================================

// Low-battery voltage warnings (crossing toasts + periodic re-warn w/ hysteresis).
static void service_battery_warnings(int32_t loop_mv) {
    static const unsigned long BATT_REWARN_LOW_MS  = 10UL * 60UL * 1000UL;
    static const unsigned long BATT_REWARN_CRIT_MS =  2UL * 60UL * 1000UL;

    static int32_t last_battery_warning_mv = 9999;
    static unsigned long last_battery_warn_toast_ms = 0;
    static bool auto_conserved = false;
    unsigned long batt_now = millis();

    // Initial crossing-detection (unchanged behavior).
    if (loop_mv <= 3500 && last_battery_warning_mv > 3500) {
        set_toast_direct("BATT CRITICAL 3.5V", TOAST_WARNING, false);
        last_battery_warning_mv = 3500;
        last_battery_warn_toast_ms = batt_now;
        // Auto-conserve: drop to low-power mode once to stretch remaining
        // charge and shrink the peak that would otherwise trip a brownout.
        if (!auto_conserved && !low_power_mode) {
            auto_conserved = true;
            turbo_mode_active = false;
            low_power_mode = true;
            setCpuFrequencyMhz(80);
            if (!ambient_mode)
                M5Cardputer.Display.setBrightness(effective_brightness());
            apply_ble_scan_params();      // applies 50%-duty BLE params
            gps_standby(true);            // GPS to standby: biggest cuttable load
            schedule_persist();           // flush state now in case of imminent cutoff
            set_toast_direct("LOW BATT - CONSERVING", TOAST_WARNING, false);
        }
    } else if (loop_mv <= 3700 && last_battery_warning_mv > 3700) {
        set_toast_direct("BATT LOW 3.7V", TOAST_WARNING, false);
        last_battery_warning_mv = 3700;
        last_battery_warn_toast_ms = batt_now;
    } else if (last_battery_warning_mv < 9999
               && loop_mv >= last_battery_warning_mv + 100) {
        // Voltage recovered (charger plugged in, or rebound from load drop).
        // Clear the latch so it can re-arm on a future dip; do NOT auto-restore
        // low-power mode — the user can turn it off from the menu if plugged in.
        last_battery_warning_mv = 9999;
        last_battery_warn_toast_ms = 0;
        auto_conserved = false;
    }

    // Periodic re-warn while below a threshold.
    if (last_battery_warning_mv == 3500 && last_battery_warn_toast_ms != 0
        && (batt_now - last_battery_warn_toast_ms) >= BATT_REWARN_CRIT_MS) {
        char buf[TOAST_TEXT_LEN];
        int32_t mv_now = loop_mv;
        snprintf(buf, sizeof(buf), "BATT CRITICAL %ld.%02ldV",
                 (long)(mv_now / 1000), (long)((mv_now % 1000) / 10));
        set_toast_direct(buf, TOAST_WARNING, false);
        last_battery_warn_toast_ms = batt_now;
    } else if (last_battery_warning_mv == 3700 && last_battery_warn_toast_ms != 0
               && (batt_now - last_battery_warn_toast_ms) >= BATT_REWARN_LOW_MS) {
        char buf[TOAST_TEXT_LEN];
        int32_t mv_now = loop_mv;
        snprintf(buf, sizeof(buf), "BATT LOW %ld.%02ldV",
                 (long)(mv_now / 1000), (long)((mv_now % 1000) / 10));
        set_toast_direct(buf, TOAST_WARNING, false);
        last_battery_warn_toast_ms = batt_now;
    }
}

static bool wdt_subscribed = false;
// Subscribe this task to the hardware watchdog (once), then pet it each iteration.
static void service_watchdog() {
    if (!wdt_subscribed) {
        esp_err_t err = esp_task_wdt_add(NULL);
        if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) wdt_subscribed = true;
    }
    if (wdt_subscribed) esp_task_wdt_reset();
}

// Drive export web-server mode: tick connect, service clients, time-box the session.
static void service_export_mode() {
    if (export_connecting) {
        export_tick_connect();
    }
    if (export_mode_active) {
        if (export_server) export_server->handleClient();
        if ((millis() - export_mode_started_at) > EXPORT_MODE_MAX_MS) {
            if (!export_server) {
                export_mode_stop();
            } else {
                WiFiClient check_client = export_server->client();
                if (!check_client || !check_client.connected()) {
                    export_mode_stop();
                } else {
                    // Extend 60s while a client is active; prevents premature
                    // server teardown during slow chunked transfers.
                    export_mode_started_at = millis() - EXPORT_MODE_MAX_MS + 60000UL;
                }
            }
        }
    }
}

// Stack health monitoring — three layers:
//   1. One-shot 60s baseline log (initial usage after all code paths hit).
//   2. Periodic hourly re-log (catches slow growth under sustained load).
//   3. Per-second critical-low check (toasts if any task is near overflow).
//
// Watermark values are bytes of *unused* stack. Once under STACK_CRITICAL_BYTES,
// the task is one bad call away from corrupting whatever sits below its stack —
// typically another task's stack or FreeRTOS internal state.
static void service_stack_health() {
    static const UBaseType_t STACK_CRITICAL_BYTES = 256;
    static const unsigned long STACK_REPORT_INTERVAL_MS = 60UL * 60UL * 1000UL;
    static const unsigned long STACK_CRITICAL_CHECK_MS  = 1000UL;
    static const unsigned long STACK_TOAST_COOLDOWN_MS  = 60UL * 1000UL;

    static bool stack_baseline_reported = false;
    static unsigned long stack_last_report_ms = 0;
    static unsigned long stack_last_critical_check_ms = 0;
    static unsigned long stack_last_toast_ms[5] = {0};

    unsigned long stack_now = millis();

    auto gather_watermarks = [&](UBaseType_t out[5]) {
        out[0] = ScannerTaskHandle ? uxTaskGetStackHighWaterMark(ScannerTaskHandle) : (UBaseType_t)-1;
        out[1] = GPSTaskHandle     ? uxTaskGetStackHighWaterMark(GPSTaskHandle)     : (UBaseType_t)-1;
        out[2] = BLEWorkerHandle   ? uxTaskGetStackHighWaterMark(BLEWorkerHandle)   : (UBaseType_t)-1;
        out[3] = LedTaskHandle     ? uxTaskGetStackHighWaterMark(LedTaskHandle)     : (UBaseType_t)-1;
        out[4] = uxTaskGetStackHighWaterMark(NULL);
    };

    auto log_watermarks = [&](const char* tag, const UBaseType_t hw[5]) {
        Serial.printf("[STACK] %s — bytes remaining:\n", tag);
        Serial.printf("  Scanner: %u / 2048\n", (unsigned)hw[0]);
        Serial.printf("  GPS:     %u / 2048\n", (unsigned)hw[1]);
        Serial.printf("  BLE:     %u / 2752\n", (unsigned)hw[2]);
        Serial.printf("  LED:     %u / 1536\n", (unsigned)hw[3]);
        Serial.printf("  Loop:    %u / 7168\n", (unsigned)hw[4]);
    };

    // Layer 1: baseline log at 60 seconds.
    if (!stack_baseline_reported && stack_now > 60000) {
        stack_baseline_reported = true;
        UBaseType_t hw[5];
        gather_watermarks(hw);
        log_watermarks("baseline @ 60s", hw);
        Serial.printf("[STACK] Safe to reduce any task where remaining > 512 bytes.\n");
        Serial.printf("  Recommended: new_stack = current - (remaining - 256)\n");
        stack_last_report_ms = stack_now;
    }

    // Layer 2: periodic re-log every hour after baseline.
    if (stack_baseline_reported &&
        (stack_now - stack_last_report_ms) >= STACK_REPORT_INTERVAL_MS) {
        stack_last_report_ms = stack_now;
        UBaseType_t hw[5];
        gather_watermarks(hw);
        log_watermarks("hourly check", hw);
    }

    // Layer 3: critical-low check every second. Five register reads — negligible cost.
    if ((stack_now - stack_last_critical_check_ms) >= STACK_CRITICAL_CHECK_MS) {
        stack_last_critical_check_ms = stack_now;
        UBaseType_t hw[5];
        gather_watermarks(hw);
        static const char* task_names[5] = { "SCANNER", "GPS", "BLE", "LED", "LOOP" };
        for (int i = 0; i < 5; i++) {
            if (hw[i] == (UBaseType_t)-1) continue;
            if (hw[i] < STACK_CRITICAL_BYTES) {
                Serial.printf("[STACK] CRITICAL: %s task has %u bytes free\n",
                              task_names[i], (unsigned)hw[i]);
                if ((stack_now - stack_last_toast_ms[i]) >= STACK_TOAST_COOLDOWN_MS) {
                    stack_last_toast_ms[i] = stack_now;
                    char toast_buf[TOAST_TEXT_LEN];
                    snprintf(toast_buf, sizeof(toast_buf),
                             "STACK LOW: %s %uB", task_names[i], (unsigned)hw[i]);
                    set_toast_direct(toast_buf, TOAST_WARNING, false);
                }
            }
        }
    }
}

// Heap health: warn (and toast) when free internal heap drops below 8KB.
// Catches slow leaks / FAT-driver mallocs piling up before they cause
// an unrecoverable abort.
static void service_heap_health() {
    size_t free_heap = esp_get_free_heap_size();
    static size_t min_heap_seen = 999999;
    if (free_heap < min_heap_seen) min_heap_seen = free_heap;
    if (free_heap < 3000) {
        Serial.printf("[HEAP] FATAL: %u bytes free — restarting\n", (unsigned)free_heap);
        delay(100);
        ESP.restart();
    }
    if (free_heap < 6000) {
        static unsigned long last_heap_warn = 0;
        if (millis() - last_heap_warn > 30000) {
            Serial.printf("[HEAP] CRITICAL: %u bytes free (min: %u)\n",
                          (unsigned)free_heap, (unsigned)min_heap_seen);
            set_toast_direct("LOW MEMORY", TOAST_WARNING, false);
            last_heap_warn = millis();
        }
    }
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest_block < 2048) {
        static unsigned long last_frag_log = 0;
        if (millis() - last_frag_log > 60000) {
            last_frag_log = millis();
            Serial.printf("[HEAP] Fragmented: largest block %u bytes (free: %u)\n",
                          (unsigned)largest_block, (unsigned)free_heap);
        }
    }
    // Diagnostic: free >> 15000 but largest near/below it -> FRAGMENTATION
    //             free itself dropping toward 15000             -> EXHAUSTION
    static size_t min_largest_seen = 999999;
    if (largest_block < min_largest_seen) min_largest_seen = largest_block;
    static unsigned long last_heaplog_ms = 0;
    if (millis() - last_heaplog_ms >= 5000) {
        last_heaplog_ms = millis();
        Serial.printf("[HEAPLOG] free=%u min_free=%u largest=%u min_largest=%u\n",
                      (unsigned)free_heap, (unsigned)min_heap_seen,
                      (unsigned)largest_block, (unsigned)min_largest_seen);
        uint32_t enq  = __atomic_load_n(&wifi_pkt_enqueued, __ATOMIC_RELAXED);
        uint32_t drop = __atomic_load_n(&wifi_pkt_dropped,  __ATOMIC_RELAXED);
        uint32_t pct  = (enq + drop > 0) ? (drop * 100u / (enq + drop)) : 0u;
        Serial.printf("[LOADLOG] enq=%u drop=%u drop_pct=%u free=%u largest=%u\n",
                      (unsigned)enq, (unsigned)drop, (unsigned)pct,
                      (unsigned)free_heap, (unsigned)largest_block);
        Serial.printf("[SDDROP] csv=%lu wifi_pcap=%lu ble_pcap=%lu\n",
                      (unsigned long)sd_log_dropped,
                      (unsigned long)sd_wifi_pcap_dropped,
                      (unsigned long)sd_ble_pcap_dropped);
    }
}

// Leave ambient (and the deeper screen-off tier) and restore the panel. Order
// matters: screen-off sleeps the panel, so it must be woken before a brightness
// change means anything.
static void display_wake_from_idle() {
    if (screen_off_mode) {
        screen_off_mode = false;
        M5Cardputer.Display.wakeup();
        screen_dirty = true;
    }
    ambient_mode = false;
    M5Cardputer.Display.setBrightness(effective_brightness());
}

static void service_ambient_mode() {
    // Enter ambient mode after sustained idle
    if (!ambient_mode && !toast_active && !signal_active && !export_mode_active &&
        (millis() - last_user_input_ms) > current_ambient_timeout_ms()) {
        ambient_mode = true;
        show_feed_expanded = false;
        // Clamp: AMBIENT_BRIGHTNESS (40) is HIGHER than BRIGHTNESS_LEVELS[0]
        // (32), so at 1/4 the old unconditional set made idling *brighter* and
        // cost power instead of saving it.
        uint8_t amb = effective_brightness();
        if (amb > AMBIENT_BRIGHTNESS) amb = AMBIENT_BRIGHTNESS;
        M5Cardputer.Display.setBrightness(amb);
    }

    // Deeper tier: kill the backlight entirely after a longer idle.
    if (ambient_mode && !screen_off_mode && !toast_active
        && !signal_active && !export_mode_active
        && (millis() - last_user_input_ms) > current_screen_off_timeout_ms()) {
        screen_off_mode = true;
        M5Cardputer.Display.setBrightness(0);
        M5Cardputer.Display.sleep();
    }

    // Exit ambient if conditions change from non-input sources
    if (ambient_mode && (signal_active || export_mode_active || toast_active)) {
        display_wake_from_idle();
    }
}

// Re-assert GPS standby while in low power. gps_standby() fires once at the
// toggle, but the $PCAS12 window unit is ambiguous across CASIC firmwares (ms
// vs s) — if milliseconds, 65535 is only ~65s and the receiver silently resumes
// acquiring at 25-40mA for the rest of the session, which is the single largest
// load on this board. Charge Mode already re-issues for exactly this reason
// (see its gps_renap counter); low power never did, so its biggest claimed
// saving may not have been happening at all.
static void service_gps_standby_refresh() {
    if (!plume_is_adv || !low_power_mode) return;
    static unsigned long last_renap_ms = 0;
    unsigned long now_g = millis();
    if (now_g - last_renap_ms < 30000UL) return;
    last_renap_ms = now_g;
    gps_standby(true);
}

static void service_gps_timezone() {
    // Recompute timezone from GPS every 5 minutes (handles driving across zones)
    if (millis() - auto_tz_last_compute_ms > AUTO_TZ_INTERVAL_MS || !auto_tz_valid) {
        bool tz_gps_ok = false;
        double tz_lat = 0, tz_lng = 0;
        int tz_year = 0, tz_month = 0, tz_day = 0;
        if (take_data_mutex()) {
            if (gps.location.isValid() && gps.location.age() < 5000 &&
                gps.date.isValid() && gps.date.year() >= 2020) {
                tz_lat   = gps.location.lat();
                tz_lng   = gps.location.lng();
                tz_year  = gps.date.year();
                tz_month = gps.date.month();
                tz_day   = gps.date.day();
                tz_gps_ok = true;
            }
            give_data_mutex();
        }
        if (tz_gps_ok) tz_compute(tz_lat, tz_lng, tz_year, tz_month, tz_day);
    }
}

static void service_sd_hotplug() {
    // SD hot-plug: periodically attempt remount if card is absent, or probe if present
    sd_check_hotplug();
    if (wdt_subscribed) esp_task_wdt_reset();

    if (millis() - last_sd_flush_check >= 500) {
        last_sd_flush_check = millis();
        bool should_flush = false;

        xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
        if (sd_write_count >= MAX_LOG_BUFFER || pcap_write_count >= MAX_PCAP_BUFFER ||
            ble_pcap_write_count >= MAX_PCAP_BUFFER ||
            (millis() - last_sd_flush > SD_FLUSH_INTERVAL &&
             (sd_write_count > 0 || pcap_write_count > 0 || ble_pcap_write_count > 0))) {
            should_flush = true;
        }
        xSemaphoreGiveRecursive(dataMutex);

        if (should_flush) flush_sd_buffer();
    }
    if (wdt_subscribed) esp_task_wdt_reset();

    {
        bool was_dirty = false;
        if (current_screen == 2 && !hist_detail_open) {
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            if (sd_hist_dirty) {
                sd_hist_dirty = false;
                was_dirty = true;
            }
            xSemaphoreGiveRecursive(dataMutex);
        }
        if (was_dirty) {
            // Same timed-take pattern — skip the load if PersistTask is busy;
            // sd_hist_dirty has already been cleared, so this iteration just
            // shows the previous snapshot until the next dirty cycle.
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                load_sd_history();
                xSemaphoreGive(sdMutex);
            }
        }
    }
}

static void service_ble_restart() {
    // Periodic BLE stack health restart — prevents NimBLE internal state
    // corruption that can build up during multi-hour continuous scanning.
    // Skip when export is active: NimBLE is already deinited then.
    if (export_mode_active || export_connecting) {
        last_ble_restart_ms = millis();  // skip this cycle
    } else if (millis() - last_ble_restart_ms > BLE_RESTART_INTERVAL_MS) {
        last_ble_restart_ms = millis();

        // 1. Stop scanning so the callback stops producing new events.
        scanner_ready = false;
        if (ScannerTaskHandle) vTaskSuspend(ScannerTaskHandle);
        if (pBLEScan) {
            pBLEScan->stop();
            pBLEScan->clearResults();
        }

        // 2. Drain the BLE event queue so the worker doesn't pick up stale
        //    slot indices after deinit.
        xQueueReset(ble_event_queue);

        // 3. Wait for any in-flight pool slot to finish. Timeout 500ms.
        {
            unsigned long drain_start = millis();
            bool all_clear = false;
            while ((millis() - drain_start) < 500) {
                all_clear = true;
                for (int i = 0; i < BLE_POOL_SIZE; i++) {
                    if (__atomic_load_n(&ble_pool[i].in_use, __ATOMIC_ACQUIRE)) {
                        all_clear = false;
                        break;
                    }
                }
                if (all_clear) break;
                vTaskDelay(5 / portTICK_PERIOD_MS);
            }
            if (!all_clear) {
                Serial.println("[BLE] Warning: pool slots still in-use after drain timeout");
                for (int i = 0; i < BLE_POOL_SIZE; i++) {
                    __atomic_store_n(&ble_pool[i].in_use, 0u, __ATOMIC_RELEASE);
                }
            }
        }

        // 4. Reset write cursor so post-reinit callbacks start clean.
        __atomic_store_n(&ble_pool_write, 0u, __ATOMIC_RELEASE);

        // 5. Drop WiFi entirely so the allocator can coalesce free blocks.
        //    NimBLE init() needs ~20-30KB *contiguous*; under WiFi promiscuous
        //    the heap is fragmented (largest free block observed ~7.7KB in the
        //    field) and that block may not exist, which aborts the re-init and
        //    reboots the device. Turning WiFi off lets the heap compact first —
        //    same technique as export_restore_promiscuous().
        esp_wifi_set_promiscuous(false);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(WIFI_MODE_SETTLE_LONG_MS);

        // 6. Tear down and reinitialize the BLE stack while heap is contiguous.
        NimBLEDevice::deinit(true);
        delay(100);
        NimBLEDevice::init("");
        NimBLEDevice::setPowerLevel(BLE_TX_POWER);
        pBLEScan = NimBLEDevice::getScan();
        pBLEScan->setScanCallbacks(&ble_cb_singleton, false);
        pBLEScan->setActiveScan(false);
        apply_ble_scan_params();
        pBLEScan->setMaxResults(0);

        // 7. Restore WiFi promiscuous sniffing (mirrors export_restore_promiscuous).
        WiFi.mode(WIFI_STA);
        delay(WIFI_MODE_SETTLE_SHORT_MS);
        wifi_promiscuous_filter_t pf_restart;
        pf_restart.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&pf_restart);
        esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);

        // 8. Resume scanning.
        if (ScannerTaskHandle) vTaskResume(ScannerTaskHandle);
        scanner_ready = true;
        Serial.printf("[BLE] Periodic stack restart completed (WiFi cycled; free=%u largest=%u)\n",
                      (unsigned)esp_get_free_heap_size(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
}

static void redraw_now() { draw_current_screen(); render_frame(); }

static void service_stats_render() {
    if (screen_off_mode) return;   // panel asleep — compositing would be wasted
    if (ambient_mode) {
        // Render the normal scanner screen at reduced framerate (~15 fps).
        // Same layout, same feed, same viz — just dimmed via backlight.
        static unsigned long last_ambient_draw = 0;
        unsigned long now_amb = millis();
        if (now_amb - last_ambient_draw > 66) {
            // Deliberately NOT forcing the scanner screen here. Ambient used to
            // yank you back to screen 0 after the idle timeout, which meant
            // sitting on Stats or GPS for two minutes silently lost your place.
            // Ambient is a brightness/framerate state, not a navigation event.
            redraw_now();
            last_ambient_draw = now_amb;
        }
    } else {
        static unsigned long last_fast_anim = 0; static unsigned long last_slow_ui = 0; unsigned long now = millis();

        // Screen 4 escalates to the fast path while the eased scroll position
        // is still chasing its target — keeps the smooth scroll animation
        // running at ~60fps without permanently promoting stats to fast path.
        bool stats_scrolling = (current_screen == 4) &&
            (fabsf(stats_scroll_y_f - (float)stats_scroll_target) > 0.5f);

        // Same idea for per-character roll-ups: while any glyph anim is
        // still in flight (within UI_ANIM_QUICK of its start), drive the
        // fast path so the slide is actually animated frame-by-frame.
        bool stats_rolling = false;
        if (current_screen == 4) {
            for (int i = 0; i < STATS_CARD_COUNT && !stats_rolling; i++) {
                for (int ci = 0; ci < STAT_MAX_CHARS; ci++) {
                    if (stats_char_anim[i][ci] != 0 &&
                        (now - stats_char_anim[i][ci]) < UI_ANIM_QUICK) {
                        stats_rolling = true;
                        break;
                    }
                }
            }
        }

        // Detections screen escalates while the selection ease is chasing
        // its target or while the detail overlay open fade is in flight.
        // (Close is instant — no animation to gate.)
        bool hist_animating = false;
        if (current_screen == 2) {
            int hist_target_y = (history_selected_idx - history_scroll_offset) * HIST_ROW_H;
            bool sel_settling = fabsf(hist_sel_y_f - (float)hist_target_y) > 0.5f;
            bool open_running = (hist_detail_open &&
                                 hist_detail_open_ms != 0 &&
                                 (now - hist_detail_open_ms) < UI_FADE_IN_MS + 30);
            hist_animating = sel_settling || open_running;
        }

        if (menu_open ||
            current_screen == 0 || current_screen == 1 || current_screen == 3 ||
            show_vol_overlay || toast_active || stats_scrolling || stats_rolling ||
            hist_animating ||
            (now - last_fast_anim < 30)) {
            // Stats screen caps at 30 fps to suppress the SPI/scan-line
            // tearing that 60 fps pushes produced. Other animated screens
            // stay at 60 fps where the artifact isn't visible.
            // Low power caps at ~20fps: compositing the 54KB sprite and pushing
            // it over SPI is the heaviest CPU work in the app, and 60fps buys
            // nothing when the point of the mode is to charge.
            unsigned long min_frame_ms = low_power_mode ? 50
                                       : ((current_screen == 4) ? 33 : 15);
            if (now - last_fast_anim >= min_frame_ms) {
                redraw_now();
                last_fast_anim = now;
                screen_dirty = false;
            }
        }
        else {
            // Screen 4 has a live SESSION timer. Mark dirty every slow-UI
            // cycle (100ms) so the seconds digit updates within one tick of
            // the actual boundary. Roll animation is gated by stats_rolling,
            // so this doesn't promote stats to the fast path when idle.
            if (current_screen == 4) {
                screen_dirty = true;
            }
            if (now - last_slow_ui >= 100) {
                if (screen_dirty || toast_active) {
                    redraw_now();
                    screen_dirty = false;
                }
                last_slow_ui = now;
            }
        }
    }
}

static void handle_keyboard_input() {
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        last_user_input_ms = millis();
        screen_dirty = true;
        if (ambient_mode) {
            display_wake_from_idle();
            // Same I2S wake as the BtnA path.
            M5Cardputer.Speaker.stop();
        }
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        
        if (status.tab) {
            show_help_overlay = !show_help_overlay;
            if (show_help_overlay) {
                show_feed_expanded = false;
                help_ease_start = millis();
            }
            draw_current_screen(); render_frame();
        }

        // Tracks whether ENTER was already handled by the status.word loop
        // so the status.enter fallback below doesn't re-fire the same action
        // on firmwares that report ENTER in both places.
        bool enter_consumed = false;
        bool screen_transitioned = false;

        for (auto c : status.word) {

            if (c == '\n' || c == '\r') enter_consumed = true;

            // The keyboard never emits ASCII ESC on its own: the key labeled
            // "esc" is the '`' key, and the library reports it as '`' with
            // status.fn set when the Fn chord is held. Translate the chord
            // here so every `c == 0x1B` handler below actually fires —
            // without this they are all dead code.
            if (status.fn && c == '`') c = 0x1B;

            // ── WiFi Config input intercept ──
            // When the wifi config overlay is open, all keys are routed here.
            // In editing mode, printable chars feed the active text buffer.
            // In navigation mode, ; / . move between fields and ENTER acts.
            if (wifi_config_open) {
                if (wifi_config_editing) {
                    char* buf;
                    int max_len;
                    if (wifi_config_field == 0) {
                        buf = wifi_config_ssid_buf;
                        max_len = 32;
                    } else {
                        buf = wifi_config_pass_buf;
                        max_len = 64;
                    }
                    int cur_len = strlen(buf);

                    if (c == '\n' || c == '\r') {
                        // Confirm edit — exit text input mode
                        wifi_config_editing = false;
                        M5Cardputer.Speaker.tone(660, 5);
                    } else if (c == 0x1B) {
                        // ESC — cancel edit, revert to stored value
                        if (wifi_config_field == 0) {
                            strncpy(wifi_config_ssid_buf, export_ssid, sizeof(wifi_config_ssid_buf) - 1);
                            wifi_config_ssid_buf[sizeof(wifi_config_ssid_buf) - 1] = '\0';
                        } else {
                            strncpy(wifi_config_pass_buf, export_pass, sizeof(wifi_config_pass_buf) - 1);
                            wifi_config_pass_buf[sizeof(wifi_config_pass_buf) - 1] = '\0';
                        }
                        wifi_config_editing = false;
                        wifi_config_cursor = 0;
                    } else if (c >= 32 && c <= 126 && cur_len < max_len) {
                        // Insert at cursor
                        for (int i = cur_len + 1; i > wifi_config_cursor; i--) {
                            buf[i] = buf[i - 1];
                        }
                        buf[wifi_config_cursor] = c;
                        wifi_config_cursor++;
                    }
                    // DEL handled in status.del block below
                } else {
                    // Navigation mode — arrow keys move between fields
                    if (IS_KEY_UP(c)) {
                        wifi_config_field--;
                        if (wifi_config_field < 0) wifi_config_field = 0;
                        M5Cardputer.Speaker.tone(660, 5);
                    } else if (IS_KEY_DOWN(c)) {
                        wifi_config_field++;
                        if (wifi_config_field > 3) wifi_config_field = 3;
                        M5Cardputer.Speaker.tone(660, 5);
                    } else if (c == 's' && wifi_config_field == 1) {
                        // Toggle plaintext reveal of the password field.
                        wifi_config_show_pass = !wifi_config_show_pass;
                        M5Cardputer.Speaker.tone(660, 5);
                    } else if (c == '\n' || c == '\r') {
                        if (wifi_config_field == 0 || wifi_config_field == 1) {
                            wifi_config_editing = true;
                            if (wifi_config_field == 0) {
                                wifi_config_cursor = strlen(wifi_config_ssid_buf);
                            } else {
                                wifi_config_cursor = strlen(wifi_config_pass_buf);
                            }
                            M5Cardputer.Speaker.tone(660, 5);
                        } else if (wifi_config_field == 2) {
                            // Save
                            strncpy(export_ssid, wifi_config_ssid_buf, sizeof(export_ssid) - 1);
                            export_ssid[sizeof(export_ssid) - 1] = '\0';
                            strncpy(export_pass, wifi_config_pass_buf, sizeof(export_pass) - 1);
                            export_pass[sizeof(export_pass) - 1] = '\0';
                            save_wifi_credentials();
                            if (!persist_in_flight) schedule_persist();
                            set_toast_direct("WIFI SAVED", TOAST_SUCCESS);
                            wifi_config_open = false;
                            wifi_config_show_pass = false;  // never persist plaintext reveal
                        } else if (wifi_config_field == 3) {
                            // Clear
                            export_ssid[0] = '\0';
                            export_pass[0] = '\0';
                            wifi_config_ssid_buf[0] = '\0';
                            wifi_config_pass_buf[0] = '\0';
                            save_wifi_credentials();
                            if (!persist_in_flight) schedule_persist();
                            set_toast_direct("WIFI CLEARED", TOAST_WARNING, false);
                            wifi_config_open = false;
                            wifi_config_show_pass = false;  // never persist plaintext reveal
                        }
                    } else if (c == 0x1B || c == '`') {
                        // Accept a BARE '`' as well as the translated 0x1B. The
                        // key labelled "esc" is the '`' key and only reports
                        // status.fn when the Fn chord is held, so pressing the
                        // key the footer tells you to press ("ESC close") never
                        // reached the 0x1B branch — it fell through every case
                        // here and did nothing.
                        //
                        // Safe in navigation mode only: '`' is mute globally, but
                        // the overlay swallows every key, so it has no other
                        // meaning here. Editing mode deliberately still requires
                        // Fn+` — '`' is a legitimate character in an SSID or
                        // password and must stay typable.
                        wifi_config_open = false;
                        wifi_config_show_pass = false;  // never persist plaintext reveal
                    }
                }
                draw_current_screen(); render_frame();
                continue;  // swallow all keys while wifi config is open
            }

            // ── Menu navigation — swallow all keys while menu is open ──
            if (menu_open) {
                if (IS_KEY_UP(c)) {
                    stats_clear_confirming = false;   // moving off the item disarms
                    menu_selected = menu_next_idx(menu_selected, -1);
                    menu_click();
                } else if (IS_KEY_DOWN(c)) {
                    stats_clear_confirming = false;
                    menu_selected = menu_next_idx(menu_selected, +1);
                    menu_click();
                } else if (c == '\n' || c == '\r') {
                    // handle_menu_select() owns whether the menu closes. It still
                    // closes for every action (so the toast and the effect are
                    // visible), but the clear-stats confirm holds it open so the
                    // two ENTER presses stay adjacent.
                    handle_menu_select();
                } else if (c == 0x08 || c == 0x7F || c == 0x1B || c == 'm' || IS_KEY_LEFT(c)) {
                    stats_clear_confirming = false;
                    menu_open = false;
                    screen_dirty = true;
                }
                continue;  // swallow ALL keys while menu is open
            }



            if (c == 0x1B) {  // ASCII Escape
                // Priority order: close overlays first, then navigate home.
                if (export_mode_active || export_connecting) {
                    export_mode_stop();
                    screen_dirty = true;
                    continue;
                }
                if (menu_open) {
                    menu_open = false;
                    screen_dirty = true;
                    continue;
                }
                if (show_feed_expanded) {
                    show_feed_expanded = false;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (show_help_overlay) {
                    show_help_overlay = false;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (current_screen == 2 && hist_detail_open) {
                    hist_delete_confirming = false;
                    hist_detail_open = false;
                    screen_dirty = true;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (current_screen != 0) {
                    transition_screen(0, -1);
                    continue;
                }
                // Already on scanner with no overlays — nothing to do.
            }
            else if (c == 'm') {
                menu_open = !menu_open;
                stats_clear_confirming = false;   // every open/close starts disarmed
                if (menu_open) {
                    show_feed_expanded = false;
                    menu_open_ms = millis();
                    menu_scroll_offset = 0;
                    menu_scroll_y_f    = 0.0f;
                    menu_last_frame_ms = 0;
                    menu_sel_y_seeded  = false;
                    menu_click();
                }
                screen_dirty = true;
            }
            else if (c == '`') {
                // This key is labeled "esc" — during export it must act like
                // one, Fn chord or not. The footer promises "ESC stop export";
                // without this, pressing the esc key just toggled mute.
                if (export_mode_active || export_connecting) {
                    export_mode_stop();
                    screen_dirty = true;
                }
                else if (is_muted) {
                    is_muted = false;
                    if (current_volume == 0) current_volume = 75;
                    M5Cardputer.Speaker.setVolume(current_volume);
                    beep(600, 50);
                    set_toast_direct("UNMUTED", TOAST_SUCCESS);
                } else {
                    is_muted = true;
                    current_volume = 0;
                    M5Cardputer.Speaker.setVolume(0);
                    set_toast_direct("MUTED", TOAST_NEUTRAL);
                }
                screen_dirty = true;
            }
            else if (IS_KEY_UP(c)) {
                if (show_feed_expanded) {
                    if (feed_expanded_selected > 0) feed_expanded_selected--;
                    if (feed_expanded_selected < feed_expanded_offset)
                        feed_expanded_offset = feed_expanded_selected;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (current_screen == 2) {
                    if (hist_detail_open) { /* no nav while detail is open */ }
                    else if (history_selected_idx > 0) {
                        history_selected_idx--;
                        if (history_selected_idx < history_scroll_offset)
                            history_scroll_offset = history_selected_idx;
                        draw_current_screen(); render_frame();
                    }
                } else if (current_screen == 4) {
                    if (stats_scroll_target > 0) {
                        stats_scroll_target -= STATS_SCROLL_STEP;
                        if (stats_scroll_target < 0) stats_scroll_target = 0;
                        screen_dirty = true;
                    }
                }
                // Up never changes screen — only left/right do
            }
            else if (IS_KEY_DOWN(c)) {
                if (show_feed_expanded) {
                    // Range over the WHOLE feed, not the visible 6 — the old
                    // min(count, 6) cap made the oldest entries unreachable.
                    int max_sel = scan_local_count - 1;
                    if (max_sel < 0) max_sel = 0;
                    if (feed_expanded_selected < max_sel) feed_expanded_selected++;
                    if (feed_expanded_selected >= feed_expanded_offset + FEED_EXPANDED_ROWS)
                        feed_expanded_offset = feed_expanded_selected - FEED_EXPANDED_ROWS + 1;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (current_screen == 2) {
                    if (hist_detail_open) { /* no nav while detail is open */ }
                    else {
                        int hist_total = sd_available ? sd_hist_count : capture_history_count;
                        if (history_selected_idx < hist_total - 1) {
                            history_selected_idx++;
                            if (history_selected_idx >= history_scroll_offset + HIST_VISIBLE_ROWS)
                                history_scroll_offset = history_selected_idx - HIST_VISIBLE_ROWS + 1;
                            draw_current_screen(); render_frame();
                        }
                    }
                } else if (current_screen == 4) {
                    if (stats_scroll_target < STATS_MAX_SCROLL) {
                        stats_scroll_target += STATS_SCROLL_STEP;
                        if (stats_scroll_target > STATS_MAX_SCROLL)
                            stats_scroll_target = STATS_MAX_SCROLL;
                        screen_dirty = true;
                    }
                }
                // Down never changes screen — only left/right do
            }
            else if (IS_KEY_LEFT(c)) {
                if (export_mode_active) continue;
                if (show_feed_expanded) {
                    show_feed_expanded = false;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (!screen_transitioned) {
                    int prev = screen_step(current_screen, -1);
                    // Slide forward only when we wrapped past the start.
                    int d = (prev > current_screen) ? 1 : -1;
                    transition_screen(prev, d);
                    screen_transitioned = true;
                }
            }
            else if (IS_KEY_RIGHT(c)) {
                if (export_mode_active) continue;
                if (show_feed_expanded) {
                    show_feed_expanded = false;
                    draw_current_screen(); render_frame();
                    continue;
                }
                if (!screen_transitioned) {
                    int next = screen_step(current_screen, +1);
                    // Slide backward only when we wrapped past the end.
                    int d = (next < current_screen) ? -1 : 1;
                    transition_screen(next, d);
                    screen_transitioned = true;
                }
            }
            else if (c == '-') {
                // Volume down
                current_volume -= 15; if (current_volume < 0) current_volume = 0;
                if (current_volume == 0 && !is_muted) { is_muted = true; }
                M5Cardputer.Speaker.setVolume(current_volume); beep(400, 50);
                if (!show_vol_overlay) {
                    vol_overlay_start = millis(); show_vol_overlay = true;
                } else {
                    unsigned long el = millis() - vol_overlay_start;
                    if (el >= 120 && el <= 1380) vol_overlay_start = millis() - 120;
                }
                screen_dirty = true;
            }
            else if (c == '+' || c == '=') {
                // Volume up
                current_volume += 15; if (current_volume > 255) current_volume = 255;
                if (is_muted && current_volume > 0) { is_muted = false; }
                M5Cardputer.Speaker.setVolume(current_volume); beep(800, 50);
                if (!show_vol_overlay) {
                    vol_overlay_start = millis(); show_vol_overlay = true;
                } else {
                    unsigned long el = millis() - vol_overlay_start;
                    if (el >= 120 && el <= 1380) vol_overlay_start = millis() - 120;
                }
                screen_dirty = true;
            }
            else if (c == 'v') {
                if (current_screen == 0) {
                    scanner_viz_mode = (scanner_viz_mode + 1) % SCANNER_VIZ_COUNT;
                    screen_dirty = true;
                    menu_click();

                }
            }
            else if (c == 'd') {
                if (current_screen == 2 && hist_detail_open) {
                    if (hist_delete_confirming) {
                        // Second press of 'd' also confirms (alternative to ENTER)
                        perform_detection_delete(history_selected_idx);
                        hist_delete_confirming = false;
                        hist_detail_open       = false;
                    } else {
                        hist_delete_confirming = true;
                    }
                    screen_dirty = true;
                    draw_current_screen(); render_frame();
                }
            }
            else if (c == 'w') {
                if (current_screen == 2 && hist_detail_open && !hist_delete_confirming) {
                    int hist_total = sd_available ? sd_hist_count : capture_history_count;
                    if (hist_total > 0) {
                        const char* mac_to_wl = "";
                        int idx = history_selected_idx;
                        if (sd_available && idx < sd_hist_count)
                            mac_to_wl = sd_hist[idx].mac;
                        else if (idx < capture_history_count)
                            mac_to_wl = capture_history[idx].mac;

                        if (whitelist_add(mac_to_wl)) {
                            save_whitelist();
                            char wl_toast[TOAST_TEXT_LEN];
                            snprintf(wl_toast, sizeof(wl_toast), "WHITELISTED %d/%d", mac_whitelist_count, MAX_WHITELIST);
                            set_toast_direct(wl_toast, TOAST_SUCCESS);
                        } else if (is_mac_whitelisted(mac_to_wl)) {
                            set_toast_direct("ALREADY WHITELISTED", TOAST_NEUTRAL);
                        } else {
                            set_toast_direct("WHITELIST FULL", TOAST_WARNING, false);
                        }
                        screen_dirty = true;
                    }
                }
            }
#if DEBUG_KEYS
            else if (c == 'x') {
                static bool sim_wifi = true;
                char fake_mac[18];
                snprintf(fake_mac, sizeof(fake_mac), "00:11:22:33:%02X:%02X", random(0, 255), random(0, 255));

                if (sim_wifi) {
                    log_detection("SIMULATION", "WIFI", random(-80, -30), fake_mac, "Test_WiFi", 6, 0, "Beacon", "manual_test", 100, 1);
                    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                    session_flock_wifi--; lifetime_wifi--;
                    lifetime_flock_total--;
                    xSemaphoreGiveRecursive(dataMutex);
                } else {
                    log_detection("SIMULATION", "BLE", random(-90, -40), fake_mac, "Test_BLE", 0, 0, "Adv", "manual_test", 100, 1);
                    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                    session_flock_ble--; lifetime_ble--;
                    lifetime_flock_total--;
                    xSemaphoreGiveRecursive(dataMutex);
                }
                // Set alarm trigger under mutex — both fields together,
                // matching the producer pattern in process_wifi_event_queue
                // and ble_worker_task.
                xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                trigger_alarm_confidence = 100;
                trigger_alarm_source = sim_wifi ? 0 : 1;  // 0=WiFi, 1=BLE
                xSemaphoreGiveRecursive(dataMutex);

                sim_wifi = !sim_wifi;
            }
#endif // DEBUG_KEYS
            else if (c == 't') {
                if (show_feed_expanded) {
                    if (scan_local_count > 0 && feed_expanded_selected < scan_local_count) {
                        int idx = (scan_local_head - feed_expanded_selected + FEED_SIZE * 2) % FEED_SIZE;
                        FeedEntry& fe = scan_local_feed[idx];
                        if (fe.mac[0] != '\0') {
                            const char* type_str = fe.is_flock
                                ? (fe.proto == 0 ? "FLOCK_WIFI" : "FLOCK_BLE")
                                : (fe.proto == 0 ? "WIFI" : "BLE");
                            signal_start(fe.mac, fe.name, type_str, 0);
                            trigger_toast("TARGET", fe.name, 0);
                            // Already jumps to the target screen — which is now
                            // the only way in, since it is out of the carousel.
                            show_feed_expanded = false;
                            transition_screen(SCREEN_TARGET, 1);
                        }
                    }
                } else if (current_screen == 2 && hist_detail_open) {
                    // Target the detection currently shown in detail view
                    const char* t_mac = "";
                    const char* t_name = "";
                    const char* t_type = "";
                    int t_id = 0;
                    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                    int idx = history_selected_idx;
                    if (sd_available && idx < sd_hist_count) {
                        t_mac = sd_hist[idx].mac; t_name = sd_hist[idx].name;
                        t_type = sd_hist[idx].type; t_id = sd_hist[idx].id;
                    } else if (idx < capture_history_count) {
                        t_mac = capture_history[idx].mac; t_name = capture_history[idx].name;
                        t_type = capture_history[idx].type; t_id = capture_history[idx].id;
                    }
                    xSemaphoreGiveRecursive(dataMutex);
                    if (strlen(t_mac) > 0) {
                        signal_start(t_mac, t_name, t_type, t_id);
                        trigger_toast("TARGET", t_name, 0);
                        hist_detail_open = false;
                        transition_screen(SCREEN_TARGET, 1);
                    }
                } else if (capture_history_count > 0) {
                    static int target_select_idx = -1;
                    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
                    int current_hist_count = capture_history_count;
                    target_select_idx = (target_select_idx + 1) % current_hist_count;
                    char t_mac[18];  strncpy(t_mac,  capture_history[target_select_idx].mac,  17); t_mac[17]  = '\0';
                    char t_name[65]; strncpy(t_name, capture_history[target_select_idx].name, 64); t_name[64] = '\0';
                    char t_type[16]; strncpy(t_type, capture_history[target_select_idx].type, 15); t_type[15] = '\0';
                    int t_conf = capture_history[target_select_idx].confidence;
                    int t_id   = capture_history[target_select_idx].id;
                    xSemaphoreGiveRecursive(dataMutex);

                    signal_start(t_mac, t_name, t_type, t_id);
                    trigger_toast("TARGET", t_name, t_conf);
                    transition_screen(SCREEN_TARGET, 1);
                } else {
                    trigger_toast("INFO", "No targets yet", 0);
                }
            }
            else if (c == 'b') {
                brightness_level = (brightness_level + 1) % 4;
                M5Cardputer.Display.setBrightness(effective_brightness());
                // Disable LED at dim levels, re-enable at full brightness
                if (brightness_level < 3) {
                    led_breathing_on = false;
                } else {
                    led_breathing_on = true;
                }
                // On-screen feedback — and the reason nothing visibly
                // changes in low power, which pins the backlight to the
                // ambient dim regardless of the selected level.
                if (low_power_mode) {
                    set_toast_direct("LOW POWER LOCKS DIM", TOAST_WARNING);
                } else {
                    char bmsg[16];
                    snprintf(bmsg, sizeof(bmsg), "BRIGHTNESS %d/4", brightness_level + 1);
                    set_toast_direct(bmsg, TOAST_NEUTRAL);
                }
                schedule_persist();
            }
            else if (c == 'n') {
                night_mode = !night_mode;
                apply_color_palette();
                screen_dirty = true;
                schedule_persist();
                if (night_mode) {
                    set_toast_direct("NIGHT MODE", TOAST_SUCCESS);
                } else {
                    set_toast_direct("DAY MODE", TOAST_NEUTRAL);
                }
            }
            else if (c == 'c') {
                enter_charge_mode_reboot();
            }
            else if (c == 'l') {
                if (signal_active) {
                    signal_stop();
                    trigger_toast("TARGET", "Target cleared", 0);
                    beep(500, 60);
                    screen_dirty = true;
                }
            }
            else if (c == '\\') {
                // Single press: toggle LED breathing on/off (deferred to detect double-tap).
                // Double press: random LED color from palette.
                unsigned long now_ms = millis();

                if (bs_pending_exists && (now_ms - last_bs_press_ms) < DOUBLE_TAP_MS) {
                    bs_pending_exists = false;
                    bs_pending_until = 0;

                    int n_colors = (int)(sizeof(LED_COLORS) / sizeof(LED_COLORS[0]));
                    int new_idx;
                    if (n_colors > 1) {
                        do { new_idx = random(0, n_colors); } while (new_idx == led_col_idx);
                    } else {
                        new_idx = 0;
                    }
                    led_col_idx = new_idx;
                    led_r = LED_COLORS[led_col_idx][0];
                    led_g = LED_COLORS[led_col_idx][1];
                    led_b = LED_COLORS[led_col_idx][2];
                    if (!led_breathing_on) led_breathing_on = true;
                    beep(900, 40);
                } else {
                    bs_pending_exists = true;
                    bs_pending_until = now_ms + DOUBLE_TAP_MS;
                    last_bs_press_ms = now_ms;
                }
            }
            else if (c == 'f') {
                if (show_feed_expanded) {
                    show_feed_expanded = false;
                } else {
                    if (current_screen != 0 && current_screen != 1) transition_screen(0, -1);
                    menu_open        = false;
                    show_help_overlay = false;
                    wifi_config_open  = false;
                    show_feed_expanded = true;
                    feed_expand_ms = millis();
                    feed_expanded_selected = 0;
                    feed_expanded_offset   = 0;   // open at the newest entry
                }
                draw_current_screen();
                render_frame();
            }
            else if (c == '\n' || c == '\r') {
                if (current_screen == 2) {
                    int hist_total = sd_available ? sd_hist_count : capture_history_count;
                    if (hist_total > 0) {
                        if (hist_detail_open && hist_delete_confirming) {
                            perform_detection_delete(history_selected_idx);
                            hist_delete_confirming = false;
                            hist_detail_open       = false;
                        } else if (!hist_detail_open) {
                            hist_detail_open    = true;
                            hist_detail_open_ms = millis();
                        } else {
                            hist_detail_open       = false;
                            hist_delete_confirming = false;
                        }
                        screen_dirty = true;
                        draw_current_screen(); render_frame();
                    }
                }
                // ENTER on other screens is a no-op (menu handles navigation)
            }
        }

        // Fallback ENTER check — some Cardputer ADV firmware doesn't put
        // ENTER in status.word. Check status.enter directly only when the
        // loop above didn't already act on it (firmwares that report ENTER
        // in both places would otherwise toggle every action twice).
        if (status.enter && !enter_consumed) {
            if (wifi_config_open) {
                if (wifi_config_editing) {
                    wifi_config_editing = false;
                    menu_click();
                } else {
                    if (wifi_config_field == 0 || wifi_config_field == 1) {
                        wifi_config_editing = true;
                        if (wifi_config_field == 0) {
                            wifi_config_cursor = strlen(wifi_config_ssid_buf);
                        } else {
                            wifi_config_cursor = strlen(wifi_config_pass_buf);
                        }
                        menu_click();
                    } else if (wifi_config_field == 2) {
                        strncpy(export_ssid, wifi_config_ssid_buf, sizeof(export_ssid) - 1);
                        export_ssid[sizeof(export_ssid) - 1] = '\0';
                        strncpy(export_pass, wifi_config_pass_buf, sizeof(export_pass) - 1);
                        export_pass[sizeof(export_pass) - 1] = '\0';
                        save_wifi_credentials();
                        if (!persist_in_flight) schedule_persist();
                        set_toast_direct("WIFI SAVED", TOAST_SUCCESS);
                        wifi_config_open = false;
                        wifi_config_show_pass = false;  // never persist plaintext reveal
                    } else if (wifi_config_field == 3) {
                        export_ssid[0] = '\0';
                        export_pass[0] = '\0';
                        wifi_config_ssid_buf[0] = '\0';
                        wifi_config_pass_buf[0] = '\0';
                        save_wifi_credentials();
                        if (!persist_in_flight) schedule_persist();
                        set_toast_direct("WIFI CLEARED", TOAST_WARNING, false);
                        wifi_config_open = false;
                        wifi_config_show_pass = false;  // never persist plaintext reveal
                    }
                }
                draw_current_screen(); render_frame();
            } else if (menu_open) {
                menu_open = false;
                handle_menu_select();
            } else if (current_screen == 2) {
                int hist_total = sd_available ? sd_hist_count : capture_history_count;
                if (hist_total > 0) {
                    if (hist_detail_open && hist_delete_confirming) {
                        // ENTER confirms delete
                        perform_detection_delete(history_selected_idx);
                        hist_delete_confirming = false;
                        hist_detail_open       = false;
                    } else if (!hist_detail_open) {
                        hist_detail_open    = true;
                        hist_detail_open_ms = millis();
                    } else {
                        hist_detail_open = false;
                        hist_delete_confirming = false;
                    }
                    screen_dirty = true;
                    draw_current_screen(); render_frame();
                }
            }
        }

        if (status.del) {
            // DEL = universal "close / go back" — priority order:
            if (wifi_config_open) {
                if (wifi_config_editing) {
                    // Backspace within text input
                    char* buf = (wifi_config_field == 0) ? wifi_config_ssid_buf : wifi_config_pass_buf;
                    if (wifi_config_cursor > 0) {
                        int cur_len = strlen(buf);
                        for (int i = wifi_config_cursor - 1; i < cur_len; i++) {
                            buf[i] = buf[i + 1];
                        }
                        wifi_config_cursor--;
                    }
                } else {
                    // Not typing — DEL is the universal "close / go back", and
                    // this overlay is a thing to close. It used to instead jump
                    // into editing mode and eat the last character, which meant
                    // the one key that closes everything else silently mutated
                    // the field you were sitting on.
                    wifi_config_open      = false;
                    wifi_config_show_pass = false;   // never persist plaintext reveal
                    screen_dirty          = true;
                }
                draw_current_screen(); render_frame();
            } else if (show_feed_expanded) {
                show_feed_expanded = false;
                draw_current_screen(); render_frame();
            } else if (show_help_overlay) {
                show_help_overlay = false;
                draw_current_screen(); render_frame();
            } else if (menu_open) {
                menu_open = false;
                screen_dirty = true;
            } else if (current_screen == 2 && hist_detail_open) {
                hist_delete_confirming = false;
                hist_detail_open = false;
                screen_dirty = true;
                draw_current_screen(); render_frame();
            } else if (export_mode_active || export_connecting) {
                // DEL is the universal "close / go back" — export mode is a
                // thing to close. Without this (and with ESC previously dead)
                // the only exit was menu -> Stop Export.
                export_mode_stop();
                screen_dirty = true;
            } else if (current_screen != 0) {
                // Nothing to close — go home to scanner
                transition_screen(0, -1);
            }
        }
    }
}

static void handle_key_repeat() {
    // ── Key-hold repeat for the ; / . arrow keys ──
    // Runs OUTSIDE the isChange() block so it fires during sustained holds.
    // isChange() is edge-triggered — only true on press/release transitions,
    // not during a continuous hold. The repeat logic needs to run every
    // loop iteration to detect when HOLD_DELAY has elapsed.
    {
        static unsigned long arrow_hold_start = 0;
        static unsigned long arrow_last_repeat = 0;
        static char arrow_held_key = 0;
        const unsigned long HOLD_DELAY      = 500;  // ms before repeat starts
        const unsigned long REPEAT_INTERVAL = 150;  // ms between repeats

        bool up_held = false, down_held = false;
        if (!menu_open && !wifi_config_open) {
            Keyboard_Class::KeysState hold_st = M5Cardputer.Keyboard.keysState();
            for (auto c : hold_st.word) {
                if (IS_KEY_UP(c))   up_held   = true;
                if (IS_KEY_DOWN(c)) down_held = true;
            }
        }
        char cur_arrow = up_held ? ';' : (down_held ? '.' : 0);

        if (cur_arrow && cur_arrow == arrow_held_key) {
            unsigned long hold_dur = millis() - arrow_hold_start;
            if (hold_dur > HOLD_DELAY &&
                (millis() - arrow_last_repeat) > REPEAT_INTERVAL) {
                if (current_screen == 2 && !hist_detail_open) {
                    if (IS_KEY_UP(cur_arrow)) {
                        if (history_selected_idx > 0) {
                            history_selected_idx--;
                            if (history_selected_idx < history_scroll_offset)
                                history_scroll_offset = history_selected_idx;
                        }
                    } else {
                        int ht = sd_available ? sd_hist_count : capture_history_count;
                        if (history_selected_idx < ht - 1) {
                            history_selected_idx++;
                            if (history_selected_idx >= history_scroll_offset + HIST_VISIBLE_ROWS)
                                history_scroll_offset = history_selected_idx - HIST_VISIBLE_ROWS + 1;
                        }
                    }
                    screen_dirty = true;
                } else if (current_screen == 4) {
                    if (IS_KEY_UP(cur_arrow)) {
                        if (stats_scroll_target > 0) {
                            stats_scroll_target -= STATS_SCROLL_STEP;
                            if (stats_scroll_target < 0) stats_scroll_target = 0;
                        }
                    } else {
                        if (stats_scroll_target < STATS_MAX_SCROLL) {
                            stats_scroll_target += STATS_SCROLL_STEP;
                            if (stats_scroll_target > STATS_MAX_SCROLL)
                                stats_scroll_target = STATS_MAX_SCROLL;
                        }
                    }
                    screen_dirty = true;
                }
                // LEFT/RIGHT omitted: screen transitions don't benefit from
                // auto-repeat and would oscillate due to animation overlap.
                arrow_last_repeat = millis();
            }
        } else if (cur_arrow) {
            arrow_held_key = cur_arrow;
            arrow_hold_start = millis();
            arrow_last_repeat = millis();
        } else {
            arrow_held_key = 0;
        }
    }
}

static void handle_pending_backslash() {
    // Fire pending '\' single-press action if double-tap window expired.
    // Signed-diff compare handles the 49-day millis() wraparound edge case.
    if (bs_pending_exists && (long)(millis() - bs_pending_until) >= 0) {
        bs_pending_exists = false;
        bs_pending_until = 0;
        led_breathing_on = !led_breathing_on;
        beep(led_breathing_on ? 800 : 400, 30);
    }
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    service_watchdog();
    M5Cardputer.update(); yield();

    service_export_mode();

    // Dynamically calculate expected hardware voltage sag for this loop iteration
    update_load_sag();

    int32_t loop_mv = get_filtered_voltage();

#if DEBUG_BAT_LOG
    static uint32_t _bat_dbg_ms = 0;
    if (millis() - _bat_dbg_ms >= 1000) {
        _bat_dbg_ms = millis();
        Serial.printf("[bat] raw=%dmV filtered=%dmV sag=%dmV charging=%d\n",
            (int)M5Cardputer.Power.getBatteryVoltage(),
            (int)get_filtered_voltage(),
            (int)current_load_sag_mv,
            (int)M5Cardputer.Power.isCharging());
    }
#endif

    service_battery_warnings(loop_mv);

    service_stack_health();

    service_heap_health();

    if (pending_delete_count > 0 && pending_delete_dirty_ms != 0 &&
        (millis() - pending_delete_dirty_ms) > 5000)
        flush_pending_deletes();

    process_wifi_event_queue();
    process_c5_serial();        // drain 5 GHz hits from the C5 (self-guards when link is off)
    service_c5_probe();         // latch TX/RX orientation before anything transmits
    service_c5_link();          // sig + time push, presence-edge driven
    feed_commit_pending();
    update_channel_histogram();

    // Gap-fill: push a floor-level sample when the target is silent so the
    // trace curve drops rather than freezing at the last reading.
    if (signal_active) {
        unsigned long now = millis();
        if ((now - sig_trace_last_sample) >= SIG_TRACE_INTERVAL_MS) {
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            if (signal_active && (millis() - sig_trace_last_sample) >= SIG_TRACE_INTERVAL_MS) {
                sig_trace[sig_trace_head].rssi = (int8_t)RSSI_VIS_FLOOR;
                sig_trace_head = (sig_trace_head + 1) % SIG_TRACE_SIZE;
                if (sig_trace_count < SIG_TRACE_SIZE) sig_trace_count++;
                sig_trace_last_sample = millis();
                sig_trace_total++;
            }
            xSemaphoreGiveRecursive(dataMutex);
        }
    }
    if (wdt_subscribed) esp_task_wdt_reset();

    int conf_snapshot = 0;
    int src_snapshot = 0;
    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    if (trigger_alarm_confidence >= 50) {
        conf_snapshot = trigger_alarm_confidence;
        src_snapshot = trigger_alarm_source;
    }
    trigger_alarm_confidence = 0;
    trigger_alarm_source = 0;
    xSemaphoreGiveRecursive(dataMutex);

    if (conf_snapshot >= 50) {
        play_escalated_alarm(conf_snapshot, src_snapshot);
    }
    if (wdt_subscribed) esp_task_wdt_reset();

    if (M5Cardputer.BtnA.wasClicked()) {
        last_user_input_ms = millis();
        if (ambient_mode) {
            display_wake_from_idle();
            // Wake the I2S peripheral from idle so the next tone() call
            // (often a UI click or alarm chime) doesn't hit a DMA assertion.
            M5Cardputer.Speaker.stop();
        }
        if (menu_open) {
            menu_open = false;
            screen_dirty = true;
        } else if (show_feed_expanded) {
            show_feed_expanded = false;
            redraw_now();
        } else if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) {
            // Only advance on BtnA when no keyboard key is simultaneously
            // pressed — prevents double-transition when BtnA fires
            // alongside an arrow key on the Cardputer keyboard deck.
            if (!export_mode_active) {
                int next_screen = screen_step(current_screen, +1);
                int dir = (next_screen < current_screen) ? -1 : 1;
                transition_screen(next_screen, dir);
            }
        }
    }

    handle_keyboard_input();

    if (wdt_subscribed) esp_task_wdt_reset();

    handle_key_repeat();

    handle_pending_backslash();

    if (millis() - last_time_save >= 1000) { xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY); lifetime_seconds++; xSemaphoreGiveRecursive(dataMutex); last_time_save = millis(); }
    if (millis() - last_persist_save >= current_persist_interval_ms()) {
        // Only advance the gate when the spawn actually took. If heap was
        // too low to allocate the task stack, retry on the next loop tick
        // (~10ms) instead of waiting another full PERSIST_INTERVAL.
        if (schedule_persist()) {
            last_persist_save = millis();
        }
    }
    rssi_track_expire();
    if (take_data_mutex()) {
        seen_mac_expire();
        give_data_mutex();
    }

    service_ble_restart();

    service_sd_hotplug();

    service_gps_timezone();

    service_gps_standby_refresh();

    service_ambient_mode();

    service_stats_render();
    vTaskDelay(10 / portTICK_PERIOD_MS);
}
