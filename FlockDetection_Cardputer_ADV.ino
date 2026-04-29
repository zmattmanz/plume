// ============================================================================
// FLOCK DETECTOR v9.0-ADV — Tactical Edition (Build 2)
// ============================================================================

#include <M5Cardputer.h>
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
#include "driver/gpio.h"
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <math.h>
#include "ui_beep.h"  // PCM sound for screen transitions

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Boot stage personalities — must be defined before any function that returns
// it, including Arduino's auto-generated forward declarations near the top of
// the translation unit.
enum BootPersonality {
    BOOT_NORMAL,   // smooth advance, brief settle
    BOOT_LURCH,    // overshoot target slightly, settle back
    BOOT_RUSH,     // fast advance, no settle dwell
    BOOT_STALL,    // pause near target, then snap home
};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void draw_header_spr(int screen_num);
void draw_toast_spr();
void draw_vol_overlay();
void drawCard(int x, int y, int w, int h);
static void drawPill(int x, int y, const char* text, uint16_t accent_col,
                     float bg_accent_pct = 0.18f, bool filled = false);
void draw_current_screen();
void transition_screen(int new_screen, int dir);
void play_escalated_alarm(int confidence, int source);
void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b);
void beep(int frequency, int duration_ms);
void apply_color_palette();
void draw_help_overlay();
void draw_menu_overlay();
void handle_menu_select();
void save_stats_to_sd();
void draw_feed_expanded_overlay();
void draw_wifi_config_overlay();
static void save_wifi_credentials();
static void load_wifi_credentials();
void draw_gps_screen();
void load_sd_history();
void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type);
static void set_toast_direct(const char* text, uint16_t accent);
static void apply_ble_scan_params();
struct WifiEvent;
static void parse_wifi_event(struct WifiEvent* ev);

// ============================================================================
// DISPLAY & PALETTE VARIABLES (Swappable for Night Mode)
// =====================================================================================
#define DISP_W       240
#define DISP_H       135

uint16_t BG_COLOR, CARD_COLOR, CARD_BORDER;
uint16_t HEADER_COLOR, TEXT_COLOR, DIM_COLOR;
uint16_t ACCENT_COLOR, TEAL_COLOR, PURPLE_COLOR;
uint16_t CAUTION_COLOR, GPS_COLOR;
bool night_mode = false;
bool show_help_overlay = false;
static unsigned long help_ease_start = 0;

// Ambient mode: dim minimal UI after sustained idle
static unsigned long last_user_input_ms = 0;
static const unsigned long AMBIENT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
static const uint8_t AMBIENT_BRIGHTNESS = 40;
static bool ambient_mode = false;

unsigned long vol_overlay_start = 0;
bool show_vol_overlay = false;
static bool show_feed_expanded = false;
static unsigned long feed_expand_ms = 0;
bool north_mode = false;
int  brightness_level = 2;  // 0=dim, 1=mid, 2=full — cycled by 'b' key

// ── WiFi Config overlay state ──
static bool wifi_config_open = false;
static int  wifi_config_field = 0;        // 0 = SSID, 1 = Password, 2 = Save, 3 = Clear
static bool wifi_config_editing = false;   // true = text input mode active
static char wifi_config_ssid_buf[33] = "";
static char wifi_config_pass_buf[65] = "";
static int  wifi_config_cursor = 0;        // cursor position in active field
static unsigned long wifi_config_open_ms = 0;

// ── Menu overlay state ──
static bool show_menu = false;
static int  menu_selected = 0;
static unsigned long menu_open_ms = 0;
#define MENU_ITEM_COUNT 11
static int   menu_scroll_offset = 0;        // index of first visible item

// Low-power mode: reduces scan cadence across WiFi/BLE for longer runtime
static bool low_power_mode = false;
static const int BRIGHTNESS_LEVELS[3] = {40, 120, 255};

// RGB LED state — color cycles with C key, on/off with L when locator idle
static uint8_t led_r = 110, led_g = 200, led_b = 255; // default ice blue (matches ACCENT_COLOR)
static bool    led_breathing_on = true;
static const uint8_t LED_COLORS[][3] = {
    {110, 200, 255},  // ice blue (matches ACCENT_COLOR — default)
    { 80, 200, 255},  // cyan
    {  0, 200,   0},  // green
    {  0, 160, 200},  // teal
    {200, 100, 255},  // purple
    {255, 255, 255},  // white
    {255,  80,   0},  // orange
    {255,  30,  30},  // red
};
static int led_col_idx = 0;

// Detection-flash state — overrides breathing color briefly on new detection
static unsigned long led_detection_flash_until = 0;
static uint8_t  led_detect_r = 0, led_detect_g = 0, led_detect_b = 0;
static bool     led_detect_active = false;

void apply_color_palette() {
    if (night_mode) {
        // Night: red chrome, lifted dim for readability, amber caution.
        BG_COLOR      = lgfx::color565( 10,   0,   0);   // #0A0000
        CARD_COLOR    = lgfx::color565( 58,  10,  10);   // #3A0A0A
        CARD_BORDER   = lgfx::color565( 90,  20,  20);   // #5A1414
        HEADER_COLOR  = lgfx::color565(255,  90,  90);   // #FF5A5A
        TEXT_COLOR    = lgfx::color565(255, 208, 208);   // #FFD0D0
        DIM_COLOR     = lgfx::color565(180,  90,  90);   // #B45A5A lifted red-dim
        ACCENT_COLOR  = lgfx::color565(255,  90,  90);   // = HEADER (unified)
        CAUTION_COLOR = lgfx::color565(255, 181,  71);   // #FFB547 amber
        TEAL_COLOR    = lgfx::color565(255, 208, 208);   // = TEXT (collapsed)
        PURPLE_COLOR  = lgfx::color565(255, 208, 208);   // = TEXT (collapsed)
        GPS_COLOR     = lgfx::color565(255,  90,  90);   // = HEADER (collapsed)
    } else {
        // Palette #3: ice blue + coral + bright steel.
        // Softer, wider header hue reads as calm-professional.
        // Coral caution is warmer and more urgent than amber.
        // Dim lifted to #95A5B8 for card-background readability.
        BG_COLOR      = lgfx::color565(  5,  10,  20);   // #050A14
        CARD_COLOR    = lgfx::color565( 29,  50,  88);   // #1D3258
        CARD_BORDER   = lgfx::color565( 46,  70, 112);   // #2E4670
        HEADER_COLOR  = lgfx::color565(110, 200, 255);   // #6EC8FF ice blue
        TEXT_COLOR    = lgfx::color565(232, 239, 255);   // #E8EFFF
        DIM_COLOR     = lgfx::color565(149, 165, 184);   // #95A5B8 bright steel
        ACCENT_COLOR  = lgfx::color565(110, 200, 255);   // = HEADER (unified)
        TEAL_COLOR    = lgfx::color565(232, 239, 255);   // = TEXT (collapsed)
        PURPLE_COLOR  = lgfx::color565(232, 239, 255);   // = TEXT (collapsed)
        CAUTION_COLOR = lgfx::color565(255, 123,  92);   // #FF7B5C coral
        GPS_COLOR     = lgfx::color565(110, 200, 255);   // = HEADER (collapsed)
    }
}

// ── Module-level rendering helpers ──────────────────────────────────────────
// lerp_col16: linearly interpolate two RGB565 colours by t ∈ [0,1]
static inline uint16_t lerp_col16(uint16_t fc, uint16_t tc, float t) {
    int fr=((fc>>11)&0x1F)<<3, fg=((fc>>5)&0x3F)<<2, fb=(fc&0x1F)<<3;
    int tr=((tc>>11)&0x1F)<<3, tg=((tc>>5)&0x3F)<<2, tb=(tc&0x1F)<<3;
    return lgfx::color565((uint8_t)(fr+(tr-fr)*t),(uint8_t)(fg+(tg-fg)*t),(uint8_t)(fb+(tb-fb)*t));
}

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

static void safe_copy(char* dest, const char* src, size_t dest_size) {
    if (dest_size == 0) return;
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
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

// Overlay/toast fade tiers — names alias the purpose so call sites
// can self-document without inventing new durations.
static const unsigned long UI_FADE_IN_MS  = 150;  // overlays/toasts appearing
static const unsigned long UI_FADE_OUT_MS = 200;  // overlays/toasts disappearing

// Pulse period vocabulary — three tiers for sin-based oscillators.
// Slow: calm/idle. Medium: attention/status. Fast: active/urgent.
static const unsigned long UI_PULSE_BREATHE = 1200;
static const unsigned long UI_PULSE_MEDIUM  = 600;
static const unsigned long UI_PULSE_FAST    = 300;

// ── Type scale — named tiers replacing magic textSize values ──
// Maps the design system's 5-tier scale to hardware setTextSize() values.
//
// Tier guide:
//   TS_MICRO  — footnotes, RSSI dBm, footer hints, scrollbar labels
//   TS_BODY   — primary UI: labels, feed rows, status badges, kprint text
//   TS_STRONG — hero inline values: target name, stat card numbers
//   TS_H2     — menu items, overlay titles, locator distance
//   TS_H1     — big numerals: scanner counters, ambient mode count
static const float TS_MICRO  = 1.0f;
static const float TS_BODY   = 1.2f;
static const float TS_STRONG = 1.5f;
static const float TS_H2     = 2.0f;
static const float TS_H1     = 3.0f;

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
#define GPS_BAUD     115200 

#define MAX_CHANNEL 13
#define BLE_SCAN_DURATION 2
#define BLE_SCAN_INTERVAL      3000   // normal inter-scan gap (ms)
#define BLE_SCAN_INTERVAL_LOCK  800   // boosted gap during 10s channel lock window
#define BUZZER_COOLDOWN 60000
#define IGNORE_WEAK_RSSI -80

#define MAX_LOG_BUFFER 6
#define MAX_PCAP_BUFFER 5
#define SD_FLUSH_INTERVAL 10000
#define CHANNEL_DWELL_MS 400

#define REDETECT_WINDOW_MS 300000

#define RSSI_TRACK_MAX_DEVICES 16
#define RSSI_TRACK_SAMPLES 5
#define RSSI_TRACK_EXPIRY_MS 15000

#define PERSIST_INTERVAL_MS 60000
#define FLASH_FAIL_TOAST_THRESHOLD 3
#define PERSIST_FILE  "/flock_session.dat"
#define DETECT_FILE   "/flock_detections.txt"
#define TOAST_DURATION_MS 3500

#define LOC_MAX_SAMPLES 40
#define LOC_SAMPLE_INTERVAL 500
#define LOC_MIN_SAMPLES_EST 3

#define SCORE_DEFINITIVE 100  
#define SCORE_STRONG     60   
#define SCORE_WEAK       25   
#define SCORE_BONUS_RSSI 10   
#define SCORE_BONUS_STAT 15   

#define CONFIDENCE_ALARM_THRESHOLD 75   
#define CONFIDENCE_HIGH            85   
#define CONFIDENCE_CERTAIN         100  

#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_CS_PIN       12

// Version string — single source of truth
#define VERSION_STRING "FLOCK DETECTOR v9.6"

// Pre-configure WiFi credentials for export mode. User edits these in source
// once, then they're saved to flash on first boot. To change later, edit
// source and re-flash. This is a hobby/field-tool compromise.
#define EXPORT_WIFI_SSID "YOUR_SSID_HERE"
#define EXPORT_WIFI_PASS "YOUR_PASSWORD_HERE"

// Compile-time guard: screen name array in draw_header_spr() must stay in sync.
#define NUM_SCREENS 5

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

static uint8_t current_channel = 1;
static unsigned long last_channel_hop = 0;
static volatile unsigned long channel_lock_until = 0; 
static unsigned long last_ble_scan = 0;
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
bool littlefs_available = false;
volatile int trigger_alarm_confidence = 0;
volatile int trigger_alarm_source = 0;   // 0 = WiFi, 1 = BLE
volatile bool is_alarming = false;

#define SD_LINE_LEN 384
char sd_write_buffer[MAX_LOG_BUFFER][SD_LINE_LEN];
int  sd_write_count = 0;
unsigned long last_sd_flush = 0;
static int flash_write_fail_count = 0; 

static const char* current_log_file = "/FLOCK_FINDER/logs/FlockLog.csv";
static const char* current_pcap_file = "/FLOCK_FINDER/captures/Threats.pcap";
static const char* current_ble_pcap_file = "/FLOCK_FINDER/captures/BLE_Threats.pcap";

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

int current_screen = 0;
bool system_fully_booted = false;
static bool screen_dirty = true;   // Forces redraw; set by any state change
bool stealth_mode = false;
bool is_muted = false;       
int current_volume = 150;    

long session_wifi = 0;
long session_ble = 0;
long session_flock_wifi = 0;
long session_flock_ble = 0;
long session_raven = 0;

unsigned long session_start_time = 0;
unsigned long lifetime_seconds = 0;

long lifetime_wifi = 0;
long lifetime_ble = 0;
long lifetime_flock_total = 0;
long lifetime_boots = 0;
long lifetime_flash_writes = 0;

#define MAX_SEEN_MACS 100

struct SeenMacEntry {
    char   mac[18];
    unsigned long ts;
    bool   occupied;
};
static SeenMacEntry seen_mac_table[MAX_SEEN_MACS * 2];
static uint32_t seen_mac_evict_cursor = 0;

static uint32_t hash_mac(const char* mac) {
    uint32_t h = 2166136261u;
    for (const char* p = mac; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

static const int SEEN_MAC_TABLE_SIZE = MAX_SEEN_MACS * 2;
// Bound the linear-probe walk. Once eviction starts overwriting slots
// out-of-band, an unbounded probe degrades into a full-table linear
// scan and the linear-probe invariant is violated anyway. Capping at 8
// keeps lookup cost O(1); occasional false negatives just trigger a
// re-toast within the 5-minute redetect window — acceptable.
static const int SEEN_MAC_PROBE_LIMIT = 8;

bool is_mac_recently_seen(const char* mac) {
    const char* key = mac;
    uint32_t idx = hash_mac(key) % SEEN_MAC_TABLE_SIZE;
    unsigned long now = millis();
    for (int probe = 0; probe < SEEN_MAC_PROBE_LIMIT; probe++) {
        int slot = (idx + probe) % SEEN_MAC_TABLE_SIZE;
        if (!seen_mac_table[slot].occupied) return false;
        if (strncmp(seen_mac_table[slot].mac, key, 17) == 0) {
            if ((now - seen_mac_table[slot].ts) < REDETECT_WINDOW_MS) return true;
            seen_mac_table[slot].ts = now;
            return false;
        }
    }
    return false;
}

void add_seen_mac(const char* mac) {
    const char* key = mac;
    uint32_t idx = hash_mac(key) % SEEN_MAC_TABLE_SIZE;
    for (int probe = 0; probe < SEEN_MAC_PROBE_LIMIT; probe++) {
        int slot = (idx + probe) % SEEN_MAC_TABLE_SIZE;
        if (!seen_mac_table[slot].occupied ||
            strncmp(seen_mac_table[slot].mac, key, 17) == 0) {
            strncpy(seen_mac_table[slot].mac, key, 17);
            seen_mac_table[slot].mac[17] = '\0';
            seen_mac_table[slot].ts       = millis();
            seen_mac_table[slot].occupied = true;
            return;
        }
    }
    // Round-robin eviction: O(1), approximates LRU well enough given the
    // 5-minute redetect window and 400-slot capacity. The cursor advances
    // monotonically so we don't repeatedly evict the same slot.
    int evict = (int)(seen_mac_evict_cursor % SEEN_MAC_TABLE_SIZE);
    seen_mac_evict_cursor++;
    strncpy(seen_mac_table[evict].mac, key, 17);
    seen_mac_table[evict].mac[17] = '\0';
    seen_mac_table[evict].ts       = millis();
    seen_mac_table[evict].occupied = true;
}

char last_cap_type[16]       = "None";
char last_cap_mac[18]        = "--:--:--:--:--:--";
char last_cap_name[65]       = "";
int  last_cap_rssi           = 0;
int  last_cap_confidence     = 0;
char last_cap_time[9]        = "00:00:00";
char last_cap_det_method[64] = "";
int  last_cap_seq_num        = -1;

#define CAPTURE_HISTORY_SIZE 5
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

// SD-backed detection history (recent detections screen)
#define SD_HIST_SIZE 20
struct SDHistEntry {
    char type[16];
    char mac[18];
    char name[32];
    int  rssi;
    int  confidence;
    char method[24];
    char timestamp[9];
    int  id;        // sequential detection number; 0 = loaded from old SD log
};
SDHistEntry sd_hist[SD_HIST_SIZE];
int  sd_hist_count      = 0;
int  history_selected_idx = 0;
bool hist_detail_open   = false;
volatile bool sd_hist_dirty = false;

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
static unsigned long hist_detail_close_ms = 0;
static bool          hist_detail_closing  = false;

// Stats screen vertical scroll (Option D — uniform card grid, smoothed).
// Keys set stats_scroll_target (instant). Renderer eases stats_scroll_y_f
// toward the target via anim_filter() with a snappy 120 ms time constant.
static int   stats_scroll_target  = 0;
static float stats_scroll_y_f     = 0.0f;
static unsigned long stats_last_frame_ms = 0;
static const int STATS_CONTENT_H   = 302;  // hero 50 + 6×36 + 6×6 gaps
static const int STATS_VIEW_H      = 115;  // DISP_H - CONTENT_Y
static const int STATS_SCROLL_STEP = 42;   // one standard card (36) + gap (6)
static const int STATS_MAX_SCROLL  = STATS_CONTENT_H - STATS_VIEW_H;
static const float STATS_SCROLL_TC = 80.0f;   // ms — snappier; 120 felt sluggish

// Stats screen roll-up animation state — one slot per card index.
// New value vs prev value is checked each draw; if changed, roll_start
// kicks off an anim_slide_in() roll for UI_ANIM_QUICK ms. PACKETS is
// additionally throttled to a 3 s update cadence so it doesn't churn.
enum StatsCardIdx {
    SC_DETECTIONS = 0, SC_WIFI, SC_BLE, SC_RAVEN,
    SC_SESSION, SC_LIFETIME,
    SC_BATTERY, SC_HEAP,
    SC_PACKETS, SC_SD,
    SC_BOOTS, SC_FLASH, SC_VERSION,
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

#define TOAST_QUEUE_SIZE 3
struct ToastEntry {
    char text[32];
    uint16_t accent;
    bool is_action;
};
static ToastEntry toast_queue[TOAST_QUEUE_SIZE];
static int toast_queue_head = 0;
static int toast_queue_count = 0;
static unsigned long toast_start = 0;
static bool toast_active = false;

// Compatibility shims for direct-write sites (battery warnings, flash errors)
static char toast_text[32] = "";
static uint16_t toast_accent_color = 0;
static bool toast_is_action = false;

unsigned long last_time_save = 0;
unsigned long last_sd_flush_check = 0;
unsigned long last_persist_save = 0;
unsigned long last_blip_time = 0;

// ── Live activity feed (scanner screen) ──
#define FEED_SIZE 8
#define FEED_DEDUP_WINDOW_MS   30000UL
#define FEED_MIN_PUSH_INTERVAL_MS 800UL

struct FeedEntry {
    char     mac[18];
    char     name[20];
    int8_t   rssi;
    uint8_t  proto;        // 0=WiFi, 1=BLE
    bool     is_flock;
    unsigned long timestamp;
};
static FeedEntry feed_entries[FEED_SIZE];
static int feed_count = 0;
static int feed_head  = 0;
static unsigned long last_feed_push_ms = 0;
static FeedEntry feed_pending;
static bool feed_pending_valid = false;

static const unsigned long DOUBLE_TAP_MS = 400;
static bool bs_pending_exists = false;
static unsigned long bs_pending_until = 0;
static unsigned long last_bs_press_ms = 0;

struct RSSITrack { 
    char mac[18];
    int samples[RSSI_TRACK_SAMPLES]; 
    int sample_count; 
    unsigned long last_seen; 
    bool scored; 
};
RSSITrack rssi_tracker[RSSI_TRACK_MAX_DEVICES];
int rssi_tracker_count = 0;
static int locator_tracker_idx = -1;  // cached index into rssi_tracker for locator target

struct LocSample { 
    double lat; 
    double lng; 
    int rssi; 
    unsigned long timestamp; 
};

bool locator_active = false;
char locator_target_mac[18]  = "";
char locator_target_name[65] = "";
char locator_target_type[8]  = "";   // "WiFi", "BLE", or ""
int  locator_target_id       = 0;    // sequential detection ID; 0 = unknown
LocSample locator_samples[LOC_MAX_SAMPLES];
int locator_sample_count = 0;
unsigned long locator_last_sample = 0;
unsigned long locator_newest_sample_ms = 0;  

double locator_est_lat = 0.0;
double locator_est_lng = 0.0;
float locator_est_distance = 0.0;
float locator_bearing = 0.0;
float locator_confidence_radius = 0.0;
bool locator_has_estimate = false;
int locator_peak_rssi = -120;
bool locator_estimate_announced = false;

// Trend tracking: last N distance samples for "getting warmer" arrow
#define LOC_TREND_SAMPLES 8
static float locator_dist_history[LOC_TREND_SAMPLES] = {0};
static int locator_dist_history_count = 0;
static int locator_dist_history_head = 0;
static unsigned long locator_last_trend_sample_ms = 0;
volatile bool locator_announce_pending = false;

TinyGPSPlus gps;
HardwareSerial SerialGPS(2);

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
    if (!stealth_mode && !is_muted) { 
        M5Cardputer.Speaker.tone(frequency, duration_ms); 
    }
}

void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(21, r, g, b);
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
        if (led_detect_active && now < led_detection_flash_until) {
            float pulse = anim_pulse(UI_PULSE_FAST);
            r = (uint8_t)((float)led_detect_r * pulse);
            g = (uint8_t)((float)led_detect_g * pulse);
            b = (uint8_t)((float)led_detect_b * pulse);
        } else {
            led_detect_active = false;
            if (led_breathing_on && !stealth_mode && brightness_level >= 2) {
                float breath = anim_pulse(UI_PULSE_BREATHE);
                float dim    = 0.15f + breath * 0.35f;
                r = (uint8_t)((float)led_r * dim);
                g = (uint8_t)((float)led_g * dim);
                b = (uint8_t)((float)led_b * dim);
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
const float EMA_ALPHA = 0.05f;

// Load-Aware Telemetry Variables
static int32_t current_load_sag_mv = 0;
const int32_t SAG_WIFI_PROMISC = 45; // Estimated mV drop for continuous Wi-Fi Rx
const int32_t SAG_BLE_SCAN = 35;     // Estimated mV drop for active BLE scanning
const int32_t SAG_SPEAKER = 80;      // Estimated mV drop during active PCM audio playback

int32_t get_filtered_voltage() {
    static uint32_t last_adc_ms = 0;
    static int32_t cached_raw_mv = 0;
    uint32_t now_adc = (uint32_t)millis();
    if (cached_raw_mv != 0 && (now_adc - last_adc_ms) < 250) {
        return (int32_t)ema_voltage;
    }
    // Inject anticipated peripheral voltage sag before applying the EMA filter
    int32_t raw_mv = M5Cardputer.Power.getBatteryVoltage() + current_load_sag_mv;
    cached_raw_mv = raw_mv;
    last_adc_ms = now_adc;
    if (ema_voltage == 0.0f) {
        ema_voltage = (float)raw_mv;
    }
    ema_voltage = (EMA_ALPHA * raw_mv) + ((1.0f - EMA_ALPHA) * ema_voltage);
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

// ============================================================================
// SIGNATURE DATABASE
// ============================================================================
static const char* wifi_ssid_patterns[] = {
    "FS Ext Battery", "Penguin", "Pigvision", "FlockOS",
    "flocksafety", "OFS_IoT", "PFS_", "test_flck"
};
static const int NUM_SSID_PATTERNS = sizeof(wifi_ssid_patterns) / sizeof(wifi_ssid_patterns[0]);

static const char* mac_prefixes_tier1[] = {
    "8c:1f:64", "4c:6e:44",
    "ec:1b:bd", "58:8e:81", "d8:a0:d8",
    "dc:54:75", "e0:e2:e6", "f0:9f:c2", "68:b6:b3", "a0:b7:65", "24:0a:c4",
    "a4:e5:7c", "78:e3:6d", "fc:f5:c4", "b0:b2:1c",
    "b4:1e:52", "90:35:ea", "f0:82:c0", "b4:e3:f9", "04:0d:84"
};
static const int NUM_MAC_TIER1 = sizeof(mac_prefixes_tier1) / sizeof(mac_prefixes_tier1[0]);

static const char* mac_prefixes_tier2[] = {
    "48:e7:29", "74:4c:a1", "94:34:69", "38:5b:44",
    "94:08:53", "1c:34:f1", "a4:cf:12",
    "3c:91:80", "80:30:49", "14:5a:fc", "9c:2f:9d", "e4:aa:ea",
    "c8:c9:a3", "70:c9:4e", "d0:39:57",
    "24:b2:b9", "00:f4:8d", "e0:0a:f6",
    "08:3a:88", "d8:f3:bc",
    // Field-validated additions from the NitekryDPaul 31-prefix research list.
    "b8:35:32", "c0:35:32", "f4:6a:dd", "f8:a2:d6",
    "e8:d0:fc", "e0:4f:43", "b8:1e:a4", "70:08:94",
    "3c:71:bf", "58:00:e3", "5c:93:a2", "64:6e:69",
    "48:27:ea", "82:6b:f2"
};
static const int NUM_MAC_TIER2 = sizeof(mac_prefixes_tier2) / sizeof(mac_prefixes_tier2[0]);

static const char* device_name_patterns[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision", "FlockCam", "FS-", "RWLS-"
};
static const int NUM_NAME_PATTERNS = sizeof(device_name_patterns) / sizeof(device_name_patterns[0]);

static const char* raven_custom_service_uuids[] = {
    "00003100-0000-1000-8000-00805f9b34fb", 
    "00003200-0000-1000-8000-00805f9b34fb", 
    "00003300-0000-1000-8000-00805f9b34fb", 
    "00003400-0000-1000-8000-00805f9b34fb", 
    "00003500-0000-1000-8000-00805f9b34fb", 
};
static const int NUM_RAVEN_CUSTOM_UUIDS = sizeof(raven_custom_service_uuids) / sizeof(raven_custom_service_uuids[0]);

static const char* raven_standard_service_uuids[] = {
    "0000180a-0000-1000-8000-00805f9b34fb", 
    "00001809-0000-1000-8000-00805f9b34fb", 
    "00001819-0000-1000-8000-00805f9b34fb", 
};
static const int NUM_RAVEN_STANDARD_UUIDS = sizeof(raven_standard_service_uuids) / sizeof(raven_standard_service_uuids[0]);

#define FLOCK_MFG_COMPANY_ID 0x09C8

int check_mac_prefix_tiered(const uint8_t* mac) {
    char mac_str[9]; 
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
    for (int i = 0; i < NUM_MAC_TIER1; i++) {
        if (strncmp(mac_str, mac_prefixes_tier1[i], 8) == 0) return 1;
    }
    for (int i = 0; i < NUM_MAC_TIER2; i++) {
        if (strncmp(mac_str, mac_prefixes_tier2[i], 8) == 0) return 2;
    }
    return 0;
}

bool check_ssid_pattern(const char* ssid) {
    if (!ssid || strlen(ssid) == 0) return false;
    for (int i = 0; i < NUM_SSID_PATTERNS; i++) {
        if (strcasestr(ssid, wifi_ssid_patterns[i])) return true;
    }
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
    for (int i = 0; i < NUM_NAME_PATTERNS; i++) {
        if (strcasestr(name, device_name_patterns[i])) return true;
    }
    return false;
}

bool is_penguin_numeric_name(const char* name) {
    if (!name) return false; 
    int len = strlen(name);
    if (len < 8 || len > 12) return false;
    for (int i = 0; i < len; i++) {
        if (!isdigit(name[i])) return false;
    }
    return true;
}

bool check_manufacturer_id(const uint8_t* mfg_data, size_t mfg_len) {
    if (mfg_len >= 2) {
        uint16_t mfg_id = mfg_data[0] | (mfg_data[1] << 8);
        if (mfg_id == FLOCK_MFG_COMPANY_ID) return true;
    }
    return false;
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
    return (uint32_t)(days * 86400L + hour * 3600L + min * 60L + sec);
}

void format_time_buf(unsigned long total_sec, char* buf, size_t buf_len) {
    unsigned long h = total_sec / 3600;
    unsigned long m = (total_sec / 60) % 60;
    unsigned long s = total_sec % 60;
    snprintf(buf, buf_len, "%02lu:%02lu:%02lu", h, m, s);
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
        {"mfg_0x09C8",     "Flock mfg ID"},
        {"tn_serial",      "TN serial"},
        {"ssid_fmt",       "Flock SSID format"},
        {"wildcard_probe", "Wildcard probe (Flock sig)"},
        {"wpa2_psk",       "WPA2-PSK"},
        {"penguin_num",    "Penguin name"},
        {"name",           "Known name"},
        {"mac_t1",         "Known MAC"},
        {"mac_t2",         "Similar MAC"},
        {"ssid",           "SSID pattern"},
        {"static_addr",    "Static addr"},
        {"addr1_t1",       "Receiver MAC (known)"},
        {"addr1_t2",       "Receiver MAC (similar)"},
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
// GPS lock is available. Caller must NOT hold dataMutex (non-recursive).
static void compute_pcap_ts(uint32_t* sec, uint32_t* usec) {
    unsigned long ms = millis();
    bool got_gps = false;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
            uint32_t epoch = utc_to_epoch(
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
            if (epoch > 0) { *sec = epoch; got_gps = true; }
        }
        xSemaphoreGive(dataMutex);
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
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (pcap_write_count < MAX_PCAP_BUFFER) {
        pcap_write_buffer[pcap_write_count].ts_sec  = ts_sec;
        pcap_write_buffer[pcap_write_count].ts_usec = ts_usec;
        pcap_write_buffer[pcap_write_count].incl_len = capture_len;
        pcap_write_buffer[pcap_write_count].orig_len = length;
        memcpy(pcap_write_buffer[pcap_write_count].payload, payload, capture_len);
        pcap_write_count++;
    }
    xSemaphoreGive(dataMutex);
}

void write_ble_pcap(const uint8_t* payload, uint32_t length) {
    if (!sd_available) return;
    uint32_t capture_len = (length > 256) ? 256 : length;
    uint32_t ts_sec = 0, ts_usec = 0;
    compute_pcap_ts(&ts_sec, &ts_usec);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (ble_pcap_write_count < MAX_PCAP_BUFFER) {
        ble_pcap_write_buffer[ble_pcap_write_count].ts_sec  = ts_sec;
        ble_pcap_write_buffer[ble_pcap_write_count].ts_usec = ts_usec;
        ble_pcap_write_buffer[ble_pcap_write_count].incl_len = capture_len;
        ble_pcap_write_buffer[ble_pcap_write_count].orig_len = length;
        memcpy(ble_pcap_write_buffer[ble_pcap_write_count].payload, payload, capture_len);
        ble_pcap_write_count++;
    }
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// HTTP EXPORT SERVER
// ============================================================================

void export_server_setup_routes() {
    if (!export_server) return;

    // Index: simple HTML file list
    export_server->on("/", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><title>Flock Detector Export</title>"
                      "<style>body{font-family:monospace;background:#0a1430;color:#dce8ff;padding:20px;}"
                      "a{color:#00d7eb;text-decoration:none;display:block;padding:8px 0;}"
                      "a:hover{color:#32ff64;}h1{color:#32ff64;}"
                      ".meta{color:#6490b4;font-size:0.9em;}</style></head><body>";
        html += "<h1>FLOCK DETECTOR EXPORT</h1>";
        html += "<p class='meta'>" + String(VERSION_STRING) + "</p>";
        html += "<hr><h2>Files</h2>";
        if (sd_available) {
            html += "<a href='/FlockLog.csv'>FlockLog.csv &nbsp; <span class='meta'>detections CSV</span></a>";
            html += "<a href='/Threats.pcap'>Threats.pcap &nbsp; <span class='meta'>WiFi capture</span></a>";
            html += "<a href='/BLE_Threats.pcap'>BLE_Threats.pcap &nbsp; <span class='meta'>BLE capture</span></a>";
        } else {
            html += "<p class='meta'>SD unavailable</p>";
        }
        html += "<hr><p class='meta'>Auto-exit in ";
        unsigned long remaining = EXPORT_MODE_MAX_MS - (millis() - export_mode_started_at);
        html += String(remaining / 60000UL) + "m " + String((remaining / 1000UL) % 60) + "s</p>";
        html += "</body></html>";
        export_server->send(200, "text/html", html);
    });

    auto serve_sd_file = [](const char* path, const char* mime) {
        if (!sd_available) { export_server->send(503, "text/plain", "SD unavailable"); return; }
        // Serialize against PersistTask / flush_sd_buffer — the Arduino SD/FAT
        // driver isn't reentrancy-safe and concurrent reads/writes through
        // separate File handles can corrupt the FAT cluster table mid-pcap.
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            export_server->send(503, "text/plain", "SD busy");
            return;
        }
        File f = SD.open(path, FILE_READ);
        if (!f) {
            xSemaphoreGive(sdMutex);
            export_server->send(404, "text/plain", "Not found");
            return;
        }
        size_t total = f.size();
        export_server->setContentLength(total);
        export_server->sendHeader("Content-Disposition",
                                  String("attachment; filename=\"") + (path + 1) + "\"");
        export_server->send(200, mime, "");

        // Stream in 1 KB chunks, resetting the WDT and yielding to TCP/WiFi
        // tasks between each. streamFile() would block the main loop for the
        // entire transfer (10–30s on slow clients with multi-MB pcaps),
        // tripping the 30-second WDT and stalling alarms / event drain.
        WiFiClient client = export_server->client();
        uint8_t buf[1024];
        while (f.available() && client.connected()) {
            int n = f.read(buf, sizeof(buf));
            if (n <= 0) break;
            client.write(buf, n);
            esp_task_wdt_reset();
            yield();
        }
        f.close();
        xSemaphoreGive(sdMutex);
    };

    export_server->on("/FlockLog.csv", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/FLOCK_FINDER/logs/FlockLog.csv", "text/csv");
    });
    export_server->on("/Threats.pcap", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/FLOCK_FINDER/captures/Threats.pcap", "application/vnd.tcpdump.pcap");
    });
    export_server->on("/BLE_Threats.pcap", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/FLOCK_FINDER/captures/BLE_Threats.pcap", "application/vnd.tcpdump.pcap");
    });

    export_server->onNotFound([]() {
        export_server->send(404, "text/plain", "Not found");
    });
}

// Restore the promiscuous sniffer after export mode is finished or aborted.
static void export_restore_promiscuous() {
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    wifi_promiscuous_filter_t pf_restore;
    pf_restore.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&pf_restore);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
}

// Finish the connect sequence once WiFi.status() == WL_CONNECTED.
// Returns true on success, false if server allocation failed (in which case
// promiscuous has already been restored and a toast has been shown).
static bool export_finalize_connect() {
    IPAddress ip = WiFi.localIP();
    snprintf(export_ip_str, sizeof(export_ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    export_server = new(std::nothrow) WebServer(80);
    if (!export_server) {
        set_toast_direct("EXPORT ALLOC FAIL", CAUTION_COLOR);
        export_restore_promiscuous();
        return false;
    }
    export_server_setup_routes();
    export_server->begin();

    export_mode_active = true;
    export_mode_started_at = millis();

    char ip_toast[32];
    snprintf(ip_toast, sizeof(ip_toast), "http://%s", export_ip_str);
    set_toast_direct(ip_toast, ACCENT_COLOR);
    return true;
}

// Kick off the WiFi connect. Returns true if the attempt was started; the
// caller should not assume export_mode_active is set on return. The
// non-blocking poll in loop() (export_tick_connect) completes or aborts it.
bool export_mode_start() {
    if (export_mode_active || export_connecting) return true;
    if (strlen(export_ssid) == 0) {
        set_toast_direct("NO WIFI CONFIGURED", CAUTION_COLOR);
        return false;
    }

    // Shut down promiscuous sniffing before joining a network
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(export_ssid, export_pass);

    export_connecting = true;
    export_connect_start_ms = millis();
    set_toast_direct("CONNECTING...", GPS_COLOR);
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
        set_toast_direct("WIFI CONNECT FAIL", CAUTION_COLOR);
        export_restore_promiscuous();
    }
}

void export_mode_stop() {
    // Cancel a pending connect attempt.
    if (export_connecting) {
        export_connecting = false;
        export_restore_promiscuous();
        set_toast_direct("EXPORT CANCELLED", DIM_COLOR);
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
    set_toast_direct("EXPORT MODE OFF", DIM_COLOR);
}

// ============================================================================
// PERSISTENCE & TRACKING
// ============================================================================
void save_detections_to_flash() {
    if (!littlefs_available) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int cnt = capture_history_count;
    CaptureEntry local_hist[CAPTURE_HISTORY_SIZE];
    for (int i = 0; i < cnt; i++) local_hist[i] = capture_history[i];
    xSemaphoreGive(dataMutex);
    File f = LittleFS.open(DETECT_FILE, "w");
    if (!f) return;
    for (int i = 0; i < cnt; i++) {
        f.printf("%s|%s|%s|%d|%.6f|%.6f\n",
            local_hist[i].type, local_hist[i].mac, local_hist[i].name,
            local_hist[i].confidence, local_hist[i].lat, local_hist[i].lng);
    }
    f.close();
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
        strncpy(capture_history[idx].time, "??:??:??", 8);
        capture_history_count++;
    }
    f.close();
}

// Load last SD_HIST_SIZE detections from SD CSV (most recent first in sd_hist[])
void load_sd_history() {
    sd_hist_count = 0;
    if (!sd_available) return;
    File f = SD.open(current_log_file, FILE_READ);
    if (!f) return;

    // Ring buffer: keep last SD_HIST_SIZE parsed entries
    SDHistEntry ring[SD_HIST_SIZE];
    int ri = 0, total = 0;
    char buf[SD_LINE_LEN];

    while (f.available()) {
        // Long log files can take many ms to scan — keep the WDT alive every
        // ~32 lines so a big history load doesn't trip the 30s timeout.
        if ((total & 0x1F) == 0) esp_task_wdt_reset();
        int len = 0;
        while (len < SD_LINE_LEN - 1 && f.available()) {
            char c = (char)f.read();
            if (c == '\n' || c == '\r') break;
            buf[len++] = c;
        }
        buf[len] = '\0';
        if (len < 10) continue;

        // CSV: 0=Uptime_ms,1=EpochUTC,2=EpochIsGPS,3=Channel,4=Type,5=Proto,
        //      6=RSSI,7=MAC,8=Name,9=TXPower,10=Method,11=Conf,...
        int fs[21]; int fc = 0;
        fs[0] = 0;
        for (int ci = 0; ci < len && fc < 20; ci++) {
            if (buf[ci] == ',') fs[++fc] = ci + 1;
        }
        if (fc < 11) { total++; continue; }

        // Extract a field: from fs[n] up to the next comma (or end of buf)
        auto copy_f = [&](int n, char* dest, int maxlen) {
            int start = fs[n];
            int end = start;
            while (end < len && buf[end] != ',') end++;
            int flen = end - start;
            if (flen >= maxlen) flen = maxlen - 1;
            strncpy(dest, buf + start, flen);
            dest[flen] = '\0';
        };

        SDHistEntry& e = ring[ri % SD_HIST_SIZE];
        copy_f(4,  e.type,   16);
        copy_f(7,  e.mac,    18);
        copy_f(8,  e.name,   32);
        copy_f(10, e.method, 24);
        e.rssi       = atoi(buf + fs[6]);
        e.confidence = atoi(buf + fs[11]);
        // SD CSV doesn't carry an ID column — entries loaded from disk show
        // "----" until they're replaced by fresh detections logged this run.
        e.id         = 0;
        {
            unsigned long uptime = (unsigned long)strtoul(buf + fs[0], NULL, 10);
            format_time_buf(uptime / 1000, e.timestamp, sizeof(e.timestamp));
        }

        ri++;
        total++;
    }
    f.close();

    int count = (total < SD_HIST_SIZE) ? total : SD_HIST_SIZE;
    sd_hist_count = count;
    // Reorder so sd_hist[0] = most recent
    for (int i = 0; i < count; i++) {
        int ridx = ((ri - 1 - i) % SD_HIST_SIZE + SD_HIST_SIZE) % SD_HIST_SIZE;
        sd_hist[i] = ring[ridx];
    }
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
        return;
    }

    long l_wifi, l_ble, l_sec, l_flock, l_boots, l_writes, l_next_id;
    int l_vol;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    l_wifi = lifetime_wifi; l_ble = lifetime_ble; l_sec = lifetime_seconds;
    l_flock = lifetime_flock_total; l_vol = current_volume; l_boots = lifetime_boots;
    l_writes = lifetime_flash_writes + 1;
    l_next_id = next_detection_id;
    xSemaphoreGive(dataMutex);

    bool write_ok = false;
    for (int attempt = 0; attempt < 3 && !write_ok; attempt++) {
        File f = LittleFS.open(PERSIST_FILE, "w");
        if (!f) {
            Serial.printf("[FS] PERSIST_FILE open failed (attempt %d)\n", attempt);
            delay(5);
            continue;
        }
        size_t written = f.printf("%ld\n%ld\n%lu\n%ld\n%d\n%ld\n%ld\n%s\n%s\n%ld\n",
                                  l_wifi, l_ble, l_sec, l_flock, l_vol, l_boots, l_writes,
                                  export_ssid, export_pass, l_next_id);
        f.close();
        if (written > 10) {
            write_ok = true;
        } else {
            Serial.printf("[FS] Write truncated: %u bytes (attempt %d)\n",
                          (unsigned)written, attempt);
        }
    }

    if (write_ok) {
        flash_write_fail_count = 0;
        last_persist_save = millis();
        save_detections_to_flash();
        save_stats_to_sd();
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        lifetime_flash_writes = l_writes;
        xSemaphoreGive(dataMutex);
        // Wear toasts: once at 80K (warning) and once at 100K (critical).
        // Equality checks fire exactly once per crossing.
        if (l_writes == 80000) {
            set_toast_direct("FLASH WEAR HIGH", CAUTION_COLOR);
        } else if (l_writes == 100000) {
            set_toast_direct("FLASH WEAR CRIT", CAUTION_COLOR);
        }
    } else {
        flash_write_fail_count++;
        if (flash_write_fail_count >= FLASH_FAIL_TOAST_THRESHOLD) {
            set_toast_direct("FLASH WRITE FAIL", CAUTION_COLOR);
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
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    l_wifi   = lifetime_wifi;
    l_ble    = lifetime_ble;
    l_sec    = lifetime_seconds;
    l_flock  = lifetime_flock_total;
    l_boots  = lifetime_boots;
    l_writes = lifetime_flash_writes;
    xSemaphoreGive(dataMutex);

    // Timed take — if the lock is held for too long, skip this cycle. The
    // next persist tick will retry. Keeps PersistTask's WDT alive even when
    // the main loop is mid-flush.
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return;
    }
    File f = SD.open("/FLOCK_FINDER/stats/lifetime.txt", FILE_WRITE);
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
// SD.begin() at 4 MHz then 20 MHz; on success, recreates the /FLOCK_FINDER
// directory tree, writes PCAP headers, reloads history, and toasts. When a
// card is present, probes a known file; if the open fails silently, treats it
// as a removal.
static void sd_check_hotplug() {
    unsigned long now = millis();
    if (now - last_sd_check_ms < SD_CHECK_INTERVAL_MS) return;
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
            sd_available = true;

            if (!SD.exists("/FLOCK_FINDER"))          SD.mkdir("/FLOCK_FINDER");
            if (!SD.exists("/FLOCK_FINDER/logs"))     SD.mkdir("/FLOCK_FINDER/logs");
            if (!SD.exists("/FLOCK_FINDER/captures")) SD.mkdir("/FLOCK_FINDER/captures");
            if (!SD.exists("/FLOCK_FINDER/stats"))    SD.mkdir("/FLOCK_FINDER/stats");
            esp_task_wdt_reset();

            if (!SD.exists(current_log_file)) {
                File f = SD.open(current_log_file, FILE_WRITE);
                if (f) {
                    f.println("Uptime_ms,EpochUTC,EpochIsGPS,Channel,Type,Proto,RSSI,MAC,Name,TXPower,Method,Conf,ConfLabel,Extra,SeqNum,Lat,Lon,SpeedMPH,HeadingDeg,AltM");
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

            // load_sd_history & save_stats_to_sd both touch SD. We already
            // hold sdMutex, so call load_sd_history directly (it doesn't
            // self-lock) and inline the stats write rather than recursing
            // into save_stats_to_sd which would try to take the mutex again.
            load_sd_history();
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            sd_hist_dirty = false;
            long          l_wifi   = lifetime_wifi;
            long          l_ble    = lifetime_ble;
            unsigned long l_sec    = lifetime_seconds;
            long          l_flock  = lifetime_flock_total;
            long          l_boots  = lifetime_boots;
            long          l_writes = lifetime_flash_writes;
            xSemaphoreGive(dataMutex);

            File sf = SD.open("/FLOCK_FINDER/stats/lifetime.txt", FILE_WRITE);
            if (sf) {
                sf.printf("lifetime_wifi=%ld\n",          l_wifi);
                sf.printf("lifetime_ble=%ld\n",           l_ble);
                sf.printf("lifetime_seconds=%lu\n",       l_sec);
                sf.printf("lifetime_flock_total=%ld\n",   l_flock);
                sf.printf("lifetime_boots=%ld\n",         l_boots);
                sf.printf("lifetime_flash_writes=%ld\n",  l_writes);
                sf.close();
            }

            set_toast_direct("SD CARD MOUNTED", ACCENT_COLOR);
            Serial.println("[SD] Hot-plug mount succeeded");
        }
    } else {
        // Probe at the controller level — independent of any file existing.
        // The previous file-probe falsely reported "removed" within the first
        // 60s of boot when /FLOCK_FINDER/stats/lifetime.txt had not yet been
        // written by the persist task.
        if (SD.cardType() == CARD_NONE && sd_was_available) {
            sd_available = false;
            SD.end();
            set_toast_direct("SD CARD REMOVED", CAUTION_COLOR);
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
    // next_detection_id was added later; older save files end here and the
    // counter stays at its 1 default. Files that include it pick up where
    // they left off so detection IDs don't restart on reboot.
    line = f.readStringUntil('\n');
    if (line.length() > 0) {
        long parsed = line.toInt();
        if (parsed >= 1) next_detection_id = parsed;
    }
    f.close();
}

// ── Simple XOR cipher using the ESP32 eFuse MAC as key ──
// Not cryptographically strong, but prevents casual reading of
// credentials from a removed SD card or flash dump. The MAC is
// unique per physical device and lives in one-time-programmable
// eFuse — not in any user-accessible storage.
static void xor_cipher(char* data, size_t len) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint8_t key[16];
    for (int i = 0; i < 16; i++) {
        key[i] = mac[i % 6] ^ (uint8_t)(i * 0x5A + 0x37);
    }
    for (size_t i = 0; i < len; i++) {
        data[i] ^= key[i % 16];
    }
}

#define WIFI_CRED_FILE "/wifi_cred.dat"

static void save_wifi_credentials() {
    if (!littlefs_available) return;
    char enc_ssid[33], enc_pass[65];
    strncpy(enc_ssid, export_ssid, sizeof(enc_ssid) - 1);
    enc_ssid[sizeof(enc_ssid) - 1] = '\0';
    strncpy(enc_pass, export_pass, sizeof(enc_pass) - 1);
    enc_pass[sizeof(enc_pass) - 1] = '\0';

    size_t ssid_len = strlen(enc_ssid);
    size_t pass_len = strlen(enc_pass);

    xor_cipher(enc_ssid, ssid_len);
    xor_cipher(enc_pass, pass_len);

    File f = LittleFS.open(WIFI_CRED_FILE, "w");
    if (!f) return;
    f.write((uint8_t)ssid_len);
    f.write((uint8_t)pass_len);
    f.write((const uint8_t*)enc_ssid, ssid_len);
    f.write((const uint8_t*)enc_pass, pass_len);
    f.close();
}

static void load_wifi_credentials() {
    if (!littlefs_available) return;
    if (!LittleFS.exists(WIFI_CRED_FILE)) return;
    File f = LittleFS.open(WIFI_CRED_FILE, "r");
    if (!f) return;

    int ssid_len_raw = f.read();
    int pass_len_raw = f.read();
    if (ssid_len_raw < 0 || pass_len_raw < 0) { f.close(); return; }
    uint8_t ssid_len = (uint8_t)ssid_len_raw;
    uint8_t pass_len = (uint8_t)pass_len_raw;

    if (ssid_len > 32 || pass_len > 64) { f.close(); return; }

    char enc_ssid[33] = {0}, enc_pass[65] = {0};
    f.readBytes(enc_ssid, ssid_len);
    f.readBytes(enc_pass, pass_len);
    f.close();

    xor_cipher(enc_ssid, ssid_len);
    xor_cipher(enc_pass, pass_len);

    // Validate: if decrypted result contains non-printable chars,
    // the file is corrupt or from a different device — discard.
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
    }
}

void rssi_track_update(const char* mac, int rssi) {
    unsigned long now = millis();
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, mac, 17) == 0) {
            if (rssi_tracker[i].sample_count < RSSI_TRACK_SAMPLES) {
                rssi_tracker[i].samples[rssi_tracker[i].sample_count++] = rssi;
            } else {
                for (int j = 0; j < RSSI_TRACK_SAMPLES - 1; j++) rssi_tracker[i].samples[j] = rssi_tracker[i].samples[j + 1];
                rssi_tracker[i].samples[RSSI_TRACK_SAMPLES - 1] = rssi;
                rssi_tracker[i].scored = false;
            }
            rssi_tracker[i].last_seen = now;
            if (locator_active && strncmp(rssi_tracker[i].mac, locator_target_mac, 17) == 0) {
                locator_tracker_idx = i;
            }
            xSemaphoreGive(dataMutex);
            return;
        }
    }
    if (rssi_tracker_count < RSSI_TRACK_MAX_DEVICES) {
        strncpy(rssi_tracker[rssi_tracker_count].mac, mac, 17);
        rssi_tracker[rssi_tracker_count].mac[17] = '\0';
        rssi_tracker[rssi_tracker_count].samples[0] = rssi;
        rssi_tracker[rssi_tracker_count].sample_count = 1;
        rssi_tracker[rssi_tracker_count].last_seen = now;
        rssi_tracker[rssi_tracker_count].scored = false;
        rssi_tracker_count++;
    }
    xSemaphoreGive(dataMutex);
}

bool rssi_track_is_stationary(const char* mac) {
    bool result = false;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, mac, 17) == 0
            && rssi_tracker[i].sample_count >= 3
            && !rssi_tracker[i].scored) {
            int n = rssi_tracker[i].sample_count;
            int* s = rssi_tracker[i].samples;

            // Branch A: peak-shape (operator moving past stationary device)
            int peak_idx = 0;
            for (int j = 1; j < n; j++) if (s[j] > s[peak_idx]) peak_idx = j;
            int range_peak = s[peak_idx] - min(s[0], s[n - 1]);
            bool peak_match = (peak_idx > 0 && peak_idx < n - 1 && range_peak >= 6);

            // Branch B: flat variance (both device and operator stationary)
            int s_min = s[0], s_max = s[0];
            for (int j = 1; j < n; j++) {
                if (s[j] < s_min) s_min = s[j];
                if (s[j] > s_max) s_max = s[j];
            }
            int total_range = s_max - s_min;
            bool flat_match = (total_range <= 3);

            if (peak_match || flat_match) {
                rssi_tracker[i].scored = true;
                result = true;
            }
            break;
        }
    }
    xSemaphoreGive(dataMutex);
    return result;
}

