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
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_task_wdt.h"
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

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void draw_header_spr(int screen_num);
void draw_toast_spr();
void draw_vol_overlay();
void drawCard(int x, int y, int w, int h);
void draw_current_screen();
void transition_screen(int new_screen, int dir);
void play_escalated_alarm(int confidence, int source);
void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b);
void beep(int frequency, int duration_ms);
void apply_color_palette();
void draw_help_overlay();
void draw_locator_help_overlay();
void draw_feed_expanded_overlay();
void draw_gps_screen();
void load_sd_history();
void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type);
static void set_toast_direct(const char* text, uint16_t accent);

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
static float help_ease = 0.0f;
static unsigned long help_ease_start = 0;

// Ambient mode: dim minimal UI after sustained idle
static unsigned long last_user_input_ms = 0;
static const unsigned long AMBIENT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
static const uint8_t AMBIENT_BRIGHTNESS = 8;
static bool ambient_mode = false;

unsigned long vol_overlay_start = 0;
bool show_vol_overlay = false;
bool show_locator_help = false;  // 'h' on locator screen
static bool show_feed_expanded = false;
bool north_mode = false;
int  brightness_level = 2;  // 0=dim, 1=mid, 2=full — cycled by 'b' key
static const int BRIGHTNESS_LEVELS[3] = {40, 120, 255};

static float ease_muted = 0.0f;