void rssi_track_expire() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    unsigned long now = millis();
    for (int i = rssi_tracker_count - 1; i >= 0; i--) {
        if ((now - rssi_tracker[i].last_seen) > RSSI_TRACK_EXPIRY_MS) {
            for (int j = i; j < rssi_tracker_count - 1; j++) rssi_tracker[j] = rssi_tracker[j + 1];
            rssi_tracker_count--;
            // Keep locator_tracker_idx consistent across the array shift
            if (locator_tracker_idx == i)       locator_tracker_idx = -1;
            else if (locator_tracker_idx > i)   locator_tracker_idx--;
        }
    }
    // Re-validate after all shifts — defensive against combined adjustments
    if (locator_tracker_idx >= rssi_tracker_count) {
        locator_tracker_idx = -1;
    }
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// THREAT EQUALIZER DATA
// ============================================================================
#define radar_cx 58
#define radar_cy 46
#define radar_r  34
#define inner_r  14

#define NUM_RADIAL_BANDS 36
float radial_spikes[NUM_RADIAL_BANDS] = {0};
uint16_t radial_colors[NUM_RADIAL_BANDS] = {0};
int radial_rssi[NUM_RADIAL_BANDS] = {0};
unsigned long radial_spike_birth[NUM_RADIAL_BANDS] = {0};
static float hud_rotation = 0.0f;

#define NUM_PARTICLES 24
static float noise_r[NUM_PARTICLES] = {0};
static float noise_a[NUM_PARTICLES] = {0};
static int noise_life[NUM_PARTICLES] = {0};
static int noise_max_life[NUM_PARTICLES] = {0};

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
                                int proto, bool is_flock) {
    if (!mac || mac[0] == '\0') return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (feed_recently_pushed(mac)) { xSemaphoreGive(dataMutex); return; }
    if (feed_pending_valid && rssi <= feed_pending.rssi) { xSemaphoreGive(dataMutex); return; }

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
    feed_pending.timestamp = millis();
    feed_pending_valid     = true;
    xSemaphoreGive(dataMutex);
}

static void feed_commit_pending() {
    unsigned long now = millis();
    unsigned long interval = show_feed_expanded ? 667UL : FEED_MIN_PUSH_INTERVAL_MS;
    if (now - last_feed_push_ms < interval) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (!feed_pending_valid) { xSemaphoreGive(dataMutex); return; }
    feed_head = (feed_head + 1) % FEED_SIZE;
    feed_entries[feed_head] = feed_pending;
    feed_entries[feed_head].timestamp = now;
    if (feed_count < FEED_SIZE) feed_count++;
    feed_pending_valid = false;
    last_feed_push_ms  = now;
    xSemaphoreGive(dataMutex);
}

void add_blip(uint16_t blip_color, int rssi) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    last_blip_time = millis();
    int band = random(0, NUM_RADIAL_BANDS);
    float strength_mult = constrain(map(rssi, -100, -30, 50, 200), 50, 200) / 100.0f;
    radial_spikes[band]      = 0.5f + strength_mult;
    radial_colors[band]      = blip_color;
    radial_rssi[band]        = rssi;
    radial_spike_birth[band] = millis();
    xSemaphoreGive(dataMutex);
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

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    log_count = sd_write_count;
    for (int i = 0; i < log_count; i++) {
        strncpy(local_log_buf[i], sd_write_buffer[i], SD_LINE_LEN - 1);
        local_log_buf[i][SD_LINE_LEN - 1] = '\0';
    }
    sd_write_count = 0;

    pcap_count = pcap_write_count;
    for (int i = 0; i < pcap_count; i++) {
        local_pcap_buf[i] = pcap_write_buffer[i];
    }
    pcap_write_count = 0;
    ble_pcap_count = ble_pcap_write_count;
    for (int i = 0; i < ble_pcap_count; i++) {
        local_ble_pcap_buf[i] = ble_pcap_write_buffer[i];
    }
    ble_pcap_write_count = 0;
    xSemaphoreGive(dataMutex);

    if (!sd_available) return;

    // Skip flush if heap is critically low — the SD FAT driver mallocs
    // internally and will abort() if it gets NULL. Better to drop a flush
    // cycle than crash; the buffers will retry next interval.
    if (esp_get_free_heap_size() < 6000) {
        Serial.println("[SD] Skipping flush — heap too low");
        return;
    }

    // Short timed take — flush runs every 500ms and gladly retries on the
    // next tick. Blocking for seconds here freezes the main loop and
    // starves alarms / WiFi event processing.
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    if (log_count > 0) {
        File file = SD.open(current_log_file, FILE_APPEND);
        if (file) {
            for (int i = 0; i < log_count; i++) {
                if ((i & 0x0F) == 0) esp_task_wdt_reset();  // every 16 lines
                file.println(local_log_buf[i]);
            }
            file.close();
        } else if (sd_available) {
            // Soft failure: controller may be doing GC or a brief voltage dip.
            // Don't tear down the SD connection — the 5s hot-plug probe owns the
            // real removal check, and this write will retry on the next flush.
            set_toast_direct("SD WRITE FAIL", CAUTION_COLOR);
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
                pfile.write((const uint8_t*)&pph, sizeof(pph));
                pfile.write(local_pcap_buf[i].payload, local_pcap_buf[i].incl_len);
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
                bfile.write((const uint8_t*)&pph, sizeof(pph));
                bfile.write(local_ble_pcap_buf[i].payload, local_ble_pcap_buf[i].incl_len);
            }
            bfile.close();
        }
    }

    xSemaphoreGive(sdMutex);

    if (log_count > 0 || pcap_count > 0 || ble_pcap_count > 0) {
        last_sd_flush = millis();
    }
}

// Thread-safe direct toast setter. Used for single-message notifications
// (battery warnings, flash errors, export-mode messages) that bypass the queue.
// Does NOT touch the queue — it only sets the currently-displayed toast.
static void set_toast_direct(const char* text, uint16_t accent) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (toast_active && toast_queue_count > 0) {
        // Replace the head entry so the expiry handler in draw_toast_spr
        // doesn't pop a phantom slot.
        strncpy(toast_queue[toast_queue_head].text, text,
                sizeof(toast_queue[toast_queue_head].text) - 1);
        toast_queue[toast_queue_head].text[sizeof(toast_queue[toast_queue_head].text) - 1] = '\0';
        toast_queue[toast_queue_head].accent    = accent;
        toast_queue[toast_queue_head].is_action = true;
    } else {
        // No active toast — push as a new queue entry so expiry accounting
        // matches. Covers both the toast_queue_count == 0 case and the
        // !toast_active && toast_queue_count > 0 case (the latter would
        // otherwise be silently lost: expiry would advance past a queued
        // toast that was never displayed).
        if (toast_queue_count >= TOAST_QUEUE_SIZE) {
            toast_queue_head = (toast_queue_head + 1) % TOAST_QUEUE_SIZE;
            toast_queue_count--;
        }
        int slot = (toast_queue_head + toast_queue_count) % TOAST_QUEUE_SIZE;
        strncpy(toast_queue[slot].text, text, sizeof(toast_queue[slot].text) - 1);
        toast_queue[slot].text[sizeof(toast_queue[slot].text) - 1] = '\0';
        toast_queue[slot].accent    = accent;
        toast_queue[slot].is_action = true;
        toast_queue_count++;
        // Advance head to the just-pushed entry so the legacy mirror and
        // expiry pop refer to the same slot we're about to display.
        toast_queue_head = slot;
    }
    strncpy(toast_text, text, sizeof(toast_text) - 1);
    toast_text[sizeof(toast_text) - 1] = '\0';
    toast_accent_color = accent;
    toast_is_action    = true;
    toast_start        = millis();
    toast_active       = true;
    screen_dirty       = true;
    xSemaphoreGive(dataMutex);
}

void trigger_toast(const char* type, const char* name, int confidence) {
    uint16_t accent;
    if      (strncmp(type, "RAVEN",     5) == 0) accent = TEAL_COLOR;
    else if (strcmp (type, "FLOCK_BLE") == 0)    accent = PURPLE_COLOR;
    else if (strcmp (type, "TARGET")    == 0)    accent = HEADER_COLOR;
    else                                          accent = CAUTION_COLOR;

    const char* src = (name && name[0] != '\0' && strcmp(name, "Hidden") != 0) ? name : type;
    bool is_action = (confidence == 0);
    char full_text[32];
    if (is_action) {
        snprintf(full_text, sizeof(full_text), "%.30s", src);
    } else {
        char pct_str[6];
        snprintf(pct_str, sizeof(pct_str), " %d%%", confidence);
        snprintf(full_text, sizeof(full_text), "%.14s%s", src, pct_str);
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);

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
        strncpy(toast_text, toast_queue[toast_queue_head].text, sizeof(toast_text) - 1);
        toast_text[sizeof(toast_text) - 1] = '\0';
        toast_accent_color = toast_queue[toast_queue_head].accent;
        toast_is_action = toast_queue[toast_queue_head].is_action;
        toast_start = millis();
        toast_active = true;
    }
    screen_dirty = true;

    xSemaphoreGive(dataMutex);
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
                   int confidence, int seq_num) {
    unsigned long now_ms = millis();
    char current_time[9];
    format_time_buf((now_ms - session_start_time) / 1000, current_time, sizeof(current_time));

    // Brief window 1: GPS epoch computation
    uint32_t ts_epoch = 0;
    bool ts_is_gps = false;
    {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
            uint32_t epoch = utc_to_epoch(
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
            if (epoch > 0) { ts_epoch = epoch; ts_is_gps = true; }
        }
        xSemaphoreGive(dataMutex);
    }

    // Brief window 2: is_new check, counters, history, LED
    bool is_new = false;
    uint16_t blip_col = ACCENT_COLOR;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    is_new = !is_mac_recently_seen(mac);

    if (is_new) {
        add_seen_mac(mac);
        if (strcmp(proto, "WIFI") == 0) {
            session_wifi++; lifetime_wifi++; session_flock_wifi++;
            blip_col = ACCENT_COLOR;   // condensed: all detections use accent
        } else {
            session_ble++; lifetime_ble++; blip_col = ACCENT_COLOR;
        }
        if (strstr(type, "RAVEN") != NULL) { session_raven++; blip_col = ACCENT_COLOR; }
        else if (strcmp(proto, "BLE") == 0) { session_flock_ble++; }
        lifetime_flock_total++;
        add_to_capture_history(type, mac, name, rssi, confidence);
        // NOTE: trigger_toast() is deferred until after we release dataMutex —
        // it takes dataMutex internally and our mutex is non-recursive.
        // Flash LED: yellow for WiFi, purple for BLE
        if (strcmp(proto, "WIFI") == 0) {
            led_detect_r = 255; led_detect_g = 200; led_detect_b = 0;
        } else {
            led_detect_r = 180; led_detect_g = 0; led_detect_b = 255;
        }
        led_detection_flash_until = millis() + 15000;
        led_detect_active = true;

        // Append to sd_hist in-memory so the Detections screen doesn't
        // need to re-scan the SD log file. We already hold dataMutex.
        // Without this, every new MAC would trigger a full FlockLog.csv
        // re-read in loop() — which is megabytes after a day of use.
        int new_count = (sd_hist_count < SD_HIST_SIZE) ? sd_hist_count + 1 : SD_HIST_SIZE;
        for (int i = new_count - 1; i > 0; i--) sd_hist[i] = sd_hist[i - 1];
        safe_copy(sd_hist[0].type,   type,             sizeof(sd_hist[0].type));
        safe_copy(sd_hist[0].mac,    mac,              sizeof(sd_hist[0].mac));
        safe_copy(sd_hist[0].name,   name,             sizeof(sd_hist[0].name));
        safe_copy(sd_hist[0].method, detection_method, sizeof(sd_hist[0].method));
        sd_hist[0].rssi       = rssi;
        sd_hist[0].confidence = confidence;
        format_time_buf((millis() - session_start_time) / 1000,
                        sd_hist[0].timestamp, sizeof(sd_hist[0].timestamp));
        // Sequential ID — assigned to both the SD-history mirror and the
        // in-memory capture_history entry that add_to_capture_history just
        // pushed at index 0. Counter is persisted in PERSIST_FILE.
        sd_hist[0].id          = (int)next_detection_id;
        capture_history[0].id  = (int)next_detection_id;
        next_detection_id++;
        if (sd_hist_count < SD_HIST_SIZE) sd_hist_count++;

        // Leave sd_hist_dirty alone — the loop's was_dirty path becomes a
        // safety net rather than a normal-flow trigger. (Boot, screen
        // entry, and hot-plug remount still call load_sd_history directly.)
    }

    safe_copy(last_cap_type,       type,             sizeof(last_cap_type));
    safe_copy(last_cap_mac,        mac,              sizeof(last_cap_mac));
    safe_copy(last_cap_name,       name,             sizeof(last_cap_name));
    safe_copy(last_cap_time,       current_time,     sizeof(last_cap_time));
    safe_copy(last_cap_det_method, detection_method, sizeof(last_cap_det_method));
    last_cap_rssi       = rssi;
    last_cap_confidence = confidence;
    last_cap_seq_num    = seq_num;
    xSemaphoreGive(dataMutex);

    if (is_new) {
        trigger_toast(type, name, confidence);
        add_blip(blip_col, rssi);
        screen_dirty = true;
    }

    // Heavy work outside mutex
    if (is_new && sd_available) {
        char clean_name[64];
        safe_copy(clean_name, name, sizeof(clean_name));
        for (char* p = clean_name; *p; p++) if (*p == ',') *p = ' ';

        char clean_extra[64];
        safe_copy(clean_extra, extra_data, sizeof(clean_extra));
        for (char* p = clean_extra; *p; p++) if (*p == ',') *p = ' ';

        // Brief window: GPS snapshot
        char gps_fields[80];
        bool gps_fresh; double g_lat=0, g_lng=0; float g_spd=0, g_crs=0, g_alt=0;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        gps_fresh = gps.location.isValid() && (gps.location.age() < 2000);
        if (gps_fresh) {
            g_lat = gps.location.lat(); g_lng = gps.location.lng();
            g_spd = gps.speed.isValid()    ? gps.speed.mph()       : 0.0f;
            g_crs = gps.course.isValid()   ? gps.course.deg()      : 0.0f;
            g_alt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0f;
        }
        xSemaphoreGive(dataMutex);

        if (gps_fresh)
            snprintf(gps_fields, sizeof(gps_fields), "%.6f,%.6f,%.1f,%.1f,%.1f", g_lat, g_lng, g_spd, g_crs, g_alt);
        else
            strncpy(gps_fields, "0.000000,0.000000,0.0,0.0,0.0", sizeof(gps_fields));

        char local_line[SD_LINE_LEN];
        snprintf(local_line, SD_LINE_LEN,
            "%lu,%u,%d,%d,%s,%s,%d,%s,%s,%d,%s,%d,%s,%s,%d,%s",
            now_ms, ts_epoch, ts_is_gps ? 1 : 0, channel, type, proto, rssi, mac, clean_name,
            tx_power, detection_method, confidence,
            confidence_label(confidence), clean_extra, seq_num, gps_fields);

        // Brief window 3: commit line to sd_write_buffer
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (sd_write_count < MAX_LOG_BUFFER) {
            strncpy(sd_write_buffer[sd_write_count], local_line, SD_LINE_LEN - 1);
            sd_write_buffer[sd_write_count][SD_LINE_LEN - 1] = '\0';
            sd_write_count++;
        }
        xSemaphoreGive(dataMutex);
    }
}

// ============================================================================
// LOCATOR ENGINE
// ============================================================================
double rssi_to_weight(int rssi) { 
    if (rssi > -20) rssi = -20; if (rssi < -100) rssi = -100; return pow(10.0, (double)(rssi + 100) / 20.0); 
}

double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    double dLat = radians(lat2 - lat1);
    double dLon = radians(lon2 - lon1);
    double a = sin(dLat/2) * sin(dLat/2)
             + cos(radians(lat1)) * cos(radians(lat2))
             * sin(dLon/2) * sin(dLon/2);
    return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

float bearing_to(double lat1, double lon1, double lat2, double lon2) {
    double dLon = radians(lon2 - lon1);
    double y = sin(dLon) * cos(radians(lat2));
    double x = cos(radians(lat1)) * sin(radians(lat2))
             - sin(radians(lat1)) * cos(radians(lat2)) * cos(dLon);
    double brng = degrees(atan2(y, x));
    if (brng < 0) brng += 360.0;
    return (float)brng;
}

// Returns -1 if distance is decreasing (closer), +1 if increasing (farther), 0 if stable.
static int locator_distance_trend() {
    if (locator_dist_history_count < 4) return 0;
    int n = locator_dist_history_count;
    int half = n / 2;
    float old_avg = 0, new_avg = 0;
    for (int i = 0; i < half; i++) {
        int idx_old = (locator_dist_history_head - n + i + LOC_TREND_SAMPLES * 2) % LOC_TREND_SAMPLES;
        int idx_new = (locator_dist_history_head - half + i + LOC_TREND_SAMPLES * 2) % LOC_TREND_SAMPLES;
        old_avg += locator_dist_history[idx_old];
        new_avg += locator_dist_history[idx_new];
    }
    old_avg /= half;
    new_avg /= half;
    float diff = new_avg - old_avg;
    if (diff < -2.0f) return -1;
    if (diff >  2.0f) return  1;
    return 0;
}

void locator_add_sample(const char* mac, int rssi) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (!locator_active || strncmp(mac, locator_target_mac, 17) != 0) {
        xSemaphoreGive(dataMutex);
        return;
    }
    if (millis() - locator_last_sample < LOC_SAMPLE_INTERVAL) {
        xSemaphoreGive(dataMutex);
        return;
    }
    if (!gps.location.isValid() || gps.location.age() > 2000) {
        xSemaphoreGive(dataMutex);
        return;
    }

    int idx = locator_sample_count;
    if (idx >= LOC_MAX_SAMPLES) {
        const unsigned long LOC_EVICT_AGE_MS = 300000UL;
        unsigned long now_ms = millis();
        int worst = 0;
        float worst_score = 1e9f;
        for (int i = 0; i < LOC_MAX_SAMPLES; i++) {
            float r = (float)(locator_samples[i].rssi + 100) / 80.0f;
            if (r < 0) r = 0; if (r > 1) r = 1;
            unsigned long age = now_ms - locator_samples[i].timestamp;
            float rec = 1.0f - (float)age / (float)LOC_EVICT_AGE_MS;
            if (rec < 0) rec = 0;
            float combined = r * rec;
            if (combined < worst_score) { worst_score = combined; worst = i; }
        }
        float new_r   = (float)(rssi + 100) / 80.0f;
        if (new_r < 0) new_r = 0; if (new_r > 1) new_r = 1;
        float new_score = new_r * 1.0f;
        if (new_score > worst_score) { idx = worst; }
        else { xSemaphoreGive(dataMutex); return; }
    } else { locator_sample_count++; }

    locator_samples[idx] = {gps.location.lat(), gps.location.lng(), rssi, millis()};
    locator_last_sample = millis();
    locator_newest_sample_ms = millis();
    if (rssi > locator_peak_rssi) locator_peak_rssi = rssi;

    if (locator_sample_count >= LOC_MIN_SAMPLES_EST) {
        double sum_wlat = 0, sum_wlng = 0, sum_w = 0;
        for (int i = 0; i < locator_sample_count; i++) {
            double w = rssi_to_weight(locator_samples[i].rssi);
            sum_wlat += locator_samples[i].lat * w; sum_wlng += locator_samples[i].lng * w; sum_w += w;
        }
        if (sum_w > 0) {
            locator_est_lat = sum_wlat / sum_w; locator_est_lng = sum_wlng / sum_w; locator_has_estimate = true;
            if (gps.location.isValid()) {
                locator_est_distance = haversine_m(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
                locator_bearing = bearing_to(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
            }
            double sum_wd2 = 0;
            for (int i = 0; i < locator_sample_count; i++) {
                double d = haversine_m(locator_samples[i].lat, locator_samples[i].lng, locator_est_lat, locator_est_lng);
                double w = rssi_to_weight(locator_samples[i].rssi); sum_wd2 += w * d * d;
            }
            locator_confidence_radius = sqrt(sum_wd2 / sum_w);
            if (!locator_estimate_announced) {
                locator_estimate_announced = true;
                locator_announce_pending = true;
            }
        }
    }
    xSemaphoreGive(dataMutex);
}

void locator_start(const char* mac, const char* name, const char* type = "", int id = 0) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = true;
    safe_copy(locator_target_mac,  mac,  sizeof(locator_target_mac));
    safe_copy(locator_target_name, name, sizeof(locator_target_name));
    safe_copy(locator_target_type, type, sizeof(locator_target_type));
    locator_target_id = id;
    locator_sample_count = 0; locator_has_estimate = false; locator_peak_rssi = -120;
    locator_estimate_announced = false;
    locator_est_distance = 0; locator_bearing = 0; locator_confidence_radius = 0;
    locator_newest_sample_ms = 0;
    locator_dist_history_count = 0;
    locator_dist_history_head = 0;
    locator_last_trend_sample_ms = 0;
    locator_tracker_idx = -1;
    xSemaphoreGive(dataMutex);
}

void locator_stop() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = false; locator_has_estimate = false; locator_sample_count = 0;
    locator_newest_sample_ms = 0;
    locator_estimate_announced = false;
    locator_dist_history_count = 0;
    locator_dist_history_head = 0;
    locator_last_trend_sample_ms = 0;
    locator_tracker_idx = -1;
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// WIFI PACKET HANDLER — LOCK-FREE DEFERRED PROCESSING
// ============================================================================
typedef struct {
    unsigned frame_ctrl:16; unsigned duration_id:16; uint8_t addr1[6];
    uint8_t addr2[6]; uint8_t addr3[6]; unsigned sequence_ctrl:16; uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct { wifi_ieee80211_mac_hdr_t hdr; uint8_t payload[0]; } wifi_ieee80211_packet_t;

#define WIFI_EVENT_QUEUE_SIZE 8

struct WifiEvent {
    uint8_t  mac[6];      // addr2 — transmitter
    uint8_t  addr1[6];    // addr1 — receiver/destination (sleeping-device check)
    int8_t   rssi;
    uint8_t  channel;
    uint16_t seq_num;
    char     ssid[33];
    bool     is_beacon;
    bool     is_probe_req;   // mgmt subtype 4 — required for the wildcard-probe signature
    uint8_t  payload_snap[256];
    uint16_t payload_snap_len;
    uint16_t orig_len;
    volatile bool ready;
    bool     is_wpa2_psk;
    uint8_t  vendor_ouis[4][3];
    uint8_t  vendor_oui_count;
};

static WifiEvent wifi_event_queue[WIFI_EVENT_QUEUE_SIZE];
static volatile uint8_t wifi_eq_write_idx = 0;
static uint8_t          wifi_eq_read_idx  = 0;

void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buff;
    if (ppkt->rx_ctrl.sig_len < 24) return;
    ambient_packet_count++;

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
    if (wifi_event_queue[wifi_eq_write_idx].ready) return;
    uint8_t next = (wifi_eq_write_idx + 1) % WIFI_EVENT_QUEUE_SIZE;

    WifiEvent* ev = &wifi_event_queue[wifi_eq_write_idx];

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
    ev->orig_len         = (uint16_t)ppkt->rx_ctrl.sig_len;
    ev->payload_snap_len = (ppkt->rx_ctrl.sig_len < 256) ? (uint16_t)ppkt->rx_ctrl.sig_len : 256;
    memcpy(ev->payload_snap, ppkt->payload, ev->payload_snap_len);

    // Clear parsed fields — they'll be populated by parse_wifi_event()
    memset(ev->ssid, 0, sizeof(ev->ssid));
    ev->is_wpa2_psk = false;
    ev->vendor_oui_count = 0;

    __sync_synchronize();   // release: publish all field writes before `ready`
    ev->ready = true;
    wifi_eq_write_idx = next;
}

// Parse tagged parameters from a locally-copied WiFi event. Extracts SSID,
// RSN/WPA2-PSK status, and vendor OUIs from the raw payload snapshot.
// Called from process_wifi_event_queue() after the event has been copied
// out of the ring buffer — never from ISR/callback context.
static void parse_wifi_event(WifiEvent* ev) {
    // Locate the tagged parameters within the frame body.
    // Management frame: 24-byte MAC header, then frame body.
    // Beacon: 12 bytes of fixed fields (timestamp, interval, capability)
    //         before tagged parameters.
    // Probe Request: tagged parameters start immediately after MAC header.
    if (ev->payload_snap_len < 24) return;

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
    // Subtract FCS (4 bytes) if present in the snapshot
    remaining -= 4;
    if (remaining < 0) remaining = 0;

    // ── SSID extraction (tag 0) ──
    memset(ev->ssid, 0, sizeof(ev->ssid));
    if (remaining > 2 && tagged_params[0] == 0
        && tagged_params[1] <= 32 && tagged_params[1] <= remaining - 2) {
        memcpy(ev->ssid, &tagged_params[2], tagged_params[1]);
        ev->ssid[tagged_params[1]] = '\0';
    }

    // ── RSN and vendor OUI parsing ──
    ev->is_wpa2_psk = false;
    ev->vendor_oui_count = 0;

    if (ev->is_beacon) {
        uint8_t* p = tagged_params;
        int rem = remaining;
        while (rem >= 2) {
            uint8_t tag_id = p[0];
            uint8_t tag_len = p[1];
            if (tag_len > rem - 2) break;

            // RSN Information Element (tag 48) — check for WPA2-PSK AKM
            if (tag_id == 48 && tag_len >= 20) {
                uint8_t* rsn = p + 2;
                int rsn_len = tag_len;
                if (rsn_len >= 8) {
                    uint16_t pw_count = rsn[6] | (rsn[7] << 8);
                    int akm_off = 8 + pw_count * 4;
                    if (akm_off + 2 <= rsn_len) {
                        uint16_t akm_count = rsn[akm_off] | (rsn[akm_off + 1] << 8);
                        int akm_list = akm_off + 2;
                        for (uint16_t a = 0; a < akm_count && akm_list + 4 <= rsn_len; a++) {
                            if (rsn[akm_list] == 0x00 && rsn[akm_list+1] == 0x0F
                                && rsn[akm_list+2] == 0xAC && rsn[akm_list+3] == 0x02) {
                                ev->is_wpa2_psk = true;
                                break;
                            }
                            akm_list += 4;
                        }
                    }
                }
            }

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
    }
}

void process_wifi_event_queue() {
    // Cap drained events per call so a packet burst (e.g. crowded
    // conference center) cannot starve the rest of loop().
    int budget = 4;
    while (budget-- > 0 && wifi_event_queue[wifi_eq_read_idx].ready) {
        WifiEvent* ev = &wifi_event_queue[wifi_eq_read_idx];
        __sync_synchronize();   // acquire: pair with producer's release below

        WifiEvent local;
        memcpy(&local, ev, sizeof(WifiEvent));
        __sync_synchronize();   // release: ensure memcpy of *ev completes before producer can reuse slot
        ev->ready = false;
        wifi_eq_read_idx = (wifi_eq_read_idx + 1) % WIFI_EVENT_QUEUE_SIZE;

        if (local.rssi < IGNORE_WEAK_RSSI) continue;

        // Parse tagged parameters from the raw payload snapshot.
        // This was previously done in the sniffer callback; deferring it
        // here keeps the callback fast and prevents back-pressure on the
        // WiFi driver's internal queue in dense RF environments.
        parse_wifi_event(&local);

        clean_device_name_char(local.ssid);

        // Skip locally-administered (randomized) MACs for OUI scoring — a
        // randomized MAC can coincidentally match a Flock OUI prefix. SSID
        // pattern matching still runs (it doesn't depend on OUI), so a real
        // Flock device with a randomized MAC can still be detected via SSID.
        // BLE handles this via addr_type; WiFi needs an explicit bit check.
        bool is_random_mac = (local.mac[0] & 0x02) != 0;
        int  mac_score     = is_random_mac ? 0 : check_mac_prefix_tiered(local.mac);

        // Push to live feed (every observed device, preview flock status)
        {
            char mac_str_feed[18];
            snprintf(mac_str_feed, sizeof(mac_str_feed),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     local.mac[0], local.mac[1], local.mac[2],
                     local.mac[3], local.mac[4], local.mac[5]);
            const char* feed_name = (strlen(local.ssid) > 0) ? local.ssid : "Hidden";
            bool preview_is_flock  = (strlen(local.ssid) > 0
                                      && (is_flock_ssid_format(local.ssid)
                                          || check_ssid_pattern(local.ssid)))
                                     || mac_score == 1;
            feed_push_candidate(mac_str_feed, feed_name, local.rssi, 0, preview_is_flock);
        }

        int  confidence    = 0;
        char methods[64]   = {0};
        bool ssid_generic  = (strlen(local.ssid) > 0 && check_ssid_pattern(local.ssid));
        bool ssid_flock_fmt = (strlen(local.ssid) > 0 && is_flock_ssid_format(local.ssid));

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
            confidence = SCORE_DEFINITIVE;
            strlcat(methods, "wildcard_probe ", sizeof(methods));
        }

        if (confidence > 0 && local.rssi > -50) confidence += SCORE_BONUS_RSSI;
        if (ssid_flock_fmt && local.is_wpa2_psk) {
            strlcat(methods, "wpa2_psk ", sizeof(methods));
            confidence += 10;
        }

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
            local.mac[0], local.mac[1], local.mac[2],
            local.mac[3], local.mac[4], local.mac[5]);
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
                int addr1_mac_score = check_mac_prefix_tiered(local.addr1);
                if (addr1_mac_score > 0 && confidence == 0) {
                    // addr1 matched but addr2 didn't — sleeping-device hit.
                    if (addr1_mac_score == 1) {
                        confidence = SCORE_STRONG;
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
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            channel_lock_until = millis() + 10000;
            xSemaphoreGive(dataMutex);
            rssi_track_update(mac_str, local.rssi);
            if (rssi_track_is_stationary(mac_str)) confidence += SCORE_BONUS_STAT;
            if (confidence > 100) confidence = 100;
            locator_add_sample(mac_str, local.rssi);

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

            log_detection("FLOCK_WIFI", "WIFI", local.rssi, mac_str, name_str,
                          local.channel, 0, extra_combined, methods, confidence, local.seq_num);
            write_threat_pcap(local.payload_snap, local.payload_snap_len);

            xSemaphoreTake(dataMutex, portMAX_DELAY);
            if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                trigger_alarm_confidence = confidence;
                trigger_alarm_source = 0;  // WiFi
                last_buzzer_time = millis();
            }
            xSemaphoreGive(dataMutex);
        }
    }
}

// ============================================================================
// BLE CALLBACK — STACK-SAFE DEFERRED PROCESSING
// ============================================================================
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
    char     service_uuids[8][37];
    uint8_t  uuid_count;
    uint8_t  adv_channel;  // 37/38/39 if available, 0 if unknown
    volatile bool in_use;  // pool slot occupancy flag
};

// Static pool — eliminates all malloc/free from the BLE advertisement
// hot path. 12 slots × ~380 bytes ≈ 4.5KB static cost, zero fragmentation.
#define BLE_POOL_SIZE 12
static BleEventData ble_pool[BLE_POOL_SIZE];
static volatile uint8_t ble_pool_write = 0;

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

        if (ev->rssi < IGNORE_WEAK_RSSI) { ev->in_use = false; continue; }

        int  confidence   = 0;
        char methods[64]  = {0};
        char capture_type[16] = "FLOCK_BLE";
        bool got_name = false, got_mfg = false, got_raven = false;

        bool got_penguin_name = false;

        int mac_score = check_mac_prefix_tiered(ev->mac);

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
        }

        if (ev->have_mfg) {
            if (check_manufacturer_id(ev->mfg_data, ev->mfg_data_len)) {
                strlcat(methods, "mfg_0x09C8 ", sizeof(methods)); got_mfg = true;
                if (has_tn_serial(ev->mfg_data, ev->mfg_data_len)) strlcat(methods, "tn_serial ", sizeof(methods));
            }
        }

        int raven_custom_count = 0;
        int raven_std_count = 0;
        for (int i = 0; i < ev->uuid_count; i++) {
            for (int j = 0; j < NUM_RAVEN_CUSTOM_UUIDS; j++) {
                if (strcasecmp(ev->service_uuids[i], raven_custom_service_uuids[j]) == 0) {
                    raven_custom_count++; break;
                }
            }
            for (int j = 0; j < NUM_RAVEN_STANDARD_UUIDS; j++) {
                if (strcasecmp(ev->service_uuids[i], raven_standard_service_uuids[j]) == 0) {
                    raven_std_count++; break;
                }
            }
        }
        int raven_uuid_count = raven_custom_count + raven_std_count;

        if (raven_custom_count > 0 || raven_uuid_count >= 2) {
            strncpy(capture_type, "RAVEN_BLE", sizeof(capture_type)); got_raven = true;
            if (raven_uuid_count >= 3)        strlcat(methods, "raven_multi ", sizeof(methods));
            else if (raven_custom_count > 0)  strlcat(methods, "raven_custom ", sizeof(methods));
            else                              strlcat(methods, "raven_uuid ", sizeof(methods));
        }

        if (got_raven || got_name || got_mfg) {
            confidence = SCORE_DEFINITIVE;
            if (mac_score == 1) strlcat(methods, "mac_t1 ", sizeof(methods));
        } else if (mac_score == 1) {
            confidence = SCORE_STRONG;
            strlcat(methods, "mac_t1 ", sizeof(methods));
            if (got_penguin_name) { confidence = SCORE_DEFINITIVE; }
        } else {
            if (mac_score == 2) { confidence += SCORE_WEAK; strlcat(methods, "mac_t2 ", sizeof(methods)); }
            if (got_penguin_name) { confidence += SCORE_WEAK; }
            if ((mac_score == 2 || got_penguin_name) &&
                (ev->addr_type == 0 || (ev->addr_type == 1 && (ev->mac[0] >> 6) == 0x03))) {
                confidence += SCORE_WEAK; strlcat(methods, "static_addr ", sizeof(methods));
            }
        }
        if (confidence > 0 && ev->rssi > -50) confidence += SCORE_BONUS_RSSI;

        char mac_string[18];
        snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x",
            ev->mac[0], ev->mac[1], ev->mac[2],
            ev->mac[3], ev->mac[4], ev->mac[5]);

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            channel_lock_until = millis() + 10000;
            xSemaphoreGive(dataMutex);
            rssi_track_update(mac_string, ev->rssi);
            if (rssi_track_is_stationary(mac_string)) confidence += SCORE_BONUS_STAT;
            locator_add_sample(mac_string, ev->rssi);
        }
        if (confidence > 100) confidence = 100;

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
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

            log_detection(capture_type, "BLE", ev->rssi, mac_string, dev_name_char,
                          ev->adv_channel, ev->have_tx_power ? ev->tx_power : 0,
                          extra_data, methods, confidence, -1);

            xSemaphoreTake(dataMutex, portMAX_DELAY);
            if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                trigger_alarm_confidence = confidence;
                trigger_alarm_source = 1;  // BLE
                last_buzzer_time = millis();
            }
            xSemaphoreGive(dataMutex);
        }

        ev->in_use = false;
    }
}

class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        // Claim a pool slot. If the slot is still being processed by the
        // worker task, drop this advertisement — better than heap-allocating.
        uint8_t slot = ble_pool_write;
        if (ble_pool[slot].in_use) return;  // pool exhausted, drop

        BleEventData* ev = &ble_pool[slot];
        memset(ev, 0, sizeof(BleEventData));

        NimBLEAddress addr = advertisedDevice->getAddress();
        // NimBLE stores addresses little-endian; we display big-endian.
        // Pulling the raw bytes avoids a heap-allocated std::string and a
        // sscanf round-trip on every advertisement (50–200/sec under
        // typical urban traffic — the steady alloc churn was the most
        // consistent heap-fragmentation source in the code).
        const uint8_t* native = addr.getNative();
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
            ev->uuid_count = (uint8_t)(count > 8 ? 8 : count);
            for (int i = 0; i < ev->uuid_count; i++) {
                std::string uuid = advertisedDevice->getServiceUUID(i).toString();
                strncpy(ev->service_uuids[i], uuid.c_str(), 36);
                ev->service_uuids[i][36] = '\0';
            }
        }

        // NimBLE does not reliably expose advertising channel on ESP32.
        ev->adv_channel = 0;

        // Mark slot as occupied and advance the write cursor BEFORE queuing —
        // the worker dereferences ble_pool[idx] directly and relies on
        // in_use to know the slot is valid.
        ev->in_use = true;
        ble_pool_write = (slot + 1) % BLE_POOL_SIZE;

        // Queue the slot index. If the queue is full, release the slot.
        uint8_t idx = slot;
        if (xQueueSend(ble_event_queue, &idx, 0) != pdTRUE) {
            ev->in_use = false;
        }
    }
};