// RGB LED state — color cycles with C key, on/off with L when locator idle
static uint8_t led_r = 50, led_g = 255, led_b = 100; // default green (matches ACCENT_COLOR)
static bool    led_breathing_on = true;
static const uint8_t LED_COLORS[][3] = {
    { 50, 255, 100},  // green (matches ACCENT_COLOR — default)
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
        BG_COLOR      = lgfx::color565(  8,   0,   0);
        CARD_COLOR    = lgfx::color565( 25,   0,   0);
        CARD_BORDER   = lgfx::color565( 60,   5,   5);
        HEADER_COLOR  = lgfx::color565(255,  60,  60);
        TEXT_COLOR    = lgfx::color565(220, 180, 180);
        DIM_COLOR     = lgfx::color565(140,  30,  30);
        ACCENT_COLOR  = lgfx::color565(255, 100, 100);  // bright red — primary

        // Three-tier night palette: bright, warning, dim.
        // Categories that were distinct in day mode (Raven, BLE, GPS) collapse
        // toward ACCENT in night mode — geometry/position carries the meaning.
        CAUTION_COLOR = lgfx::color565(255, 140,  20);  // amber — warnings only
        TEAL_COLOR    = lgfx::color565(255, 100, 100);  // = ACCENT (Raven)
        PURPLE_COLOR  = lgfx::color565(200,  60,  80);  // dim red-pink (BLE)
        GPS_COLOR     = lgfx::color565(255, 100, 100);  // = ACCENT (GPS)
    } else {
        BG_COLOR      = lgfx::color565( 10,  20,  48);
        CARD_COLOR    = lgfx::color565( 18,  36,  80);
        CARD_BORDER   = lgfx::color565( 24,  46, 100);
        HEADER_COLOR  = lgfx::color565(  0, 215, 235);
        TEXT_COLOR    = lgfx::color565(220, 232, 255);
        DIM_COLOR     = lgfx::color565(100, 140, 180);
        ACCENT_COLOR  = lgfx::color565( 50, 255, 100);
        TEAL_COLOR    = lgfx::color565(  0, 215, 160);
        PURPLE_COLOR  = lgfx::color565(210, 110, 255);
        CAUTION_COLOR = lgfx::color565(255, 224,   0);
        GPS_COLOR     = lgfx::color565( 80, 200, 255);
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
#define GPS_RX_PIN   15  
#define GPS_TX_PIN   13   
#define GPS_BAUD     115200 

#define LOW_FREQ  200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define DETECT_FREQ_HIGH 1200
#define DETECT_FREQ_CERTAIN 1500

#define MAX_CHANNEL 13
#define BLE_SCAN_DURATION 2
#define BLE_SCAN_INTERVAL      3000   // normal inter-scan gap (ms)
#define BLE_SCAN_INTERVAL_LOCK  800   // boosted gap during 10s channel lock window
#define BUZZER_COOLDOWN 60000
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define IGNORE_WEAK_RSSI -80

#define MAX_LOG_BUFFER 10
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
#define LOC_PATH_LOSS_N 2.5
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
#define VERSION_STRING "FLOCK DETECTOR v9.3 [ADV]"

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

TaskHandle_t ScannerTaskHandle;
TaskHandle_t GPSTaskHandle; 
SemaphoreHandle_t dataMutex;

static uint8_t current_channel = 1;
static unsigned long last_channel_hop = 0;
static volatile unsigned long channel_lock_until = 0; 
static unsigned long last_ble_scan = 0;
static unsigned long last_buzzer_time = 0;
static NimBLEScan* pBLEScan;
static uint32_t ble_scan_cycle = 0;
static volatile uint32_t ambient_packet_count = 0;
#define BLE_MAX_CONCURRENT_TASKS 6
QueueHandle_t ble_event_queue;
bool sd_available = false;
bool littlefs_available = false;
volatile int trigger_alarm_confidence = 0;
volatile int trigger_alarm_source = 0;   // 0 = WiFi, 1 = BLE
volatile bool is_alarming = false;

#define SD_LINE_LEN 512
char sd_write_buffer[MAX_LOG_BUFFER][SD_LINE_LEN];
int  sd_write_count = 0;
unsigned long last_sd_flush = 0;
static int flash_write_fail_count = 0; 

String current_log_file = "/FlockLog.csv";
String current_pcap_file = "/Threats.pcap";
String current_ble_pcap_file = "/BLE_Threats.pcap";

// Export server state
static WebServer* export_server = nullptr;
static bool export_mode_active = false;
static unsigned long export_mode_started_at = 0;
static const unsigned long EXPORT_MODE_MAX_MS = 600000UL;  // 10 min auto-exit
static char export_ssid[33] = "";  // configured WiFi SSID (persisted)
static char export_pass[65] = "";  // configured WiFi password (persisted)
static char export_ip_str[20] = "0.0.0.0";

int current_screen = 0;
bool system_fully_booted = false;
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

#define MAX_SEEN_MACS 200

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

bool is_mac_recently_seen(const char* mac) {
    const char* key = mac;
    uint32_t idx = hash_mac(key) % SEEN_MAC_TABLE_SIZE;
    unsigned long now = millis();
    for (int probe = 0; probe < SEEN_MAC_TABLE_SIZE; probe++) {
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
    for (int probe = 0; probe < SEEN_MAC_TABLE_SIZE; probe++) {
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
};
CaptureEntry capture_history[CAPTURE_HISTORY_SIZE];
int capture_history_count = 0;

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
};
SDHistEntry sd_hist[SD_HIST_SIZE];
int  sd_hist_count      = 0;
int  history_selected_idx = 0;
bool hist_detail_open   = false;
static int device_info_scroll = 0;
static const int DEVICE_INFO_CONTENT_HEIGHT = 302;
volatile bool sd_hist_dirty = false;

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
LocSample locator_samples[LOC_MAX_SAMPLES];
int locator_sample_count = 0;
unsigned long locator_last_sample = 0;
unsigned long locator_start_time = 0;
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

void speaker_off() { 
    M5Cardputer.Speaker.stop(); 
}

void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(21, r, g, b);
}

// ============================================================================
// CHARGE LED TASK — persistent FreeRTOS task (never created/deleted after boot).
// Spawned once in setup(), lives on Core 0 at priority 5 forever.
// led_breathing_on flag controls output; toggling it is the only lifecycle control.
// Persistent task (not dynamic create/delete) eliminates the FreeRTOS scheduling
// jitter that caused WS2812 corruption — the task never stops, it just sets dark.
// Reference: BruceDevices/firmware src/core/led_control.cpp (ledEffectTask)
// ============================================================================
static TaskHandle_t chargeLedTaskHandle = NULL;

void chargeLedTask(void* pvParameters) {
    for (;;) {
        // Snapshot detection-flash state under mutex
        bool   active_snap;
        unsigned long until_snap;
        uint8_t r_snap, g_snap, b_snap;
        bool   expired_clear = false;

        xSemaphoreTake(dataMutex, portMAX_DELAY);
        active_snap = led_detect_active;
        until_snap  = led_detection_flash_until;
        r_snap      = led_detect_r;
        g_snap      = led_detect_g;
        b_snap      = led_detect_b;
        if (active_snap && millis() >= until_snap) {
            led_detect_active = false;
            expired_clear = true;
        }
        xSemaphoreGive(dataMutex);

        if (active_snap && !expired_clear) {
            set_cardputer_led(r_snap, g_snap, b_snap);
        } else if (expired_clear) {
            set_cardputer_led(0, 0, 0);
        } else if (led_breathing_on) {
            float phase = sinf((millis() / 1000.0f) * (float)M_PI);
            uint8_t val = (uint8_t)((phase + 1.0f) * 100.0f); // 0..200
            set_cardputer_led((led_r * val) / 200, (led_g * val) / 200, (led_b * val) / 200);
        } else {
            set_cardputer_led(0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
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
    "08:3a:88", "d8:f3:bc", "cc:cc:cc"
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

bool check_manufacturer_id(const std::string& mfg_data) {
    if (mfg_data.length() >= 2) {
        uint16_t mfg_id = (uint8_t)mfg_data[0] | ((uint8_t)mfg_data[1] << 8);
        if (mfg_id == FLOCK_MFG_COMPANY_ID) return true;
    }
    return false;
}

bool has_tn_serial(const std::string& mfg_data) {
    if (mfg_data.length() < 4) return false;
    for (size_t i = 2; i < mfg_data.length() - 1; i++) {
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
        {"raven_multi",  "3+ Raven UUIDs"},
        {"raven_custom", "Raven custom UUID"},
        {"raven_uuid",   "Raven UUID"},
        {"mfg_0x09C8",   "Flock mfg ID"},
        {"tn_serial",    "TN serial"},
        {"ssid_fmt",     "Flock SSID format"},
        {"wpa2_psk",     "WPA2-PSK"},
        {"penguin_num",  "Penguin name"},
        {"name",         "Known name"},
        {"mac_t1",       "Known MAC"},
        {"mac_t2",       "Similar MAC"},
        {"ssid",         "SSID pattern"},
        {"static_addr",  "Static addr"},
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

uint16_t confidence_color(int score) {
    if (score >= CONFIDENCE_CERTAIN) return ACCENT_COLOR;
    if (score >= CONFIDENCE_HIGH)    return CAUTION_COLOR;
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return CAUTION_COLOR;
    return TEXT_COLOR;
}

void write_threat_pcap(const uint8_t* payload, uint32_t length) {
    if (!sd_available) return;
    uint32_t capture_len = (length > 256) ? 256 : length;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (pcap_write_count < MAX_PCAP_BUFFER) {
        unsigned long now_ms = millis();
        const uint32_t PCAP_EPOCH_BASE = 1700000000UL;
        uint32_t elapsed_sec  = (uint32_t)(now_ms / 1000UL);
        uint32_t elapsed_usec = (uint32_t)((now_ms % 1000UL) * 1000UL);
        pcap_write_buffer[pcap_write_count].ts_sec  = PCAP_EPOCH_BASE + elapsed_sec;
        pcap_write_buffer[pcap_write_count].ts_usec = elapsed_usec;
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
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (ble_pcap_write_count < MAX_PCAP_BUFFER) {
        unsigned long now_ms = millis();
        const uint32_t PCAP_EPOCH_BASE = 1700000000UL;
        uint32_t elapsed_sec  = (uint32_t)(now_ms / 1000UL);
        uint32_t elapsed_usec = (uint32_t)((now_ms % 1000UL) * 1000UL);
        ble_pcap_write_buffer[ble_pcap_write_count].ts_sec  = PCAP_EPOCH_BASE + elapsed_sec;
        ble_pcap_write_buffer[ble_pcap_write_count].ts_usec = elapsed_usec;
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
        File f = SD.open(path, FILE_READ);
        if (!f) { export_server->send(404, "text/plain", "Not found"); return; }
        export_server->sendHeader("Content-Disposition",
                                  String("attachment; filename=\"") + (path + 1) + "\"");
        export_server->streamFile(f, mime);
        f.close();
    };

    export_server->on("/FlockLog.csv", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/FlockLog.csv", "text/csv");
    });
    export_server->on("/Threats.pcap", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/Threats.pcap", "application/vnd.tcpdump.pcap");
    });
    export_server->on("/BLE_Threats.pcap", HTTP_GET, [serve_sd_file]() {
        serve_sd_file("/BLE_Threats.pcap", "application/vnd.tcpdump.pcap");
    });

    export_server->onNotFound([]() {
        export_server->send(404, "text/plain", "Not found");
    });
}

bool export_mode_start() {
    if (export_mode_active) return true;
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

    unsigned long connect_start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - connect_start) < 15000) {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        set_toast_direct("WIFI CONNECT FAIL", CAUTION_COLOR);
        // Restore sniffing
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_STA);
        wifi_promiscuous_filter_t pf_restore; pf_restore.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&pf_restore);
        esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
        return false;
    }

    IPAddress ip = WiFi.localIP();
    snprintf(export_ip_str, sizeof(export_ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    export_server = new WebServer(80);
    if (!export_server) {
        set_toast_direct("EXPORT ALLOC FAIL", CAUTION_COLOR);
        // Restore promiscuous sniffing — same restoration the connect-fail path does.
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_STA);
        wifi_promiscuous_filter_t pf_restore;
        pf_restore.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&pf_restore);
        esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
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

void export_mode_stop() {
    if (!export_mode_active) return;
    if (export_server) {
        export_server->stop();
        delete export_server;
        export_server = nullptr;
    }
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    wifi_promiscuous_filter_t pf; pf.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&pf);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
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
    File f = SD.open(current_log_file.c_str(), FILE_READ);
    if (!f) return;

    // Ring buffer: keep last SD_HIST_SIZE parsed entries
    SDHistEntry ring[SD_HIST_SIZE];
    int ri = 0, total = 0;
    char buf[SD_LINE_LEN];

    while (f.available()) {
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

void save_session_to_flash() {
    if (!littlefs_available) return;
    
    long l_wifi, l_ble, l_sec, l_flock, l_boots, l_writes;
    int l_vol;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    l_wifi = lifetime_wifi; l_ble = lifetime_ble; l_sec = lifetime_seconds;
    l_flock = lifetime_flock_total; l_vol = current_volume; l_boots = lifetime_boots;
    l_writes = lifetime_flash_writes + 1;
    xSemaphoreGive(dataMutex);

    bool write_ok = false;
    for (int attempt = 0; attempt < 3 && !write_ok; attempt++) {
        File f = LittleFS.open(PERSIST_FILE, "w");
        if (!f) { delay(5); continue; }
        size_t written = f.printf("%ld\n%ld\n%lu\n%ld\n%d\n%ld\n%ld\n%s\n%s\n",
                                  l_wifi, l_ble, l_sec, l_flock, l_vol, l_boots, l_writes,
                                  export_ssid, export_pass);
        f.close();
        if (written > 10) write_ok = true;
    }

    if (write_ok) {
        flash_write_fail_count = 0;
        last_persist_save = millis();
        save_detections_to_flash();
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        lifetime_flash_writes = l_writes;
        xSemaphoreGive(dataMutex);
        if (l_writes == 80000) {
            set_toast_direct("FLASH WEAR HIGH", CAUTION_COLOR);
        }
    } else {
        flash_write_fail_count++;
        if (flash_write_fail_count >= FLASH_FAIL_TOAST_THRESHOLD) {
            set_toast_direct("FLASH WRITE FAIL", CAUTION_COLOR);
            last_persist_save = millis();
        }
    }
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
    f.close();
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
        }
    }
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// THREAT EQUALIZER DATA
// ============================================================================
#define radar_cx 58
#define radar_cy 70
#define radar_r  46
#define inner_r  18

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
    if (now - last_feed_push_ms < FEED_MIN_PUSH_INTERVAL_MS) return;
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

    if (log_count > 0) {
        File file = SD.open(current_log_file.c_str(), FILE_APPEND);
        if (file) {
            for (int i = 0; i < log_count; i++) file.println(local_log_buf[i]);
            file.close();
        }
    }

    if (pcap_count > 0) {
        File pfile = SD.open(current_pcap_file.c_str(), FILE_APPEND);
        if (pfile) {
            for (int i = 0; i < pcap_count; i++) {
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
        File bfile = SD.open(current_ble_pcap_file.c_str(), FILE_APPEND);
        if (bfile) {
            for (int i = 0; i < ble_pcap_count; i++) {
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

    if (log_count > 0 || pcap_count > 0 || ble_pcap_count > 0) {
        last_sd_flush = millis();
    }
}

// Thread-safe direct toast setter. Used for single-message notifications
// (battery warnings, flash errors, export-mode messages) that bypass the queue.
// Does NOT touch the queue — it only sets the currently-displayed toast.
static void set_toast_direct(const char* text, uint16_t accent) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    strncpy(toast_text, text, sizeof(toast_text) - 1);
    toast_text[sizeof(toast_text) - 1] = '\0';
    toast_accent_color = accent;
    toast_is_action = true;
    toast_start = millis();
    toast_active = true;
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
            session_wifi++; lifetime_wifi++; session_flock_wifi++; blip_col = CAUTION_COLOR;
        } else {
            session_ble++; lifetime_ble++; blip_col = PURPLE_COLOR;
        }
        if (strstr(type, "RAVEN") != NULL) { session_raven++; blip_col = TEAL_COLOR; }
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
        sd_hist_dirty = true;
    }

    strncpy(last_cap_type,       type,             sizeof(last_cap_type) - 1);
    last_cap_type[sizeof(last_cap_type) - 1] = '\0';
    strncpy(last_cap_mac,        mac,              sizeof(last_cap_mac) - 1);
    last_cap_mac[sizeof(last_cap_mac) - 1] = '\0';
    strncpy(last_cap_name,       name,             sizeof(last_cap_name) - 1);
    last_cap_name[sizeof(last_cap_name) - 1] = '\0';
    strncpy(last_cap_time,       current_time,     sizeof(last_cap_time) - 1);
    last_cap_time[sizeof(last_cap_time) - 1] = '\0';
    strncpy(last_cap_det_method, detection_method, sizeof(last_cap_det_method) - 1);
    last_cap_det_method[sizeof(last_cap_det_method) - 1] = '\0';
    last_cap_rssi       = rssi;
    last_cap_confidence = confidence;
    last_cap_seq_num    = seq_num;
    xSemaphoreGive(dataMutex);

    if (is_new) {
        trigger_toast(type, name, confidence);
        add_blip(blip_col, rssi);
    }

    // Heavy work outside mutex
    if (is_new && sd_available) {
        char clean_name[64];
        strncpy(clean_name, name, sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0';
        for (char* p = clean_name; *p; p++) if (*p == ',') *p = ' ';

        char clean_extra[64];
        strncpy(clean_extra, extra_data, sizeof(clean_extra) - 1);
        clean_extra[sizeof(clean_extra) - 1] = '\0';
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

void locator_start(const char* mac, const char* name, const char* type = "") {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = true;
    strncpy(locator_target_mac,  mac,  sizeof(locator_target_mac)  - 1); locator_target_mac[17]  = '\0';
    strncpy(locator_target_name, name, sizeof(locator_target_name) - 1); locator_target_name[64] = '\0';
    strncpy(locator_target_type, type, sizeof(locator_target_type) - 1); locator_target_type[7]  = '\0';
    locator_sample_count = 0; locator_has_estimate = false; locator_peak_rssi = -120;
    locator_estimate_announced = false;
    locator_est_distance = 0; locator_bearing = 0; locator_confidence_radius = 0;
    locator_start_time = millis(); locator_newest_sample_ms = 0;
    locator_dist_history_count = 0;
    locator_dist_history_head = 0;
    locator_last_trend_sample_ms = 0;
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
    uint8_t  mac[6];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t seq_num;
    char     ssid[33];
    bool     is_beacon;
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

    uint8_t next = (wifi_eq_write_idx + 1) % WIFI_EVENT_QUEUE_SIZE;
    if (wifi_event_queue[next].ready) return;

    WifiEvent* ev = &wifi_event_queue[wifi_eq_write_idx];

    memset(ev->ssid, 0, sizeof(ev->ssid));
    uint8_t* frame_body    = (uint8_t*)ipkt + 24;
    uint8_t* tagged_params;
    int      remaining;
    if (is_beacon) {
        if (ppkt->rx_ctrl.sig_len < 24 + 12 + 2) return;
        tagged_params = frame_body + 12;
        remaining     = (int)ppkt->rx_ctrl.sig_len - 24 - 12 - 4;
    } else {
        tagged_params = frame_body;
        remaining     = (int)ppkt->rx_ctrl.sig_len - 24 - 4;
    }
    if (remaining > 2 && tagged_params[0] == 0
        && tagged_params[1] <= 32 && tagged_params[1] <= remaining - 2) {
        memcpy(ev->ssid, &tagged_params[2], tagged_params[1]);
        ev->ssid[tagged_params[1]] = '\0';
    }

    memcpy(ev->mac, hdr->addr2, 6);
    ev->rssi             = (int8_t)ppkt->rx_ctrl.rssi;
    ev->channel          = (uint8_t)ppkt->rx_ctrl.channel;
    ev->seq_num          = (uint16_t)(hdr->sequence_ctrl >> 4);
    ev->is_beacon        = is_beacon;
    ev->orig_len         = (uint16_t)ppkt->rx_ctrl.sig_len;
    ev->payload_snap_len = (ppkt->rx_ctrl.sig_len < 256) ? (uint16_t)ppkt->rx_ctrl.sig_len : 256;
    memcpy(ev->payload_snap, ppkt->payload, ev->payload_snap_len);

    ev->is_wpa2_psk = false;
    ev->vendor_oui_count = 0;
    if (is_beacon) {
        uint8_t* p = tagged_params;
        int rem = remaining;
        while (rem >= 2) {
            uint8_t tag_id = p[0];
            uint8_t tag_len = p[1];
            if (tag_len > rem - 2) break;
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

    __sync_synchronize();   // release: publish all field writes before `ready`
    ev->ready = true;
    wifi_eq_write_idx = next;
}

void process_wifi_event_queue() {
    while (wifi_event_queue[wifi_eq_read_idx].ready) {
        WifiEvent* ev = &wifi_event_queue[wifi_eq_read_idx];
        __sync_synchronize();   // acquire: pair with producer's release below

        WifiEvent local;
        memcpy(&local, ev, sizeof(WifiEvent));
        __sync_synchronize();   // release: ensure memcpy of *ev completes before producer can reuse slot
        ev->ready = false;
        wifi_eq_read_idx = (wifi_eq_read_idx + 1) % WIFI_EVENT_QUEUE_SIZE;

        if (local.rssi < IGNORE_WEAK_RSSI) continue;

        clean_device_name_char(local.ssid);

        // Push to live feed (every observed device, preview flock status)
        {
            char mac_str_feed[18];
            snprintf(mac_str_feed, sizeof(mac_str_feed),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     local.mac[0], local.mac[1], local.mac[2],
                     local.mac[3], local.mac[4], local.mac[5]);
            const char* feed_name = (strlen(local.ssid) > 0) ? local.ssid : "Hidden";
            int  preview_mac_score = check_mac_prefix_tiered(local.mac);
            bool preview_is_flock  = (strlen(local.ssid) > 0
                                      && (is_flock_ssid_format(local.ssid)
                                          || check_ssid_pattern(local.ssid)))
                                     || preview_mac_score == 1;
            feed_push_candidate(mac_str_feed, feed_name, local.rssi, 0, preview_is_flock);
        }

        int  confidence    = 0;
        char methods[64]   = {0};
        int  mac_score     = check_mac_prefix_tiered(local.mac);
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
};

static void ble_worker_task(void* pvParameters) {
    BleEventData* ev;
    for (;;) {
        if (xQueueReceive(ble_event_queue, &ev, portMAX_DELAY) != pdTRUE) continue;

        if (ev->rssi < IGNORE_WEAK_RSSI) { free(ev); continue; }

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
            bool preview_is_flock = (check_mac_prefix_tiered(ev->mac) == 1
                                     || check_device_name_pattern(dev_name_char)
                                     || is_penguin_numeric_name(dev_name_char));
            feed_push_candidate(mac_str_feed,
                                ev->have_name ? dev_name_char : "",
                                ev->rssi, 1, preview_is_flock);
        }

        std::string mfg_std((const char*)ev->mfg_data, ev->mfg_data_len);
        if (ev->have_mfg) {
            if (check_manufacturer_id(mfg_std)) {
                strlcat(methods, "mfg_0x09C8 ", sizeof(methods)); got_mfg = true;
                if (has_tn_serial(mfg_std)) strlcat(methods, "tn_serial ", sizeof(methods));
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

        free(ev);
    }
}

class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        BleEventData* ev = (BleEventData*)malloc(sizeof(BleEventData));
        if (!ev) return;

        memset(ev, 0, sizeof(BleEventData));

        NimBLEAddress addr = advertisedDevice->getAddress();
        unsigned int mac_tmp[6] = {0};
        sscanf(addr.toString().c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
            &mac_tmp[0], &mac_tmp[1], &mac_tmp[2],
            &mac_tmp[3], &mac_tmp[4], &mac_tmp[5]);
        for (int i = 0; i < 6; i++) ev->mac[i] = (uint8_t)mac_tmp[i];
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

        if (xQueueSend(ble_event_queue, &ev, 0) != pdTRUE) { free(ev); }
    }
};

// ============================================================================
// DEDICATED TASKS (DUAL CORE)
// ============================================================================
void ScannerLoopTask(void* pvParameters) {
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        unsigned long now = millis();
        if ((long)(now - channel_lock_until) > 0) {
            if (now - last_channel_hop > CHANNEL_DWELL_MS) {
                current_channel++; 
                if (current_channel > MAX_CHANNEL) current_channel = 1;
                esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); 
                last_channel_hop = now;
            }
        }

        unsigned long ble_interval = ((long)(now - channel_lock_until) < 0)
            ? BLE_SCAN_INTERVAL_LOCK
            : BLE_SCAN_INTERVAL;

        if (millis() - last_ble_scan >= ble_interval) {
            if (!pBLEScan->isScanning()) {
                bool active = (ble_scan_cycle % 3 == 0);
                pBLEScan->setActiveScan(active);
                pBLEScan->start(BLE_SCAN_DURATION, false);
                last_ble_scan = millis();
                ble_scan_cycle++;
            }
        }
        if (!pBLEScan->isScanning() && (millis() - last_ble_scan > (unsigned long)(BLE_SCAN_DURATION * 1000 + 500))) {
            pBLEScan->clearResults();
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void GPSLoopTask(void* pvParameters) {
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
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
    vTaskDelete(NULL);
}

void LocatorChimeTask(void* pvParameters) {
    if (!stealth_mode && !is_muted) {
        M5Cardputer.Speaker.tone(660, 70);
        vTaskDelay(80 / portTICK_PERIOD_MS);
        M5Cardputer.Speaker.tone(880, 90);
    }
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
    spr.setTextColor(HEADER_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, 5); kprint(spr, screen_names[screen_num]);

    // ── Status icon row ──────────────────────────────────────────────────────
    const uint16_t ICON_COL = lgfx::color565(255, 255, 255);

    bool gps_lock_now;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    gps_lock_now = gps.satellites.isValid() && gps.satellites.value() >= 1;
    xSemaphoreGive(dataMutex);
    bool muted_now = is_muted;

    ease_muted += ((muted_now ? 1.0f : 0.0f) - ease_muted) * 0.12f;

    // Alarm cooldown state: fires during the 60s window after a detection alarm
    bool cooldown_now;
    unsigned long cooldown_elapsed = 0;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    cooldown_elapsed = millis() - last_buzzer_time;
    cooldown_now = (last_buzzer_time > 0) && (cooldown_elapsed < BUZZER_COOLDOWN);
    xSemaphoreGive(dataMutex);

    static float ease_cooldown = 0.0f;
    ease_cooldown += ((cooldown_now ? 1.0f : 0.0f) - ease_cooldown) * 0.08f;

    static float ease_gps_missing = 0.0f;
    ease_gps_missing += ((!gps_lock_now ? 1.0f : 0.0f) - ease_gps_missing) * 0.08f;

    int icon_right = DISP_W - 6;
    int icon_y = 4;

    auto lerp_icon = [&](float t) -> uint16_t {
        return lerp_col16(BG_COLOR, ICON_COL, t);
    };
    auto should_draw = [](float t) -> bool { return t > 0.05f; };

    // Unified mode indicators — N / S / L text pills
    {
        struct ModeBadge {
            bool active;
            const char* letter;
            uint16_t color;
            bool pulse;
        };
        ModeBadge badges[3] = {
            { night_mode,     "N", HEADER_COLOR, false },
            { stealth_mode,   "S", DIM_COLOR,    false },
            { locator_active, "L", HEADER_COLOR, true  },
        };

        for (int i = 0; i < 3; i++) {
            if (!badges[i].active) continue;

            uint16_t fg = badges[i].color;
            if (badges[i].pulse) {
                float pulse = (sinf((float)millis() / 600.0f) + 1.0f) * 0.5f;
                fg = lerp_col16(badges[i].color, ACCENT_COLOR, pulse * 0.5f);
            }
            uint16_t bg = lerp_col16(BG_COLOR, fg, 0.18f);

            int pill_x = icon_right - 12;
            spr.fillRoundRect(pill_x, icon_y, 11, 11, 2, bg);
            spr.setTextColor(fg, bg);
            spr.setTextSize(1);
            spr.setCursor(pill_x + 3, icon_y + 2);
            spr.print(badges[i].letter);
            icon_right = pill_x - 3;
        }
    }

    // Muted icon — larger 10×10 speaker with slash
    if (should_draw(ease_muted)) {
        int ix = icon_right - 12;
        uint16_t c = lerp_icon(ease_muted);
        // Speaker body (rectangle + triangle cone)
        spr.fillRect(ix + 1, icon_y + 4, 3, 4, c);
        spr.fillTriangle(ix + 4, icon_y + 2, ix + 8, icon_y, ix + 8, icon_y + 10, c);
        // Slash through it
        spr.drawLine(ix, icon_y, ix + 10, icon_y + 10, c);
        spr.drawLine(ix + 1, icon_y, ix + 10, icon_y + 9, c);
        icon_right -= 16;
    }

    // SD missing indicator (always occupies the slot; does not consume ease)
    if (system_fully_booted && !sd_available) {
        int ix = icon_right - 8;
        uint16_t c = lgfx::color565(180, 40, 40);
        spr.drawRect(ix + 1, icon_y + 1, 8, 9, c);
        spr.drawLine(ix + 1, icon_y + 1, ix + 8, icon_y + 9, c);
        icon_right -= 14;
    }

    // GPS-missing amber pin (ease_gps_missing fades in when lock is lost)
    if (should_draw(ease_gps_missing)) {
        int ix = icon_right - 8;
        uint16_t c = lerp_col16(BG_COLOR, CAUTION_COLOR, ease_gps_missing);
        spr.fillCircle(ix + 4, icon_y + 3, 3, c);
        spr.fillTriangle(ix + 2, icon_y + 5, ix + 6, icon_y + 5, ix + 4, icon_y + 9, c);
        spr.fillCircle(ix + 4, icon_y + 3, 1, BG_COLOR);
        icon_right -= 16;
    }

    // Detection counter pill
    {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        uint32_t det_total = lifetime_flock_total;
        xSemaphoreGive(dataMutex);

        char det_str[8];
        snprintf(det_str, sizeof(det_str), "%lu", det_total);
        int det_w = (int)strlen(det_str) * 6 + 6;
        uint16_t pill_bg = lerp_col16(BG_COLOR, ACCENT_COLOR, 0.18f);
        spr.fillRoundRect(icon_right - det_w, icon_y, det_w, 11, 3, pill_bg);
        spr.setTextColor(lgfx::color565(255, 255, 255), pill_bg);
        spr.setTextSize(1);
        spr.setCursor(icon_right - det_w + 3, icon_y + 2);
        spr.print(det_str);
        icon_right -= det_w + 2;
    }

    // Alert slot: EXP takes priority over cooldown
    if (export_mode_active) {
        uint16_t c = lgfx::color565(255, 140, 0);
        spr.fillRoundRect(icon_right - 22, icon_y, 22, 11, 3, c);
        spr.setTextColor(lgfx::color565(0, 0, 0), c);
        spr.setTextSize(1);
        spr.setCursor(icon_right - 19, icon_y + 2);
        spr.print("EXP");
        icon_right -= 26;
    } else if (should_draw(ease_cooldown) && cooldown_now) {
        int ix = icon_right - 8;
        float progress = (float)cooldown_elapsed / (float)BUZZER_COOLDOWN;
        float pulse_rate = 400.0f + progress * 1200.0f;
        float pulse = (sinf((float)millis() / pulse_rate) + 1.0f) * 0.5f;
        uint8_t intensity = (uint8_t)(120 + pulse * 80);
        uint16_t c = lerp_col16(BG_COLOR, lgfx::color565(intensity, intensity, intensity), ease_cooldown);
        spr.drawCircle(ix + 4, icon_y + 5, 4, c);
        spr.drawCircle(ix + 4, icon_y + 5, 5, lerp_col16(BG_COLOR, c, 0.4f));
        spr.drawLine(ix + 1, icon_y + 2, ix + 7, icon_y + 8, c);
        icon_right -= 14;
    }

    // Rotation position dots — tight spacing, active is filled+larger, inactive is 2px line
    int title_len = (int)strlen(screen_names[screen_num]);
    int title_w = title_len * 7;
    int dots_x = 4 + title_w + 8;
    int dots_y = 9;
    for (int d = 0; d < NUM_SCREENS; d++) {
        int dx = dots_x + d * 5;
        if (d == screen_num) {
            // Active: small filled square (2x2)
            spr.fillRect(dx - 1, dots_y - 1, 3, 3, HEADER_COLOR);
        } else {
            // Inactive: single pixel dot
            spr.drawPixel(dx, dots_y, CARD_BORDER);
        }
    }

}

void draw_toast_spr() {
    if (!toast_active) return;
    unsigned long elapsed = millis() - toast_start;
    if (elapsed > TOAST_DURATION_MS) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        // Advance queue
        if (toast_queue_count > 0) {
            toast_queue_head = (toast_queue_head + 1) % TOAST_QUEUE_SIZE;
            toast_queue_count--;
        }
        if (toast_queue_count > 0) {
            strncpy(toast_text, toast_queue[toast_queue_head].text, sizeof(toast_text) - 1);
            toast_text[sizeof(toast_text) - 1] = '\0';
            toast_accent_color = toast_queue[toast_queue_head].accent;
            toast_is_action = toast_queue[toast_queue_head].is_action;
            toast_start = millis();
            toast_active = true;
        } else {
            toast_active = false;
        }
        xSemaphoreGive(dataMutex);
        return;
    }

    int y_pos = DISP_H - 34;
    if (elapsed < 150) y_pos = DISP_H - 10 - (int)((elapsed / 150.0f) * 24);
    else if (elapsed > TOAST_DURATION_MS - 150) y_pos = DISP_H - 34 + (int)(((elapsed - (TOAST_DURATION_MS - 150)) / 150.0f) * 24);

    uint16_t accent = toast_accent_color ? toast_accent_color : CAUTION_COLOR;
    int t_w = 210; int t_x = (DISP_W - t_w) / 2;

    spr.fillRect(t_x, y_pos, t_w, 26, CARD_COLOR);
    spr.drawRect(t_x, y_pos, t_w, 26, CARD_BORDER);

    spr.setTextColor(accent, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(t_x + 6, y_pos + 9); spr.print(toast_is_action ? "[i]" : "[!]");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR);
    spr.setCursor(t_x + 26, y_pos + 9); spr.print(toast_text);
    if (toast_queue_count > 1) {
        char qnum[6];
        snprintf(qnum, sizeof(qnum), "+%d", toast_queue_count - 1);
        spr.setTextColor(DIM_COLOR, CARD_COLOR);
        spr.setCursor(t_x + t_w - 22, y_pos + 9);
        spr.print(qnum);
    }
}

void draw_vol_overlay() {
    if (!show_vol_overlay) return;
    unsigned long elapsed = millis() - vol_overlay_start;
    const unsigned long SHOW_MS = 2200;
    if (elapsed > SHOW_MS) { show_vol_overlay = false; return; }

    // Quick fade-in (100ms), quadratic ease-out fade — no position change
    float alpha;
    const unsigned long FADE_IN_MS = 100;
    if (elapsed < FADE_IN_MS) {
        alpha = (float)elapsed / (float)FADE_IN_MS;
    } else {
        float t = (float)(elapsed - FADE_IN_MS) / (float)(SHOW_MS - FADE_IN_MS);
        alpha = 1.0f - (t * t);
    }

    const int t_w = 92;
    const int t_h = 22;
    const int t_x = DISP_W - t_w - 3;
    const int t_y = DISP_H - t_h - 3;

    auto blend16 = [](uint16_t bg, uint16_t fg, float a) -> uint16_t {
        int br = ((bg >> 11) & 0x1F) << 3, bg_g = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
        int fr = ((fg >> 11) & 0x1F) << 3, fg_g = ((fg >> 5) & 0x3F) << 2, fb = (fg & 0x1F) << 3;
        return lgfx::color565((uint8_t)(br+(fr-br)*a),(uint8_t)(bg_g+(fg_g-bg_g)*a),(uint8_t)(bb+(fb-bb)*a));
    };

    uint16_t bg_c  = blend16(BG_COLOR, CARD_COLOR,   alpha);
    uint16_t brd_c = blend16(BG_COLOR, CARD_BORDER,  alpha);
    uint16_t hdr_c = blend16(BG_COLOR, HEADER_COLOR, alpha);
    uint16_t txt_c = blend16(BG_COLOR, TEXT_COLOR,    alpha);

    spr.fillRect(t_x, t_y, t_w, t_h, bg_c);
    spr.drawRect(t_x, t_y, t_w, t_h, brd_c);

    int vol_pct = map(current_volume, 0, 255, 0, 100);
    char vol_str[5]; snprintf(vol_str, sizeof(vol_str), "%d%%", vol_pct);

    spr.setTextColor(hdr_c, bg_c); spr.setTextSize(1);
    spr.setCursor(t_x + 5, t_y + 7); spr.print("VOL");
    spr.setTextColor(txt_c, bg_c);
    spr.setCursor(t_x + 28, t_y + 7); spr.print(vol_str);

    int bar_x = t_x + 50; int bar_y = t_y + 6;
    int bar_w = t_w - 54;  int bar_h = 10;
    spr.drawRect(bar_x, bar_y, bar_w, bar_h, brd_c);
    int fill = (current_volume * (bar_w - 2)) / 255;
    if (fill > 0) {
        spr.fillRect(bar_x + 1, bar_y + 1, fill, bar_h - 2, blend16(BG_COLOR, HEADER_COLOR, alpha));
    }
}

void drawCard(int x, int y, int w, int h) {
    spr.fillRect(x, y, w, h, CARD_COLOR); spr.drawRect(x, y, w, h, CARD_BORDER);
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
    unsigned long elapsed = millis() - help_ease_start;
    float target = 1.0f;
    if (elapsed < 80) {
        float t = (float)elapsed / 80.0f;
        target = 1.0f - (1.0f - t) * (1.0f - t);
    }
    help_ease += (target - help_ease) * 0.5f;
    if (help_ease > 1.0f) help_ease = 1.0f;
    if (help_ease < 0.02f) return;

    for (int dy = 18; dy < DISP_H; dy += 2) {
        uint16_t bg = lerp_col16(BG_COLOR, lgfx::color565(0, 0, 0), help_ease * 0.5f);
        spr.drawFastHLine(0, dy, DISP_W, bg);
    }

    int full_w = DISP_W - 20;
    int full_h = DISP_H - 28;
    int panel_w = (int)(full_w * help_ease);
    int panel_h = (int)(full_h * help_ease);
    int panel_x = (DISP_W - panel_w) / 2;
    int panel_y = 18 + (full_h - panel_h) / 2;

    uint16_t panel_bg = lerp_col16(BG_COLOR, CARD_COLOR, help_ease);
    uint16_t panel_border = lerp_col16(BG_COLOR, HEADER_COLOR, help_ease);
    spr.fillRoundRect(panel_x, panel_y, panel_w, panel_h, 6, panel_bg);
    spr.drawRoundRect(panel_x, panel_y, panel_w, panel_h, 6, panel_border);

    if (help_ease < 0.3f) return;

    struct HelpKey { const char* key; const char* desc; };
    const HelpKey* keys;
    int key_count;
    const char* title;

    static const HelpKey global_keys[] = {
        {"</>",  "Prev/Next"},
        {"1-5",  "Jump screen"},
        {"ESC",  "Scanner"},
        {"TAB",  "Close help"},
        {"m/q",  "Mute"},
        {";/.",  "Vol/scroll"},
        {"n",    "Night"},
        {"s",    "Stealth"},
        {"b",    "Brightness"},
        {"\\",   "LED"},
        {"e",    "Export"},
        {"f",    "Feed view"},
        {"o",    "Reset sess"},
    };

    static const HelpKey scanner_keys[] = {
        {"x", "Simulate"},
        {"t", "Locate"},
    };
    static const HelpKey locator_keys[] = {
        {"l", "Start/stop"},
        {"t", "Cycle target"},
        {"g", "N-mode"},
    };
    static const HelpKey detections_keys[] = {
        {";/.", "Navigate"},
        {"ENT", "Detail"},
        {"DEL", "Close"},
    };
    static const HelpKey gps_keys[] = {
        {"(info)", ""},
    };
    static const HelpKey devinfo_keys[] = {
        {";/.", "Scroll"},
    };

    switch (current_screen) {
        case 0: keys = scanner_keys;    key_count = sizeof(scanner_keys) / sizeof(scanner_keys[0]);       title = "SCANNER"; break;
        case 1: keys = locator_keys;    key_count = sizeof(locator_keys) / sizeof(locator_keys[0]);       title = "LOCATOR"; break;
        case 2: keys = detections_keys; key_count = sizeof(detections_keys) / sizeof(detections_keys[0]); title = "DETECT"; break;
        case 3: keys = gps_keys;        key_count = sizeof(gps_keys) / sizeof(gps_keys[0]);               title = "GPS"; break;
        case 4: keys = devinfo_keys;    key_count = sizeof(devinfo_keys) / sizeof(devinfo_keys[0]);       title = "STATS"; break;
        default: keys = scanner_keys; key_count = 0; title = "HELP"; break;
    }

    spr.setTextSize(1);
    spr.setTextColor(HEADER_COLOR, panel_bg);
    spr.setCursor(panel_x + 6, panel_y + 5);
    kprint(spr, title);
    spr.drawFastHLine(panel_x + 4, panel_y + 14, panel_w - 8, CARD_BORDER);

    const int ROW_H = 10;
    int col_left_x  = panel_x + 6;
    int col_right_x = panel_x + panel_w / 2 + 2;
    int row_y = panel_y + 18;

    spr.setTextColor(ACCENT_COLOR, panel_bg);
    spr.setCursor(col_left_x, row_y); kprint(spr, "SCREEN");
    row_y += ROW_H;

    for (int i = 0; i < key_count && row_y < panel_y + panel_h - 8; i++) {
        spr.setTextColor(HEADER_COLOR, panel_bg);
        spr.setCursor(col_left_x, row_y);
        spr.print(keys[i].key);
        spr.setTextColor(TEXT_COLOR, panel_bg);
        spr.setCursor(col_left_x + 26, row_y);
        spr.print(keys[i].desc);
        row_y += ROW_H;
    }

    row_y = panel_y + 18;
    spr.setTextColor(ACCENT_COLOR, panel_bg);
    spr.setCursor(col_right_x, row_y); kprint(spr, "GLOBAL");
    row_y += ROW_H;

    int global_count = sizeof(global_keys) / sizeof(global_keys[0]);
    for (int i = 0; i < global_count && row_y < panel_y + panel_h - 8; i++) {
        spr.setTextColor(HEADER_COLOR, panel_bg);
        spr.setCursor(col_right_x, row_y);
        spr.print(global_keys[i].key);
        spr.setTextColor(TEXT_COLOR, panel_bg);
        spr.setCursor(col_right_x + 26, row_y);
        spr.print(global_keys[i].desc);
        row_y += ROW_H;
    }

    spr.setTextColor(DIM_COLOR, panel_bg);
    spr.setCursor(panel_x + 6, panel_y + panel_h - 8);
    spr.print("TAB=close");
}

void draw_locator_help_overlay() {
    spr.fillRoundRect(62, 22, DISP_W - 66, DISP_H - 28, 5, lgfx::color565(10, 15, 25));
    spr.drawRoundRect(62, 22, DISP_W - 66, DISP_H - 28, 5, HEADER_COLOR);
    spr.fillRect(62, 22, 4, DISP_H - 28, HEADER_COLOR);
    spr.setTextColor(HEADER_COLOR); spr.setTextSize(1);
    spr.setCursor(70, 28); spr.print("LOCATOR KEYS");
    spr.drawLine(62, 38, DISP_W - 4, 38, CARD_BORDER);
    spr.setTextColor(TEXT_COLOR);
    spr.setCursor(68, 42);  spr.print("l : start/stop track");
    spr.setCursor(68, 52);  spr.print("t : cycle target");
    spr.setCursor(68, 62);  spr.print("arrow = bearing");
    spr.setCursor(68, 72);  spr.print("N   = true north");
    spr.setCursor(68, 82);  spr.print("blue rim = GPS lock");
    spr.setCursor(68, 92);  spr.print("amber = stale data");
    spr.setTextColor(DIM_COLOR);
    spr.setCursor(68, 107); spr.print("h to close");
}

// ============================================================================
// UI RENDERING - SCREENS 
// ============================================================================
void draw_scanner_screen() {
    int divider_x = 118;
    spr.fillSprite(BG_COLOR);
    draw_header_spr(0);
    spr.setClipRect(0, 18, divider_x, DISP_H - 18);
    
    float TILT = 0.55f;
    int rcx = radar_cx;
    int rcy = radar_cy;
    int THICKNESS = 10;

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
    spr.drawEllipse(rcx, rcy - 1, radar_r, radar_r * TILT, HEADER_COLOR);
    spr.drawEllipse(rcx, rcy,     radar_r, radar_r * TILT, HEADER_COLOR);

    // Hatching on both sides of the cylinder rim seam
    {
        const int HAT_TICKS = 20;
        uint16_t hat_col = lgfx::color565(30, 60, 100);
        for (int t = 0; t < HAT_TICKS; t++) {
            float a  = (float)t / HAT_TICKS * (float)M_PI * 2.0f;
            float ca = cosf(a), sa = sinf(a);
            int ex = rcx + (int)(radar_r * ca);
            int ey = rcy + (int)(radar_r * TILT * sa);
            // Cylinder wall side: 3px tick downward from rim
            spr.drawLine(ex, ey + 1, ex, ey + 4, hat_col);
            // Top face side: 4px tick inward from rim toward center
            int ix = rcx + (int)((radar_r - 4) * ca);
            int iy = rcy + (int)((radar_r - 4) * TILT * sa);
            spr.drawLine(ex, ey, ix, iy, hat_col);
        }
    }

    // Redraw bottom rim so it shows over the top face fill — doubled for thickness
    spr.drawEllipse(rcx, rcy + THICKNESS,     radar_r, radar_r * TILT, DIM_COLOR);
    spr.drawEllipse(rcx, rcy + THICKNESS + 1, radar_r, radar_r * TILT, DIM_COLOR);

    // Structural ribs on left wall removed

    spr.drawEllipse(rcx, rcy,     inner_r, inner_r * TILT, DIM_COLOR);
    spr.drawEllipse(rcx, rcy - 1, inner_r, inner_r * TILT, DIM_COLOR);

    float sweep_rad = (millis() / 3000.0f) * (float)M_PI * 2.0f;

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

    float local_spikes[NUM_RADIAL_BANDS];
    uint16_t local_colors[NUM_RADIAL_BANDS];

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
            float sweep_proximity = 1.0f - fminf(1.0f, fabsf(diff_sweep) / 0.5f);  // 1 at sweep, 0 at >0.5 rad away
            int sweep_boost = (int)(sweep_proximity * 80);
            int total_intensity = intensity + sweep_boost;
            if (total_intensity > 255) total_intensity = 255;

            int px = rcx + (int)(noise_r[i] * cosf(noise_a[i]));
            int py = rcy + (int)(noise_r[i] * sinf(noise_a[i]) * TILT);

            // Color based on intensity — cyan-green family for day, red-orange for night
            uint16_t p_col = night_mode
                ? lgfx::color565(total_intensity, total_intensity / 5, 0)
                : lgfx::color565(0, total_intensity * 2 / 3, total_intensity);

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

    uint16_t modulated_col = (sinf(millis() / 500.0f) > 0) ? HEADER_COLOR : DIM_COLOR;
    float breathe   = sinf(millis() / 500.0f);
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

    float angle_step = (float)M_PI * 2.0f / NUM_RADIAL_BANDS; bool any_active = false;

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
        if (local_spikes[i] > 0.1f) any_active = true;
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
                spr.drawLine(base_x, base_y, base_x, base_y - spike_len, line_col);
                // Shape encodes protocol — matches stats column vocabulary:
                // BLE = diamond (◆), WiFi = triangle (▲)
                bool is_ble_blip = (line_col == PURPLE_COLOR);
                int tip_y = base_y - spike_len;
                if (is_ble_blip) {
                    int cx_d = base_x, cy_d = tip_y + 3;
                    spr.drawLine(cx_d - 3, cy_d,     cx_d,     cy_d - 3, line_col);
                    spr.drawLine(cx_d,     cy_d - 3, cx_d + 3, cy_d,     line_col);
                    spr.drawLine(cx_d + 3, cy_d,     cx_d,     cy_d + 3, line_col);
                    spr.drawLine(cx_d,     cy_d + 3, cx_d - 3, cy_d,     line_col);
                } else {
                    spr.drawTriangle(base_x - 3, tip_y + 5,
                                     base_x + 3, tip_y + 5,
                                     base_x,     tip_y, line_col);
                }
            } else {
                // Small noise-style dash for weak signals
                spr.drawLine(base_x - 1, base_y, base_x + 1, base_y, line_col);
            }
        }
    }
    hud_rotation += 0.0033f;
    spr.clearClipRect();


    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long sw = session_flock_wifi; long sb = session_flock_ble;
    xSemaphoreGive(dataMutex);

    int right_text_x = divider_x + 5;

    bool ble_active = pBLEScan->isScanning();
    bool wifi_active = !ble_active;

    uint16_t inactive_col = CARD_BORDER;

    // Smooth easing for indicator color transitions
    static float wf_ease = 0.0f;
    static float ble_ease = 0.0f;
    wf_ease  += ((wifi_active ? 1.0f : 0.0f) - wf_ease)  * 0.2f;
    ble_ease += ((ble_active  ? 1.0f : 0.0f) - ble_ease) * 0.2f;

    uint16_t wf_col  = lerp_col16(inactive_col, CAUTION_COLOR, wf_ease);
    uint16_t ble_col = lerp_col16(inactive_col, PURPLE_COLOR,  ble_ease);

    // ── Combined segmented pill: [WiFi: ch | BLE] ──
    char wf_label[14]; snprintf(wf_label, sizeof(wf_label), "WiFi: %d", current_channel);
    bool wf_locked = (millis() < channel_lock_until);
    int wf_seg_w = (int)(strlen(wf_label) + (wf_locked ? 2 : 0)) * 6 + 10;
    int ble_seg_w = 26;

    bool ble_scan_window_active = (pBLEScan != nullptr && pBLEScan->isScanning());
    bool ble_pulse_on = ble_scan_window_active && (((millis() / 500) % 2) == 0);
    float ble_pulse_t = ble_scan_window_active ? (ble_pulse_on ? 1.0f : 0.35f) : 0.18f;
    uint16_t ble_badge_col = lerp_col16(CARD_BORDER, ble_col, ble_pulse_t);

    uint16_t wf_fill  = lerp_col16(BG_COLOR, CAUTION_COLOR, wf_ease  * 0.22f);
    uint16_t ble_fill = lerp_col16(BG_COLOR, PURPLE_COLOR,  ble_ease * 0.22f);

    int badge_x = right_text_x - 5;
    int badge_y = 24;
    int badge_h = 16;
    int total_w = wf_seg_w + ble_seg_w;
    uint16_t outer_border = lerp_col16(wf_col, ble_badge_col, 0.5f);

    // Clear pill area, then paint each segment fill
    spr.fillRoundRect(badge_x, badge_y, total_w, badge_h, 7, BG_COLOR);
    // WiFi segment — full pill in wf_fill, square off right edge
    spr.fillRoundRect(badge_x, badge_y, wf_seg_w + 6, badge_h, 7, wf_fill);
    spr.fillRect(badge_x + wf_seg_w - 6, badge_y, 12, badge_h, wf_fill);
    // BLE segment — rounded right, square off left edge
    spr.fillRoundRect(badge_x + wf_seg_w - 6, badge_y, ble_seg_w + 6, badge_h, 7, ble_fill);
    spr.fillRect(badge_x + wf_seg_w - 6, badge_y, 12, badge_h, ble_fill);

    // Outer rounded outline + 45° slash divider
    spr.drawRoundRect(badge_x, badge_y, total_w, badge_h, 7, outer_border);
    {
        int sx = badge_x + wf_seg_w;
        int sy_top = badge_y + 2;
        int sy_bot = badge_y + badge_h - 3;
        spr.drawLine(sx + 3, sy_top, sx - 3, sy_bot, outer_border);
        spr.drawLine(sx + 4, sy_top, sx - 2, sy_bot, outer_border);
    }

    // WiFi text
    spr.setTextColor(wf_col, wf_fill); spr.setTextSize(1);
    spr.setCursor(right_text_x, 29);
    spr.print(wf_label);
    if (wf_locked) {
        spr.setTextColor(CAUTION_COLOR, wf_fill);
        spr.print(" L");
    }

    // BLE text
    spr.setTextColor(ble_badge_col, ble_fill);
    spr.setCursor(badge_x + wf_seg_w + 6, 29);
    spr.print("BLE");

    // ── Combined stats divider ──
    // Horizontal line through the right column; WIFI ▲ N on left, BLE ◆ N on right.
    // Text blocks overdraw (clear) the line behind each block.
    {
        const int sd_y      = 44;
        const int sd_line_y = 47;
        const int sd_left   = right_text_x;
        const int sd_right  = DISP_W - 4;
        uint16_t  line_col  = lerp_col16(BG_COLOR, CARD_BORDER, 0.7f);

        spr.drawFastHLine(sd_left, sd_line_y, sd_right - sd_left, line_col);

        // WIFI block (left)
        char wifi_num[6]; snprintf(wifi_num, sizeof(wifi_num), "%ld", (long)sw);
        int wifi_block_w = 24 + 5 + 7 + 4 + (int)strlen(wifi_num) * 6;
        spr.fillRect(sd_left + 2, sd_line_y - 1, wifi_block_w + 4, 3, BG_COLOR);

        int wx = sd_left + 4;
        spr.setTextSize(1);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setCursor(wx, sd_y); kprint(spr, "WIFI");
        wx += 24 + 5;
        spr.fillTriangle(wx, sd_y + 6, wx + 6, sd_y + 6, wx + 3, sd_y, CAUTION_COLOR);
        wx += 7 + 4;
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setCursor(wx, sd_y); spr.print(wifi_num);

        // BLE block (right-aligned)
        char ble_num[6]; snprintf(ble_num, sizeof(ble_num), "%ld", (long)sb);
        int ble_block_w = 18 + 5 + 7 + 4 + (int)strlen(ble_num) * 6;
        int bx_start = sd_right - 4 - ble_block_w;
        spr.fillRect(bx_start - 2, sd_line_y - 1, ble_block_w + 4, 3, BG_COLOR);

        int bx = bx_start;
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setCursor(bx, sd_y); kprint(spr, "BLE");
        bx += 18 + 5;
        spr.fillTriangle(bx, sd_y + 3, bx + 3, sd_y,     bx + 6, sd_y + 3, PURPLE_COLOR);
        spr.fillTriangle(bx, sd_y + 3, bx + 3, sd_y + 6, bx + 6, sd_y + 3, PURPLE_COLOR);
        bx += 7 + 4;
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setCursor(bx, sd_y); spr.print(ble_num);
    }

    // ── Live device feed (right column) ──
    {
        const int feed_col_left  = right_text_x;
        const int feed_col_right = DISP_W - 4;
        const int feed_top_y     = 64;
        const int feed_row_h     = 11;
        const int max_visible    = 5;

        FeedEntry local_feed[FEED_SIZE];
        int local_count, local_head;
        unsigned long local_now;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        local_count = feed_count;
        local_head  = feed_head;
        for (int i = 0; i < local_count; i++) local_feed[i] = feed_entries[i];
        local_now = millis();
        xSemaphoreGive(dataMutex);

        if (local_count == 0) {
            spr.setTextColor(lerp_col16(BG_COLOR, DIM_COLOR, 0.4f), BG_COLOR);
            spr.setTextSize(1);
            spr.setCursor(feed_col_left + 4, feed_top_y + 14);
            spr.print("listening...");
        } else {
            int rendered = 0;
            for (int i = 0; i < local_count && rendered < max_visible; i++) {
                int idx = (local_head - i + FEED_SIZE * 2) % FEED_SIZE;
                FeedEntry& e = local_feed[idx];

                unsigned long age = local_now - e.timestamp;

                float age_fade;
                if (age < 30000UL)      age_fade = 1.0f;
                else if (age < 90000UL) age_fade = 1.0f - (float)(age - 30000UL) / 60000.0f;
                else                    age_fade = 0.0f;
                if (age_fade < 0.10f) { rendered++; continue; }

                float in_ease = 1.0f;
                if (age < 200UL) {
                    float t = (float)age / 200.0f;
                    in_ease = 1.0f - (1.0f - t) * (1.0f - t);
                }
                float total_alpha = age_fade * in_ease;

                int slide_offset = (age < 200UL) ? (int)((1.0f - in_ease) * -3.0f) : 0;
                int row_y = feed_top_y + rendered * feed_row_h + slide_offset;

                uint16_t proto_col = (e.proto == 0) ? CAUTION_COLOR : PURPLE_COLOR;
                if (e.is_flock) proto_col = lerp_col16(proto_col, ACCENT_COLOR, 0.5f);
                proto_col = lerp_col16(BG_COLOR, proto_col, total_alpha);
                uint16_t name_col = lerp_col16(BG_COLOR, TEXT_COLOR, total_alpha);

                const char* strength_str;
                uint16_t strength_base;
                if (e.rssi > -60)      { strength_str = "STR"; strength_base = ACCENT_COLOR; }
                else if (e.rssi > -80) { strength_str = "MED"; strength_base = CAUTION_COLOR; }
                else                   { strength_str = "WK";  strength_base = DIM_COLOR; }
                uint16_t strength_col = lerp_col16(BG_COLOR, strength_base, total_alpha);

                char prefix[6];
                snprintf(prefix, sizeof(prefix), "[%c%s]",
                         e.proto == 0 ? 'W' : 'B', e.is_flock ? "*" : "");
                spr.setTextSize(1);
                spr.setTextColor(proto_col, BG_COLOR);
                spr.setCursor(feed_col_left, row_y);
                spr.print(prefix);

                int strength_w = (int)strlen(strength_str) * 6;
                spr.setTextColor(strength_col, BG_COLOR);
                spr.setCursor(feed_col_right - strength_w, row_y);
                spr.print(strength_str);

                int prefix_w = (int)strlen(prefix) * 6;
                int name_x = feed_col_left + prefix_w + 3;
                int name_x_end = feed_col_right - strength_w - 3;
                int name_max_chars = (name_x_end - name_x) / 6;
                if (name_max_chars > (int)sizeof(e.name) - 1) name_max_chars = sizeof(e.name) - 1;
                if (name_max_chars < 1) name_max_chars = 1;

                char name_disp[20];
                strncpy(name_disp, e.name, name_max_chars);
                name_disp[name_max_chars] = '\0';
                spr.setTextColor(name_col, BG_COLOR);
                spr.setCursor(name_x, row_y);
                spr.print(name_disp);

                rendered++;
            }
        }
    }

}

// FNV-1a 32-bit hash of MAC address → 6 uppercase hex chars (deterministic, no storage)
void mac_to_short_id(const char* mac, char* out7) {
    uint32_t h = 2166136261UL;
    for (const char* p = mac; *p; p++) { h ^= (uint8_t)*p; h *= 16777619UL; }
    snprintf(out7, 7, "%06X", h & 0xFFFFFF);
}

void draw_locator_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active=locator_active, has_est=locator_has_estimate;
    char target_mac[18];  strncpy(target_mac,  locator_target_mac,  sizeof(target_mac)  - 1); target_mac[sizeof(target_mac)-1]   = '\0';
    char target_name[65]; strncpy(target_name, locator_target_name, sizeof(target_name) - 1); target_name[sizeof(target_name)-1] = '\0';
    float dist=locator_est_distance, brng=locator_bearing;
    bool gps_valid = gps.course.isValid() && gps.speed.isValid() && gps.speed.mph() > 2.0;
    bool has_loc   = gps.location.isValid();
    float gps_course = gps.course.deg();
    unsigned long newest_ms = locator_newest_sample_ms;
    int sample_count = locator_sample_count;
    int target_rssi = 0; bool has_rssi = false;
    for (int _i = 0; _i < rssi_tracker_count; _i++) {
        if (strncmp(rssi_tracker[_i].mac, locator_target_mac, 17) == 0
            && rssi_tracker[_i].sample_count > 0) {
            target_rssi = rssi_tracker[_i].samples[rssi_tracker[_i].sample_count - 1];
            has_rssi = true; break;
        }
    }
    xSemaphoreGive(dataMutex);

    unsigned long est_age_ms = (has_est && newest_ms > 0) ? (millis() - newest_ms) : 0;
    bool est_stale = has_est && (est_age_ms > 60000UL);
    bool demo = !active;
    const char* demo_name = "Pixel-6A-7F";
    const int   demo_rssi = -67;
    const float demo_dist = 43.0f;

    spr.fillSprite(BG_COLOR); draw_header_spr(1);

    // ── Diagonal-scrolling infinite grid — smaller cells and narrower panel ──
    const int GRID_RIGHT = 110;
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

    for (int gx = grid_o - GRID_STEP; gx <= GRID_RIGHT; gx += GRID_STEP)
        spr.drawLine(gx, 18, gx, DISP_H - 1, CARD_BORDER);
    for (int gy = 18 + grid_o - GRID_STEP; gy < DISP_H; gy += GRID_STEP)
        if (gy >= 18) spr.drawLine(0, gy, GRID_RIGHT, gy, CARD_BORDER);
    // Solid vertical separator on right edge of grid panel
    spr.drawFastVLine(GRID_RIGHT, 18, DISP_H - 18, CARD_BORDER);

    const int cx = 55, cy = 62;

    // ── Arrow heading (GPS bearing when tracking, slow drift otherwise) ──
    static float ease_arrow = 0.0f;
    static unsigned long last_ar_ms = 0;
    unsigned long ar_dt = now_ms - last_ar_ms;
    if (ar_dt > 100) ar_dt = 15;
    last_ar_ms = now_ms;

    if (active && has_est) {
        float rel = brng - (gps_valid ? gps_course : 0.0f);
        float tgt = radians(rel - 90.0f);
        float d = tgt - ease_arrow;
        while (d >  (float)M_PI) d -= 2.0f*(float)M_PI;
        while (d < -(float)M_PI) d += 2.0f*(float)M_PI;
        ease_arrow += 0.09f * d;
    } else {
        // Clockwise drift: 1 rev per 12s
        ease_arrow += (float)ar_dt * (2.0f*(float)M_PI / 12000.0f);
        if (ease_arrow > 2.0f*(float)M_PI) ease_arrow -= 2.0f*(float)M_PI;
    }
    float ang = ease_arrow;

    // ── Compass rose ring ──
    spr.drawCircle(cx, cy, 18, CARD_BORDER);
    spr.drawCircle(cx, cy, 19, BG_COLOR);  // thin separation from arrow

    // ── Arrow: cursor/pointer shape — 7-point polygon ──
    auto rotpt = [&](float lx, float ly, float a, int* ox, int* oy) {
        float ca = cosf(a), sa = sinf(a);
        *ox = cx + (int)roundf(lx * ca - ly * sa);
        *oy = cy + (int)roundf(lx * sa + ly * ca);
    };
    // Local-space vertices (pointing up before rotation):
    //   0=tip, 1=right-outer, 2=right-inner, 3=right-base, 4=left-base, 5=left-inner, 6=left-outer
    const float A_TIP_Y   = -15.0f, A_SHLDR_Y = -4.0f, A_BASE_Y = 9.5f;
    const float A_HEAD_HW =  8.0f,  A_STEM_HW =  4.0f;
    const float lx7[7] = {  0,          A_HEAD_HW,  A_STEM_HW,  A_STEM_HW, -A_STEM_HW, -A_STEM_HW, -A_HEAD_HW };
    const float ly7[7] = { A_TIP_Y, A_SHLDR_Y, A_SHLDR_Y, A_BASE_Y,  A_BASE_Y, A_SHLDR_Y,  A_SHLDR_Y };
    int ax7[7], ay7[7];
    for (int vi = 0; vi < 7; vi++) rotpt(lx7[vi], ly7[vi], ang, &ax7[vi], &ay7[vi]);

    // BG fill: arrowhead triangle + stem rectangle (two triangles)
    spr.fillTriangle(ax7[0], ay7[0], ax7[1], ay7[1], ax7[6], ay7[6], BG_COLOR);
    spr.fillTriangle(ax7[2], ay7[2], ax7[3], ay7[3], ax7[4], ay7[4], BG_COLOR);
    spr.fillTriangle(ax7[2], ay7[2], ax7[4], ay7[4], ax7[5], ay7[5], BG_COLOR);

    // Hatching: narrow spacing near tip, wider toward base (quadratic from tip)
    {
        uint16_t hatch_col = lgfx::color565(45, 135, 210);
        for (int hi = 1; hi < 16; hi++) {
            float ly = A_TIP_Y + 0.4f * (float)(hi * hi);  // quadratic from tip → base
            if (ly > A_BASE_Y) break;
            // Half-width at this y level
            float hw;
            if (ly <= A_SHLDR_Y) {
                float t = (ly - A_TIP_Y) / (A_SHLDR_Y - A_TIP_Y);
                hw = A_HEAD_HW * t;
            } else {
                hw = A_STEM_HW;
            }
            if (hw < 0.5f) continue;
            int hx0, hy0, hx1, hy1;
            rotpt(-hw, ly, ang, &hx0, &hy0);
            rotpt( hw * 0.5f, ly, ang, &hx1, &hy1);  // stop at 3/4 of total width
            spr.drawLine(hx0, hy0, hx1, hy1, hatch_col);
        }
    }

    // GPS_COLOR outline — draw all 7 edges
    for (int vi = 0; vi < 7; vi++) {
        int ni = (vi + 1) % 7;
        spr.drawLine(ax7[vi], ay7[vi], ax7[ni], ay7[ni], GPS_COLOR);
    }

    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(cx - 24, cy + 22);
    kprint(spr, "DIRECTION");

    // ── Sample boxes: GPS_COLOR, bottom of left panel, centered, thick X ──
    int sc = active ? sample_count : 0;
    bool lock = active ? has_est : false;

    static unsigned long smpl_birth[3] = {0, 0, 0};
    static int smpl_prev = 0;
    if (sc != smpl_prev) {
        if (sc > smpl_prev) {
            for (int i = smpl_prev; i < sc && i < LOC_MIN_SAMPLES_EST; i++)
                smpl_birth[i] = now_ms;
        } else {
            for (int i = 0; i < LOC_MIN_SAMPLES_EST; i++) smpl_birth[i] = 0;
        }
        smpl_prev = sc;
    }

    const int BOX = 11;
    const int by0 = 95;        // near bottom of left panel
    const int bx0 = cx - 25;  // centers 3 boxes under cx

    char samp_label[16];
    snprintf(samp_label, sizeof(samp_label), "SAMPLES %d/%d", sc, LOC_MIN_SAMPLES_EST);
    spr.setTextColor(sc >= LOC_MIN_SAMPLES_EST ? ACCENT_COLOR : DIM_COLOR, BG_COLOR);
    spr.setTextSize(1);
    spr.setCursor(bx0, by0 - 10);
    kprint(spr, samp_label);

    for (int di = 0; di < LOC_MIN_SAMPLES_EST; di++) {
        int bxi = bx0 + di * 17;
        bool filled = di < sc;

        uint16_t box_col;
        if (filled) {
            // Pulse ACCENT_COLOR using sin wave
            float pulse_t = sinf((float)now_ms * 0.004f) * 0.5f + 0.5f;
            uint16_t dark_accent = lerp_col16(BG_COLOR, ACCENT_COLOR, 0.4f);
            box_col = lerp_col16(dark_accent, ACCENT_COLOR, pulse_t);
        } else {
            box_col = DIM_COLOR;
        }

        spr.drawRect(bxi, by0, BOX, BOX, box_col);
        if (filled) {
            // Oversized static X — 2px thick, extends 1px outside box edges
            int mx = bxi + BOX / 2, my = by0 + BOX / 2;
            int xr = BOX / 2 + 1;
            spr.drawLine(mx - xr, my - xr, mx + xr, my + xr, box_col);
            spr.drawLine(mx - xr + 1, my - xr, mx + xr, my + xr - 1, box_col);
            spr.drawLine(mx + xr, my - xr, mx - xr, my + xr, box_col);
            spr.drawLine(mx + xr - 1, my - xr, mx - xr, my + xr - 1, box_col);
        }
    }
    // lock indicator: subtle glow on the last sample box instead of text

    // ── Right panel ──
    int rx = 114;
    int rpx = rx + 8;

    // Status — dynamic-width box
    const char* status_base; uint16_t status_col; bool status_anim = false;
    if (north_mode) {
        status_base = "Pointing North";  status_col = GPS_COLOR;
    } else if (!has_loc && !gps_valid) {
        status_base = "GPS";             status_col = GPS_COLOR;     status_anim = true;
    } else if (!active) {
        status_base = "Need Target";     status_col = CAUTION_COLOR; status_anim = true;
    } else {
        status_base = "Hunting";         status_col = ACCENT_COLOR;
    }
    char status_str[26];
    if (status_anim) {
        int nd = (int)(now_ms / 500) % 4;
        snprintf(status_str, sizeof(status_str), "%s%s", status_base,
                 nd == 0 ? "" : nd == 1 ? "." : nd == 2 ? ".." : "...");
    } else {
        strncpy(status_str, status_base, sizeof(status_str) - 1);
        status_str[sizeof(status_str) - 1] = '\0';
    }
    int max_chars = (int)strlen(status_base) + (status_anim ? 3 : 0);
    int box_w = max_chars * 7 + 12;   // 7px/char (kerned) + left+right padding
    int avail_w = DISP_W - rpx - 2;
    if (box_w > avail_w) box_w = avail_w;
    // Tinted fill toward status color, matching scanner badge treatment
    uint16_t status_fill = lerp_col16(BG_COLOR, status_col, 0.22f);
    spr.fillRoundRect(rpx, 23, box_w, 16, 5, status_fill);
    spr.drawRoundRect(rpx, 23, box_w, 16, 5, status_col);
    spr.setTextColor(status_col, status_fill); spr.setTextSize(1);
    spr.setClipRect(rpx + 1, 24, box_w - 2, 14);
    spr.setCursor(rpx + 6, 27); kprint(spr, status_str);
    spr.clearClipRect();

    // DISTANCE (hero)
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(rpx, 48); kprint(spr, "DISTANCE");

    // Trend arrow next to DISTANCE label — green ↓ closer, amber ↑ farther
    {
        int trend = (active && has_est) ? locator_distance_trend() : 0;
        if (trend != 0) {
            int tx = rpx + 58;
            int ty = 48;
            if (trend < 0) {
                spr.fillTriangle(tx, ty, tx + 6, ty, tx + 3, ty + 6, ACCENT_COLOR);
            } else {
                spr.fillTriangle(tx, ty + 6, tx + 6, ty + 6, tx + 3, ty, CAUTION_COLOR);
            }
        }
    }
    {
        float sd = (active && has_est) ? dist : (demo ? demo_dist : -1.0f);
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(3);
        spr.setCursor(rpx, 58);
        if (sd < 0) {
            spr.print("--");
        } else {
            char db[12];
            if (sd < 100) snprintf(db, sizeof(db), "%.0f", sd);
            else          snprintf(db, sizeof(db), "%.1f", sd / 1000.0f);
            spr.print(db);
            spr.setTextSize(1);
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.print(sd < 100 ? "m" : "km");
        }
    }

    // TARGET (secondary)
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(rpx, 90); kprint(spr, "TARGET");
    {
        char tname[15];
        if (active) {
            bool nok = (target_name[0] != '\0' && strcmp(target_name,"Hidden")!=0 && strcmp(target_name,"Unknown")!=0);
            const char* src = nok ? target_name : ((strlen(target_mac)>8)?target_mac+9:target_mac);
            strncpy(tname, src, 14); tname[14] = '\0';
        } else { strncpy(tname, demo_name, 14); tname[14] = '\0'; }
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(rpx, 100); spr.print(tname);
    }

    // SIGNAL (secondary)
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(rpx, 114); kprint(spr, "SIGNAL");
    {
        int sr = (active && has_rssi) ? target_rssi : (demo ? demo_rssi : -999);
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(rpx, 124);
        if (sr == -999) { spr.print("--"); }
        else { spr.print(sr > -60 ? "STRONG" : sr > -80 ? "MEDIUM" : "WEAK"); }
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
        if (history_selected_idx < history_scroll_offset)          history_scroll_offset = history_selected_idx;
        if (history_selected_idx >= history_scroll_offset + 4)     history_scroll_offset = history_selected_idx - 3;
        int max_scroll = max(0, total - 4);
        if (history_scroll_offset > max_scroll) history_scroll_offset = max_scroll;
    }

    spr.fillSprite(BG_COLOR);
    draw_header_spr(2);

    if (total == 0) {
        for (int i = 0; i < 4; i++) {
            int y = 19 + i * 28;
            spr.fillRect(0, y, 3, 27, CARD_BORDER);
            spr.fillRect(3, y, DISP_W - 3, 27, (i % 2 == 0) ? CARD_COLOR : BG_COLOR);
            spr.setTextColor(CARD_BORDER, (i % 2 == 0) ? CARD_COLOR : BG_COLOR); spr.setTextSize(1);
            spr.setCursor(10, y + 9); spr.print(use_sd ? "-- No SD log --" : "-- Listening...");
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
    for (int i = history_scroll_offset; i < total && rows_shown < 4; i++, rows_shown++) {
        int y = 19 + rows_shown * 28;
        bool selected = (i == history_selected_idx);

        // Pull fields from whichever source
        const char* e_type;  uint16_t e_conf_col;
        char e_mac[18], e_name[32], e_method[24];
        int e_rssi, e_conf;

        if (use_sd) {
            e_type = sd_hist[i].type;
            strncpy(e_mac,    sd_hist[i].mac,    17); e_mac[17]    = '\0';
            strncpy(e_name,   sd_hist[i].name,   31); e_name[31]   = '\0';
            strncpy(e_method, sd_hist[i].method, 23); e_method[23] = '\0';
            e_rssi = sd_hist[i].rssi;
            e_conf = sd_hist[i].confidence;
        } else {
            e_type = mem_hist[i].type;
            strncpy(e_mac,  mem_hist[i].mac,  17); e_mac[17]  = '\0';
            strncpy(e_name, mem_hist[i].name, 31); e_name[31] = '\0';
            e_method[0] = '\0';
            e_rssi = mem_hist[i].rssi;
            e_conf = mem_hist[i].confidence;
        }

        const char* proto_lbl; uint16_t proto_col;
        hist_type_info(e_type, &proto_lbl, &proto_col);

        // Row background: selected = slightly lighter
        uint16_t row_bg = selected ? CARD_BORDER : ((rows_shown % 2 == 0) ? CARD_COLOR : BG_COLOR);
        spr.fillRect(0, y, 4, 27, proto_col);
        spr.fillRect(4, y, DISP_W - 4, 27, row_bg);

        if (selected) {
            spr.fillRect(0, y, 6, 27, HEADER_COLOR);
            int ay_mid = y + 14;
            for (int p = 0; p < 4; p++) {
                spr.drawLine(7 + p, ay_mid - (3 - p), 7 + p, ay_mid + (3 - p), HEADER_COLOR);
            }
            spr.drawRect(0, y, DISP_W, 27, HEADER_COLOR);
        }

        if (selected) {
            // "Has detail" chevron in right gutter
            int chevron_x = DISP_W - 8;
            int chevron_y_mid = y + 14;
            spr.drawLine(chevron_x,     chevron_y_mid - 3, chevron_x + 3, chevron_y_mid,     HEADER_COLOR);
            spr.drawLine(chevron_x + 3, chevron_y_mid,     chevron_x,     chevron_y_mid + 3, HEADER_COLOR);
            spr.drawLine(chevron_x - 1, chevron_y_mid - 3, chevron_x + 2, chevron_y_mid,     HEADER_COLOR);  // 2px thick
            spr.drawLine(chevron_x + 2, chevron_y_mid,     chevron_x - 1, chevron_y_mid + 3, HEADER_COLOR);
        }

        int content_x = selected ? 14 : 10;

        // Protocol label in proto_col
        spr.setTextColor(proto_col, row_bg); spr.setTextSize(1);
        spr.setCursor(content_x, y + 4); spr.print(proto_lbl);

        // Name (or last MAC octets)
        bool name_ok = (e_name[0] != '\0' &&
                        strcmp(e_name, "Hidden")  != 0 &&
                        strcmp(e_name, "Unknown") != 0);
        char nom[13] = "";
        if (name_ok) { strncpy(nom, e_name, 12); nom[12] = '\0'; }
        else { const char* sm = (strlen(e_mac) > 8) ? e_mac + 9 : e_mac; strncpy(nom, sm, 12); nom[12] = '\0'; }

        spr.setTextColor(TEXT_COLOR, row_bg); spr.setTextSize(1);
        spr.setCursor(content_x + 28, y + 4); spr.print(nom);

        // Right-aligned timestamp
        const char* ts_src = use_sd ? sd_hist[i].timestamp : "";
        if (ts_src[0]) {
            spr.setTextColor(DIM_COLOR, row_bg);
            int ts_x = selected ? DISP_W - 62 : DISP_W - 52;
            spr.setCursor(ts_x, y + 4); spr.print(ts_src);
        }

        // Second line: MAC + method for selected; stats otherwise
        spr.setTextColor(DIM_COLOR, row_bg);
        spr.setCursor(content_x, y + 17);
        if (selected) {
            char det_buf[30];
            snprintf(det_buf, sizeof(det_buf), "%s  %s", e_mac, e_method[0] ? e_method : "");
            spr.print(det_buf);
        } else {
            char stat_buf[22];
            snprintf(stat_buf, sizeof(stat_buf), "%d%%  %ddBm", e_conf, e_rssi);
            spr.print(stat_buf);
        }
    }

    {
        const int list_y = 19;
        const int list_h = 4 * 28;
        if (total > 4) {
            if (history_scroll_offset > 0)
                draw_scroll_fade(list_y, list_h, 5, true);
            if (history_scroll_offset < total - 4)
                draw_scroll_fade(list_y, list_h, 5, false);
        }
    }

    // Scroll indicator
    if (total > 4) {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(DISP_W - 56, DISP_H - 8);
        char scr_buf[16]; snprintf(scr_buf, sizeof(scr_buf), "%d/%d ;/.", history_selected_idx + 1, total);
        spr.print(scr_buf);
    }

    // Detail overlay — shown when enter was pressed on selected row
    if (hist_detail_open && total > 0) {
        int di = history_selected_idx;
        if (di < 0) di = 0; if (di >= total) di = total - 1;

        const char* d_type;
        char d_mac[18], d_name[32], d_method[24];
        int d_rssi, d_conf;
        if (use_sd) {
            d_type = sd_hist[di].type;
            strncpy(d_mac,    sd_hist[di].mac,    17); d_mac[17]    = '\0';
            strncpy(d_name,   sd_hist[di].name,   31); d_name[31]   = '\0';
            strncpy(d_method, sd_hist[di].method, 23); d_method[23] = '\0';
            d_rssi = sd_hist[di].rssi; d_conf = sd_hist[di].confidence;
        } else {
            d_type = mem_hist[di].type;
            strncpy(d_mac,  mem_hist[di].mac,  17); d_mac[17]  = '\0';
            strncpy(d_name, mem_hist[di].name, 31); d_name[31] = '\0';
            d_method[0] = '\0';
            d_rssi = mem_hist[di].rssi; d_conf = mem_hist[di].confidence;
        }

        const char* proto_lbl; uint16_t proto_col;
        hist_type_info(d_type, &proto_lbl, &proto_col);

        // Dim backdrop
        for (int dy = 21; dy < DISP_H; dy += 2)
            spr.drawFastHLine(0, dy, DISP_W, lgfx::color565(8, 8, 14));

        // Card
        int cx = 6, cy = 19, cw = DISP_W - 12, ch = DISP_H - 25;
        spr.fillRoundRect(cx, cy, cw, ch, 6, CARD_COLOR);
        spr.drawRoundRect(cx, cy, cw, ch, 6, proto_col);

        // Header bar inside card
        spr.fillRoundRect(cx, cy, cw, 16, 6, proto_col);
        spr.fillRect(cx, cy + 10, cw, 6, proto_col);  // square bottom of header
        spr.setTextColor(BG_COLOR, proto_col); spr.setTextSize(1);
        spr.setCursor(cx + 6, cy + 4); spr.print(proto_lbl);
        bool dn_ok = (d_name[0] != '\0' && strcmp(d_name,"Hidden")!=0 && strcmp(d_name,"Unknown")!=0);
        if (dn_ok) { spr.setCursor(cx + 30, cy + 4); spr.print(d_name); }

        // Fields
        int fy = cy + 22;
        auto det_row = [&](const char* lbl, const char* val) {
            spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
            spr.setCursor(cx + 6, fy);
            kprint(spr, lbl);
            spr.setTextColor(TEXT_COLOR, CARD_COLOR);
            spr.setCursor(cx + 6 + (int)strlen(lbl) * 7 + 4, fy);
            spr.print(val);
        };
        char tmp[32];
        det_row("MAC: ", d_mac); fy += 14;
        snprintf(tmp, sizeof(tmp), "%d dBm", d_rssi); det_row("RSSI: ", tmp); fy += 14;
        {
            const char* band;
            if      (d_conf >= CONFIDENCE_CERTAIN)         band = "CERTAIN";
            else if (d_conf >= CONFIDENCE_HIGH)            band = "HIGH";
            else if (d_conf >= CONFIDENCE_ALARM_THRESHOLD) band = "MEDIUM";
            else                                           band = "below alarm";
            snprintf(tmp, sizeof(tmp), "%d%% (%s)", d_conf, band);
            det_row("CONF: ", tmp);
            fy += 14;
        }
        if (d_method[0]) {
            char human[48];
            methods_to_human(d_method, human, sizeof(human));
            det_row("MATCH:", human);
            fy += 14;
        }
        const char* d_ts = use_sd ? sd_hist[di].timestamp : "";
        if (d_ts[0]) { det_row("TIME: ", d_ts); fy += 14; }

        // Footer hint
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(cx + 6, cy + ch - 11); spr.print("ENTER to close");
    }
}

void draw_gps_screen() {
    spr.fillSprite(BG_COLOR);
    draw_header_spr(3);

    bool has_loc, stale, speed_valid;
    int sats;
    double d_lat, d_lng;
    float f_spd;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    has_loc     = gps.location.isValid();
    stale       = has_loc && (gps.location.age() > 2000);
    sats        = gps.satellites.isValid() ? gps.satellites.value() : 0;
    d_lat       = gps.location.lat();
    d_lng       = gps.location.lng();
    speed_valid = gps.speed.isValid();
    f_spd       = gps.speed.mph();
    xSemaphoreGive(dataMutex);

    // ── Off-axis 3D wireframe globe ──────────────────────────────────────────
    // Solid BG fill, diagonal axis tilt like a real globe on a stand
    const int gx = 55, gy = 72, gr = 30;  // gy=72 leaves room below for orbit+SATS

    // TILT: X-axis — north pole backward; ROLL: Z-axis — axis diagonal upper-left→lower-right
    const float TILT  = -0.30f;
    const float ROLL  =  0.45f;
    float rot = fmodf((float)millis() / 8000.0f, 1.0f) * 2.0f * (float)M_PI;

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

    // Solid BG fill so globe interior matches screen background
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
                     : (has_loc ? GPS_COLOR : lgfx::color565(48, 108, 220));
    spr.drawCircle(gx, gy, gr,     rim_col);
    spr.drawCircle(gx, gy, gr + 1, lgfx::color565(24, 50, 110));

    // ── Multi-plane orbital satellite animation ──────────────────────────────
    {
        // 3 orbital planes: inclinations, radii, speeds, satellite counts
        struct OrbPlane {
            float inc;       // inclination (radians)
            float radius;    // screen pixels from globe center
            float speed;     // angular speed factor (applied to millis)
            int   n_sats;    // satellites in this plane
            float phase_off; // phase offset for plane
        };
        const OrbPlane planes[3] = {
            { radians(55.0f), (float)(gr + 12), 0.0003f,  3, 0.0f },
            { radians(55.0f), (float)(gr + 16), 0.00025f, 2, 2.09f },
            { radians(80.0f), (float)(gr + 10), 0.0004f,  2, 1.05f },
        };

        // Per-plane projection lambda — applies globe tilt/roll then inclination
        for (int pi = 0; pi < 3; pi++) {
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

            // Draw dashed orbit ring for this plane
            const int N_RING = 48;
            for (int ri = 0; ri < N_RING; ri++) {
                if (ri % 2 != 0) continue;
                float a0 = (float)ri       / N_RING * 2.0f * (float)M_PI;
                float a1 = (float)(ri + 1) / N_RING * 2.0f * (float)M_PI;
                int px0, py0, px1, py1;
                float tz0 = orb_proj_p(a0, &px0, &py0);
                float tz1 = orb_proj_p(a1, &px1, &py1);
                if ((tz0 + tz1) * 0.5f > 0.0f)
                    spr.drawLine(px0, py0, px1, py1, lgfx::color565(18, 40, 72));
            }

            // Draw satellite dots with motion trails
            float orbit_t = (float)millis() * pl.speed + pl.phase_off;
            for (int si = 0; si < pl.n_sats; si++) {
                float base_ang = orbit_t + (float)si / (float)pl.n_sats * 2.0f * (float)M_PI;
                // Motion trail: 4 ghost dots fading behind
                for (int tr = 3; tr >= 0; tr--) {
                    float trail_ang = base_ang - (float)(tr + 1) * 0.12f;
                    int tpx, tpy;
                    float ttz = orb_proj_p(trail_ang, &tpx, &tpy);
                    if (ttz > 0.0f) {
                        float fade = 1.0f - (float)(tr + 1) / 5.0f;
                        // Brighter for acquired sats, dimmer for ghost
                        bool acquired = (si < sats);
                        uint16_t trail_col = acquired
                            ? lerp_col16(BG_COLOR, GPS_COLOR, fade * 0.5f)
                            : lerp_col16(BG_COLOR, DIM_COLOR, fade * 0.3f);
                        spr.drawPixel(tpx, tpy, trail_col);
                    }
                }
                // Main satellite dot
                int dpx, dpy;
                float tz2 = orb_proj_p(base_ang, &dpx, &dpy);
                if (tz2 > 0.0f) {
                    bool acquired = (si < sats);
                    uint16_t sat_col = acquired
                        ? lgfx::color565(255, 255, 255)
                        : lerp_col16(BG_COLOR, DIM_COLOR, 0.5f);
                    spr.fillCircle(dpx, dpy, acquired ? 2 : 1, sat_col);
                }
            }
        }
    }

    // SAT count — single line below the full orbit extent, number in white
    {
        int sat_y = gy + (gr + 14) + 9;  // below orbit (orb_r = gr+14)
        int sat_x = gx - 24;
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(sat_x, sat_y); kprint(spr, "SATS ");
        spr.setTextColor(lgfx::color565(255, 255, 255), BG_COLOR);
        spr.setCursor(sat_x + 35, sat_y); spr.print(sats);
    }

    // ── Right panel: STATUS badge + LAT / LON / SPEED ───────────────────────
    const int RX = 122, RW = 114;
    int ry = 25;

    // STATUS badge (top-right, matches scanner/locator badge style)
    {
        const char* status_base;
        uint16_t    status_col;
        bool        status_anim = false;
        if      (has_loc && !stale) { status_base = "GPS LOCKED";   status_col = GPS_COLOR; }
        else if (stale)             { status_base = "SIGNAL LOST";  status_col = CAUTION_COLOR; }
        else                        { status_base = "GPS";           status_col = GPS_COLOR; status_anim = true; }
        char status_str[22];
        if (status_anim) {
            int nd = (int)(millis() / 500) % 4;
            snprintf(status_str, sizeof(status_str), "%s%s", status_base,
                     nd==0?"":nd==1?".":nd==2?"..":"...");
        } else {
            strncpy(status_str, status_base, sizeof(status_str)-1);
            status_str[sizeof(status_str)-1] = '\0';
        }
        int bw = (int)strlen(status_base) * 7 + (status_anim ? 3*7 : 0) + 12;
        if (bw > RW) bw = RW;
        uint16_t sfill = lerp_col16(BG_COLOR, status_col, 0.22f);
        spr.fillRoundRect(RX, ry, bw, 16, 5, sfill);
        spr.drawRoundRect(RX, ry, bw, 16, 5, status_col);
        spr.setTextColor(status_col, sfill); spr.setTextSize(1);
        spr.setClipRect(RX+1, ry+1, bw-2, 14);
        spr.setCursor(RX+6, ry+4); kprint(spr, status_str);
        spr.clearClipRect();
    }

    // LAT
    ry += 22;
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(RX, ry); kprint(spr, "LAT");
    spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(1);
    char lat_buf[14]; snprintf(lat_buf, sizeof(lat_buf), "%.5f", has_loc ? d_lat : 0.0);
    spr.setCursor(RX, ry + 11); spr.print(lat_buf);

    // LON
    ry += 28;
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(RX, ry); kprint(spr, "LON");
    spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(1);
    char lon_buf[14]; snprintf(lon_buf, sizeof(lon_buf), "%.5f", has_loc ? d_lng : 0.0);
    spr.setCursor(RX, ry + 11); spr.print(lon_buf);

    // SPEED
    ry += 28;
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(RX, ry); kprint(spr, "SPEED");
    spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(1);
    char spd_buf[12]; snprintf(spd_buf, sizeof(spd_buf), "%.1f mph", speed_valid ? f_spd : 0.0f);
    spr.setCursor(RX, ry + 11); spr.print(spd_buf);
}

void draw_device_info_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long lt = lifetime_flock_total;
    long sr = session_raven;
    long sw = session_flock_wifi;
    long sb = session_flock_ble;
    long lb = lifetime_boots;
    long lfw = lifetime_flash_writes;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(4);

    const int content_top_y = 18;
    const int content_bottom_y = DISP_H - 1;
    spr.setClipRect(0, content_top_y, DISP_W - 6, content_bottom_y - content_top_y);

    int yoff = 19 - device_info_scroll;

    // 2-column grid layout
    const int CARD_W = 112;
    const int COL_L  = 4;
    const int COL_R  = 124;
    const int CARD_H = 38;

    // Version card (full width)
    drawCard(COL_L, yoff, DISP_W - 8, 20);
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(10, yoff + 5); spr.print(VERSION_STRING);

    // Row 1: BOOTS (left), LIFETIME (right)
    int row1_y = yoff + 24;
    drawCard(COL_L, row1_y, CARD_W, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_L + 4, row1_y + 4); kprint(spr, "BOOTS");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(COL_L + 4, row1_y + 14); spr.print(lb);

    drawCard(COL_R, row1_y, CARD_W, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_R + 4, row1_y + 4); kprint(spr, "LIFETIME");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_R + 4, row1_y + 16);
    { char tb[9]; format_time_buf(lifetime_seconds, tb, sizeof(tb)); spr.print(tb); }

    // Row 1.5: SESSION TIME (full width) — migrated from scanner screen
    int row_sess_y = yoff + 66;
    drawCard(COL_L, row_sess_y, DISP_W - 8, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(COL_L + 4, row_sess_y + 4); kprint(spr, "SESSION");
    {
        char sess_buf[9];
        format_time_buf((millis() - session_start_time) / 1000, sess_buf, sizeof(sess_buf));
        spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(COL_L + 4, row_sess_y + 14); spr.print(sess_buf);
    }

    // Row 2: ALL-TIME (left), RAVEN (right)
    int row2_y = yoff + 108;
    drawCard(COL_L, row2_y, CARD_W, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_L + 4, row2_y + 4); kprint(spr, "ALL-TIME");
    spr.setTextColor(CAUTION_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(COL_L + 4, row2_y + 14); spr.print(lt);

    drawCard(COL_R, row2_y, CARD_W, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_R + 4, row2_y + 4); kprint(spr, "RAVEN");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(COL_R + 4, row2_y + 14); spr.print(sr);

    // Row 3: WIFI SESS (left), BLE SESS (right)
    int row3_y = yoff + 150;
    drawCard(COL_L, row3_y, CARD_W, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_L + 4, row3_y + 4); kprint(spr, "WIFI SESS");
    spr.setTextColor(CAUTION_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(COL_L + 4, row3_y + 14); spr.print(sw);

    drawCard(COL_R, row3_y, CARD_W, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(COL_R + 4, row3_y + 4); kprint(spr, "BLE SESS");
    spr.setTextColor(PURPLE_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(COL_R + 4, row3_y + 14); spr.print(sb);

    // Row 4 label area: FLASH WRITES (full width)
    int row4fw_y = yoff + 192;
    drawCard(COL_L, row4fw_y, DISP_W - 8, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(COL_L + 4, row4fw_y + 4); kprint(spr, "FLASH WRITES");

    int wear_pct = (int)((lfw * 100) / 100000);
    if (wear_pct > 100) wear_pct = 100;
    uint16_t wear_col = (wear_pct >= 80) ? CAUTION_COLOR
                      : (wear_pct >= 50) ? CAUTION_COLOR
                      : ACCENT_COLOR;

    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(2);
    spr.setCursor(COL_L + 4, row4fw_y + 14); spr.print(lfw);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(COL_L + 4, row4fw_y + 30); spr.print("writes");

    int bar_x = 110, bar_y = row4fw_y + 16, bar_w = 116, bar_h = 8;
    spr.drawRect(bar_x, bar_y, bar_w, bar_h, CARD_BORDER);
    int fill = (wear_pct * (bar_w - 2)) / 100;
    if (fill > 0) spr.fillRect(bar_x + 1, bar_y + 1, fill, bar_h - 2, wear_col);
    char pct_str[8]; snprintf(pct_str, sizeof(pct_str), "%d%%", wear_pct);
    spr.setTextColor(wear_col, CARD_COLOR);
    spr.setCursor(bar_x + bar_w - 28, bar_y + bar_h + 4); spr.print(pct_str);

    // Row 5: VOLTAGE (left) + POWER SOURCE (right)
    int row5_y = yoff + 234;
    {
        int32_t bat_mv_snap = get_filtered_voltage();

        // Voltage card (left)
        drawCard(COL_L, row5_y, CARD_W, CARD_H);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(COL_L + 4, row5_y + 4); kprint(spr, "VOLTAGE");
        uint16_t v_col;
        if      (bat_mv_snap >= 3800) v_col = ACCENT_COLOR;
        else if (bat_mv_snap >= 3600) v_col = CAUTION_COLOR;
        else                          v_col = lgfx::color565(220, 60, 60);
        spr.setTextColor(v_col, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(COL_L + 4, row5_y + 14);
        char volt_str[10]; snprintf(volt_str, sizeof(volt_str), "%.2fV", bat_mv_snap / 1000.0f);
        spr.print(volt_str);

        // Power source card (right) — best-effort indication
        drawCard(COL_R, row5_y, CARD_W, CARD_H);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(COL_R + 4, row5_y + 4); kprint(spr, "POWER");
        spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(COL_R + 4, row5_y + 16);
        const char* pwr_label = (bat_mv_snap >= 4200) ? "USB" : "BATTERY";
        spr.print(pwr_label);
        spr.setTextColor(DIM_COLOR, CARD_COLOR);
        spr.setCursor(COL_R + 4, row5_y + 28); spr.print("(estimated)");
    }

    // Row 6: SD STATUS — full width
    int row6_y = yoff + 276;
    drawCard(COL_L, row6_y, DISP_W - 8, CARD_H);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(COL_L + 4, row6_y + 4); kprint(spr, "SD CARD");
    spr.setTextColor(sd_available ? ACCENT_COLOR : DIM_COLOR, CARD_COLOR);
    spr.setCursor(COL_L + 58, row6_y + 4);
    spr.print(sd_available ? "MOUNTED" : "NOT FOUND");
    spr.setTextColor(DIM_COLOR, CARD_COLOR);
    spr.setCursor(150, row6_y + 4);
    spr.print(littlefs_available ? "FS OK" : "FS ERR");

    spr.clearClipRect();

    {
        const int visible_h = (DISP_H - 1) - 18;
        const int max_scroll_for_fade = DEVICE_INFO_CONTENT_HEIGHT - visible_h;
        if (max_scroll_for_fade > 0) {
            if (device_info_scroll > 0)
                draw_scroll_fade(21, visible_h, 6, true);
            if (device_info_scroll < max_scroll_for_fade)
                draw_scroll_fade(21, visible_h, 6, false);
        }
    }

    const int max_scroll = DEVICE_INFO_CONTENT_HEIGHT - (content_bottom_y - content_top_y);
    if (max_scroll > 0) {
        const int track_x = DISP_W - 4;
        const int track_y = content_top_y + 2;
        const int track_h = (content_bottom_y - content_top_y) - 4;
        spr.drawFastVLine(track_x + 1, track_y, track_h, CARD_BORDER);
        int thumb_h = (track_h * (content_bottom_y - content_top_y)) / DEVICE_INFO_CONTENT_HEIGHT;
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = track_y + ((track_h - thumb_h) * device_info_scroll) / max_scroll;
        spr.fillRect(track_x, thumb_y, 3, thumb_h, HEADER_COLOR);
    }
}

// ============================================================================
// EXPANDED FEED OVERLAY — fullscreen activity feed view
// ============================================================================
void draw_feed_expanded_overlay() {
    FeedEntry local_feed[FEED_SIZE];
    int local_count, local_head;
    unsigned long local_now;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    local_count = feed_count;
    local_head  = feed_head;
    for (int i = 0; i < local_count; i++) local_feed[i] = feed_entries[i];
    local_now = millis();
    xSemaphoreGive(dataMutex);

    spr.fillRect(0, 18, DISP_W, DISP_H - 18, BG_COLOR);

    // Title bar
    spr.fillRect(0, 18, DISP_W, 14, CARD_COLOR);
    spr.drawFastHLine(0, 32, DISP_W, CARD_BORDER);
    spr.setTextSize(1);
    spr.setTextColor(HEADER_COLOR, CARD_COLOR);
    spr.setCursor(4, 22);
    kprint(spr, "ACTIVITY FEED");

    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d %s",
             local_count, local_count == 1 ? "entry" : "entries");
    int count_w = (int)strlen(count_str) * 6;
    spr.setTextColor(DIM_COLOR, CARD_COLOR);
    spr.setCursor(DISP_W - count_w - 4, 22);
    spr.print(count_str);

    if (local_count == 0) {
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(1);
        const char* msg = "no activity yet";
        int mw = (int)strlen(msg) * 6;
        spr.setCursor((DISP_W - mw) / 2, DISP_H / 2 - 4);
        spr.print(msg);
    } else {
        const int row_h    = 11;
        const int row_top  = 36;
        const int max_rows = (DISP_H - row_top - 12) / row_h;

        int rendered = 0;
        for (int i = 0; i < local_count && rendered < max_rows; i++) {
            int idx = (local_head - i + FEED_SIZE * 2) % FEED_SIZE;
            FeedEntry& e = local_feed[idx];
            int row_y = row_top + rendered * row_h;

            unsigned long age = local_now - e.timestamp;
            int age_sec = (int)(age / 1000UL);

            uint16_t proto_col = (e.proto == 0) ? CAUTION_COLOR : PURPLE_COLOR;
            if (e.is_flock) proto_col = lerp_col16(proto_col, ACCENT_COLOR, 0.5f);

            const char* strength_str;
            uint16_t strength_col;
            if (e.rssi > -60)      { strength_str = "STR"; strength_col = ACCENT_COLOR; }
            else if (e.rssi > -80) { strength_str = "MED"; strength_col = CAUTION_COLOR; }
            else                   { strength_str = "WK";  strength_col = DIM_COLOR; }

            spr.setTextSize(1);

            char prefix[6];
            snprintf(prefix, sizeof(prefix), "[%c%s]",
                     e.proto == 0 ? 'W' : 'B', e.is_flock ? "*" : "");
            spr.setTextColor(proto_col, BG_COLOR);
            spr.setCursor(4, row_y);
            spr.print(prefix);

            char name_disp[13];
            strncpy(name_disp, e.name, 12); name_disp[12] = '\0';
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setCursor(32, row_y);
            spr.print(name_disp);

            int mac_len = (int)strlen(e.mac);
            const char* mac_tail = (mac_len > 5) ? e.mac + (mac_len - 5) : e.mac;
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setCursor(108, row_y);
            spr.print(mac_tail);

            char rssi_str[8];
            snprintf(rssi_str, sizeof(rssi_str), "%d", e.rssi);
            spr.setTextColor(TEXT_COLOR, BG_COLOR);
            spr.setCursor(144, row_y);
            spr.print(rssi_str);

            spr.setTextColor(strength_col, BG_COLOR);
            spr.setCursor(174, row_y);
            spr.print(strength_str);

            char age_str[8];
            if (age_sec < 60) snprintf(age_str, sizeof(age_str), "%ds", age_sec);
            else              snprintf(age_str, sizeof(age_str), "%dm", age_sec / 60);
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setCursor(200, row_y);
            spr.print(age_str);

            rendered++;
        }
    }

    spr.setTextColor(DIM_COLOR, BG_COLOR);
    spr.setTextSize(1);
    spr.setCursor(4, DISP_H - 9);
    spr.print("f or ESC to close");
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
    if (show_locator_help && current_screen == 1) draw_locator_help_overlay();
    draw_toast_spr();
}

void transition_screen(int new_screen, int dir) {
    if (stealth_mode) { current_screen = new_screen; return; }
    if (!is_muted) {
        int prev_vol = current_volume;
        M5Cardputer.Speaker.setVolume(max(prev_vol, 35));
        M5Cardputer.Speaker.playRaw(ui_beep_pcm, UI_BEEP_SAMPLES, UI_BEEP_RATE, false, 1, 0, false);
        M5Cardputer.Speaker.setVolume(prev_vol);
    }
    if (new_screen == 2) {
        history_scroll_offset = 0;
        history_selected_idx = 0;
        hist_detail_open = false;
        load_sd_history();
        sd_hist_dirty = false;
    }
    if (new_screen == 4) {
        device_info_scroll = 0;
    }
    if (new_screen != 1) show_locator_help = false;
    current_screen = new_screen;
    draw_current_screen();

    const int STEP = 30;
    const unsigned long FRAME_MS = 5;
    unsigned long frame_due = millis();

    if (dir > 0) {
        for (int x = DISP_W; x >= 0; x -= STEP) {
            if (trigger_alarm_confidence > 0) break; 
            while (millis() < frame_due) { vTaskDelay(1 / portTICK_PERIOD_MS); }
            spr.pushSprite(x, 0);
            frame_due += FRAME_MS;
        }
    } else {
        for (int x = -DISP_W; x <= 0; x += STEP) {
            if (trigger_alarm_confidence > 0) break;
            while (millis() < frame_due) { vTaskDelay(1 / portTICK_PERIOD_MS); }
            spr.pushSprite(x, 0);
            frame_due += FRAME_MS;
        }
    }
    spr.pushSprite(0, 0);
}

// ============================================================================
// SYSTEM SETUP
// ============================================================================
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    // dataMutex MUST be created before any task that uses it is spawned.
    // chargeLedTask (started just below) takes this mutex on every loop iteration.
    dataMutex = xSemaphoreCreateMutex();

    // LED: start dark, then spawn the persistent breathing task.
    set_cardputer_led(0, 0, 0);
    // Task runs forever on Core 0 / priority 5. Never deleted — toggle led_breathing_on.
    xTaskCreatePinnedToCore(chargeLedTask, "ChargeLed", 4096, NULL, 5, &chargeLedTaskHandle, 0);

    M5Cardputer.Speaker.setVolume(0);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(255);
    brightness_level = 2;
    apply_color_palette();

    delay(100); SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN); delay(100);
    WiFi.mode(WIFI_STA); delay(50);

    spr.setColorDepth(16);
    spr.createSprite(DISP_W, DISP_H);
    
    // Prime the EMA filter to eliminate startup ADC noise before taking the baseline
    for (int i = 0; i < 20; i++) {
        update_load_sag();
        get_filtered_voltage();
        delay(2);
    }

    Serial.begin(115200); delay(200);  
    setCpuFrequencyMhz(240); ble_event_queue = xQueueCreate(15, sizeof(BleEventData*));
    xTaskCreatePinnedToCore(ble_worker_task, "BLEWorker", 6144, NULL, 1, NULL, 1);

    memset(seen_mac_table, 0, sizeof(seen_mac_table));

    if (!LittleFS.begin(true)) { littlefs_available = false; } 
    else { 
        littlefs_available = true;
        load_session_from_flash();
        load_detections_from_flash();
    }

    // First-boot WiFi credential initialization from #defines if flash is empty
    if (strlen(export_ssid) == 0 && strcmp(EXPORT_WIFI_SSID, "YOUR_SSID_HERE") != 0) {
        strncpy(export_ssid, EXPORT_WIFI_SSID, sizeof(export_ssid) - 1);
        export_ssid[sizeof(export_ssid) - 1] = '\0';
    }
    if (strlen(export_pass) == 0 && strcmp(EXPORT_WIFI_PASS, "YOUR_PASSWORD_HERE") != 0) {
        strncpy(export_pass, EXPORT_WIFI_PASS, sizeof(export_pass) - 1);
        export_pass[sizeof(export_pass) - 1] = '\0';
    }

    lifetime_boots++;
    save_session_to_flash();

    bool mount_success = false;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_CS_PIN);
    static const uint32_t sd_speeds[] = {4000000, 10000000, 20000000};
    for (int si = 0; si < 3 && !mount_success; si++) {
        for (int attempt = 0; attempt < 3 && !mount_success; attempt++) {
            if (SD.begin(SD_CS_PIN, SPI, sd_speeds[si])) { mount_success = true; }
            else { SD.end(); delay(80); }
        }
    }
    if (mount_success) {
        sd_available = true; current_log_file = "/FlockLog.csv";
        if (!SD.exists(current_log_file.c_str())) {
            File file = SD.open(current_log_file.c_str(), FILE_WRITE);
            if (file) { file.println("Uptime_ms,EpochUTC,EpochIsGPS,Channel,Type,Proto,RSSI,MAC,Name,TXPower,Method,Conf,ConfLabel,Extra,SeqNum,Lat,Lon,SpeedMPH,HeadingDeg,AltM"); file.close(); }
        }
        if (!SD.exists(current_pcap_file.c_str())) {
            File pfile = SD.open(current_pcap_file.c_str(), FILE_WRITE);
            if (pfile) {
                uint32_t pcap_header[6] = {0xa1b2c3d4, 0x00040002, 0x00000000, 0x00000000, 0x0000ffff, 0x00000069};
                pfile.write((const uint8_t*)pcap_header, 24); pfile.close();
            }
        }
        if (!SD.exists(current_ble_pcap_file.c_str())) {
            File bfile = SD.open(current_ble_pcap_file.c_str(), FILE_WRITE);
            if (bfile) {
                uint32_t ble_pcap_header[6] = {0xa1b2c3d4, 0x00040002, 0x00000000, 0x00000000, 0x0000ffff, 0x000000fb};
                bfile.write((const uint8_t*)ble_pcap_header, 24); bfile.close();
            }
        }
    }

    session_start_time = millis();

    draw_current_screen(); spr.pushSprite(0,0);

    if (!is_muted) {
        int boot_vol = current_volume > 50 ? 50 : current_volume;
        M5Cardputer.Speaker.setVolume(boot_vol);
        delay(200);
        M5Cardputer.Speaker.tone(1320, 220); delay(320);
        M5Cardputer.Speaker.tone(880,  220); delay(320);
        M5Cardputer.Speaker.tone(660,  220); delay(320);
        M5Cardputer.Speaker.tone(1760, 320); delay(720);
    }
    M5Cardputer.Speaker.setVolume(current_volume);

    NimBLEDevice::init(""); NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    pBLEScan = NimBLEDevice::getScan(); pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), false);
    pBLEScan->setActiveScan(false); pBLEScan->setInterval(97); pBLEScan->setWindow(97);

    WiFi.disconnect(); delay(100); esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t filt; filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt); esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); delay(100);

    last_channel_hop = millis(); last_sd_flush = millis(); last_persist_save = millis();
    // Task watchdog: 10-second timeout, panic on trigger
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);
    // Note: if the struct API causes a compile error, fallback is: esp_task_wdt_init(10, true);
    xTaskCreatePinnedToCore(ScannerLoopTask, "ScannerTask", 8192, NULL, 1, &ScannerTaskHandle, 0);
    xTaskCreatePinnedToCore(GPSLoopTask, "GPSTask", 4096, NULL, 1, &GPSTaskHandle, 0);
    last_user_input_ms = millis();
    system_fully_booted = true;
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    M5Cardputer.update(); yield();

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

    process_wifi_event_queue();
    feed_commit_pending();

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

    if (M5Cardputer.BtnA.wasClicked() && !stealth_mode) {
        last_user_input_ms = millis();
        if (ambient_mode) {
            ambient_mode = false;
            M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]);
        }
        int next_screen = current_screen + 1;
        int dir = (next_screen >= NUM_SCREENS) ? -1 : 1;
        if (next_screen >= NUM_SCREENS) next_screen = 0;
        transition_screen(next_screen, dir);
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        last_user_input_ms = millis();
        if (ambient_mode) {
            ambient_mode = false;
            M5Cardputer.Display.setBrightness(BRIGHTNESS_LEVELS[brightness_level]);
        }
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        
        if (status.tab && !stealth_mode) {
            show_help_overlay = !show_help_overlay;
            if (show_help_overlay) {
                help_ease = 0.0f;
                help_ease_start = millis();
            }
            draw_current_screen(); spr.pushSprite(0,0);
        }

        for (auto c : status.word) {

            if (c == 0x1B && !stealth_mode) {  // ASCII Escape → jump to Scanner
                if (show_feed_expanded) { show_feed_expanded = false; }
                else if (show_help_overlay) { show_help_overlay = false; }
                else if (show_locator_help) { show_locator_help = false; }
                else if (current_screen == 2 && hist_detail_open) { hist_detail_open = false; }
                else if (current_screen != 0) { transition_screen(0, -1); continue; }
                draw_current_screen(); spr.pushSprite(0, 0);
            }
            else if (c == 'n') {
                night_mode = !night_mode; apply_color_palette();
                draw_current_screen(); spr.pushSprite(0,0);
            } 
            else if (c == 'q' || c == 'm') { 
                is_muted = !is_muted; draw_current_screen(); spr.pushSprite(0,0);
                if (!is_muted) beep(600, 50); 
            }
            else if (c == '.') {
                if (current_screen == 2) {
                    int hist_total = sd_available ? sd_hist_count : capture_history_count;
                    history_selected_idx++;
                    if (history_selected_idx >= hist_total) history_selected_idx = max(0, hist_total - 1);
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else if (current_screen == 4) {
                    int visible_h = DISP_H - 18;
                    int max_scroll = DEVICE_INFO_CONTENT_HEIGHT - visible_h;
                    if (max_scroll < 0) max_scroll = 0;
                    device_info_scroll += 12;
                    if (device_info_scroll > max_scroll) device_info_scroll = max_scroll;
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
            }
            else if (c == ';') {
                if (current_screen == 2) {
                    history_selected_idx--;
                    if (history_selected_idx < 0) history_selected_idx = 0;
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else if (current_screen == 4) {
                    device_info_scroll -= 12;
                    if (device_info_scroll < 0) device_info_scroll = 0;
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
            }
            else if (c == '-') {
                current_volume -= 15; if (current_volume < 0) current_volume = 0;
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
                current_volume += 15; if (current_volume > 255) current_volume = 255;
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
                    } else {
                        log_detection("SIMULATION", "BLE", random(-90, -40), fake_mac, "Test_BLE", 0, 0, "Adv", "manual_test", 100, 1);
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
                    int t_conf = capture_history[target_select_idx].confidence;
                    xSemaphoreGive(dataMutex);
                    
                    locator_start(t_mac, t_name, capture_history[target_select_idx].type);
                    trigger_toast("TARGET", t_name, t_conf);
                    transition_screen(1, 1);
                } else if (!stealth_mode) {
                    trigger_toast("INFO", "No targets yet", 0);
                }
            }
            else if (c == '/' || c == '>') {
                if (!stealth_mode) {
                    int next = current_screen + 1;
                    int d = (next >= NUM_SCREENS) ? -1 : 1;
                    if (next >= NUM_SCREENS) next = 0;
                    transition_screen(next, d);
                }
            }
            else if (c == ',' || c == '<') {
                if (!stealth_mode) {
                    int prev = current_screen - 1;
                    int d = (prev < 0) ? 1 : -1;
                    if (prev < 0) prev = NUM_SCREENS - 1;
                    transition_screen(prev, d);
                }
            }
            else if (c >= '1' && c <= '5') {
                if (!stealth_mode) { 
                    int target = c - '1'; 
                    if (target < NUM_SCREENS) transition_screen(target, (target >= current_screen) ? 1 : -1); 
                } 
            } 
            else if (c == '0') { if (!stealth_mode) transition_screen(1, 1); } 
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
            else if (c == 'o') {
                if (!stealth_mode) {
                    xSemaphoreTake(dataMutex, portMAX_DELAY);
                    session_wifi = 0;
                    session_ble = 0;
                    session_flock_wifi = 0;
                    session_flock_ble = 0;
                    session_raven = 0;
                    session_start_time = millis();
                    xSemaphoreGive(dataMutex);
                    trigger_toast("INFO", "Session reset", 0);
                    beep(800, 60);
                    draw_current_screen();
                    spr.pushSprite(0, 0);
                }
            }
            else if (c == 'g') {
                if (current_screen == 1) {
                    // Locator screen: toggle Pointing North mode
                    north_mode = !north_mode;
                    draw_current_screen(); spr.pushSprite(0, 0);
                }
                // On other screens, 'g' is currently a no-op.
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
            else if (c == 'c') {
                // Cycle LED color
                led_col_idx = (led_col_idx + 1) % (int)(sizeof(LED_COLORS) / sizeof(LED_COLORS[0]));
                led_r = LED_COLORS[led_col_idx][0];
                led_g = LED_COLORS[led_col_idx][1];
                led_b = LED_COLORS[led_col_idx][2];
                if (!led_breathing_on) { led_breathing_on = true; }
            }
            else if (c == 'e') {
                if (!stealth_mode) {
                    if (export_mode_active) {
                        export_mode_stop();
                    } else {
                        export_mode_start();
                    }
                    draw_current_screen();
                    spr.pushSprite(0, 0);
                }
            }
            else if (c == 'f') {
                if (!stealth_mode) {
                    show_feed_expanded = !show_feed_expanded;
                    draw_current_screen();
                    spr.pushSprite(0, 0);
                }
            }
            else if (c == '\n' || c == '\r') {
                if (!stealth_mode) {
                    if (current_screen == 2) {
                        int hist_total = sd_available ? sd_hist_count : capture_history_count;
                        if (hist_total > 0) {
                            hist_detail_open = !hist_detail_open;
                            draw_current_screen(); spr.pushSprite(0, 0);
                        }
                    } else {
                        int next = current_screen + 1;
                        int d = (next >= NUM_SCREENS) ? -1 : 1;
                        if (next >= NUM_SCREENS) next = 0;
                        transition_screen(next, d);
                    }
                }
            }
        }
        if (status.del && !stealth_mode) {
            if (current_screen == 2 && hist_detail_open) {
                hist_detail_open = false;
                draw_current_screen(); spr.pushSprite(0, 0);
            } else {
                int prev = current_screen - 1;
                int d = (prev < 0) ? 1 : -1;
                if (prev < 0) prev = NUM_SCREENS - 1;
                transition_screen(prev, d);
            }
        }
    }

    // Fire pending '\' single-press action if double-tap window expired
    if (bs_pending_exists && millis() >= bs_pending_until) {
        bs_pending_exists = false;
        bs_pending_until = 0;
        led_breathing_on = !led_breathing_on;
        beep(led_breathing_on ? 800 : 400, 30);
    }

    if (millis() - last_time_save >= 1000) { xSemaphoreTake(dataMutex, portMAX_DELAY); lifetime_seconds++; xSemaphoreGive(dataMutex); last_time_save = millis(); }
    if (millis() - last_persist_save >= PERSIST_INTERVAL_MS) save_session_to_flash();
    rssi_track_expire();

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
        if (was_dirty) load_sd_history();
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
        if (millis() - last_ambient_draw > 2000) {
            long det_total;
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            det_total = session_flock_wifi + session_flock_ble + session_raven;
            xSemaphoreGive(dataMutex);

            spr.fillSprite(BG_COLOR);
            char det_buf[16];
            snprintf(det_buf, sizeof(det_buf), "%ld", det_total);
            spr.setTextDatum(MC_DATUM);
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setTextSize(3);
            spr.drawString(det_buf, DISP_W / 2, DISP_H / 2 - 6);
            spr.setTextSize(1);
            spr.drawString("DETECTIONS", DISP_W / 2, DISP_H / 2 + 16);
            spr.setTextDatum(TL_DATUM);
            spr.pushSprite(0, 0);
            last_ambient_draw = millis();
        }
    } else if (stealth_mode) {
        // Minimal stealth render: tiny dim "S" in bottom-right corner every 2s.
        static unsigned long last_stealth_draw = 0;
        if (millis() - last_stealth_draw > 2000) {
            spr.fillSprite(BG_COLOR);
            uint16_t s_col = lerp_col16(BG_COLOR, lgfx::color565(180, 30, 30), 0.6f);
            spr.setTextColor(s_col, BG_COLOR);
            spr.setTextSize(1);
            spr.setCursor(DISP_W - 8, DISP_H - 9);
            spr.print("S");
            spr.pushSprite(0, 0);
            last_stealth_draw = millis();
        }
    } else {
        static unsigned long last_fast_anim = 0; static unsigned long last_slow_ui = 0; unsigned long now = millis();

        if (current_screen == 0 || current_screen == 1 || current_screen == 3 || show_vol_overlay || toast_active || (now - last_fast_anim < 30)) {
            if (now - last_fast_anim >= 15) { draw_current_screen(); spr.pushSprite(0, 0); last_fast_anim = now; }
        }
        else { if (now - last_slow_ui >= 100) { draw_current_screen(); spr.pushSprite(0, 0); last_slow_ui = now; } }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
}