// Single shared instance — passed to setAdvertisedDeviceCallbacks at boot
// and on every periodic NimBLE restart. Avoids the slow heap leak that
// `new AdvertisedDeviceCallbacks()` produced every restart cycle.
static AdvertisedDeviceCallbacks ble_cb_singleton;

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
        if ((long)(now - channel_lock_until) > 0) {
            unsigned long dwell = low_power_mode ? 800UL : CHANNEL_DWELL_MS;
            if (now - last_channel_hop > dwell) {
                current_channel++; 
                if (current_channel > MAX_CHANNEL) current_channel = 1;
                esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); 
                last_channel_hop = now;
            }
        }

        unsigned long base_interval = low_power_mode ? 6000UL : BLE_SCAN_INTERVAL;
        unsigned long ble_interval = ((long)(now - channel_lock_until) < 0)
            ? BLE_SCAN_INTERVAL_LOCK
            : base_interval;

        bool scanning = pBLEScan ? pBLEScan->isScanning() : false;

        // Hang detection: if isScanning() has been true for more than 2x the
        // configured scan duration plus 3s headroom, the NimBLE stack has
        // wedged. Force-stop and clear so the next branch can restart cleanly.
        const unsigned long SCAN_HANG_LIMIT_MS =
            (unsigned long)(BLE_SCAN_DURATION * 2000UL + 3000UL);
        if (scanning && pBLEScan && scan_start_ms > 0 &&
            (now - scan_start_ms) > SCAN_HANG_LIMIT_MS) {
            Serial.println("[BLE] Scan hang detected — forcing stop");
            pBLEScan->stop();
            pBLEScan->clearResults();
            scanning = false;
            scan_start_ms = 0;
            last_ble_scan = now;  // delay next start by full interval
        }

        if (pBLEScan && millis() - last_ble_scan >= ble_interval) {
            if (!scanning) {
                bool active = low_power_mode ? false : (ble_scan_cycle % 3 == 0);
                pBLEScan->setActiveScan(active);
                pBLEScan->start(BLE_SCAN_DURATION, false);
                scan_start_ms = millis();
                last_ble_scan = millis();
                ble_scan_cycle++;
                scanning = true;  // we just started
            }
        }
        if (pBLEScan && !scanning &&
            (millis() - last_ble_scan > (unsigned long)(BLE_SCAN_DURATION * 1000 + 500))) {
            pBLEScan->clearResults();
            scan_start_ms = 0;  // clear so a stale value doesn't trip hang check
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
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            for(int i = 0; i < bytes_read; i++) {
                gps.encode(buf[i]);
            }
            xSemaphoreGive(dataMutex);
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

void LocatorChimeTask(void* pvParameters) {
    esp_task_wdt_add(NULL);

    // Same I2S flush as AlarmTask — see comment there.
    M5Cardputer.Speaker.stop();
    vTaskDelay(10 / portTICK_PERIOD_MS);

    if (!stealth_mode && !is_muted) {
        M5Cardputer.Speaker.tone(660, 70);
        vTaskDelay(80 / portTICK_PERIOD_MS);
        M5Cardputer.Speaker.tone(880, 90);
    }
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

void play_escalated_alarm(int confidence, int source) {
    if (stealth_mode || is_muted || is_alarming) return;
    is_alarming = true;
    intptr_t param = ((intptr_t)confidence & 0xFFFF) | ((intptr_t)(source & 0x1) << 16);
    xTaskCreate(AlarmTask, "AlarmTask", 2048, (void*)param, 2, NULL);
}

// ============================================================================
// UI RENDERING - BASE COMPONENTS
// ============================================================================
void draw_header_spr(int screen_num) {
    static const char* screen_names[NUM_SCREENS] = {
        "SCANNER", "LOCATOR", "DETECTIONS", "GPS", "STATS"
    };
    if (screen_num < 0 || screen_num >= NUM_SCREENS) screen_num = 0;

    spr.fillRect(0, 0, DISP_W, 20, BG_COLOR);
    spr.setTextColor(HEADER_COLOR, BG_COLOR); spr.setTextSize(TS_BODY);
    spr.setCursor(4, 5); kprint(spr, screen_names[screen_num]);

    // ── Status pill row ─────────────────────────────────────────────────────
    // Right-aligned pills: mode badges, counts, status indicators.
    // Each pill renders right-to-left, tracking icon_right as the cursor.

    bool gps_lock_now;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    gps_lock_now = gps.satellites.isValid() && gps.satellites.value() >= 1;
    long pill_det  = lifetime_flock_total;
    xSemaphoreGive(dataMutex);
    bool muted_now = is_muted;

    int icon_right = DISP_W - 4;
    int icon_y = 4;

    // Mode badges: N / S / L / P — same as before but using drawPill
    {
        struct ModeBadge {
            bool active;
            const char* letter;
            uint16_t color;
        };
        ModeBadge badges[4] = {
            { night_mode,      "N", HEADER_COLOR },
            { stealth_mode,    "S", DIM_COLOR },
            { locator_active,  "L", CAUTION_COLOR },
            { low_power_mode,  "P", ACCENT_COLOR },
        };
        for (int i = 0; i < 4; i++) {
            if (!badges[i].active) continue;
            int pw = (int)strlen(badges[i].letter) * 7 + 6;
            drawPill(icon_right - pw, icon_y, badges[i].letter, badges[i].color);
            icon_right -= pw + 2;
        }
    }

    // Export mode indicator
    if (export_mode_active) {
        drawPill(icon_right - 27, icon_y, "EXP", CAUTION_COLOR, 0.0f, true);
        icon_right -= 29;
    }

    // Muted indicator
    if (muted_now) {
        drawPill(icon_right - 13, icon_y, "M", DIM_COLOR);
        icon_right -= 15;
    }

    // SD missing
    if (system_fully_booted && !sd_available) {
        uint16_t sd_warn = lgfx::color565(180, 40, 40);
        drawPill(icon_right - 20, icon_y, "SD", sd_warn);
        icon_right -= 22;
    }

    // GPS missing
    if (!gps_lock_now) {
        drawPill(icon_right - 27, icon_y, "GPS", CAUTION_COLOR);
        icon_right -= 29;
    }

    // Detection count pill — filled accent, inverted text
    {
        char det_str[8];
        snprintf(det_str, sizeof(det_str), "D%lu", (unsigned long)pill_det);
        int dw = (int)strlen(det_str) * 7 + 6;
        drawPill(icon_right - dw, icon_y, det_str, ACCENT_COLOR, 0.0f, true);
        icon_right -= dw + 2;
    }
}

void draw_toast_spr() {
    // Snapshot all toast state under mutex before rendering. Producer tasks
    // on either core can write toast_text mid-strncpy; rendering from live
    // globals would read torn data and crash spr.print() when it walks past
    // a missing null terminator.
    bool          active_snap;
    char          text_snap[32];
    uint16_t      accent_snap    = 0;
    bool          is_action_snap = false;
    unsigned long start_snap     = 0;
    int           queue_count_snap = 0;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    active_snap = toast_active;
    if (active_snap) {
        strncpy(text_snap, toast_text, sizeof(text_snap) - 1);
        text_snap[sizeof(text_snap) - 1] = '\0';
        accent_snap      = toast_accent_color;
        is_action_snap   = toast_is_action;
        start_snap       = toast_start;
        queue_count_snap = toast_queue_count;
    }
    xSemaphoreGive(dataMutex);

    if (!active_snap) return;

    unsigned long elapsed = millis() - start_snap;

    // Expiration handling — advance queue or clear under mutex
    if (elapsed > TOAST_DURATION_MS) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (toast_queue_count > 0) {
            toast_queue_head = (toast_queue_head + 1) % TOAST_QUEUE_SIZE;
            toast_queue_count--;
        }
        if (toast_queue_count > 0) {
            strncpy(toast_text, toast_queue[toast_queue_head].text, sizeof(toast_text) - 1);
            toast_text[sizeof(toast_text) - 1] = '\0';
            toast_accent_color = toast_queue[toast_queue_head].accent;
            toast_is_action    = toast_queue[toast_queue_head].is_action;
            toast_start        = millis();
            toast_active       = true;
        } else {
            toast_active = false;
        }
        xSemaphoreGive(dataMutex);
        return;
    }

    // Render from snapshot — no global toast_* access below this point.
    int y_pos = DISP_H - 34;

    // Fade in over UI_FADE_IN_MS, hold, fade out over UI_FADE_OUT_MS.
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

    // Full-width toast with 4px side margins, 3px left accent bar
    int t_x = 4;
    int t_w = DISP_W - 8;
    int t_h = 22;

    // Card background and border
    spr.fillRect(t_x, y_pos, t_w, t_h, ta(CARD_COLOR));
    spr.drawRect(t_x, y_pos, t_w, t_h, ta(CARD_BORDER));

    // Left accent bar — 3px wide, inset by 1px from card edge
    spr.fillRect(t_x, y_pos, 3, t_h, ta(accent));

    // Sentinel: [i] or [!]
    int text_y = y_pos + (t_h - 8) / 2;  // vertically center 8px text
    spr.setTextColor(ta(accent), ta(CARD_COLOR));
    spr.setTextSize(TS_BODY);
    spr.setCursor(t_x + 7, text_y);
    spr.print(is_action_snap ? "[i]" : "[!]");

    // Toast text
    spr.setTextColor(ta(TEXT_COLOR), ta(CARD_COLOR));
    spr.setCursor(t_x + 30, text_y);
    spr.print(text_snap);

    // Queue count
    if (queue_count_snap > 1) {
        char qnum[6];
        snprintf(qnum, sizeof(qnum), "+%d", queue_count_snap - 1);
        spr.setTextColor(ta(DIM_COLOR), ta(CARD_COLOR));
        spr.setCursor(t_x + t_w - 24, text_y);
        spr.print(qnum);
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

    // Pill geometry — matches the boot bar: thin fill inside a generous
    // rounded outline, centered on the screen.
    const int bar_w = 130;
    const int bar_h = 20;
    const int bar_x = (DISP_W - bar_w) / 2;
    const int bar_y = DISP_H / 2 + 8;
    const int bar_r = bar_h / 2;

    int vol_pct = is_muted ? 0 : map(current_volume, 0, 255, 0, 100);
    char vol_str[12];
    if (is_muted) {
        snprintf(vol_str, sizeof(vol_str), "MUTED");
    } else {
        snprintf(vol_str, sizeof(vol_str), "VOL %d%%", vol_pct);
    }
    spr.setTextColor(va(is_muted ? DIM_COLOR : TEXT_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setTextDatum(TC_DATUM);
    spr.drawString(vol_str, DISP_W / 2, bar_y - 20);
    spr.setTextDatum(TL_DATUM);

    // Outer outline — single 1px rounded rect.
    spr.drawRoundRect(bar_x, bar_y, bar_w, bar_h, bar_r, va(HEADER_COLOR));

    // Inner fill — thin 4px pill, vertically centered, inset by the
    // outline radius so it never enters the rounded corner zone.
    const int fill_h = 4;
    const int fill_y = bar_y + (bar_h - fill_h) / 2;
    const int fill_max_w = bar_w - bar_h;
    const int fill_x = bar_x + bar_h / 2;
    int fill_w = is_muted ? 0 : (current_volume * fill_max_w) / 255;

    if (fill_w > 0) {
        int fill_r = fill_h / 2;
        if (fill_r < 1) fill_r = 1;
        if (fill_w < fill_r * 2) {
            spr.fillRect(fill_x, fill_y, fill_w, fill_h, va(HEADER_COLOR));
        } else {
            spr.fillRoundRect(fill_x, fill_y, fill_w, fill_h, fill_r, va(HEADER_COLOR));
        }
    }
}

void drawCard(int x, int y, int w, int h) {
    spr.fillRect(x, y, w, h, CARD_COLOR); spr.drawRect(x, y, w, h, CARD_BORDER);
}

// Draw a header-bar status pill: rounded rect with text inside.
// bg_accent_pct: how much of accent_col to mix into BG (0.0–1.0).
// filled: if true, the pill is solid accent_col with BG-colored text.
static void drawPill(int x, int y, const char* text, uint16_t accent_col,
                     float bg_accent_pct, bool filled) {
    int tw = (int)strlen(text) * 7 + 6;  // 7px per char + 6px padding
    int th = 11;
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

void draw_scroll_fade(int region_y, int region_h, int fade_height, bool top) {
    for (int i = 0; i < fade_height; i++) {
        int y = top ? (region_y + i) : (region_y + region_h - 1 - i);
        float t = 1.0f - ((float)i / (float)fade_height);
        uint16_t fade_col = lerp_col16(BG_COLOR, lgfx::color565(0, 0, 0), t * 0.6f);
        for (int x = 0; x < DISP_W; x += 2) {
            if ((x + i) & 1) spr.drawPixel(x, y, fade_col);
        }
    }
}

void draw_help_overlay() {
    float alpha = ui_progress(help_ease_start, UI_FADE_IN_MS);
    if (alpha < 0.02f) return;

    auto ea = [&](uint16_t c) -> uint16_t { return lerp_col16(BG_COLOR, c, alpha); };

    // Solid backdrop below header (like expanded feed)
    spr.fillRect(0, 18, DISP_W, DISP_H - 18, BG_COLOR);

    // Subtitle in header area (overwrite screen name)
    spr.fillRect(0, 0, 90, CONTENT_Y, BG_COLOR);
    spr.setTextColor(ea(lerp_col16(HEADER_COLOR, ACCENT_COLOR, 0.4f)), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setCursor(4, 5);
    kprint(spr, "HELP");

    struct HelpKey { const char* key; const char* desc; };
    const HelpKey* keys;
    int key_count;

    static const HelpKey global_keys[] = {
        {"-/+",  "volume"},
        {"`",    "mute"},
        {"DEL",  "back"},
        {"ESC",  "home"},
        {"f",    "feed"},
    };

    static const HelpKey scanner_keys[] = {
        {"x",   "simulate"},
        {"t",   "locate"},
        {"m",   "menu"},
    };
    static const HelpKey locator_keys[] = {
        {"l",   "start/stop"},
        {"t",   "target"},
        {"n",   "north"},
    };
    static const HelpKey detections_keys[] = {
        {";/.", "navigate"},
        {"ENT", "detail"},
    };
    static const HelpKey gps_keys[] = {
        {"",    "no keys"},
    };
    static const HelpKey devinfo_keys[] = {
        {";/.", "scroll"},
        {"m",   "menu (clear)"},
    };

    switch (current_screen) {
        case 0: keys = scanner_keys;    key_count = sizeof(scanner_keys)/sizeof(scanner_keys[0]);       break;
        case 1: keys = locator_keys;    key_count = sizeof(locator_keys)/sizeof(locator_keys[0]);       break;
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
        int y = 24;

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

        // Stat guide — two columns, 7 rows × 2 = 13 cells (last cell blank).
        struct Desc { const char* name; const char* desc; };
        static const Desc descs[] = {
            {"DETECT",   "total"},
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
        };
        const int n_desc = sizeof(descs) / sizeof(descs[0]);
        const int rows   = (n_desc + 1) / 2;  // 7

        spr.setTextSize(TS_MICRO);
        for (int r = 0; r < rows && y < DISP_H - 11; r++) {
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
        spr.setCursor(UI_PAD_SM, DISP_H - 10);
        spr.print("TAB close  M=clear stats");
        spr.setCursor(DISP_W - 30, DISP_H - 10);
        spr.print("v9.6");
        return;
    }

    const int ROW_H = 10;
    const int col_left_x  = UI_PAD_SM;
    const int col_right_x = DISP_W / 2 + 4;
    int row_y;

    // ── Left column: screen-specific keys ──
    row_y = 24;
    spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setCursor(col_left_x, row_y);
    kprint(spr, "THIS SCREEN");
    row_y += ROW_H + 2;

    for (int i = 0; i < key_count && row_y < DISP_H - 12; i++) {
        spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
        spr.setCursor(col_left_x, row_y);
        spr.print(keys[i].key);
        spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
        spr.setCursor(col_left_x + 28, row_y);
        spr.print(keys[i].desc);
        row_y += ROW_H;
    }

    // ── Right column: global keys ──
    row_y = 24;
    spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setCursor(col_right_x, row_y);
    kprint(spr, "GLOBAL");
    row_y += ROW_H + 2;

    int global_count = sizeof(global_keys) / sizeof(global_keys[0]);
    for (int i = 0; i < global_count && row_y < DISP_H - 12; i++) {
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
    spr.setCursor(UI_PAD_SM, DISP_H - 10);
    spr.print("TAB to close");
    // Version tag, right-aligned
    spr.setCursor(DISP_W - 30, DISP_H - 10);
    spr.print("v9.6");
}

// Draw a dithered darkening overlay — fast checkerboard pattern that
// darkens every-other pixel toward a target color, creating the visual
// effect of semi-transparency without per-pixel alpha blending.
static void draw_dither_overlay(int x0, int y0, int w, int h, uint16_t dark_color) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0 + ((y & 1) ? 1 : 0); x < x0 + w; x += 2) {
            spr.drawPixel(x, y, dark_color);
        }
    }
}

// Soft UI click — brief tone that reads as a tactile tick.
static void menu_click() {
    if (stealth_mode || is_muted) return;
    M5Cardputer.Speaker.tone(660, 5);  // soft mechanical-keyboard tick
}

void draw_menu_overlay() {
    unsigned long now_ms = millis();
    float alpha = ui_progress(menu_open_ms, UI_FADE_IN_MS);
    auto ea = [&](uint16_t c) -> uint16_t { return lerp_col16(BG_COLOR, c, alpha); };

    // Solid backdrop matching the rest of the app
    spr.fillRect(0, 18, DISP_W, DISP_H - 18, BG_COLOR);

    // Overwrite the screen name area with "MENU"
    spr.fillRect(0, 0, 90, CONTENT_Y, BG_COLOR);
    spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);  // matches draw_header_spr style
    spr.setCursor(4, 5);
    kprint(spr, "MENU");

    struct MenuLine {
        const char* label;
        uint16_t    accent;
    };

    char night_label[20], lp_label[20], mute_label[20], export_label[28];
    snprintf(night_label, sizeof(night_label), "Night Mode: %s", night_mode ? "ON" : "OFF");
    snprintf(lp_label,    sizeof(lp_label),    "Low Power: %s",  low_power_mode ? "ON" : "OFF");
    snprintf(mute_label,  sizeof(mute_label),  "Mute: %s",       is_muted ? "ON" : "OFF");
    if (export_mode_active) {
        snprintf(export_label, sizeof(export_label), "Export: %s", export_ip_str);
    } else {
        snprintf(export_label, sizeof(export_label), "Export Data");
    }

    MenuLine items[MENU_ITEM_COUNT] = {
        {"Scanner",       HEADER_COLOR},
        {"Locator",       HEADER_COLOR},
        {"Detections",    HEADER_COLOR},
        {"GPS",           HEADER_COLOR},
        {"Stats",         HEADER_COLOR},
        {night_label,     DIM_COLOR},
        {lp_label,        DIM_COLOR},
        {mute_label,      DIM_COLOR},
        {"WiFi Config",   DIM_COLOR},
        {export_label,    export_mode_active ? ACCENT_COLOR : DIM_COLOR},
        {"Clear Stats",   CAUTION_COLOR},
    };

    if (menu_selected < 0) menu_selected = 0;
    if (menu_selected >= MENU_ITEM_COUNT) menu_selected = MENU_ITEM_COUNT - 1;

    const int row_h    = 14;
    const int row_gap  = UI_PAD_SM;          // 6px standard gap
    const int start_y  = CONTENT_Y + UI_PAD_XS;  // 2px below header

    // Scrollable viewport — compute how many items fit between start_y and bottom
    // (footer was removed; the row space extends to the bottom of the screen).
    const int footer_y = DISP_H - 2;
    const int visible_h = footer_y - start_y;
    const int menu_visible_count = visible_h / (row_h + row_gap);

    if (menu_selected < menu_scroll_offset) {
        menu_scroll_offset = menu_selected;
    }
    if (menu_selected >= menu_scroll_offset + menu_visible_count) {
        menu_scroll_offset = menu_selected - menu_visible_count + 1;
    }
    if (menu_scroll_offset < 0) menu_scroll_offset = 0;
    int max_scroll = MENU_ITEM_COUNT - menu_visible_count;
    if (max_scroll < 0) max_scroll = 0;
    if (menu_scroll_offset > max_scroll) menu_scroll_offset = max_scroll;

    // Instant highlight snap — no easing. The reduced row height makes
    // interpolation look like jitter rather than smooth motion.
    int sel_vi = menu_selected - menu_scroll_offset;
    int ease_y = start_y + sel_vi * (row_h + row_gap);

    // ── Row grid — only draw visible items ──
    for (int vi = 0; vi < menu_visible_count && (vi + menu_scroll_offset) < MENU_ITEM_COUNT; vi++) {
        int i = vi + menu_scroll_offset;
        int y = start_y + vi * (row_h + row_gap);

        spr.fillRect(0, y, DISP_W, row_h, BG_COLOR);

        int text_x = 16;  // padded to clear the selection bar
        // Selected item renders in header color (ice blue), others in text color.
        // Creates a "glow" effect without position shifts.
        uint16_t item_col = (i == menu_selected) ? HEADER_COLOR : TEXT_COLOR;
        spr.setTextColor(ea(item_col), BG_COLOR);
        spr.setTextSize(TS_STRONG);
        spr.setCursor(text_x, y + 1);
        spr.print(items[i].label);
    }

    // ── Selection bar ──
    // 3px wide vertical bar on the left edge, full row height.
    // Text color is already set per-item in the row grid loop.
    {
        spr.fillRect(UI_PAD_XS, ease_y, 3, row_h, ea(HEADER_COLOR));
    }

    // ── Scrollbar — only show if list exceeds viewport ──
    if (MENU_ITEM_COUNT > menu_visible_count) {
        const int track_x = DISP_W - 4;
        const int track_y = start_y;
        const int track_h = menu_visible_count * (row_h + row_gap) - row_gap;

        spr.drawFastVLine(track_x + 1, track_y, track_h, ea(CARD_BORDER));

        int thumb_h = max(8, (track_h * menu_visible_count) / MENU_ITEM_COUNT);
        float scroll_t = (max_scroll > 0) ? (float)menu_scroll_offset / (float)max_scroll : 0.0f;
        int thumb_y = track_y + (int)(scroll_t * (float)(track_h - thumb_h));

        spr.fillRect(track_x, thumb_y, 3, thumb_h, ea(HEADER_COLOR));
    }

    // Footer removed — extra row space goes to menu items. The keys are
    // documented in the help overlay (TAB) and are intuitive: ; / . scroll,
    // ENTER selects, DEL closes.
}

void draw_wifi_config_overlay() {
    float alpha = ui_progress(wifi_config_open_ms, UI_FADE_IN_MS);
    auto ea = [&](uint16_t c) -> uint16_t { return lerp_col16(BG_COLOR, c, alpha); };

    // Solid backdrop
    spr.fillRect(0, 18, DISP_W, DISP_H - 18, BG_COLOR);

    // Overwrite header with "WIFI CONFIG"
    spr.fillRect(0, 0, 90, CONTENT_Y, BG_COLOR);
    spr.setTextColor(ea(HEADER_COLOR), BG_COLOR);
    spr.setTextSize(TS_BODY);
    spr.setCursor(4, 5);
    kprint(spr, "WIFI CONFIG");

    // Outer card — outline only on BG, matching the lighter feel of the
    // stats and detections screens. Selected fields/buttons are
    // distinguished by border color, never by fill.
    int cx = 4, cy = CONTENT_Y + UI_PAD_XS, cw = DISP_W - 8, ch = DISP_H - CONTENT_Y - UI_PAD_SM;
    spr.drawRoundRect(cx, cy, cw, ch, 4, ea(HEADER_COLOR));

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

    const char* ssid_display = (wifi_config_editing && ssid_selected)
        ? wifi_config_ssid_buf
        : export_ssid;
    bool ssid_empty = (strlen(ssid_display) == 0);

    spr.setTextColor(ssid_empty ? ea(DIM_COLOR) : ea(TEXT_COLOR), BG_COLOR);
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
            int cursor_x = cx + 9 + wifi_config_cursor * 6;
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
    kprint(spr, "PASS");

    input_y = field_y + 12;
    uint16_t pass_border = pass_selected ? ea(HEADER_COLOR) : ea(CARD_BORDER);
    spr.drawRect(cx + 6, input_y, cw - 12, 16, pass_border);

    const char* pass_src = (wifi_config_editing && pass_selected)
        ? wifi_config_pass_buf
        : export_pass;
    bool pass_empty = (strlen(pass_src) == 0);

    spr.setTextColor(pass_empty ? ea(DIM_COLOR) : ea(TEXT_COLOR), BG_COLOR);
    spr.setTextSize(TS_MICRO);
    spr.setCursor(cx + 9, input_y + 4);

    if (pass_empty && !pass_editing) {
        spr.print("(not set)");
    } else {
        if (pass_editing) {
            char display[66];
            strncpy(display, pass_src, 64);
            display[64] = '\0';
            spr.print(display);
        } else {
            int plen = strlen(pass_src);
            for (int i = 0; i < plen && i < 32; i++) spr.print("*");
        }
        if (pass_editing && cursor_visible) {
            int cursor_x = cx + 9 + wifi_config_cursor * 6;
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
        spr.print("type to edit  ENT done  DEL bksp");
    } else {
        spr.print("ENT edit  ;/. field  DEL close");
    }
}

void handle_menu_select() {
    switch (menu_selected) {
        case 0: case 1: case 2: case 3: case 4: {
            int target = menu_selected;
            show_menu = false;
            transition_screen(target, (target >= current_screen) ? 1 : -1);
            break;
        }
        case 5:
            night_mode = !night_mode;
            apply_color_palette();
            draw_current_screen(); spr.pushSprite(0, 0);
            break;
        case 6:
            low_power_mode = !low_power_mode;
            if (low_power_mode) {
                setCpuFrequencyMhz(80);
                set_toast_direct("LOW POWER ON", ACCENT_COLOR);
            } else {
                setCpuFrequencyMhz(160);
                set_toast_direct("LOW POWER OFF", DIM_COLOR);
            }
            apply_ble_scan_params();
            draw_current_screen(); spr.pushSprite(0, 0);
            break;
        case 7:
            is_muted = !is_muted;
            if (!is_muted) beep(600, 50);
            draw_current_screen(); spr.pushSprite(0, 0);
            break;
        case 8:
            show_menu = false;
            wifi_config_open = true;
            wifi_config_open_ms = millis();
            wifi_config_field = 0;
            wifi_config_editing = false;
            strncpy(wifi_config_ssid_buf, export_ssid, sizeof(wifi_config_ssid_buf) - 1);
            wifi_config_ssid_buf[sizeof(wifi_config_ssid_buf) - 1] = '\0';
            strncpy(wifi_config_pass_buf, export_pass, sizeof(wifi_config_pass_buf) - 1);
            wifi_config_pass_buf[sizeof(wifi_config_pass_buf) - 1] = '\0';
            wifi_config_cursor = 0;
            draw_current_screen(); spr.pushSprite(0, 0);
            break;
        case 9:
            if (export_mode_active) {
                // First tap while active: show IP reminder; DEL or second tap to stop
                char ip_toast[32];
                snprintf(ip_toast, sizeof(ip_toast), "http://%s", export_ip_str);
                set_toast_direct(ip_toast, ACCENT_COLOR);
                draw_current_screen(); spr.pushSprite(0, 0);
            } else if (export_connecting) {
                show_menu = false;
                export_mode_stop();
                draw_current_screen(); spr.pushSprite(0, 0);
            } else {
                show_menu = false;
                export_mode_start();
                draw_current_screen(); spr.pushSprite(0, 0);
            }
            break;
        case 10: {
            // Clear all stats — session and lifetime
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            session_wifi = 0; session_ble = 0;
            session_flock_wifi = 0; session_flock_ble = 0;
            session_raven = 0;
            lifetime_wifi = 0; lifetime_ble = 0;
            lifetime_flock_total = 0;
            lifetime_seconds = 0;
            lifetime_boots = 0;
            lifetime_flash_writes = 0;
            session_start_time = millis();
            xSemaphoreGive(dataMutex);
            // Hand the write off to PersistTask on Core 1 so the main
            // loop doesn't freeze for the LittleFS + SD round-trip
            // (~200-800ms, longer on a slow card). schedule_persist
            // already no-ops if a write is in flight.
            schedule_persist();
            set_toast_direct("STATS CLEARED", CAUTION_COLOR);
            draw_current_screen(); spr.pushSprite(0, 0);
            break;
        }
    }
}

// ============================================================================
// UI RENDERING - SCREENS 
// ============================================================================
void draw_scanner_screen() {
    int divider_x = 112;
    spr.fillSprite(BG_COLOR);
    draw_header_spr(0);
    unsigned long frame_ms = millis();
    spr.setClipRect(0, 18, divider_x, DISP_H - 18);
    
    float TILT = 0.55f;
    int rcx = radar_cx;
    int rcy = radar_cy;
    int THICKNESS = 7;

    // Shadow below cylinder
    spr.fillEllipse(rcx, rcy + THICKNESS + 2, radar_r, radar_r * TILT, lgfx::color565(4, 8, 16));

    // Cylinder wall: solid gradient fills from bottom to top (lighter toward top)
    for (int i = THICKNESS; i >= 1; i--) {
        uint8_t wall_v = 8 + (THICKNESS - i) * 2;
        spr.fillEllipse(rcx, rcy + i, radar_r, radar_r * TILT,
                        lgfx::color565(wall_v, wall_v * 2, wall_v * 4));
    }

    // Left/right edge lines connecting top rim to bottom rim (removed)

    // Top face fill and border — draw ellipse twice (y offset 1) for thicker rim
    spr.fillEllipse(rcx, rcy, radar_r, radar_r * TILT, lgfx::color565(14, 26, 52));
    spr.drawEllipse(rcx, rcy,     radar_r, (int)(radar_r * TILT), HEADER_COLOR);
    spr.drawEllipse(rcx, rcy + 1, radar_r, (int)(radar_r * TILT), HEADER_COLOR);

    // Hatching on both sides of the cylinder rim seam

    // Redraw bottom rim so it shows over the top face fill — doubled for thickness
    spr.drawEllipse(rcx, rcy + THICKNESS,     radar_r, radar_r * TILT, DIM_COLOR);
    spr.drawEllipse(rcx, rcy + THICKNESS + 1, radar_r, radar_r * TILT, DIM_COLOR);

    // Structural ribs on left wall removed

    spr.drawEllipse(rcx, rcy,     inner_r, (int)(inner_r * TILT), DIM_COLOR);
    spr.drawEllipse(rcx, rcy + 1, inner_r, (int)(inner_r * TILT), DIM_COLOR);

    float sweep_rad = (frame_ms / 3000.0f) * (float)M_PI * 2.0f;

    static bool p_init = false;
    if (!p_init) {
        for (int i = 0; i < NUM_PARTICLES; i++) {
            noise_r[i]        = (float)random(inner_r + 4, radar_r - 4);
            noise_a[i]        = random(0, 628) * 0.01f;
            noise_max_life[i] = random(1200, 2200);
            noise_life[i]     = 0;
        }
        p_init = true;
    }

    const float TWO_PIf      = (float)M_PI * 2.0f;
    float sweep_norm = fmodf(sweep_rad, TWO_PIf);

    // Drive particles from real ambient packet activity
    {
        static uint32_t last_ambient_count = 0;
        static int particle_pending = 0;
        uint32_t cur_ambient = ambient_packet_count;
        uint32_t delta = cur_ambient - last_ambient_count;
        last_ambient_count = cur_ambient;
        // Each packet contributes 1–3 particles (clamped)
        particle_pending += (int)delta * 2;
        if (particle_pending > NUM_PARTICLES * 3) particle_pending = NUM_PARTICLES * 3;
        // Activate up to particle_pending idle particles this frame
        for (int i = 0; i < NUM_PARTICLES && particle_pending > 0; i++) {
            if (noise_life[i] <= 0) {
                noise_r[i]    = (float)random(inner_r + 4, radar_r - 4);
                noise_a[i]    = random(0, 628) * 0.01f;
                noise_life[i] = noise_max_life[i];
                particle_pending--;
            }
        }
    }

    // Static to avoid stack pressure — rendering is single-threaded from loop().
    static float local_spikes[NUM_RADIAL_BANDS];
    static uint16_t local_colors[NUM_RADIAL_BANDS];

    static const float SPIKE_DECAY_PER_MS = 0.00004f;
    static unsigned long last_decay_ms = 0;
    unsigned long now_ms = millis();
    float decay_amount = SPIKE_DECAY_PER_MS * (float)(now_ms - last_decay_ms);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    unsigned long time_since_blip = now_ms - last_blip_time;
    for (int i = 0; i < NUM_RADIAL_BANDS; i++) {
        // Enforce 2-minute max detection lifetime
        if (radial_spike_birth[i] > 0 && (now_ms - radial_spike_birth[i]) > 120000UL) {
            radial_spikes[i]      = 0;
            radial_spike_birth[i] = 0;
        }
        local_spikes[i] = radial_spikes[i];
        local_colors[i] = radial_colors[i];
        radial_spikes[i] -= decay_amount;
        if (radial_spikes[i] < 0) radial_spikes[i] = 0;
    }
    xSemaphoreGive(dataMutex);
    last_decay_ms = now_ms;

    static unsigned long last_particle_ms = 0;
    unsigned long dt_ms = now_ms - last_particle_ms;
    if (dt_ms > 100) dt_ms = 100;
    last_particle_ms = now_ms;

    float noise_dimming = 1.0f;
    if (time_since_blip < 10000) {
        noise_dimming = 0.0f;
    } else if (time_since_blip < 13000) {
        noise_dimming = (time_since_blip - 10000) / 3000.0f;
    }

    if (noise_dimming > 0.01f) {
        for (int i = 0; i < NUM_PARTICLES; i++) {
            if (noise_life[i] <= 0) continue;

            noise_life[i] -= (int)dt_ms;
            if (noise_life[i] < 0) noise_life[i] = 0;

            float fade = (float)noise_life[i] / (float)noise_max_life[i];
            int intensity = (int)(fade * 180.0f * noise_dimming);
            if (intensity > 200) intensity = 200;
            if (intensity < 20) continue;

            // Sweep-tied brightness: particle brightens dramatically when sweep line
            // passes over its angle, dims otherwise. Makes the sweep feel like it's
            // "illuminating" the ambient RF environment.
            float pa = fmodf(noise_a[i], TWO_PIf);
            float diff_sweep = sweep_norm - pa;
            if (diff_sweep >  (float)M_PI) diff_sweep -= TWO_PIf;
            if (diff_sweep < -(float)M_PI) diff_sweep += TWO_PIf;
            // Only show particles in the sweep's wake (behind the sweep line)
            // diff_sweep > 0 means sweep has passed this angle
            float trail_arc = 2.0f;  // radians of visible trail behind sweep
            if (diff_sweep < 0) diff_sweep += TWO_PIf;  // normalize to [0, 2π]
            if (diff_sweep > trail_arc) continue;  // too far behind sweep — not visible
            float sweep_proximity = 1.0f - (diff_sweep / trail_arc);  // 1 at sweep, 0 at trail_arc behind
            int sweep_boost = (int)(sweep_proximity * 80);
            int total_intensity = intensity + sweep_boost;
            if (total_intensity > 255) total_intensity = 255;

            int px = rcx + (int)(noise_r[i] * cosf(noise_a[i]));
            int py = rcy + (int)(noise_r[i] * sinf(noise_a[i]) * TILT);

            // Day: ice-blue tinted particles. Night: red tinted.
            uint16_t p_col = night_mode
                ? lgfx::color565(total_intensity, total_intensity / 8, total_intensity / 12)
                : lgfx::color565(total_intensity / 3, (total_intensity * 3) / 4, total_intensity);

            // Draw small upward triangle (matches detection triangle vocabulary but smaller)
            // Size 3: base 3px, height 3px
            spr.drawTriangle(px - 1, py + 1,
                             px + 1, py + 1,
                             px,     py - 2,
                             p_col);

            // Extra bright near sweep: fill the triangle for emphasis
            if (sweep_proximity > 0.5f) {
                spr.drawPixel(px, py - 1, p_col);
                spr.drawPixel(px, py, p_col);
            }
        }
    }

    float breathe = anim_pulse(UI_PULSE_MEDIUM) * 2.0f - 1.0f;  // remap to -1..1
    uint16_t modulated_col = (breathe > 0) ? HEADER_COLOR : DIM_COLOR;
    int dynamic_r   = (radar_r - 6) + (int)(4 * breathe);
    float cos_rot   = cosf(hud_rotation);
    float sin_rot   = sinf(hud_rotation);
    spr.drawLine(rcx - dynamic_r * cos_rot, rcy - dynamic_r * sin_rot * TILT,
                 rcx + dynamic_r * cos_rot, rcy + dynamic_r * sin_rot * TILT, modulated_col);
    spr.drawLine(rcx - dynamic_r * -sin_rot, rcy - dynamic_r * cos_rot * TILT,
                 rcx + dynamic_r * -sin_rot, rcy + dynamic_r * cos_rot * TILT, modulated_col);

    for (int i = 1; i <= 24; i++) {
        float trail_angle = sweep_rad - (i * 0.05f);
        float ratio = (24.0f - i) / 24.0f;
        int tr = 10 + (0   - 10)  * ratio;
        int tg = 20 + (238 - 20)  * ratio;
        int tb = 48 + (255 - 48)  * ratio;
        uint16_t fade_col = night_mode ? lgfx::color565(tb, 0, 0) : lgfx::color565(tr, tg, tb);
        for (int r_step = inner_r + 2; r_step < radar_r; r_step += 4) {
            spr.drawPixel(rcx + (int)(r_step * cosf(trail_angle)),
                          rcy + (int)(r_step * sinf(trail_angle) * TILT), fade_col);
        }
    }
    spr.drawLine(rcx, rcy,
                 rcx + (int)((radar_r - 1) * cosf(sweep_rad)),
                 rcy + (int)((radar_r - 1) * sinf(sweep_rad) * TILT), HEADER_COLOR);

    float angle_step = (float)M_PI * 2.0f / NUM_RADIAL_BANDS;

    const float BLIP_RELIGHT_ARC = 0.20f;
    for (int i = 0; i < NUM_RADIAL_BANDS; i++) {
        if (local_spikes[i] < 0.05f) continue;
        float band_angle = fmodf(i * angle_step, TWO_PIf);
        float diff = sweep_norm - band_angle;
        if (diff >  (float)M_PI) diff -= TWO_PIf;
        if (diff < -(float)M_PI) diff += TWO_PIf;
        if (diff >= 0.0f && diff < BLIP_RELIGHT_ARC) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            if (radial_spikes[i] > 0.05f) {
                radial_spikes[i] = min(radial_spikes[i] * 1.4f, 1.5f);
            }
            xSemaphoreGive(dataMutex);
        }
    }

    for (int i = 0; i < NUM_RADIAL_BANDS; i++) {
        float a = i * angle_step;
        if (local_spikes[i] > 0) {
            bool is_strong = (local_spikes[i] > 0.8f);

            int spike_len = (int)((radar_r - inner_r) * local_spikes[i] * 0.7f);
            if (spike_len < 2)  spike_len = 2;
            if (spike_len > 22) spike_len = 22; // cap — prevents jarring tall spike on first detection

            float dist_ratio = constrain(map(radial_rssi[i], -100, -30, 100, 0), 0, 100) / 100.0f;
            int blip_dist = inner_r + 4 + (int)((radar_r - inner_r - 6) * dist_ratio);

            int base_x = rcx + (int)(blip_dist * cosf(a));
            int base_y = rcy + (int)(blip_dist * sinf(a) * TILT);
            uint16_t line_col = local_colors[i];

            if (is_strong) {
                // Stop the stem line at the shape's bottom edge so it doesn't
                // cut through the icon. Triangle bottom = tip_y+7, diamond
                // bottom = tip_y+8 — use 8 to cover both.
                int shape_bottom = base_y - spike_len + 8;
                if (shape_bottom < base_y) {
                    spr.drawLine(base_x, base_y, base_x, shape_bottom, line_col);
                }
                // Shape encodes protocol — filled, larger for visibility
                // against the busy sweep/particle background.
                // BLE = diamond (◆), WiFi = triangle (▲)
                bool is_ble_blip = (line_col == PURPLE_COLOR);
                int tip_y = base_y - spike_len;
                if (is_ble_blip) {
                    // Outline-only 9px symmetric diamond
                    int cx_d = base_x, cy_d = tip_y + 4, bhr = 4;
                    spr.drawLine(cx_d,       cy_d - bhr, cx_d + bhr, cy_d,       line_col);
                    spr.drawLine(cx_d + bhr, cy_d,       cx_d,       cy_d + bhr, line_col);
                    spr.drawLine(cx_d,       cy_d + bhr, cx_d - bhr, cy_d,       line_col);
                    spr.drawLine(cx_d - bhr, cy_d,       cx_d,       cy_d - bhr, line_col);
                } else {
                    // Outline-only 9px upward triangle
                    spr.drawTriangle(base_x - 4, tip_y + 7,
                                     base_x + 4, tip_y + 7,
                                     base_x,     tip_y, line_col);
                }
            } else {
                // Small noise-style dash for weak signals
                spr.drawLine(base_x - 1, base_y, base_x + 1, base_y, line_col);
            }
        }
    }
    hud_rotation = fmodf(hud_rotation + 0.0033f, 2.0f * (float)M_PI);

    spr.clearClipRect();

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long sw = session_flock_wifi; long sb = session_flock_ble;
    xSemaphoreGive(dataMutex);

    int right_text_x = divider_x + 5;

    bool ble_active = pBLEScan ? pBLEScan->isScanning() : false;
    bool wifi_active = !ble_active;

    uint16_t inactive_col = CARD_BORDER;

    // Smooth easing for indicator color transitions
    static float wf_ease = 0.0f;
    static float ble_ease = 0.0f;
    wf_ease  += ((wifi_active ? 1.0f : 0.0f) - wf_ease)  * 0.2f;
    ble_ease += ((ble_active  ? 1.0f : 0.0f) - ble_ease) * 0.2f;

    uint16_t wf_col  = lerp_col16(inactive_col, CAUTION_COLOR, wf_ease);
    uint16_t ble_col = lerp_col16(inactive_col, PURPLE_COLOR,  ble_ease);

    // ── Combined segmented pill: [WiFi: ch / BLE] ──
    // Each segment fills its full half including the diagonal slash region —
    // the slash is drawn last as a divider line over the seam, not as a fill
    // boundary. This eliminates the unfilled triangular gaps.
    // Pad channel to 2 digits for fixed pill width (prevents layout jumps
    // when cycling between channel 9 and 10)
    char wf_label[14]; snprintf(wf_label, sizeof(wf_label), "WiFi:%2d", current_channel);
    bool wf_locked = (frame_ms < channel_lock_until);
    // Fixed width: "WiFi: 12" = 8 chars + optional " L" (2) padding always applied
    int wf_seg_w = 10 * 6 + 14;  // +4 more horizontal
    int ble_seg_w = 40;            // +4 more horizontal

    bool ble_pulse_on = ble_active && (((frame_ms / 500) % 2) == 0);
    float ble_pulse_t = ble_active ? (ble_pulse_on ? 1.0f : 0.35f) : 0.18f;
    uint16_t ble_badge_col = lerp_col16(CARD_BORDER, ble_col, ble_pulse_t);

    uint16_t wf_fill  = lerp_col16(BG_COLOR, CAUTION_COLOR, wf_ease  * 0.22f);
    uint16_t ble_fill = lerp_col16(BG_COLOR, PURPLE_COLOR,  ble_ease * 0.22f);

    int badge_x = right_text_x + 2;
    int badge_y = CONTENT_Y + UI_PAD_XS;
    int badge_h = 18;
    int total_w = wf_seg_w + ble_seg_w;
    uint16_t outer_border = lerp_col16(wf_col, ble_badge_col, 0.5f);

    // Step 1: clear pill area
    spr.fillRoundRect(badge_x, badge_y, total_w, badge_h, 8, BG_COLOR);

    // Step 2: fill BLE segment (right) FIRST as full pill — gives it the
    // rounded right corners
    spr.fillRoundRect(badge_x, badge_y, total_w, badge_h, 8, ble_fill);

    // Step 3: overdraw WiFi segment (left) — extends slightly past the
    // diagonal seam so no BLE color leaks through the slash region
    {
        int seam_x = badge_x + wf_seg_w;
        // Fill the WiFi-side rounded area
        spr.fillRoundRect(badge_x, badge_y, wf_seg_w, badge_h, 8, wf_fill);
        // Square off the right edge of WiFi segment up to the slash midpoint,
        // covering the seam fully. Use a triangle that extends past the
        // diagonal so no BLE color shows through.
        spr.fillRect(badge_x + wf_seg_w - 7, badge_y, 7, badge_h, wf_fill);
        // Diagonal "wedge" of WiFi color extending into the seam area —
        // matches the slope of the slash divider so the fill respects the
        // diagonal boundary, eliminating triangle gaps.
        for (int dy = 0; dy < badge_h; dy++) {
            // Slash goes from (seam_x + 4, badge_y + 2) to (seam_x - 4, badge_y + badge_h - 3)
            // For each y row, compute the x where the slash crosses
            float t = (float)(dy - 2) / (float)(badge_h - 5);
            t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
            int slash_x = (int)(seam_x + 4 - t * 8.0f);
            // Fill from previous WiFi edge up to slash_x with WiFi color
            int wfill_right = badge_x + wf_seg_w;
            if (slash_x > wfill_right) {
                spr.drawFastHLine(wfill_right, badge_y + dy,
                                  slash_x - wfill_right, wf_fill);
            }
        }
    }

    // Step 4: outer rounded outline
    spr.drawRoundRect(badge_x, badge_y, total_w, badge_h, 8, outer_border);

    // Step 5: 45° slash divider drawn on top of fills
    {
        int sx = badge_x + wf_seg_w;
        int sy_top = badge_y + 2;
        int sy_bot = badge_y + badge_h - 3;
        spr.drawLine(sx + 3, sy_top, sx - 3, sy_bot, outer_border);
        spr.drawLine(sx + 4, sy_top, sx - 2, sy_bot, outer_border);
    }

    // WiFi text — centered in WiFi segment
    spr.setTextColor(wf_col, wf_fill); spr.setTextSize(TS_BODY);
    {
        int wf_base_w = (int)strlen(wf_label) * 7;
        int wf_lock_w = wf_locked ? 14 : 0;
        int wf_text_w = wf_base_w + wf_lock_w;
        int wf_text_x = badge_x + (wf_seg_w - wf_text_w) / 2;
        spr.setCursor(wf_text_x, badge_y + 5);
        spr.print(wf_label);
        if (wf_locked) {
            spr.setTextColor(CAUTION_COLOR, wf_fill);
            spr.print(" L");
        }
    }

    // BLE text — centered in BLE segment
    spr.setTextColor(ble_badge_col, ble_fill);
    {
        int ble_text_w = 3 * 7;
        int ble_text_x = badge_x + wf_seg_w + (ble_seg_w - ble_text_w) / 2;
        spr.setCursor(ble_text_x, badge_y + 5);
        spr.print("BLE");
    }

    // ── Ambient packet counter — kerned green label + white number, with
    // throttled roll-up animation when the displayed value updates ──
    // Sits between the WiFi/BLE pill and the stats scorecards.
    {
        uint32_t pkts = ambient_packet_count;
        char pkt_str[12];

        // Roll-up animation state — throttled so the count doesn't animate
        // every frame as new packets arrive.
        static uint32_t pkt_display_val = 0;
        static unsigned long pkt_anim_start = 0;
        static unsigned long pkt_last_update = 0;
        const unsigned long PKT_UPDATE_INTERVAL = 3000;
        const unsigned long PKT_ROLL_MS = 300;

        if (frame_ms - pkt_last_update >= PKT_UPDATE_INTERVAL || pkt_display_val == 0) {
            if (pkts != pkt_display_val) {
                pkt_display_val = pkts;
                pkt_anim_start = frame_ms;
            }
            pkt_last_update = frame_ms;
        }
        snprintf(pkt_str, sizeof(pkt_str), "%lu", (unsigned long)pkt_display_val);

        int pkt_label_y = badge_y + badge_h + 4;  // 4px below the WiFi/BLE pill

        // Line 1: green label, kerned
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        {
            const char* label = "PACKETS SCANNED";
            int label_x = right_text_x + 2;
            spr.setTextDatum(TL_DATUM);
            spr.setCursor(label_x, pkt_label_y);
            kprint(spr, label);
        }

        // Line 2: white number with optional roll-up animation.
        {
            int num_x = right_text_x + 2;
            int num_y = pkt_label_y + 10;
            int num_h = 8;  // textSize 1 height

            spr.setTextColor(lgfx::color565(255, 255, 255), BG_COLOR);
            spr.setTextSize(TS_MICRO);

            spr.setClipRect(num_x, num_y, DISP_W - num_x, num_h);
            spr.fillRect(num_x, num_y, DISP_W - num_x, num_h, BG_COLOR);
            spr.setTextDatum(TL_DATUM);
            int draw_y = anim_slide_in(num_y, num_h, pkt_anim_start, PKT_ROLL_MS);
            spr.setCursor(num_x, draw_y);
            spr.print(pkt_str);
            spr.clearClipRect();
        }

        spr.setTextDatum(TL_DATUM);
    }

    // ── Stats block — labels above, symbol + number inline below ──
    // Both blocks left-aligned at a fixed column split. Symbols are
    // filled + outlined for legibility against the dark background.
    {
        long sw_local = sw;
        long sb_local = sb;

        const int stats_label_y = 62;       // top of size-1 labels
        const int stats_num_y   = 78;       // top of size-3 numbers
        const int sd_left       = right_text_x + 2;
        const int col1_x        = sd_left + 2;
        const int col2_x        = sd_left + 56;  // BLE column, fixed left-aligned split

        char wifi_num[6];
        snprintf(wifi_num, sizeof(wifi_num), "%ld", sw_local);
        char ble_num[6];
        snprintf(ble_num, sizeof(ble_num), "%ld", sb_local);

        // Roll-in animation: when count changes, new number slides up into place
        static long prev_sw = 0, prev_sb = 0;
        static unsigned long sw_anim_start = 0, sb_anim_start = 0;
        const unsigned long ROLL_DURATION = 300;
        if (sw_local != prev_sw) { sw_anim_start = frame_ms; prev_sw = sw_local; }
        if (sb_local != prev_sb) { sb_anim_start = frame_ms; prev_sb = sb_local; }

        // ── WIFI label ──
        spr.setTextSize(TS_BODY);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setCursor(col1_x, stats_label_y);
        kprint(spr, "WIFI");

        // WIFI triangle symbol
        int wtri_x = col1_x + 2;
        int wtri_y = stats_num_y + 3;
        spr.drawTriangle(wtri_x,      wtri_y + 10,
                         wtri_x + 10, wtri_y + 10,
                         wtri_x + 5,  wtri_y,
                         CAUTION_COLOR);

        // WIFI number with roll animation
        {
            const int num_h = 24;  // textSize(3) height
            spr.setClipRect(col1_x + 20, stats_num_y, 40, num_h);
            spr.fillRect(col1_x + 20, stats_num_y, 40, num_h, BG_COLOR);
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setTextSize(TS_H1);
            int draw_y = anim_slide_in(stats_num_y, num_h, sw_anim_start, ROLL_DURATION);
            spr.setCursor(col1_x + 20, draw_y);
            spr.print(wifi_num);
            spr.clearClipRect();
        }

        // ── BLE label ──
        spr.setTextSize(TS_BODY);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setCursor(col2_x, stats_label_y);
        kprint(spr, "BLE");

        // BLE diamond symbol
        int dia_x = col2_x;
        int dia_y = stats_num_y;
        int dcx = dia_x + 6, dcy = dia_y + 7, dhr = 7;
        spr.drawLine(dcx,       dcy - dhr, dcx + dhr, dcy,       PURPLE_COLOR);
        spr.drawLine(dcx + dhr, dcy,       dcx,       dcy + dhr, PURPLE_COLOR);
        spr.drawLine(dcx,       dcy + dhr, dcx - dhr, dcy,       PURPLE_COLOR);
        spr.drawLine(dcx - dhr, dcy,       dcx,       dcy - dhr, PURPLE_COLOR);

        // BLE number with roll animation
        {
            const int num_h = 24;
            spr.setClipRect(col2_x + 20, stats_num_y, 40, num_h);
            spr.fillRect(col2_x + 20, stats_num_y, 40, num_h, BG_COLOR);
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setTextSize(TS_H1);
            int draw_y = anim_slide_in(stats_num_y, num_h, sb_anim_start, ROLL_DURATION);
            spr.setCursor(col2_x + 20, draw_y);
            spr.print(ble_num);
            spr.clearClipRect();
        }
    }

    // ── Live device feed (right column) ──
    // List is anchored at the BOTTOM of the column. Existing rows stay still.
    // When a new entry arrives, it appears at the top: fades in with slight
    // slide-down from the section header. When the buffer overflows, the
    // bottom (oldest) row fades out as it scrolls off. This matches the
    // mental model of a notification/activity feed (Twitter, Slack, etc).
    {
        const int feed_col_left   = 4;
        const int feed_col_right  = divider_x - 4;
        const int feed_row_h      = 12;
        const int max_visible     = 3;
        const int feed_top_y      = 104;

        // Throttled display snapshot — only refreshes every 2 seconds.
        // Eliminates flicker caused by rapid feed_entries mutations between frames.
        static FeedEntry display_feed[FEED_SIZE];
        static int display_count = 0;
        static int display_head = 0;
        static unsigned long display_last_refresh = 0;
        static bool display_ever_populated = false;
        static char display_prev_top_mac[18] = "";
        static unsigned long display_shift_ms = 0;
        static bool display_first_shown = false;

        unsigned long local_now = frame_ms;
        const unsigned long FEED_DISPLAY_REFRESH_MS = 2000;

        if ((local_now - display_last_refresh) >= FEED_DISPLAY_REFRESH_MS || !display_ever_populated) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            display_count = feed_count;
            display_head = feed_head;
            // feed_entries is a ring keyed by feed_head; entries past
            // display_count still hold valid live data the renderer
            // will reach via its (head - i) modular index. Copy the
            // whole array so the snapshot matches the renderer's view.
            for (int i = 0; i < FEED_SIZE; i++) display_feed[i] = feed_entries[i];
            xSemaphoreGive(dataMutex);
            display_last_refresh = local_now;
            if (display_count > 0) {
                display_ever_populated = true;
                int top_idx = (display_head + FEED_SIZE * 2) % FEED_SIZE;
                if (strcmp(display_feed[top_idx].mac, display_prev_top_mac) != 0) {
                    strncpy(display_prev_top_mac, display_feed[top_idx].mac, 17);
                    display_prev_top_mac[17] = '\0';
                    if (display_first_shown) {
                        display_shift_ms = local_now;
                    } else {
                        display_shift_ms = 0;
                        display_first_shown = true;
                    }
                }
            }
        }

        FeedEntry* local_feed = display_feed;
        int local_count = display_count;
        int local_head = display_head;

        // Feed section label
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(feed_col_left, feed_top_y - 12);
        kprint(spr, "LIVE FEED");

        // Always render feed — overlay covers it during startup warm-up
        spr.setClipRect(feed_col_left, feed_top_y - 1,
                        feed_col_right - feed_col_left,
                        DISP_H - (feed_top_y - 1));

        {
            int rows_to_draw = (local_count < max_visible) ? local_count : max_visible;

            for (int i = 0; i < rows_to_draw; i++) {
                int idx = (local_head - i + FEED_SIZE * 2) % FEED_SIZE;
                FeedEntry& e = local_feed[idx];

                int row_y;
                if (i == 0 && display_shift_ms != 0) {
                    // New entry slides DOWN into target from above (existing
                    // rows shift down to make room) — negative slide distance.
                    row_y = anim_slide_in(feed_top_y, -feed_row_h, display_shift_ms, 300);
                } else {
                    row_y = feed_top_y + i * feed_row_h;
                }


                // Age-based fade-out (separate from shift animation)
                unsigned long age = local_now - e.timestamp;
                float age_fade;
                if      (age < 30000UL) age_fade = 1.0f;
                else if (age < 90000UL) age_fade = 1.0f - (float)(age - 30000UL) / 60000.0f;
                else                    age_fade = 0.0f;
                if (age_fade < 0.10f) continue;

                float total_alpha = age_fade;

                // Type prefix color
                uint16_t proto_col = (e.proto == 0) ? CAUTION_COLOR : PURPLE_COLOR;
                if (e.is_flock) proto_col = lerp_col16(proto_col, ACCENT_COLOR, 0.5f);
                proto_col = lerp_col16(BG_COLOR, proto_col, total_alpha);

                uint16_t name_col = lerp_col16(BG_COLOR, TEXT_COLOR, total_alpha);

                spr.setTextSize(TS_BODY);

                // Prefix is a small colored symbol (filled triangle/diamond)
                // instead of a bracketed letter — more visually distinctive,
                // uses less width, frees name space.
                int sym_x = feed_col_left;
                int sym_y = row_y + 1;
                uint16_t sym_col = proto_col;
                if (e.proto == 0) {
                    // Outline-only triangle (△) — WiFi — matches stats icons
                    spr.drawTriangle(sym_x,     sym_y + 5,
                                     sym_x + 5, sym_y + 5,
                                     sym_x + 2, sym_y,
                                     sym_col);
                } else {
                    // Outline-only symmetric diamond (◇) — BLE — matches stats icons
                    int fcx = sym_x + 3, fcy = sym_y + 3, fhr = 4;
                    spr.drawLine(fcx,       fcy - fhr, fcx + fhr, fcy,       sym_col);
                    spr.drawLine(fcx + fhr, fcy,       fcx,       fcy + fhr, sym_col);
                    spr.drawLine(fcx,       fcy + fhr, fcx - fhr, fcy,       sym_col);
                    spr.drawLine(fcx - fhr, fcy,       fcx,       fcy - fhr, sym_col);
                }
                // Flock indicator: small asterisk after the symbol
                if (e.is_flock) {
                    spr.setTextColor(lerp_col16(BG_COLOR, ACCENT_COLOR, total_alpha), BG_COLOR);
                    spr.setTextSize(TS_BODY);
                    spr.setCursor(sym_x + 7, row_y);
                    spr.print("*");
                }

                // Name — full width available now that strength column is gone
                int name_x = feed_col_left + (e.is_flock ? 14 : 10);
                int name_x_end = feed_col_right - 2;
                int name_max_chars = (name_x_end - name_x) / 7;
                if (name_max_chars > 14) name_max_chars = 14;
                if (name_max_chars > (int)sizeof(e.name) - 1) name_max_chars = sizeof(e.name) - 1;
                if (name_max_chars < 1) name_max_chars = 1;

                char name_disp[20];
                strncpy(name_disp, e.name, name_max_chars);
                name_disp[name_max_chars] = '\0';
                spr.setTextSize(TS_BODY);
                spr.setTextColor(name_col, BG_COLOR);
                spr.setCursor(name_x, row_y);
                spr.print(name_disp);

            }

        }

        spr.clearClipRect();

        // Show "scanning..." only until the first snapshot captures data
        if (!display_ever_populated) {
            spr.fillRect(feed_col_left, feed_top_y,
                         feed_col_right - feed_col_left,
                         max_visible * feed_row_h, BG_COLOR);
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setTextSize(TS_BODY);
            int load_y = feed_top_y + (max_visible * feed_row_h) / 2 - 4;
            char dots[4];
            anim_ellipsis(dots, sizeof(dots));
            char load_str[20];
            snprintf(load_str, sizeof(load_str), "scanning%s", dots);
            spr.setCursor(feed_col_left + 8, load_y);
            spr.print(load_str);
        }
    }
}

void draw_locator_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active=locator_active, has_est=locator_has_estimate;
    char target_mac[18];  safe_copy(target_mac,  locator_target_mac,  sizeof(target_mac));
    char target_name[65]; safe_copy(target_name, locator_target_name, sizeof(target_name));
    int  target_id = locator_target_id;
    float dist=locator_est_distance, brng=locator_bearing;
    bool gps_valid = gps.course.isValid() && gps.speed.isValid() && gps.speed.mph() > 2.0;
    bool has_loc   = gps.location.isValid();
    float gps_course = gps.course.deg();
    int sample_count = locator_sample_count;
    int target_rssi = 0; bool has_rssi = false;
    if (locator_tracker_idx >= 0 && locator_tracker_idx < rssi_tracker_count
        && rssi_tracker[locator_tracker_idx].sample_count > 0) {
        target_rssi = rssi_tracker[locator_tracker_idx].samples[
            rssi_tracker[locator_tracker_idx].sample_count - 1];
        has_rssi = true;
    }
    xSemaphoreGive(dataMutex);

    bool demo = !active;

    spr.fillSprite(BG_COLOR); draw_header_spr(1);

    // ── Diagonal-scrolling infinite grid — smaller cells and narrower panel ──
    const int GRID_RIGHT = 78;
    const int GRID_STEP  = 14;
    unsigned long now_ms = millis();

    // Direction slowly reverses every ~10s for radar feel
    static float  grid_speed = 1.0f;   // +1 or -1, eases between
    static float  grid_spd_target = 1.0f;
    static unsigned long grid_dir_ms = 0;
    if (now_ms - grid_dir_ms > 9500) {
        grid_dir_ms = now_ms;
        if ((now_ms / 9500) % 3 == 0) grid_spd_target = -grid_spd_target;
    }
    grid_speed += (grid_spd_target - grid_speed) * 0.004f; // very slow ease

    // Accumulate offset per-frame so direction reversals ease smoothly (no jump)
    static float  cumulative_grid_offset = 0.0f;
    static unsigned long last_grid_ms = 0;
    if (last_grid_ms == 0) last_grid_ms = now_ms;
    unsigned long grid_dt = now_ms - last_grid_ms;
    if (grid_dt > 100) grid_dt = 100;  // cap on first call or long gaps
    last_grid_ms = now_ms;
    cumulative_grid_offset += grid_speed * (float)grid_dt / (1500.0f / GRID_STEP);

    int grid_o = (int)fmodf(cumulative_grid_offset, (float)GRID_STEP);
    if (grid_o < 0) grid_o += GRID_STEP;
    // Wrap the accumulator after sampling so it cannot grow without
    // bound — at multi-hour timescales the float loses sub-pixel
    // precision and the grid stutters or jumps.
    cumulative_grid_offset = fmodf(cumulative_grid_offset, (float)GRID_STEP);

    for (int gx = grid_o - GRID_STEP; gx <= GRID_RIGHT; gx += GRID_STEP)
        spr.drawLine(gx, 18, gx, DISP_H - 1, CARD_BORDER);
    for (int gy = 18 + grid_o - GRID_STEP; gy < DISP_H; gy += GRID_STEP)
        if (gy >= 18) spr.drawLine(0, gy, GRID_RIGHT, gy, CARD_BORDER);
    // Solid vertical separator on right edge of grid panel
    spr.drawFastVLine(GRID_RIGHT, 18, DISP_H - 18, CARD_BORDER);

    const int cx = 39, cy = 62;

    // ── Arrow heading (GPS bearing when tracking, slow drift otherwise) ──
    static float ease_arrow = 0.0f;
    static unsigned long last_ar_ms = 0;
    unsigned long ar_dt = now_ms - last_ar_ms;
    if (ar_dt > 100) ar_dt = 15;
    last_ar_ms = now_ms;

    if (north_mode && gps_valid) {
        // Compass mode while moving: rotate by the negative of travel heading
        // so the arrow always points to true north regardless of orientation.
        float tgt = radians(-gps_course - 90.0f);
        float d = tgt - ease_arrow;
        while (d >  (float)M_PI) d -= 2.0f*(float)M_PI;
        while (d < -(float)M_PI) d += 2.0f*(float)M_PI;
        ease_arrow += 0.08f * d;
    } else if (north_mode && !gps_valid) {
        // No GPS heading available — arrow defaults to "up" (screen-relative north).
        float tgt = radians(-90.0f);
        float d = tgt - ease_arrow;
        while (d >  (float)M_PI) d -= 2.0f*(float)M_PI;
        while (d < -(float)M_PI) d += 2.0f*(float)M_PI;
        ease_arrow += 0.08f * d;
    } else if (active && has_est) {
        float rel = brng - (gps_valid ? gps_course : 0.0f);
        float tgt = radians(rel - 90.0f);
        float d = tgt - ease_arrow;
        while (d >  (float)M_PI) d -= 2.0f*(float)M_PI;
        while (d < -(float)M_PI) d += 2.0f*(float)M_PI;
        // Slower lock-on (was 0.09) — arrow visibly swings and settles
        // toward the target rather than snapping into place.
        ease_arrow += 0.06f * d;
    } else {
        // Clockwise drift: 1 rev per 8s — gives a more visible "searching" feel.
        ease_arrow += (float)ar_dt * (2.0f*(float)M_PI / 8000.0f);
        if (ease_arrow > 2.0f*(float)M_PI) ease_arrow -= 2.0f*(float)M_PI;
    }
    float ang = ease_arrow;

    // ── Direction pointer: thin line from pivot, filled dot at tip ──
    // Compass needle aesthetic — direction is the line, target lives at
    // the dot. Pivot circle marks the user's position. Hatching/grid
    // background gives the panel a tactical feel without the polygon.
    {
        const int line_len = 28;
        int tip_x = cx + (int)(line_len * cosf(ang));
        int tip_y = cy + (int)(line_len * sinf(ang));

        // Direction line and tip dot — full HEADER_COLOR for emphasis.
        spr.drawLine(cx, cy, tip_x, tip_y, HEADER_COLOR);
        spr.fillCircle(tip_x, tip_y, 3, HEADER_COLOR);

        // Open pivot ring — sits over the line at the rotation center.
        spr.drawCircle(cx, cy, 2, DIM_COLOR);
    }

    // (Sample-acquisition boxes removed — the status badge already
    // communicates LOCKED vs SEARCHING and the box count was an
    // implementation detail.)
    (void)sample_count;

    // ── Right panel ──
    int rx = 82;
    int rpx = rx + 4;

    // ── Status badge ──
    const char* status_base; uint16_t status_col; bool status_anim = false;
    if (north_mode) {
        status_base = "N Mode";  status_col = GPS_COLOR;
    } else if (!has_loc && !gps_valid) {
        status_base = "GPS";         status_col = GPS_COLOR;     status_anim = true;
    } else if (!active) {
        status_base = "Need Target"; status_col = CAUTION_COLOR; status_anim = true;
    } else {
        status_base = "Hunting";     status_col = ACCENT_COLOR;
    }
    char status_str[26];
    if (status_anim) {
        char dots[4];
        anim_ellipsis(dots, sizeof(dots));
        snprintf(status_str, sizeof(status_str), "%s%s", status_base, dots);
    } else {
        safe_copy(status_str, status_base, sizeof(status_str));
    }
    {
        int max_chars_sb = (int)strlen(status_base) + (status_anim ? 3 : 0);
        int box_w = max_chars_sb * 8 + 12;
        int avail_w = DISP_W - rpx - 2;
        if (box_w > avail_w) box_w = avail_w;
        uint16_t status_fill = lerp_col16(BG_COLOR, status_col, 0.22f);
        spr.fillRoundRect(rpx, 23, box_w, 16, 5, status_fill);
        spr.drawRoundRect(rpx, 23, box_w, 16, 5, status_col);
        spr.setTextColor(status_col, status_fill); spr.setTextSize(TS_BODY);
        spr.setClipRect(rpx + 1, 24, box_w - 2, 14);
        spr.setCursor(rpx + 6, 27); kprint(spr, status_str);
        spr.clearClipRect();
    }

    // ── TARGET label + name + ID ──
    // Section spacing follows the standard tiers: UI_PAD_MD between the
    // status pill and the TARGET section, UI_PAD_SM between rows.
    int target_label_y = 23 + 16 + UI_PAD_MD;          // pill bottom + UI_PAD_MD
    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(TS_MICRO);
    spr.setCursor(rpx, target_label_y);
    kprint(spr, "TARGET");

    int target_name_y = target_label_y + 10;
    {
        char tname[18];
        if (north_mode) {
            safe_copy(tname, "North", sizeof(tname));
        } else if (active) {
            bool nok = (target_name[0] != '\0'
                        && strcmp(target_name, "Hidden")  != 0
                        && strcmp(target_name, "Unknown") != 0);
            const char* src = nok ? target_name
                                   : ((strlen(target_mac) > 8) ? target_mac + 9 : target_mac);
            safe_copy(tname, src, sizeof(tname));
        } else {
            safe_copy(tname, "No Target", sizeof(tname));
        }
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(TS_STRONG);
        spr.setCursor(rpx, target_name_y); spr.print(tname);

        // Detection ID after the name (DIM, TS_MICRO).
        if (active && target_id > 0) {
            int name_w = (int)strlen(tname) * (int)(TS_STRONG * 6.0f);
            char id_buf[8];
            snprintf(id_buf, sizeof(id_buf), "#%03d", target_id);
            spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(TS_MICRO);
            // Vertical: align baseline to the bottom of the TS_STRONG name
            int id_y = target_name_y + (int)(TS_STRONG * 8.0f) - 8;
            spr.setCursor(rpx + name_w + UI_PAD_SM, id_y);
            spr.print(id_buf);
        }
    }

    // ── DIST + SIG (labels TS_MICRO, values TS_STRONG side-by-side) ──
    // TS_STRONG fits the worst-case strings ("9999ft" / "STRONG") in the
    // ~154 px right panel without overlap; TS_H2 would overflow.
    int dist_label_y = target_name_y + (int)(TS_STRONG * 8.0f) + UI_PAD_SM;
    int dist_value_y = dist_label_y + 10;
    int sig_x        = rpx + 64;

    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(TS_MICRO);
    spr.setCursor(rpx,   dist_label_y); kprint(spr, "DIST");
    spr.setCursor(sig_x, dist_label_y); kprint(spr, "SIG");

    // Trend indicator just past the DIST label
    {
        int trend = (active && has_est) ? locator_distance_trend() : 0;
        if (trend != 0) {
            int tx = rpx + 28, ty = dist_label_y;
            if (trend < 0) {
                spr.fillTriangle(tx, ty, tx + 5, ty, tx + 2, ty + 5, ACCENT_COLOR);
            } else {
                spr.fillTriangle(tx, ty + 5, tx + 5, ty + 5, tx + 2, ty, CAUTION_COLOR);
            }
        }
    }

    // Distance value
    {
        float sd = (active && has_est && !north_mode) ? dist : -1.0f;
        float sd_ft = sd * 3.28084f;
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(TS_STRONG);
        spr.setCursor(rpx, dist_value_y);
        if (sd < 0) {
            spr.print("--");
        } else if (sd < 300) {
            char db[12]; snprintf(db, sizeof(db), "%.0fft", sd_ft);
            spr.print(db);
        } else {
            char db[12]; snprintf(db, sizeof(db), "%.1fkm", sd / 1000.0f);
            spr.print(db);
        }
    }

    // Signal value (colored by strength)
    {
        int sr = (active && has_rssi && !north_mode) ? target_rssi : -999;
        uint16_t sig_col;
        const char* sig_str;
        if      (sr == -999) { sig_str = "--";     sig_col = DIM_COLOR; }
        else if (sr > -60)   { sig_str = "STRONG"; sig_col = ACCENT_COLOR; }
        else if (sr > -80)   { sig_str = "MED";    sig_col = CAUTION_COLOR; }
        else                 { sig_str = "WEAK";   sig_col = DIM_COLOR; }

        spr.setTextColor(sig_col, BG_COLOR); spr.setTextSize(TS_STRONG);
        spr.setCursor(sig_x, dist_value_y); spr.print(sig_str);
    }
}

static int history_scroll_offset = 0;

// Helper: extract type-label and protocol color from type string
static void hist_type_info(const char* type, const char** lbl, uint16_t* col) {
    if (strstr(type, "WIFI")) { *lbl = "WiFi"; *col = TEAL_COLOR; }
    else if (strstr(type, "BLE") || strstr(type, "RAVEN")) { *lbl = "BLE";  *col = GPS_COLOR; }
    else { *lbl = "SYS"; *col = DIM_COLOR; }
}

void draw_capture_history_screen() {
    // Use SD history when available, fall back to in-memory capture_history
    bool use_sd = sd_available && sd_hist_count > 0;

    // Build a unified view pointer array (avoid copying large structs)
    // We work directly from sd_hist or capture_history
    int total = use_sd ? sd_hist_count : 0;
    if (!use_sd) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        total = capture_history_count;
        xSemaphoreGive(dataMutex);
    }

    // Clamp selection and scroll
    if (total == 0) { history_selected_idx = 0; history_scroll_offset = 0; }
    else {
        if (history_selected_idx >= total)  history_selected_idx = total - 1;
        if (history_selected_idx < 0)       history_selected_idx = 0;
        if (history_selected_idx < history_scroll_offset)
            history_scroll_offset = history_selected_idx;
        if (history_selected_idx >= history_scroll_offset + HIST_VISIBLE_ROWS)
            history_scroll_offset = history_selected_idx - HIST_VISIBLE_ROWS + 1;
        int max_scroll = max(0, total - HIST_VISIBLE_ROWS);
        if (history_scroll_offset > max_scroll) history_scroll_offset = max_scroll;
    }

    // Selection y target — eased toward via anim_filter so quick key
    // presses produce a visible glide rather than a teleport.
    unsigned long now_ms = millis();
    float dt = (hist_last_frame_ms == 0) ? 16.0f
                                         : (float)(now_ms - hist_last_frame_ms);
    if (dt > 100.0f) dt = 100.0f;
    hist_last_frame_ms = now_ms;
    int target_sel_y = CONTENT_Y +
        (history_selected_idx - history_scroll_offset) * HIST_ROW_H;
    hist_sel_y_f = anim_filter(hist_sel_y_f, (float)target_sel_y,
                                HIST_SEL_TC, dt);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(2);

    if (total == 0) {
        for (int i = 0; i < HIST_VISIBLE_ROWS; i++) {
            int y = CONTENT_Y + i * HIST_ROW_H;
            spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(TS_MICRO);
            spr.setCursor(10, y + 11); spr.print(use_sd ? "-- No SD log --" : "-- Listening...");
        }
        return;
    }

    // Snapshot in-memory history if needed
    CaptureEntry mem_hist[CAPTURE_HISTORY_SIZE];
    if (!use_sd) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        for (int i = 0; i < total; i++) mem_hist[i] = capture_history[i];
        xSemaphoreGive(dataMutex);
    }

    int rows_shown = 0;
    for (int i = history_scroll_offset;
         i < total && rows_shown < HIST_VISIBLE_ROWS;
         i++, rows_shown++) {
        int y = CONTENT_Y + rows_shown * HIST_ROW_H;

        // Pull fields from whichever source
        const char* e_type;
        char e_mac[18], e_name[32], e_method[24];
        int e_rssi, e_conf, e_id;

        if (use_sd) {
            e_type = sd_hist[i].type;
            strncpy(e_mac,    sd_hist[i].mac,    17); e_mac[17]    = '\0';
            strncpy(e_name,   sd_hist[i].name,   31); e_name[31]   = '\0';
            strncpy(e_method, sd_hist[i].method, 23); e_method[23] = '\0';
            e_rssi = sd_hist[i].rssi;
            e_conf = sd_hist[i].confidence;
            e_id   = sd_hist[i].id;
        } else {
            e_type = mem_hist[i].type;
            strncpy(e_mac,  mem_hist[i].mac,  17); e_mac[17]  = '\0';
            strncpy(e_name, mem_hist[i].name, 31); e_name[31] = '\0';
            e_method[0] = '\0';
            e_rssi = mem_hist[i].rssi;
            e_conf = mem_hist[i].confidence;
            e_id   = mem_hist[i].id;
        }

        const char* proto_lbl; uint16_t proto_col_unused;
        hist_type_info(e_type, &proto_lbl, &proto_col_unused);
        (void)proto_col_unused;

        // Identical layout for every row regardless of selection. Selection
        // is communicated by the eased outline drawn after the loop.
        const int content_x = 6;

        // Sequential detection ID (#001 …) — leftmost column. Entries
        // loaded from the SD CSV (no ID column on disk) show "----".
        char id_str[8];
        if (e_id > 0) snprintf(id_str, sizeof(id_str), "#%03d", e_id);
        else          snprintf(id_str, sizeof(id_str), "----");
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(content_x, y + 11);
        spr.print(id_str);

        const int proto_x = content_x + 30;  // after the 4-char ID + gap

        // Protocol label (dim, TS_BODY)
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(TS_BODY);
        spr.setCursor(proto_x, y + 8); spr.print(proto_lbl);

        // Device name (TEXT, TS_MICRO) — after the label
        bool name_ok = (e_name[0] != '\0' &&
                        strcmp(e_name, "Hidden")  != 0 &&
                        strcmp(e_name, "Unknown") != 0);
        char nom[20] = "";
        if (name_ok) { strncpy(nom, e_name, 19); nom[19] = '\0'; }
        else { const char* sm = (strlen(e_mac) > 8) ? e_mac + 9 : e_mac; strncpy(nom, sm, 19); nom[19] = '\0'; }
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(TS_MICRO);
        spr.setCursor(proto_x + 36, y + 11); spr.print(nom);

        // Signal + confidence, right-aligned (DIM, TS_MICRO)
        char right_str[16];
        snprintf(right_str, sizeof(right_str), "%ddBm %d%%", e_rssi, e_conf);
        int right_w = (int)strlen(right_str) * 6;  // TS_MICRO char width
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(DISP_W - right_w - 6, y + 11); spr.print(right_str);
    }

    // Selection highlight — drawn after rows so it sits on top, at the
    // eased y so quick key presses produce a glide.
    {
        int sel_draw_y = (int)(hist_sel_y_f + 0.5f);
        spr.drawRect(0, sel_draw_y, DISP_W, HIST_ROW_H, HEADER_COLOR);
    }

    {
        const int list_y = CONTENT_Y;
        const int list_h = HIST_VISIBLE_ROWS * HIST_ROW_H;
        if (total > HIST_VISIBLE_ROWS) {
            if (history_scroll_offset > 0)
                draw_scroll_fade(list_y, list_h, 5, true);
            if (history_scroll_offset < total - HIST_VISIBLE_ROWS)
                draw_scroll_fade(list_y, list_h, 5, false);
        }
    }

    // Scroll indicator
    if (total > HIST_VISIBLE_ROWS) {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(TS_MICRO);
        spr.setCursor(DISP_W - 56, DISP_H - 8);
        char scr_buf[16]; snprintf(scr_buf, sizeof(scr_buf), "%d/%d -/+", history_selected_idx + 1, total);
        spr.print(scr_buf);
    }

    // Detail overlay — slide-up + fade in on open, reverse on close.
    // hist_detail_open stays true through the close anim; the renderer
    // clears it once the fade reaches zero.
    if (hist_detail_open && total > 0) {
        int di = history_selected_idx;
        if (di < 0) di = 0; if (di >= total) di = total - 1;

        char d_mac[18], d_name[32], d_method[24];
        int d_rssi, d_conf, d_id;
        if (use_sd) {
            strncpy(d_mac,    sd_hist[di].mac,    17); d_mac[17]    = '\0';
            strncpy(d_name,   sd_hist[di].name,   31); d_name[31]   = '\0';
            strncpy(d_method, sd_hist[di].method, 23); d_method[23] = '\0';
            d_rssi = sd_hist[di].rssi; d_conf = sd_hist[di].confidence;
            d_id   = sd_hist[di].id;
        } else {
            strncpy(d_mac,  mem_hist[di].mac,  17); d_mac[17]  = '\0';
            strncpy(d_name, mem_hist[di].name, 31); d_name[31] = '\0';
            d_method[0] = '\0';
            d_rssi = mem_hist[di].rssi; d_conf = mem_hist[di].confidence;
            d_id   = mem_hist[di].id;
        }

        // Open animation only — close is instant. Animated close left a
        // backdrop-only frame on screen if no further redraw fired before
        // the user pressed another key, which read as a black screen.
        float detail_alpha = ui_progress(hist_detail_open_ms, UI_FADE_IN_MS);

        // Slide offset — 20px from below for the standard UI feel
        int detail_slide = (int)((1.0f - detail_alpha) * 20.0f);
        auto da = [&](uint16_t c) -> uint16_t {
            return lerp_col16(BG_COLOR, c, detail_alpha);
        };

        // Solid backdrop below header — wipes the row list.
        spr.fillRect(0, 18, DISP_W, DISP_H - 18, BG_COLOR);

        // Override header with literal "DETAILS" — name moves into the body.
        spr.fillRect(0, 0, 120, 18, BG_COLOR);
        spr.setTextColor(da(HEADER_COLOR), BG_COLOR);
        spr.setTextSize(TS_BODY);
        spr.setCursor(4, 5);
        kprint(spr, "DETAILS");

        // First body row: device name (large, bright). Falls back to MAC
        // tail when we don't have a meaningful name.
        bool dn_ok = (d_name[0] != '\0'
                      && strcmp(d_name, "Hidden")  != 0
                      && strcmp(d_name, "Unknown") != 0);
        const char* big_name;
        char mac_tail[12] = "";
        if (dn_ok) {
            big_name = d_name;
        } else {
            const char* sm = (strlen(d_mac) > 8) ? d_mac + 9 : d_mac;
            strncpy(mac_tail, sm, 11); mac_tail[11] = '\0';
            big_name = mac_tail;
        }

        const int label_x = 8;
        const int value_x = 70;
        const int row_gap = 14;
        int fy = 26 + detail_slide;

        spr.setTextColor(da(TEXT_COLOR), BG_COLOR);
        spr.setTextSize(TS_STRONG);
        spr.setCursor(label_x, fy);
        spr.print(big_name);
        fy += (int)(TS_STRONG * 8.0f) + 6;  // extra space after the name

        auto detail_row = [&](const char* lbl, const char* val, uint16_t val_col = TEXT_COLOR) {
            spr.setTextColor(da(ACCENT_COLOR), BG_COLOR);
            spr.setTextSize(TS_BODY);
            spr.setCursor(label_x, fy);
            kprint(spr, lbl);
            spr.setTextColor(da(val_col), BG_COLOR);
            spr.setCursor(value_x, fy);
            spr.print(val);
            fy += row_gap;
        };

        if (d_id > 0) {
            char id_label[8];
            snprintf(id_label, sizeof(id_label), "#%03d", d_id);
            detail_row("ID", id_label);
        }

        detail_row("MAC", d_mac);

        char rssi_str[12];
        snprintf(rssi_str, sizeof(rssi_str), "%d dBm", d_rssi);
        uint16_t rssi_col = (d_rssi > -60) ? ACCENT_COLOR
                          : (d_rssi > -80) ? CAUTION_COLOR
                                           : DIM_COLOR;
        detail_row("RSSI", rssi_str, rssi_col);

        {
            const char* band;
            uint16_t conf_col;
            if      (d_conf >= CONFIDENCE_CERTAIN)         { band = "CERTAIN"; conf_col = ACCENT_COLOR;  }
            else if (d_conf >= CONFIDENCE_HIGH)            { band = "HIGH";    conf_col = ACCENT_COLOR;  }
            else if (d_conf >= CONFIDENCE_ALARM_THRESHOLD) { band = "MEDIUM";  conf_col = CAUTION_COLOR; }
            else                                           { band = "LOW";     conf_col = DIM_COLOR;     }
            char conf_str[24];
            snprintf(conf_str, sizeof(conf_str), "%d%% %s", d_conf, band);
            detail_row("CONF", conf_str, conf_col);
        }

        if (d_method[0]) {
            char human[48];
            methods_to_human(d_method, human, sizeof(human));
            spr.setTextColor(da(ACCENT_COLOR), BG_COLOR);
            spr.setTextSize(TS_BODY);
            spr.setCursor(label_x, fy);
            kprint(spr, "MATCH");
            spr.setTextColor(da(TEXT_COLOR), BG_COLOR);
            spr.setTextSize(TS_MICRO);
            spr.setCursor(value_x, fy);
            spr.print(human);
            fy += row_gap;
        }

        const char* d_ts = use_sd ? sd_hist[di].timestamp : "";
        if (d_ts[0]) {
            detail_row("TIME", d_ts);
        }

        // Footer hint
        spr.setTextColor(da(DIM_COLOR), BG_COLOR);
        spr.setTextSize(TS_MICRO);
        spr.setCursor(8, DISP_H - 10);
        spr.print("DEL or ENTER to close");
    }
}

void draw_gps_screen() {
    spr.fillSprite(BG_COLOR);
    draw_header_spr(3);
    unsigned long frame_ms = millis();

    bool has_loc, stale, speed_valid;
    int sats;
    double d_lat, d_lng;
    float f_spd;
    float f_hdop;
    bool hdop_valid, time_valid;
    int gps_hour, gps_min, gps_sec;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    has_loc     = gps.location.isValid();
    stale       = has_loc && (gps.location.age() > 2000);
    sats        = gps.satellites.isValid() ? gps.satellites.value() : 0;
    d_lat       = gps.location.lat();
    d_lng       = gps.location.lng();
    speed_valid = gps.speed.isValid();
    f_spd       = gps.speed.mph();
    hdop_valid  = gps.hdop.isValid();
    f_hdop      = hdop_valid ? gps.hdop.hdop() : 0.0f;
    time_valid  = gps.time.isValid();
    gps_hour    = time_valid ? gps.time.hour()   : 0;
    gps_min     = time_valid ? gps.time.minute() : 0;
    gps_sec     = time_valid ? gps.time.second() : 0;
    xSemaphoreGive(dataMutex);

    // ── Off-axis 3D wireframe globe ──────────────────────────────────────────
    // Solid BG fill, diagonal axis tilt like a real globe on a stand
    const int gx = 55, gy = 72, gr = 30;  // gy=72 leaves room below for orbit+SATS

    // TILT: X-axis — north pole backward; ROLL: Z-axis — axis diagonal upper-left→lower-right
    const float TILT  = -0.30f;
    const float ROLL  =  0.45f;
    float rot = fmodf((float)frame_ms / 8000.0f, 1.0f) * 2.0f * (float)M_PI;

    float sr = sinf(rot),  cr = cosf(rot);
    float st = sinf(TILT), ct = cosf(TILT);
    float sroll = sinf(ROLL), croll = cosf(ROLL);

    // Project a sphere point → screen (px, py); return z-depth [-1..1]
    auto proj = [&](float clat, float slat, float lon_r, int* px, int* py) -> float {
        float sx = clat * cosf(lon_r);
        float sy = slat;
        float sz = clat * sinf(lon_r);
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
        #define NUM_STARS 30
        static int  star_x[NUM_STARS], star_y[NUM_STARS];
        static bool stars_init = false;
        if (!stars_init) {
            const int min_x = UI_PAD_SM / 2;
            const int max_x = gx * 2 - UI_PAD_MD;  // stop well before right panel
            const int min_y = CONTENT_Y + UI_PAD_SM;
            const int max_y = DISP_H - UI_PAD_SM;
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
            float twinkle = anim_pulse(3000 + i * 200, (float)i / (float)NUM_STARS);
            // 100..255 brightness — earlier 60..180 still read faint with
            // the backlight on. Baseline 100 keeps stars clearly visible
            // while the +155 swing reads as a shimmer.
            uint8_t b = (uint8_t)(100 + twinkle * 155);
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
        float pz0 = proj(clat, slat, 0.0f, &px0, &py0);
        for (int s = 1; s <= STEPS; s++) {
            float lon = (float)s / STEPS * 2.0f * (float)M_PI;
            float pz1 = proj(clat, slat, lon, &px1, &py1);
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
    for (int m = 0; m < N_MER; m++) {
        float lon = (float)m / N_MER * 2.0f * (float)M_PI;
        int px0, py0, px1, py1;
        float clat = cosf(-1.5707f), slat = sinf(-1.5707f);
        float pz0 = proj(clat, slat, lon, &px0, &py0);
        for (int s = 1; s <= M_STEPS; s++) {
            float lat_r = -1.5707f + (float)s / M_STEPS * (float)M_PI;
            clat = cosf(lat_r); slat = sinf(lat_r);
            float pz1 = proj(clat, slat, lon, &px1, &py1);
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
    int ry = 25;

    // STATUS badge
    {
        const char* status_base;
        uint16_t    status_col;
        bool        status_anim = false;
        if      (has_loc && !stale) { status_base = "GPS LOCKED";    status_col = GPS_COLOR; }
        else if (stale)             { status_base = "REATTEMPTING";  status_col = CAUTION_COLOR; status_anim = true; }
        else                        { status_base = "GPS";           status_col = GPS_COLOR; status_anim = true; }
        char status_str[22];
        if (status_anim) {
            char dots[4];
            anim_ellipsis(dots, sizeof(dots));
            snprintf(status_str, sizeof(status_str), "%s%s", status_base, dots);
        } else {
            strncpy(status_str, status_base, sizeof(status_str) - 1);
            status_str[sizeof(status_str) - 1] = '\0';
        }
        int bw = (int)strlen(status_base) * 7 + (status_anim ? 3 * 7 : 0) + 12;
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
        int val_w = (int)strlen(value) * 7;
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

    // SAT — X/12 format
    {
        char sat_buf[8];
        snprintf(sat_buf, sizeof(sat_buf), "%d/12", sats);
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

    // TIME — UTC from GPS
    {
        char time_buf[12];
        if (time_valid) {
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                     gps_hour, gps_min, gps_sec);
        } else {
            strncpy(time_buf, "--:--:--", sizeof(time_buf));
        }
        kv_row("TIME", time_buf, DIM_COLOR);
    }
}

// Draw a single stat card. Outlined only (no fill).
// Label: TS_MICRO, DIM_COLOR, kprint with extra=2 letter-spacing — hugs top.
// Value: TS_STRONG by default (TS_H2 for hero), TEXT_COLOR — hugs bottom.
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
    int char_w         = (int)(value_size * 6.0f);
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
        strncpy(prev_str, value, STAT_MAX_CHARS - 1);
        prev_str[STAT_MAX_CHARS - 1] = '\0';
    }

    int clip_x = x + 1;
    int clip_y = (y + 1 > CONTENT_Y) ? (y + 1) : CONTENT_Y;
    int clip_right  = x + w - 1;
    int clip_bottom = (y + h - 1 < CONTENT_Y + STATS_VIEW_H)
                          ? (y + h - 1) : (CONTENT_Y + STATS_VIEW_H);
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
    long lt, sr, sw, sb, lb, lfw;
    unsigned long l_sec;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    lt    = lifetime_flock_total;
    sr    = session_raven;
    sw    = session_flock_wifi;
    sb    = session_flock_ble;
    lb    = lifetime_boots;
    lfw   = lifetime_flash_writes;
    l_sec = lifetime_seconds;
    xSemaphoreGive(dataMutex);

    int32_t  bat_mv      = get_filtered_voltage();
    size_t   free_heap   = esp_get_free_heap_size();
    uint64_t sd_bytes    = sd_available ? SD.cardSize() : 0;
    unsigned long sess_s = (frame_ms - session_start_time) / 1000;

    // PACKETS throttle — ambient_packet_count churns every frame, which would
    // strobe the roll-up animation. Only refresh the displayed value once per
    // 3 s. The first sample populates immediately so the card isn't blank.
    if (stats_pkt_last_update == 0 || frame_ms - stats_pkt_last_update >= 3000) {
        stats_pkt_display     = ambient_packet_count;
        stats_pkt_last_update = frame_ms;
    }

    spr.fillSprite(BG_COLOR);
    draw_header_spr(4);

    // ── Format value strings ──
    char det_str[10];   snprintf(det_str,   sizeof(det_str),   "%ld", lt);
    char wifi_str[10];  snprintf(wifi_str,  sizeof(wifi_str),  "%ld", sw);
    char ble_str[10];   snprintf(ble_str,   sizeof(ble_str),   "%ld", sb);
    char raven_str[10]; snprintf(raven_str, sizeof(raven_str), "%ld", sr);
    char sess_str[10];  format_time_buf(sess_s, sess_str, sizeof(sess_str));
    char life_str[10];  format_time_buf(l_sec, life_str, sizeof(life_str));
    char volt_str[10];  snprintf(volt_str,  sizeof(volt_str),  "%.2fV", bat_mv / 1000.0f);
    char heap_str[10];  snprintf(heap_str,  sizeof(heap_str),  "%uKB", (unsigned)(free_heap / 1024));
    char pkt_str[12];   snprintf(pkt_str,   sizeof(pkt_str),   "%lu",  (unsigned long)stats_pkt_display);
    char sd_str[10];
    if (sd_available && sd_bytes > 0) {
        snprintf(sd_str, sizeof(sd_str), "%.1fGB", (double)sd_bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        strncpy(sd_str, "--", sizeof(sd_str));
    }
    char boots_str[10]; snprintf(boots_str, sizeof(boots_str), "%ld", lb);
    char flash_str[10]; snprintf(flash_str, sizeof(flash_str), "%ld", lfw);

    // First-frame seed for per-character roll animation. Copy the freshly
    // formatted strings into stats_prev_strings without stamping any
    // char_anim slots, so the initial render doesn't fire 13 simultaneous
    // rolls. Subsequent frames let draw_stat_card do the per-glyph
    // comparison and timestamp bumping.
    if (!stats_values_initialized) {
        const char* seed[STATS_CARD_COUNT];
        seed[SC_DETECTIONS] = det_str;
        seed[SC_WIFI]       = wifi_str;
        seed[SC_BLE]        = ble_str;
        seed[SC_RAVEN]      = raven_str;
        seed[SC_SESSION]    = sess_str;
        seed[SC_LIFETIME]   = life_str;
        seed[SC_BATTERY]    = volt_str;
        seed[SC_HEAP]       = heap_str;
        seed[SC_PACKETS]    = pkt_str;
        seed[SC_SD]         = sd_str;
        seed[SC_BOOTS]      = boots_str;
        seed[SC_FLASH]      = flash_str;
        seed[SC_VERSION]    = "v9.6";
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
    const int x_t1     = 4;        // triple-card x positions
    const int x_t2     = 80;       // x_t1 + 70 + 6 gap
    const int x_t3     = 156;      // x_t2 + 70 + 6 gap
    const int w_tA     = 70;       // first two triple widths
    const int w_tC     = 72;       // last triple — absorbs the rounding
    const int x_h1     = 4;        // half-card x positions
    const int x_h2     = 119;      // x_h1 + 109 + 6 gap
    const int w_half   = 109;
    const int H_HERO   = 50;       // 6+8+14+16+6 — TS_H2 hero w/ generous gap
    const int H_NORMAL = 36;       // 6+8+4+12+6 — TS_STRONG fits in tighter card

    auto card = [&](int vx, int vy, int w, int h, const char* label, const char* value,
                    int idx, float val_size = TS_STRONG) {
        int sy = CONTENT_Y + vy - scroll_y;
        if (sy + h <= CONTENT_Y) return;             // entirely above viewport
        if (sy >= CONTENT_Y + STATS_VIEW_H) return;  // entirely below viewport
        draw_stat_card(vx, sy, w, h, label, value, val_size,
                       stats_prev_strings[idx], stats_char_anim[idx]);
        // The helper's internal clip rect replaces the outer scroll clip;
        // restore it so the next card's outline still gets viewport-clipped.
        spr.setClipRect(0, CONTENT_Y, DISP_W, STATS_VIEW_H);
    };

    // Clip drawing to the viewport so partially-visible cards don't bleed
    // into the header strip or off the bottom edge.
    spr.setClipRect(0, CONTENT_Y, DISP_W, STATS_VIEW_H);

    // Row 1 (vy = 0):   DETECTIONS hero, full width, 50px tall, TS_H2 value
    card(x_full, 0,   w_full, H_HERO, "DETECTIONS", det_str, SC_DETECTIONS, TS_H2);

    // Row 2 (vy = 56):  WIFI | BLE | RAVEN
    card(x_t1, 56,  w_tA, H_NORMAL, "WIFI",  wifi_str,  SC_WIFI);
    card(x_t2, 56,  w_tA, H_NORMAL, "BLE",   ble_str,   SC_BLE);
    card(x_t3, 56,  w_tC, H_NORMAL, "RAVEN", raven_str, SC_RAVEN);

    // Row 3 (vy = 98):  SESSION | LIFETIME
    card(x_h1, 98,  w_half, H_NORMAL, "SESSION",  sess_str, SC_SESSION);
    card(x_h2, 98,  w_half, H_NORMAL, "LIFETIME", life_str, SC_LIFETIME);

    // Row 4 (vy = 140): BATTERY | HEAP
    card(x_h1, 140, w_half, H_NORMAL, "BATTERY", volt_str, SC_BATTERY);
    card(x_h2, 140, w_half, H_NORMAL, "HEAP",    heap_str, SC_HEAP);

    // Row 5 (vy = 182): PACKETS | SD CARD
    card(x_h1, 182, w_half, H_NORMAL, "PACKETS", pkt_str, SC_PACKETS);
    card(x_h2, 182, w_half, H_NORMAL, "SD CARD", sd_str,  SC_SD);

    // Row 6 (vy = 224): BOOTS | FLASH
    card(x_h1, 224, w_half, H_NORMAL, "BOOTS", boots_str, SC_BOOTS);
    card(x_h2, 224, w_half, H_NORMAL, "FLASH", flash_str, SC_FLASH);

    // Row 7 (vy = 266): VERSION (half width, left only)
    card(x_h1, 266, w_half, H_NORMAL, "VERSION", "v9.6", SC_VERSION);

    spr.clearClipRect();

    // ── Scroll fade hints ──
    if (STATS_MAX_SCROLL > 0) {
        if (scroll_y > 0)                draw_scroll_fade(CONTENT_Y, STATS_VIEW_H, 5, true);
        if (scroll_y < STATS_MAX_SCROLL) draw_scroll_fade(CONTENT_Y, STATS_VIEW_H, 5, false);
    }

    // ── Scrollbar — only when content overflows. Tracks the eased value. ──
    if (STATS_CONTENT_H > STATS_VIEW_H) {
        spr.drawFastVLine(DISP_W - 4, CONTENT_Y, STATS_VIEW_H, CARD_BORDER);
        int thumb_h = (STATS_VIEW_H * STATS_VIEW_H) / STATS_CONTENT_H;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = CONTENT_Y + (scroll_y * (STATS_VIEW_H - thumb_h)) / STATS_MAX_SCROLL;
        spr.fillRect(DISP_W - 5, thumb_y, 3, thumb_h, DIM_COLOR);
    }
}

// ============================================================================
// EXPANDED FEED OVERLAY — fullscreen activity feed view
// ============================================================================
void draw_feed_expanded_overlay() {
    // Snapshot feed under mutex. Static to avoid stack pressure — rendering
    // is single-threaded from loop().
    static FeedEntry local_feed[FEED_SIZE];
    int local_count, local_head;
    unsigned long local_now;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    local_count = feed_count;
    local_head = feed_head;
    // feed_entries is a ring keyed by feed_head; copy the whole array
    // so the renderer's modular index reaches the actual newest entry.
    for (int i = 0; i < FEED_SIZE; i++) local_feed[i] = feed_entries[i];
    local_now = millis();
    xSemaphoreGive(dataMutex);

    // Slide-in animation state — tracks new entries arriving while overlay is open
    static int           expand_prev_head   = -1;
    static unsigned long expand_shift_ms    = 0;
    static unsigned long expand_open_ms_last = 0;

    if (feed_expand_ms != expand_open_ms_last) {
        expand_open_ms_last = feed_expand_ms;
        expand_prev_head    = local_head;
        expand_shift_ms     = 0;
    }
    if (local_count > 0 && expand_prev_head != -1 && local_head != expand_prev_head) {
        expand_shift_ms  = local_now;
        expand_prev_head = local_head;
    } else if (expand_prev_head == -1 && local_count > 0) {
        expand_prev_head = local_head;
    }

    float expand_alpha = ui_progress(feed_expand_ms, UI_FADE_OUT_MS);
    auto ea = [&](uint16_t c) -> uint16_t { return lerp_col16(BG_COLOR, c, expand_alpha); };

    // Solid backdrop
    spr.fillRect(0, 18, DISP_W, DISP_H - 18, BG_COLOR);

    // Subtitle appended to header bar ("SCANNER / LIVE FEED")
    spr.setTextColor(ea(lerp_col16(HEADER_COLOR, ACCENT_COLOR, 0.4f)), ea(BG_COLOR));
    spr.setTextSize(TS_BODY);
    spr.setCursor(56, 5);
    kprint(spr, "/ LIVE FEED");

    // Gate content rendering until fade-in is underway
    if (expand_alpha < 0.3f) return;

    // Empty state
    if (local_count == 0) {
        spr.setTextColor(ea(DIM_COLOR), BG_COLOR);
        spr.setTextSize(TS_BODY);
        const char* msg = "no activity yet";
        int mw = (int)strlen(msg) * 7;
        spr.setCursor((DISP_W - mw) / 2, DISP_H / 2 - 4);
        kprint(spr, msg);
    } else {
        // Column layout (240px wide, 4px side padding = 232 usable):
        //   x=4    type symbol (9px) + flock star (6px) = ~18px
        //   x=26   DEVICE name (12 chars = 84px)
        //   x=114  RSSI ("-XXdBm" = 6 chars = 42px)
        //   x=162  SIGNAL (STRONG/MEDIUM/WEAK = up to 6 chars = 42px)
        //   x=210  AGE (short form like "1m" or "45s" = 20px)

        const int col_sym    = 4;
        const int col_rssi   = 140;
        const int col_sig    = 195;

        // Column headers (faded in)
        const int hdr_y = 23;
        spr.setTextSize(TS_BODY);
        spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
        spr.setCursor(col_sym, hdr_y); kprint(spr, "DEVICE");
        spr.setCursor(col_rssi, hdr_y); kprint(spr, "RSSI");
        spr.setCursor(col_sig,  hdr_y); kprint(spr, "SIGNAL");

        // Render rows
        const int row_top    = hdr_y + 12;
        const int avail_h    = DISP_H - row_top;
        const int max_rows   = 6;
        const int row_h      = avail_h / max_rows;
        const int row_pad    = avail_h - (max_rows * row_h);
        const int row_top_adj = row_top + row_pad / 2;

        // Clip to row area to prevent slide animation from bleeding into headers
        spr.setClipRect(0, row_top_adj, DISP_W, DISP_H - row_top_adj);
        int rendered = 0;
        for (int i = 0; i < local_count && rendered < max_rows; i++) {
            int idx = (local_head - i + FEED_SIZE * 2) % FEED_SIZE;
            FeedEntry& e = local_feed[idx];
            int row_y;
            if (rendered == 0 && expand_shift_ms > 0) {
                row_y = anim_slide_in(row_top_adj, -row_h, expand_shift_ms, 300);
            } else {
                row_y = row_top_adj + rendered * row_h;
            }

            // Type symbol color (Flock entries tinted toward ACCENT), faded in
            uint16_t proto_col = (e.proto == 0) ? CAUTION_COLOR : PURPLE_COLOR;
            if (e.is_flock) proto_col = lerp_col16(proto_col, ACCENT_COLOR, 0.5f);
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
            if (e.is_flock) {
                spr.setTextColor(ea(ACCENT_COLOR), BG_COLOR);
                spr.setTextSize(TS_BODY);
                spr.setCursor(sym_x + 10, row_y + 3);
                spr.print("*");
            }

            // DEVICE name — size 2, truncated to fit before RSSI column
            int name_start_x = col_sym + 15;
            if (e.is_flock) name_start_x += 8;
            int name_max_chars = (col_rssi - name_start_x - 4) / 7;
            if (name_max_chars > 12) name_max_chars = 12;
            if (name_max_chars < 1) name_max_chars = 1;
            if (name_max_chars > (int)sizeof(e.name) - 1) name_max_chars = sizeof(e.name) - 1;
            char name_disp[20];
            strncpy(name_disp, e.name, name_max_chars);
            name_disp[name_max_chars] = '\0';
            spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
            spr.setTextSize(TS_BODY);
            spr.setCursor(name_start_x, row_y + 3);
            spr.print(name_disp);

            // RSSI in dBm with units — right-aligned within the RSSI column
            char rssi_str[10];
            snprintf(rssi_str, sizeof(rssi_str), "%ddBm", e.rssi);
            spr.setTextColor(ea(TEXT_COLOR), BG_COLOR);
            spr.setCursor(col_rssi, row_y + 3);
            spr.print(rssi_str);

            // SIGNAL (spelled out)
            const char* strength_str;
            uint16_t strength_col;
            if (e.rssi > -60)      { strength_str = "STRONG"; strength_col = ACCENT_COLOR; }
            else if (e.rssi > -80) { strength_str = "MEDIUM"; strength_col = CAUTION_COLOR; }
            else                   { strength_str = "WEAK";   strength_col = DIM_COLOR; }
            spr.setTextColor(ea(strength_col), BG_COLOR);
            spr.setCursor(col_sig, row_y + 3);
            spr.print(strength_str);

            rendered++;
        }
        spr.clearClipRect();
    }

}

// ============================================================================
// MAIN UI CONTROLLER
// ============================================================================
void draw_current_screen() {
    switch (current_screen) {
        case 0: draw_scanner_screen();         break;
        case 1: draw_locator_screen();         break;
        case 2: draw_capture_history_screen(); break;
        case 3: draw_gps_screen();             break;
        case 4: draw_device_info_screen();     break;
    }
    
    if (show_feed_expanded) draw_feed_expanded_overlay();
    if (show_vol_overlay) draw_vol_overlay();
    if (show_help_overlay) draw_help_overlay();
    if (show_menu) draw_menu_overlay();
    if (wifi_config_open) draw_wifi_config_overlay();
    draw_toast_spr();
}

void transition_screen(int new_screen, int dir) {
    screen_dirty = true;
    if (stealth_mode) { current_screen = new_screen; return; }
    if (!is_muted) {
        M5Cardputer.Speaker.tone(660, 5);   // soft warm tap, matches menu click
    }
    if (new_screen == 2) {
        history_scroll_offset = 0;
        history_selected_idx = 0;
        hist_detail_open     = false;
        hist_detail_closing  = false;
        hist_detail_open_ms  = 0;
        hist_detail_close_ms = 0;
        // Seed the eased selection at the first row so there's no fly-in
        // from a stale position when the user lands on the screen.
        hist_sel_y_f         = (float)CONTENT_Y;
        hist_last_frame_ms   = 0;
        // Short timed take so a stuck PersistTask can't freeze the screen
        // transition. If we miss this window, the screen renders with the
        // previous snapshot — sd_hist_dirty stays set so the next loop
        // iteration retries once the bus frees up.
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            load_sd_history();
            xSemaphoreGive(sdMutex);
            sd_hist_dirty = false;
        }
    }
    if (new_screen == 4) {
        stats_scroll_target  = 0;
        stats_scroll_y_f     = 0.0f;
        stats_last_frame_ms  = millis();
        // Reseed values on entry so the first render doesn't fire 13
        // simultaneous roll-ups against zero.
        stats_values_initialized = false;
        for (int i = 0; i < STATS_CARD_COUNT; i++) {
            for (int ci = 0; ci < STAT_MAX_CHARS; ci++) stats_char_anim[i][ci] = 0;
        }
    }
    if (show_feed_expanded && new_screen != 0) show_feed_expanded = false;
    current_screen = new_screen;
    draw_current_screen();
    spr.pushSprite(0, 0);   // single on-screen push — safe
    (void)dir;              // slide animation removed — first frame at x=DISP_W
                            // triggered a LovyanGFX GDMA null-deref when the
                            // clipped destination rect collapsed to zero width.
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
static char  boot_prev_digits[8]          = "";
static unsigned long boot_digit_anim_ms[8] = {0};

void draw_boot_screen(int pct, const char* status_text = nullptr) {
    auto& lcd = M5Cardputer.Display;

    uint16_t bg     = lgfx::color565(  5,  10,  20);   // matches BG_COLOR (day)
    uint16_t blue   = lgfx::color565(110, 200, 255);   // matches HEADER_COLOR (ice blue)
    uint16_t white  = lgfx::color565(232, 239, 255);   // matches TEXT_COLOR
    uint16_t dim    = lgfx::color565(149, 165, 184);   // matches DIM_COLOR (bright steel)

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    // Bar geometry — ~10% smaller again (130×20). Single 1px outline
    // (the inset inner stroke is dropped — looks cleaner at this size).
    const int bar_w = 130;
    const int bar_h = 20;
    const int bar_x = (DISP_W - bar_w) / 2;
    const int bar_y = 78;
    const int bar_r = bar_h / 2;

    // Number geometry — textSize 2 (matches stats/values typography stack).
    // textSize 2 = 12px char width, 16px char height.
    const int num_y = 49;
    const int num_w = 80;
    const int num_x = (DISP_W - num_w) / 2;
    const int num_h = 16;

    // Status text geometry — moved up for tighter overall composition.
    const int status_y = 116;
    const int status_h = 8;

    // ── Staggered intro — each element fades in at its own offset ──
    // Sequence (offsets relative to first call):
    //   0ms   → screen filled, layout starts blank
    //   0ms   → title begins fading in (+slide down) over 350ms
    //   220ms → bar outline begins drawing over 350ms
    //   500ms → percentage + status text begin allowed to render
    static unsigned long boot_intro_start_ms = 0;
    static bool          boot_title_drawn    = false;
    static int           boot_outline_drawn_to = 0;  // pixels of outline drawn so far

    if (boot_first_draw) {
        lcd.fillScreen(bg);
        boot_first_draw      = false;
        boot_eased_fill      = 0.0f;
        boot_prev_fill_w     = 0;
        boot_intro_start_ms  = millis();
        boot_title_drawn     = false;
        boot_outline_drawn_to = 0;
    }

    unsigned long intro_elapsed = millis() - boot_intro_start_ms;

    // ── Title: slide down + fade in over 350ms, settled at y=24 ──
    {
        // textSize 2 is integer-scaled (6×8 → 12×16) so strokes are uniform.
        // textSize 1.5 produced uneven strokes — fractional scaling rounds some
        // pixel rows up and others down. y bumped up to maintain ~13px gaps.
        const int TITLE_FINAL_Y  = 20;
        const int TITLE_SLIDE_PX = 10;

        if (intro_elapsed < UI_ANIM_NORMAL) {
            // Title slides DOWN from above its resting position. Same elapsed
            // window drives the color fade; both read from the same start.
            unsigned long start = millis() - intro_elapsed;
            int y = anim_slide_in(TITLE_FINAL_Y, -TITLE_SLIDE_PX, start, UI_ANIM_NORMAL);
            float ease = ui_progress(start, UI_ANIM_NORMAL);
            uint16_t col = lerp_col16(bg, blue, ease);

            lcd.startWrite();
            lcd.setClipRect(0, 14, DISP_W, 22);
            lcd.fillRect(0, 14, DISP_W, 22, bg);
            lcd.setTextColor(col, bg);
            lcd.setTextSize(1);  // integer-scaled, clean uniform strokes
            lcd.setTextDatum(TC_DATUM);
            lcd.drawString(VERSION_STRING, DISP_W / 2, y);
            lcd.clearClipRect();
            lcd.endWrite();
        } else if (!boot_title_drawn) {
            // Settled: paint once at full color, then never touch again
            lcd.startWrite();
            lcd.setTextColor(blue, bg);
            lcd.setTextSize(1);
            lcd.setTextDatum(TC_DATUM);
            lcd.drawString(VERSION_STRING, DISP_W / 2, TITLE_FINAL_Y);
            lcd.endWrite();
            boot_title_drawn = true;
        }
    }

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
    // Each digit gets its own clipped redraw window. The % symbol is drawn
    // once on first frame (when boot_first_draw cleared the screen) and
    // never touched again — no flicker, no movement.
    //
    // Per-digit independence: when pct changes, only digits whose VALUE
    // changed get a new animation start time. Unchanged digits stay still.
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
            strncpy(boot_prev_digits, pct_str, sizeof(boot_prev_digits) - 1);
            boot_prev_digits[sizeof(boot_prev_digits) - 1] = '\0';
            boot_prev_pct = pct;
        }

        // Fast roll — must complete well within the shortest boot stage
        // (BOOT_RUSH approach_max_ms = 120) so digits are never caught
        // mid-animation by a phase transition.
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

        // Always render every digit every frame — settled or animating.
        // The previous skip-if-settled path left a digit mid-roll on the
        // LCD if a personality reset the animation before the roll
        // completed; without a redraw the digit would freeze in place
        // (this is what produced the "hung digit" boot bug).
        for (int di = 0; di < n_chars - 1 && di < 8; di++) {
            int dx = start_x + di * char_w;
            int draw_y = num_y;  // default: settled position

            if (boot_digit_anim_ms[di] != 0 &&
                (long)(now_t - boot_digit_anim_ms[di]) >= 0) {
                unsigned long elapsed = now_t - boot_digit_anim_ms[di];
                if (elapsed < ROLL_MS) {
                    draw_y = anim_slide_in(num_y, num_h, boot_digit_anim_ms[di], ROLL_MS);
                }
                // else: animation complete — draw_y stays at num_y
            }

            lcd.startWrite();
            lcd.setClipRect(dx, num_y, char_w, num_h);
            lcd.fillRect(dx, num_y, char_w, num_h, bg);
            lcd.setTextColor(white, bg);
            lcd.setTextSize(2);
            lcd.setTextDatum(TL_DATUM);
            char ch[2] = { pct_str[di], '\0' };
            lcd.drawString(ch, dx, draw_y);
            lcd.clearClipRect();
            lcd.endWrite();
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

        // Boot bar fill uses a shorter duration than UI_ANIM_NORMAL so the
        // bar arrives authoritatively rather than creeping. Must be shorter
        // than BOOT_RUSH approach_max_ms (120) so even the fastest stage
        // can fully resolve to its target.
        static const unsigned long BOOT_BAR_FILL_MS = 160;
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

        int cur_w = (cur_len > 0) ? (cur_len * 7 - 1) : 0;
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

// Boot stage personalities — selected randomly per call to give each
// boot slightly different pacing. Smoothness within a stage stays at
// ~60fps; only the dwell-after-target and the bar's approach curve vary.
// (enum BootPersonality is defined near the top of the file so Arduino's
// auto-generated forward declarations can see it.)
static BootPersonality pick_boot_personality() {
    int roll = random(0, 100);
    if (roll < 65) return BOOT_NORMAL;
    if (roll < 85) return BOOT_LURCH;
    if (roll < 95) return BOOT_RUSH;
    return BOOT_STALL;
}

// Drive the boot screen until the bar fill reaches the target percentage,
// then hold for a personality-dependent dwell. Frames render at ~60fps
// (16ms each) with no per-frame jitter — smoothness is deliberate.
//
// draw_boot_screen eases the bar fill internally (UI_ANIM_NORMAL re-target
// whenever target_fill changes), so personality variations modify the
// *target* the renderer sees rather than the per-frame motion. LURCH
// retargets to an overshoot pct, lets the renderer ease there, then
// retargets to the real value — the eye sees a smooth bounce.
//
// The third parameter is retained as `int = 0` so existing call sites that
// passed a frame count keep compiling; the value is ignored.
static void boot_animate(int pct, const char* status, int /*unused*/ = 0) {
    BootPersonality p = pick_boot_personality();

    int   final_pct        = pct;
    int   intermediate_pct = pct;
    unsigned long approach_max_ms;
    unsigned long settle_ms;

    switch (p) {
        case BOOT_RUSH:
            approach_max_ms = 120;
            settle_ms       = 0;
            break;
        case BOOT_LURCH:
            intermediate_pct = pct + random(3, 6);
            if (intermediate_pct > 100) intermediate_pct = 100;
            approach_max_ms = 200;
            settle_ms       = 100;
            break;
        case BOOT_STALL:
            intermediate_pct = pct;
            approach_max_ms = 400;
            settle_ms       = 0;
            break;
        case BOOT_NORMAL:
        default:
            approach_max_ms = 180;
            settle_ms       = 60;
            break;
    }

    // Phase 1: drive frames at ~60fps until the approach window expires.
    // The renderer eases the bar toward intermediate_pct internally.
    unsigned long phase_start = millis();
    bool first_frame = true;
    while (millis() - phase_start < approach_max_ms) {
        draw_boot_screen(intermediate_pct, first_frame ? status : nullptr);
        first_frame = false;
        delay(16);
    }

    // Phase 2 (LURCH only): settle from overshoot back to the real target.
    if (p == BOOT_LURCH && intermediate_pct != final_pct) {
        unsigned long settle_start = millis();
        while (millis() - settle_start < 120) {
            draw_boot_screen(final_pct, nullptr);
            delay(16);
        }
    }

    // Phase 3: brief dwell so the eye registers the new value.
    if (settle_ms > 0) {
        unsigned long settle_start = millis();
        while (millis() - settle_start < settle_ms) {
            draw_boot_screen(final_pct, nullptr);
            delay(16);
        }
    }
}

// Configure BLE scan parameters based on current power mode.
// Called at boot and after every periodic BLE stack restart.
static void apply_ble_scan_params() {
    if (!pBLEScan) return;
    if (low_power_mode) {
        // 50% duty cycle: 125ms interval, 62.5ms window.
        // Flock devices advertise every 100-200ms — still caught reliably.
        pBLEScan->setInterval(200);   // 200 × 0.625ms = 125ms
        pBLEScan->setWindow(100);     // 100 × 0.625ms = 62.5ms
    } else {
        // Full duty cycle: 60ms interval = window. Maximum detection rate.
        pBLEScan->setInterval(97);
        pBLEScan->setWindow(97);
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
    delay(500);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    // PSRAM availability + heap snapshot. Total PSRAM = 0 means PSRAM
    // isn't enabled in the board config; setPsram(true) will silently fall
    // through to internal RAM and the sprite-fallback path will kick in.
    Serial.printf("[MEM] Total PSRAM: %u, Free PSRAM: %u, Free internal: %u\n",
                  (unsigned)ESP.getPsramSize(),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)esp_get_free_heap_size());

    // Shrink speaker DMA buffers so I2S allocation doesn't eat the DMA pool.
    {
        auto spk_cfg = M5Cardputer.Speaker.config();
        spk_cfg.dma_buf_count = 3;     // default 8
        spk_cfg.dma_buf_len   = 256;   // default 512
        M5Cardputer.Speaker.config(spk_cfg);
    }

    // dataMutex MUST be created before any task that uses it is spawned.
    dataMutex = xSemaphoreCreateMutex();
    sdMutex   = xSemaphoreCreateMutex();

    // Create the draw sprite FIRST, before WiFi / BLE / LittleFS eat internal
    // heap. Try PSRAM first (frees ~64KB of internal SRAM for the WiFi/BLE
    // radio stacks); fall back to internal SRAM on boards without PSRAM
    // configured. Hard-fail on total allocation failure.
    spr.setColorDepth(16);
    spr.setPsram(true);
    void* sprite_buf = spr.createSprite(DISP_W, DISP_H);
    if (!sprite_buf) {
        Serial.println("[GFX] PSRAM sprite failed, falling back to internal");
        spr.setPsram(false);
        sprite_buf = spr.createSprite(DISP_W, DISP_H);
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
    brightness_level = 2;
    apply_color_palette();

    // Ease the screen in: brightness ramps from 0 → target over UI_ANIM_NORMAL
    // while the title intro animation runs simultaneously. Reads as a "wakeup"
    // — screen and title come alive together rather than the layout popping in.
    M5Cardputer.Display.setBrightness(0);
    {
        const int FADE_STEPS   = 16;
        const int FADE_STEP_MS = (int)UI_ANIM_NORMAL / FADE_STEPS;
        const int target_b     = BRIGHTNESS_LEVELS[brightness_level];
        for (int i = 0; i <= FADE_STEPS; i++) {
            int b = (target_b * i) / FADE_STEPS;
            M5Cardputer.Display.setBrightness(b);
            // First call kicks off the staggered intro animation; subsequent
            // calls advance it. Status only set on first draw.
            draw_boot_screen(0, (i == 0) ? "waking up" : nullptr);
            delay(FADE_STEP_MS);
        }
    }

    boot_animate(5 + random(0, 4), "opening serial");

    boot_animate(12 + random(0, 3), "searching for SD");

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

        if (!SD.exists("/FLOCK_FINDER"))           SD.mkdir("/FLOCK_FINDER");
        if (!SD.exists("/FLOCK_FINDER/logs"))      SD.mkdir("/FLOCK_FINDER/logs");
        if (!SD.exists("/FLOCK_FINDER/captures"))  SD.mkdir("/FLOCK_FINDER/captures");
        if (!SD.exists("/FLOCK_FINDER/stats"))     SD.mkdir("/FLOCK_FINDER/stats");
        Serial.println("[SD] Directory structure OK");

        if (!SD.exists(current_log_file)) {
            File file = SD.open(current_log_file, FILE_WRITE);
            if (file) { file.println("Uptime_ms,EpochUTC,EpochIsGPS,Channel,Type,Proto,RSSI,MAC,Name,TXPower,Method,Conf,ConfLabel,Extra,SeqNum,Lat,Lon,SpeedMPH,HeadingDeg,AltM"); file.close(); }
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
    boot_animate(35, sd_available ? "mounting SD card" : "no SD found");
    // Seed hot-plug state so the first poll doesn't fire a spurious "mounted" toast
    sd_was_available = sd_available;
    last_sd_check_ms = millis();

    delay(100);
    // NMEA sentences max ~82 chars — 1024-byte default RX is 2× too big.
    // setRxBufferSize must be called BEFORE begin() to take effect.
    SerialGPS.setRxBufferSize(512);
    SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    delay(100);
    WiFi.mode(WIFI_STA); delay(50);
    boot_animate(38 + random(0, 4), "connecting GPS");
    boot_animate(46 + random(0, 4), "drawing interface");

    // Sprite was already created at the top of setup().
    boot_animate(50 + random(0, 5), "preparing display");

    // Prime the EMA filter to eliminate startup ADC noise before taking the baseline
    for (int i = 0; i < 20; i++) {
        update_load_sag();
        get_filtered_voltage();
        delay(2);
    }
    boot_animate(58 + random(0, 3), "calibrating battery");

    setCpuFrequencyMhz(240);
    boot_animate(62 + random(0, 3), "boosting CPU");
    ble_event_queue = xQueueCreate(BLE_POOL_SIZE, sizeof(uint8_t));
    xTaskCreatePinnedToCore(ble_worker_task, "BLEWorker", 2560, NULL, 1, &BLEWorkerHandle, 1);
    boot_animate(68, "starting Bluetooth");

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
    }
    Serial.printf("[BOOT] Free heap after LittleFS: %u\n",
                  (unsigned)esp_get_free_heap_size());
    boot_animate(78 + random(0, 3), "loading session");

    // First-boot WiFi credential initialization from #defines if flash is empty
    if (strlen(export_ssid) == 0 && strcmp(EXPORT_WIFI_SSID, "YOUR_SSID_HERE") != 0) {
        strncpy(export_ssid, EXPORT_WIFI_SSID, sizeof(export_ssid) - 1);
        export_ssid[sizeof(export_ssid) - 1] = '\0';
    }
    if (strlen(export_pass) == 0 && strcmp(EXPORT_WIFI_PASS, "YOUR_PASSWORD_HERE") != 0) {
        strncpy(export_pass, EXPORT_WIFI_PASS, sizeof(export_pass) - 1);
        export_pass[sizeof(export_pass) - 1] = '\0';
    }
    boot_animate(82 + random(0, 3), "reading credentials");

    lifetime_boots++;
    if (littlefs_available) {
        save_session_to_flash();
    }
    session_start_time = millis();

    // WiFi promiscuous mode — complete before scanner screen appears
    WiFi.disconnect(); delay(100); esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t filt; filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt); esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); delay(100);
    Serial.printf("[BOOT] Free heap after WiFi promisc: %u\n",
                  (unsigned)esp_get_free_heap_size());
    boot_animate(88, "starting sniffer");

    // NimBLE — complete before scanner screen appears
    NimBLEDevice::init(""); NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    Serial.printf("[BOOT] Free heap after NimBLE init: %u\n",
                  (unsigned)esp_get_free_heap_size());
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(&ble_cb_singleton, false);
    pBLEScan->setActiveScan(false);
    apply_ble_scan_params();
    // Don't store results internally — every advertisement is already handled
    // via the callback -> ble_event_queue -> ble_worker_task pipeline. The
    // default cache can hit 10–20 KB in a busy RF environment for no benefit.
    pBLEScan->setMaxResults(0);
    last_ble_restart_ms = millis();
    boot_animate(96, "arming scanner");

    // Tasks
    last_channel_hop = millis(); last_ble_scan = millis(); last_sd_flush = millis(); last_persist_save = millis();
    xTaskCreatePinnedToCore(ScannerLoopTask, "ScannerTask", 1792, NULL, 1, &ScannerTaskHandle, 0);
    xTaskCreatePinnedToCore(GPSLoopTask, "GPSTask", 1536, NULL, 1, &GPSTaskHandle, 0);
    last_user_input_ms = millis();
    system_fully_booted = true;

    // Spawn the LED task at priority 1 on Core 1 — must come after WiFi+BLE
    // are up so RMT/radio contention is avoided.
    xTaskCreatePinnedToCore(LedTask, "LedTask", 1536, NULL, 1, &LedTaskHandle, 1);

    // WDT was already initialized early in setup(); each watched task
    // self-subscribes via esp_task_wdt_add(NULL) inside its loop.

    boot_animate(100, "ready");
    delay(400);

    // Gate: wait for the WiFi sniffer to confirm radios are up (~15 packets) or
    // 4 seconds, whichever comes first. Pump event queues during the wait so the
    // live feed buffer pre-populates before the scanner screen appears.
    {
        unsigned long feed_gate_start = millis();
        const unsigned long FEED_GATE_MAX_MS = 4500;
        while ((millis() - feed_gate_start) < FEED_GATE_MAX_MS) {
            if (ambient_packet_count >= 15) break;
            draw_boot_screen(100, "listening for signals");
            delay(30);
            M5Cardputer.update();
            process_wifi_event_queue();
            feed_commit_pending();
        }
        // Reset so a later boot-screen draw (shouldn't happen) would repaint cleanly
        boot_prev_fill_w = 0;
    }

    // ── Crossfade from boot screen to scanner via the backlight PWM ──
    // Fade-out: ramp brightness to 0 over ~400ms, so the still-rendered boot
    // content dims away. Swap sprite contents while the screen is dark.
    // Fade-in: ramp brightness back to the user's target over ~400ms.
    {
        int start_brightness = BRIGHTNESS_LEVELS[brightness_level];
        const int FADE_STEPS   = 12;
        const int FADE_STEP_MS = 16;   // 12 × 16ms ≈ 192ms per phase

        for (int i = 1; i <= FADE_STEPS; i++) {
            int b = (start_brightness * (FADE_STEPS - i)) / FADE_STEPS;
            M5Cardputer.Display.setBrightness(b);
            delay(FADE_STEP_MS);
        }

        // Screen is dark — swap to the scanner content
        draw_current_screen();
        spr.pushSprite(0, 0);

        for (int i = 1; i <= FADE_STEPS; i++) {
            int b = (start_brightness * i) / FADE_STEPS;
            M5Cardputer.Display.setBrightness(b);
            delay(FADE_STEP_MS);
        }
    }

    // Boot chime — plays after the scanner is visible
    if (!is_muted) {
        int boot_vol = current_volume > 25 ? 25 : current_volume;
        M5Cardputer.Speaker.setVolume(boot_vol);
        delay(120);
        // Vintage chime — F major arpeggio rising slowly. Lower octave + longer
        // note duration matches the deliberate rhythm of the boot animations
        // (each note ≈ UI_ANIM_QUICK; gaps ≈ UI_ANIM_QUICK/4).
        M5Cardputer.Speaker.tone(349, 180); delay(220);  // F4
        M5Cardputer.Speaker.tone(440, 180); delay(220);  // A4
        M5Cardputer.Speaker.tone(523, 260); delay(300);  // C5
    }
    M5Cardputer.Speaker.setVolume(current_volume);

    // Drop to 160MHz for normal operation. Boot and radio init benefit from
    // 240MHz; steady-state scanning, rendering, and BLE processing do not.
    // Saves ~20mA continuous draw (~2 hours extra on 1750mAh).
    setCpuFrequencyMhz(160);

    if (lifetime_boots <= 3) {
        set_toast_direct("TAB = help  M = menu", HEADER_COLOR);
    }
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    static bool wdt_subscribed = false;
    if (!wdt_subscribed) {
        esp_err_t err = esp_task_wdt_add(NULL);
        if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) wdt_subscribed = true;
    }
    if (wdt_subscribed) esp_task_wdt_reset();
    M5Cardputer.update(); yield();

    if (export_connecting) {
        export_tick_connect();
    }
    if (export_mode_active) {
        if (export_server) export_server->handleClient();
        if ((millis() - export_mode_started_at) > EXPORT_MODE_MAX_MS) {
            export_mode_stop();
        }
    }

    // Dynamically calculate expected hardware voltage sag for this loop iteration
    update_load_sag();

    int32_t loop_mv = get_filtered_voltage();

    // Low-battery voltage warnings — once per crossing, 100mV hysteresis to re-arm.
    {
        static int32_t last_battery_warning_mv = 9999;
        if (loop_mv <= 3500 && last_battery_warning_mv > 3500) {
            set_toast_direct("BATT CRITICAL 3.5V", CAUTION_COLOR);
            last_battery_warning_mv = 3500;
        } else if (loop_mv <= 3700 && last_battery_warning_mv > 3700) {
            set_toast_direct("BATT LOW 3.7V", CAUTION_COLOR);
            last_battery_warning_mv = 3700;
        } else if (last_battery_warning_mv < 9999
                   && loop_mv >= last_battery_warning_mv + 100) {
            last_battery_warning_mv = 9999;
        }
    }

    // One-time stack usage diagnostic — logs after 60s when all code paths
    // (WiFi events, BLE scans, GPS parsing, SD flushes, alarms) have fired
    // at least once. Values are in bytes of unused stack (high-water mark).
    // If any value is >1500, the task's stack can be safely reduced by that
    // amount minus a 256-byte safety margin.
    {
        static bool stack_reported = false;
        if (!stack_reported && millis() > 60000) {
            stack_reported = true;
            UBaseType_t scan_hw = uxTaskGetStackHighWaterMark(ScannerTaskHandle);
            UBaseType_t gps_hw  = uxTaskGetStackHighWaterMark(GPSTaskHandle);
            UBaseType_t ble_hw  = BLEWorkerHandle ? uxTaskGetStackHighWaterMark(BLEWorkerHandle) : 0;
            UBaseType_t led_hw  = LedTaskHandle ? uxTaskGetStackHighWaterMark(LedTaskHandle) : 0;
            UBaseType_t loop_hw = uxTaskGetStackHighWaterMark(NULL);  // NULL = calling task (loop)
            Serial.printf("[STACK] High-water marks (bytes remaining):\n");
            Serial.printf("  Scanner: %u / 1792\n", (unsigned)scan_hw);
            Serial.printf("  GPS:     %u / 1536\n", (unsigned)gps_hw);
            Serial.printf("  BLE:     %u / 2560\n", (unsigned)ble_hw);
            Serial.printf("  LED:     %u / 1536\n", (unsigned)led_hw);
            Serial.printf("  Loop:    %u / 8192\n", (unsigned)loop_hw);
            Serial.printf("[STACK] Safe to reduce any task where remaining > 512 bytes.\n");
            Serial.printf("  Recommended: new_stack = current - (remaining - 256)\n");
        }
    }

    // Heap health: warn (and toast) when free internal heap drops below 8KB.
    // Catches slow leaks / FAT-driver mallocs piling up before they cause
    // an unrecoverable abort.
    {
        size_t free_heap = esp_get_free_heap_size();
        static size_t min_heap_seen = 999999;
        if (free_heap < min_heap_seen) min_heap_seen = free_heap;
        if (free_heap < 6000) {
            static unsigned long last_heap_warn = 0;
            if (millis() - last_heap_warn > 30000) {
                Serial.printf("[HEAP] CRITICAL: %u bytes free (min: %u)\n",
                              (unsigned)free_heap, (unsigned)min_heap_seen);
                set_toast_direct("LOW MEMORY", CAUTION_COLOR);
                last_heap_warn = millis();
            }
        }
    }

    process_wifi_event_queue();
    feed_commit_pending();
    if (wdt_subscribed) esp_task_wdt_reset();

    int conf_snapshot = 0;
    int src_snapshot = 0;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (trigger_alarm_confidence >= 50) {
        conf_snapshot = trigger_alarm_confidence;
        src_snapshot = trigger_alarm_source;
    }
    trigger_alarm_confidence = 0;
    trigger_alarm_source = 0;
    xSemaphoreGive(dataMutex);

    if (conf_snapshot >= 50) {
        play_escalated_alarm(conf_snapshot, src_snapshot);
    }
    if (wdt_subscribed) esp_task_wdt_reset();

    if (M5Cardputer.BtnA.wasClicked() && !stealth_mode) {
        last_user_input_ms = millis();
        if (ambient_mode) {
            ambient_mode = false;
            M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]);
            // Wake the I2S peripheral from idle so the next tone() call
            // (often a UI click or alarm chime) doesn't hit a DMA assertion.
            M5Cardputer.Speaker.stop();
        }
        if (show_menu) {
            show_menu = false;
            draw_current_screen();
            spr.pushSprite(0, 0);
        } else if (show_feed_expanded) {
            show_feed_expanded = false;
            draw_current_screen();
            spr.pushSprite(0, 0);
        } else {
            int next_screen = current_screen + 1;
            int dir = (next_screen >= NUM_SCREENS) ? -1 : 1;
            if (next_screen >= NUM_SCREENS) next_screen = 0;
            transition_screen(next_screen, dir);
        }
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        last_user_input_ms = millis();
        screen_dirty = true;
        if (ambient_mode) {
            ambient_mode = false;
            M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]);
            // Same I2S wake as the BtnA path.
            M5Cardputer.Speaker.stop();
        }
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        
        if (status.tab && !stealth_mode) {
            show_help_overlay = !show_help_overlay;
            if (show_help_overlay) {
                help_ease_start = millis();
            }
            draw_current_screen(); spr.pushSprite(0,0);
        }

        // Tracks whether ENTER was already handled by the status.word loop
        // so the status.enter fallback below doesn't re-fire the same action
        // on firmwares that report ENTER in both places.
        bool enter_consumed = false;

        for (auto c : status.word) {

            if (c == '\n' || c == '\r') enter_consumed = true;

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
                    if (c == ';') {
                        wifi_config_field--;
                        if (wifi_config_field < 0) wifi_config_field = 0;
                        M5Cardputer.Speaker.tone(660, 5);
                    } else if (c == '.') {
                        wifi_config_field++;
                        if (wifi_config_field > 3) wifi_config_field = 3;
                        M5Cardputer.Speaker.tone(660, 5);
                    } else if (c == '\n' || c == '\r') {
                        if (wifi_config_field == 0 || wifi_config_field == 1) {
                            wifi_config_editing = true;
                            if (wifi_config_field == 0) {
                                strncpy(wifi_config_ssid_buf, export_ssid, sizeof(wifi_config_ssid_buf) - 1);
                                wifi_config_ssid_buf[sizeof(wifi_config_ssid_buf) - 1] = '\0';
                                wifi_config_cursor = strlen(wifi_config_ssid_buf);
                            } else {
                                strncpy(wifi_config_pass_buf, export_pass, sizeof(wifi_config_pass_buf) - 1);
                                wifi_config_pass_buf[sizeof(wifi_config_pass_buf) - 1] = '\0';
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
                            set_toast_direct("WIFI SAVED", ACCENT_COLOR);
                            wifi_config_open = false;
                        } else if (wifi_config_field == 3) {
                            // Clear
                            export_ssid[0] = '\0';
                            export_pass[0] = '\0';
                            wifi_config_ssid_buf[0] = '\0';
                            wifi_config_pass_buf[0] = '\0';
                            save_wifi_credentials();
                            if (!persist_in_flight) schedule_persist();
                            set_toast_direct("WIFI CLEARED", CAUTION_COLOR);
                            wifi_config_open = false;
                        }
                    } else if (c == 0x1B) {
                        wifi_config_open = false;
                    }
                }
                draw_current_screen(); spr.pushSprite(0, 0);
                continue;  // swallow all keys while wifi config is open
            }

            // ── Menu input intercept — when menu is open, swallow nav keys ──
            // ; is up-arrow, . is down-arrow on the M5Cardputer keyboard.
            if (show_menu) {
                if (c == ';') {
                    int prev = menu_selected;
                    menu_selected--;
                    if (menu_selected < 0) menu_selected = 0;
                    if (menu_selected != prev) menu_click();
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else if (c == '.') {
                    int prev = menu_selected;
                    menu_selected++;
                    if (menu_selected >= MENU_ITEM_COUNT) menu_selected = MENU_ITEM_COUNT - 1;
                    if (menu_selected != prev) menu_click();
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else if (c == '\n' || c == '\r') {
                    menu_click();
                    handle_menu_select();
                } else if (c == 0x1B || c == 'm') {
                    show_menu = false;
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
                continue;  // swallow all keys while menu is open
            }

            if (c == 0x1B && !stealth_mode) {  // ASCII Escape
                // Priority order: close overlays first, then navigate home.
                if (show_feed_expanded) {
                    show_feed_expanded = false;
                    draw_current_screen(); spr.pushSprite(0, 0);
                    continue;
                }
                if (show_help_overlay) {
                    show_help_overlay = false;
                    draw_current_screen(); spr.pushSprite(0, 0);
                    continue;
                }
                if (current_screen == 2 && hist_detail_open) {
                    hist_detail_open    = false;
                    hist_detail_closing = false;
                    screen_dirty = true;
                    draw_current_screen(); spr.pushSprite(0, 0);
                    continue;
                }
                if (current_screen != 0) {
                    transition_screen(0, -1);
                    continue;
                }
                // Already on scanner with no overlays — nothing to do.
            }
            else if (c == 'm') {
                if (!stealth_mode) {
                    show_menu = !show_menu;
                    if (show_menu) {
                        menu_open_ms = millis();
                        menu_scroll_offset = 0;
                        show_feed_expanded = false;
                        show_help_overlay = false;
                    }
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
            }
            else if (c == '`') {
                if (is_muted) {
                    // Unmute — restore to a reasonable default if volume was zeroed
                    is_muted = false;
                    if (current_volume == 0) current_volume = 75;
                    M5Cardputer.Speaker.setVolume(current_volume);
                    beep(600, 50);
                } else {
                    // Mute — zero out volume
                    is_muted = true;
                    current_volume = 0;
                    M5Cardputer.Speaker.setVolume(0);
                }
                draw_current_screen(); spr.pushSprite(0,0);
            }
            else if (c == ';') {
                // Up arrow — context-dependent scroll
                if (current_screen == 2) {
                    history_selected_idx--;
                    if (history_selected_idx < 0) history_selected_idx = 0;
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else if (current_screen == 4) {
                    // Target only — render loop eases via anim_filter
                    stats_scroll_target -= STATS_SCROLL_STEP;
                    if (stats_scroll_target < 0) stats_scroll_target = 0;
                    screen_dirty = true;
                }
            }
            else if (c == '.') {
                // Down arrow — context-dependent scroll
                if (current_screen == 2) {
                    int hist_total = sd_available ? sd_hist_count : capture_history_count;
                    history_selected_idx++;
                    if (history_selected_idx >= hist_total) history_selected_idx = max(0, hist_total - 1);
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else if (current_screen == 4) {
                    // Target only — render loop eases via anim_filter
                    stats_scroll_target += STATS_SCROLL_STEP;
                    if (stats_scroll_target > STATS_MAX_SCROLL)
                        stats_scroll_target = STATS_MAX_SCROLL;
                    screen_dirty = true;
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
                draw_current_screen(); spr.pushSprite(0, 0);
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
                draw_current_screen(); spr.pushSprite(0, 0);
            }
            else if (c == 'x') {
                if (!stealth_mode) {
                    static bool sim_wifi = true;
                    char fake_mac[18];
                    snprintf(fake_mac, sizeof(fake_mac), "00:11:22:33:%02X:%02X", random(0, 255), random(0, 255));
                    
                    if (sim_wifi) {
                        log_detection("SIMULATION", "WIFI", random(-80, -30), fake_mac, "Test_WiFi", 6, 0, "Beacon", "manual_test", 100, 1);
                        xSemaphoreTake(dataMutex, portMAX_DELAY);
                        session_flock_wifi--; session_wifi--; lifetime_wifi--;
                        lifetime_flock_total--;
                        xSemaphoreGive(dataMutex);
                    } else {
                        log_detection("SIMULATION", "BLE", random(-90, -40), fake_mac, "Test_BLE", 0, 0, "Adv", "manual_test", 100, 1);
                        xSemaphoreTake(dataMutex, portMAX_DELAY);
                        session_flock_ble--; session_ble--; lifetime_ble--;
                        lifetime_flock_total--;
                        xSemaphoreGive(dataMutex);
                    }
                    // Set alarm trigger under mutex — both fields together,
                    // matching the producer pattern in process_wifi_event_queue
                    // and ble_worker_task.
                    xSemaphoreTake(dataMutex, portMAX_DELAY);
                    trigger_alarm_confidence = 100;
                    trigger_alarm_source = sim_wifi ? 0 : 1;  // 0=WiFi, 1=BLE
                    xSemaphoreGive(dataMutex);

                    sim_wifi = !sim_wifi;
                }
            }
            else if (c == 't') { 
                if (!stealth_mode && capture_history_count > 0) {
                    static int target_select_idx = -1;
                    xSemaphoreTake(dataMutex, portMAX_DELAY);
                    int current_hist_count = capture_history_count;
                    target_select_idx = (target_select_idx + 1) % current_hist_count;
                    char t_mac[18];  strncpy(t_mac,  capture_history[target_select_idx].mac,  17); t_mac[17]  = '\0';
                    char t_name[65]; strncpy(t_name, capture_history[target_select_idx].name, 64); t_name[64] = '\0';
                    char t_type[16]; strncpy(t_type, capture_history[target_select_idx].type, 15); t_type[15] = '\0';
                    int t_conf = capture_history[target_select_idx].confidence;
                    int t_id   = capture_history[target_select_idx].id;
                    xSemaphoreGive(dataMutex);

                    locator_start(t_mac, t_name, t_type, t_id);
                    trigger_toast("TARGET", t_name, t_conf);
                    transition_screen(1, 1);
                } else if (!stealth_mode) {
                    trigger_toast("INFO", "No targets yet", 0);
                }
            }
            else if (c == '/') {
                if (!stealth_mode) {
                    int next = current_screen + 1;
                    int d = (next >= NUM_SCREENS) ? -1 : 1;
                    if (next >= NUM_SCREENS) next = 0;
                    transition_screen(next, d);
                }
            }
            else if (c == ',') {
                if (!stealth_mode) {
                    int prev = current_screen - 1;
                    int d = (prev < 0) ? 1 : -1;
                    if (prev < 0) prev = NUM_SCREENS - 1;
                    transition_screen(prev, d);
                }
            }
            else if (c == 'b') {
                if (!stealth_mode) {
                    brightness_level = (brightness_level + 1) % 3;
                    M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]);
                    // Disable LED at dim levels, re-enable at full brightness
                    if (brightness_level < 2) {
                        led_breathing_on = false;
                    } else {
                        led_breathing_on = true;
                    }
                }
            }
            else if (c == 's') { stealth_mode = !stealth_mode; if (stealth_mode) { M5Cardputer.Display.setBrightness(5); } else { M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]); draw_current_screen(); spr.pushSprite(0,0); } }
            else if (c == 'n') {
                if (current_screen == 1) {
                    // Locator screen: toggle Pointing North mode
                    north_mode = !north_mode;
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
                // On other screens, 'n' is currently a no-op.
            }
            else if (c == 'l') {
                // Instant locator stop from any screen. No-op when inactive.
                if (locator_active) {
                    locator_stop();
                    trigger_toast("INFO", "Locator stopped", 0);
                    beep(500, 60);
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
                if (!stealth_mode) {
                    if (show_feed_expanded) {
                        show_feed_expanded = false;
                    } else {
                        if (current_screen != 0) transition_screen(0, -1);
                        show_feed_expanded = true;
                        feed_expand_ms = millis();
                    }
                    draw_current_screen();
                    spr.pushSprite(0, 0);
                }
            }
            else if (c == '\n' || c == '\r') {
                if (!stealth_mode && current_screen == 2) {
                    int hist_total = sd_available ? sd_hist_count : capture_history_count;
                    if (hist_total > 0) {
                        if (!hist_detail_open) {
                            hist_detail_open    = true;
                            hist_detail_closing = false;
                            hist_detail_open_ms = millis();
                        } else {
                            // Instant close — animated close left the renderer
                            // in a backdrop-only state if frames stopped before
                            // the next redraw, which read as a black screen.
                            hist_detail_open    = false;
                            hist_detail_closing = false;
                        }
                        screen_dirty = true;
                        draw_current_screen(); spr.pushSprite(0, 0);
                    }
                }
                // ENTER on other screens is a no-op (menu handles navigation)
            }
        }

        // ── Key-hold repeat for the ; / . arrow keys ──
        // The M5Cardputer keyboard doesn't auto-repeat; we track hold state
        // ourselves so a held arrow keeps scrolling at REPEAT_INTERVAL after
        // a HOLD_DELAY initial wait.
        {
            static unsigned long arrow_hold_start = 0;
            static unsigned long arrow_last_repeat = 0;
            static char arrow_held_key = 0;
            const unsigned long HOLD_DELAY      = 500;  // ms before repeat starts
            const unsigned long REPEAT_INTERVAL = 150;  // ms between repeats

            bool up_held = false, down_held = false;
            // Skip hold-repeat when the menu is open — the menu's own
            // input handler in the word loop already processed ;/. and
            // the hold-repeat would double-fire the navigation.
            if (!show_menu && !wifi_config_open) {
                for (auto c : status.word) {
                    if (c == ';') up_held = true;
                    if (c == '.') down_held = true;
                }
            }
            char cur_arrow = up_held ? ';' : (down_held ? '.' : 0);

            if (cur_arrow && cur_arrow == arrow_held_key) {
                unsigned long hold_dur = millis() - arrow_hold_start;
                if (hold_dur > HOLD_DELAY &&
                    (millis() - arrow_last_repeat) > REPEAT_INTERVAL) {
                    if (show_menu) {
                        if (cur_arrow == ';') {
                            menu_selected--;
                            if (menu_selected < 0) menu_selected = 0;
                        } else {
                            menu_selected++;
                            if (menu_selected >= MENU_ITEM_COUNT) menu_selected = MENU_ITEM_COUNT - 1;
                        }
                        draw_current_screen(); spr.pushSprite(0, 0);
                    } else if (current_screen == 2) {
                        if (cur_arrow == ';') {
                            history_selected_idx--;
                            if (history_selected_idx < 0) history_selected_idx = 0;
                        } else {
                            int ht = sd_available ? sd_hist_count : capture_history_count;
                            history_selected_idx++;
                            if (history_selected_idx >= ht) history_selected_idx = max(0, ht - 1);
                        }
                        draw_current_screen(); spr.pushSprite(0, 0);
                    } else if (current_screen == 4) {
                        // Target only — render loop eases via anim_filter
                        if (cur_arrow == ';') {
                            stats_scroll_target -= STATS_SCROLL_STEP;
                            if (stats_scroll_target < 0) stats_scroll_target = 0;
                        } else {
                            stats_scroll_target += STATS_SCROLL_STEP;
                            if (stats_scroll_target > STATS_MAX_SCROLL)
                                stats_scroll_target = STATS_MAX_SCROLL;
                        }
                        screen_dirty = true;
                    }
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

        // Fallback ENTER check — some Cardputer ADV firmware doesn't put
        // ENTER in status.word. Check status.enter directly only when the
        // loop above didn't already act on it (firmwares that report ENTER
        // in both places would otherwise toggle every action twice).
        if (status.enter && !enter_consumed && !stealth_mode) {
            if (show_menu) {
                menu_click();
                handle_menu_select();
            } else if (current_screen == 2) {
                int hist_total = sd_available ? sd_hist_count : capture_history_count;
                if (hist_total > 0) {
                    if (!hist_detail_open) {
                        hist_detail_open    = true;
                        hist_detail_closing = false;
                        hist_detail_open_ms = millis();
                    } else {
                        hist_detail_open    = false;
                        hist_detail_closing = false;
                    }
                    screen_dirty = true;
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
            }
        }

        if (status.del && !stealth_mode) {
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
                    wifi_config_open = false;
                }
                draw_current_screen(); spr.pushSprite(0, 0);
            } else if (show_feed_expanded) {
                show_feed_expanded = false;
                draw_current_screen(); spr.pushSprite(0, 0);
            } else if (show_help_overlay) {
                show_help_overlay = false;
                draw_current_screen(); spr.pushSprite(0, 0);
            } else if (show_menu) {
                show_menu = false;
                draw_current_screen(); spr.pushSprite(0, 0);
            } else if (current_screen == 2 && hist_detail_open) {
                hist_detail_open    = false;
                hist_detail_closing = false;
                screen_dirty = true;
                draw_current_screen(); spr.pushSprite(0, 0);
            } else if (current_screen != 0) {
                // Nothing to close — go home to scanner
                transition_screen(0, -1);
            }
        }
    }
    if (wdt_subscribed) esp_task_wdt_reset();

    // Fire pending '\' single-press action if double-tap window expired
    if (bs_pending_exists && millis() >= bs_pending_until) {
        bs_pending_exists = false;
        bs_pending_until = 0;
        led_breathing_on = !led_breathing_on;
        beep(led_breathing_on ? 800 : 400, 30);
    }

    if (millis() - last_time_save >= 1000) { xSemaphoreTake(dataMutex, portMAX_DELAY); lifetime_seconds++; xSemaphoreGive(dataMutex); last_time_save = millis(); }
    if (millis() - last_persist_save >= PERSIST_INTERVAL_MS) {
        // Only advance the gate when the spawn actually took. If heap was
        // too low to allocate the task stack, retry on the next loop tick
        // (~10ms) instead of waiting another full PERSIST_INTERVAL.
        if (schedule_persist()) {
            last_persist_save = millis();
        }
    }
    rssi_track_expire();

    // Periodic BLE stack health restart — prevents NimBLE internal state
    // corruption that can build up during multi-hour continuous scanning.
    if (millis() - last_ble_restart_ms > BLE_RESTART_INTERVAL_MS) {
        last_ble_restart_ms = millis();
        // Suspend ScannerLoopTask first — without this, the Core 0 task can
        // be inside pBLEScan->start()/isScanning() while we deinit the stack
        // and free the underlying object, dereferencing a dangling pointer.
        if (ScannerTaskHandle) vTaskSuspend(ScannerTaskHandle);
        if (pBLEScan) {
            pBLEScan->stop();
            pBLEScan->clearResults();
        }
        NimBLEDevice::deinit(true);
        delay(100);
        NimBLEDevice::init("");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        pBLEScan = NimBLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(&ble_cb_singleton, false);
        pBLEScan->setActiveScan(false);
        apply_ble_scan_params();
        pBLEScan->setMaxResults(0);  // see setup() — callback handles everything
        last_ble_scan = millis();
        if (ScannerTaskHandle) vTaskResume(ScannerTaskHandle);
        Serial.println("[BLE] Periodic stack restart completed");
    }

    // SD hot-plug: periodically attempt remount if card is absent, or probe if present
    sd_check_hotplug();
    if (wdt_subscribed) esp_task_wdt_reset();

    if (millis() - last_sd_flush_check >= 500) {
        last_sd_flush_check = millis(); 
        bool should_flush = false; 
        
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (sd_write_count >= MAX_LOG_BUFFER || pcap_write_count >= MAX_PCAP_BUFFER ||
            ble_pcap_write_count >= MAX_PCAP_BUFFER ||
            (millis() - last_sd_flush > SD_FLUSH_INTERVAL &&
             (sd_write_count > 0 || pcap_write_count > 0 || ble_pcap_write_count > 0))) {
            should_flush = true;
        }
        xSemaphoreGive(dataMutex); 
        
        if (should_flush) flush_sd_buffer();
    }
    if (wdt_subscribed) esp_task_wdt_reset();

    {
        bool need_update;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        need_update = locator_active && locator_has_estimate;
        if (need_update && gps.location.isValid() && gps.location.age() < 2000) {
            double my_lat = gps.location.lat();
            double my_lng = gps.location.lng();
            double tgt_lat = locator_est_lat;
            double tgt_lng = locator_est_lng;
            xSemaphoreGive(dataMutex);

            float dist = (float)haversine_m(my_lat, my_lng, tgt_lat, tgt_lng);
            float brng = bearing_to(my_lat, my_lng, tgt_lat, tgt_lng);

            xSemaphoreTake(dataMutex, portMAX_DELAY);
            locator_est_distance = dist;
            locator_bearing = brng;
            // Sample the distance into the trend history (every ~500ms)
            unsigned long now_t = millis();
            if (now_t - locator_last_trend_sample_ms > 500) {
                locator_dist_history[locator_dist_history_head] = dist;
                locator_dist_history_head = (locator_dist_history_head + 1) % LOC_TREND_SAMPLES;
                if (locator_dist_history_count < LOC_TREND_SAMPLES) locator_dist_history_count++;
                locator_last_trend_sample_ms = now_t;
            }
            xSemaphoreGive(dataMutex);
        } else {
            xSemaphoreGive(dataMutex);
        }
    }

    {
        bool was_dirty = false;
        if (current_screen == 2 && !hist_detail_open) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            if (sd_hist_dirty) {
                sd_hist_dirty = false;
                was_dirty = true;
            }
            xSemaphoreGive(dataMutex);
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

    if (locator_announce_pending) {
        locator_announce_pending = false;
        trigger_toast("INFO", "Estimate ready", 0);
        if (!is_muted && !stealth_mode) {
            xTaskCreate(LocatorChimeTask, "LocChime", 2048, NULL, 2, NULL);
        }
    }

    // Enter ambient mode after sustained idle
    if (!ambient_mode && !stealth_mode && !toast_active && !locator_active && !export_mode_active &&
        (millis() - last_user_input_ms) > AMBIENT_TIMEOUT_MS) {
        ambient_mode = true;
        M5Cardputer.Display.setBrightness(AMBIENT_BRIGHTNESS);
    }

    // Exit ambient if conditions change from non-input sources
    if (ambient_mode && (locator_active || export_mode_active || toast_active)) {
        ambient_mode = false;
        M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]);
    }

    if (ambient_mode) {
        static unsigned long last_ambient_draw = 0;
        static float amb_x = DISP_W / 2.0f, amb_y = DISP_H / 2.0f;
        static float amb_vx = 0.7f, amb_vy = 0.5f;
        unsigned long now_amb = millis();
        if (now_amb - last_ambient_draw > 33) {  // ~30fps
            long det_total;
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            det_total = session_flock_wifi + session_flock_ble + session_raven;
            xSemaphoreGive(dataMutex);

            // Bounce the text block (approx 48px wide, 26px tall)
            const int text_half_w = 24, text_half_h = 13;
            amb_x += amb_vx;
            amb_y += amb_vy;
            if (amb_x - text_half_w < 2)  { amb_x = 2 + text_half_w;  amb_vx = fabsf(amb_vx); }
            if (amb_x + text_half_w > DISP_W - 2) { amb_x = DISP_W - 2 - text_half_w; amb_vx = -fabsf(amb_vx); }
            if (amb_y - text_half_h < 2)  { amb_y = 2 + text_half_h;  amb_vy = fabsf(amb_vy); }
            if (amb_y + text_half_h > DISP_H - 2) { amb_y = DISP_H - 2 - text_half_h; amb_vy = -fabsf(amb_vy); }

            spr.fillSprite(BG_COLOR);
            char det_buf[16];
            snprintf(det_buf, sizeof(det_buf), "%ld", det_total);
            spr.setTextDatum(MC_DATUM);
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setTextSize(TS_H1);
            spr.drawString(det_buf, (int)amb_x, (int)amb_y - 6);
            spr.setTextSize(TS_MICRO);
            spr.drawString("DETECTIONS", (int)amb_x, (int)amb_y + 16);
            spr.setTextDatum(TL_DATUM);
            spr.pushSprite(0, 0);
            last_ambient_draw = now_amb;
        }
    } else if (stealth_mode) {
        // Minimal stealth render: tiny dim "S" in bottom-right corner every 2s.
        static unsigned long last_stealth_draw = 0;
        if (millis() - last_stealth_draw > 2000) {
            spr.fillSprite(BG_COLOR);
            uint16_t s_col = lerp_col16(BG_COLOR, lgfx::color565(180, 30, 30), 0.6f);
            spr.setTextColor(s_col, BG_COLOR);
            spr.setTextSize(TS_MICRO);
            spr.setCursor(DISP_W - 8, DISP_H - 9);
            spr.print("S");
            spr.pushSprite(0, 0);
            last_stealth_draw = millis();
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
            int hist_target_y = CONTENT_Y +
                (history_selected_idx - history_scroll_offset) * HIST_ROW_H;
            bool sel_settling = fabsf(hist_sel_y_f - (float)hist_target_y) > 0.5f;
            bool open_running = (hist_detail_open &&
                                 hist_detail_open_ms != 0 &&
                                 (now - hist_detail_open_ms) < UI_FADE_IN_MS + 30);
            hist_animating = sel_settling || open_running;
        }

        if (current_screen == 0 || current_screen == 1 || current_screen == 3 ||
            show_vol_overlay || toast_active || stats_scrolling || stats_rolling ||
            hist_animating ||
            (now - last_fast_anim < 30)) {
            // Stats screen caps at 30 fps to suppress the SPI/scan-line
            // tearing that 60 fps pushes produced. Other animated screens
            // stay at 60 fps where the artifact isn't visible.
            unsigned long min_frame_ms = (current_screen == 4) ? 33 : 15;
            if (now - last_fast_anim >= min_frame_ms) {
                draw_current_screen();
                // startWrite/endWrite holds FSPI for the entire push so
                // an SD transfer on the same bus can't interleave bytes
                // mid-frame and tear the image.
                M5Cardputer.Display.startWrite();
                spr.pushSprite(0, 0);
                M5Cardputer.Display.endWrite();
                last_fast_anim = now;
                screen_dirty = false;
            }
        }
        else {
            // Screen 4 has a live SESSION timer — force a redraw every 1s
            // so the seconds digit ticks. Cheaper than promoting screen 4
            // to the 60fps fast path while idle.
            if (current_screen == 4) {
                static unsigned long last_stats_tick = 0;
                if (now - last_stats_tick >= 1000) {
                    screen_dirty = true;
                    last_stats_tick = now;
                }
            }
            if (now - last_slow_ui >= 100) {
                if (screen_dirty || toast_active) {
                    draw_current_screen();
                    M5Cardputer.Display.startWrite();
                    spr.pushSprite(0, 0);
                    M5Cardputer.Display.endWrite();
                    screen_dirty = false;
                }
                last_slow_ui = now;
            }
        }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
}