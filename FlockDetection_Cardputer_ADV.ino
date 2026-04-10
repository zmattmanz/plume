// ============================================================================
// FLOCK DETECTOR v8.4-ADV — Tactical Edition (Stable Release)
// ============================================================================
// v8.4 Signature Database Corrections (Medium/Low Priority):
//
//   [FIX 1 — MISSED DETECTIONS] b4:1e:52 (Flock's own IEEE-registered OUI),
//   90:35:ea, f0:82:c0, b4:e3:f9, and 04:0d:84 (confirmed FS Ext Battery
//   BLE OUIs) promoted from Tier 2 to Tier 1. Previously these scored
//   SCORE_WEAK (25) alone — below any useful threshold. Now SCORE_STRONG (60),
//   compounding correctly with name/UUID/mfg signals.
//
//   [FIX 2 — FALSE POSITIVES] cc:cc:cc demoted from Tier 1 to Tier 2.
//   Not a real IEEE OUI — it is a locally-administered/placeholder pattern
//   that appears in spoofed, misconfigured, and non-Flock devices.
//
//   [FIX 3 — FALSE POSITIVES] 08:3a:88 and d8:f3:bc (Liteon Technology WiFi
//   module OUIs) demoted from Tier 1 to Tier 2. Both appear in Flock captures
//   but are used extensively in commodity laptops, routers, and generic IoT
//   hardware. At Tier 1 they could solo-alarm on non-Flock devices.
//
//   [FIX 4 — MISSED DETECTIONS] "test_flck" added to wifi_ssid_patterns[].
//   CVE-2025-59409: hardcoded development SSID broadcast by Flock cameras
//   during certain firmware states. Was previously undetected.
//
//   [FIX 5 — FALSE POSITIVES] static_addr bonus now requires corroboration.
//   Previously fired on any BLE device with a public or random-static address,
//   which describes the majority of BLE peripherals. Now only contributes
//   SCORE_WEAK when mac_score==2 or got_penguin_name is already true.
//
// v8.3 Detection Logic Fixes (High Priority — unchanged):
//
//   [FIX 1 — MISSED DETECTIONS] 0x09C8 manufacturer ID now scores
//   SCORE_DEFINITIVE (100) unconditionally.
//
//   [FIX 2 — FALSE POSITIVES] Raven UUID scoring now requires EITHER at least
//   one custom UUID (0x3100–0x3500) OR two or more total matching Raven UUIDs.
//
//   [FIX 3 — FALSE POSITIVES] Penguin numeric names (8–12 digit all-numeric)
//   now contribute SCORE_WEAK (25) as corroborating evidence only.
//
// ============================================================================

#include <M5Cardputer.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <vector>
#include <algorithm> 
#include "esp_wifi.h"
#include "esp_wifi_types.h"
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
void draw_header_spr(int screen_num);
void draw_toast_spr();
void draw_vol_overlay();
void drawCard(int x, int y, int w, int h);
void draw_current_screen();
void transition_screen(int new_screen, int dir);
void play_escalated_alarm(int confidence);
void dedicated_charging_loop();
void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b);
void beep(int frequency, int duration_ms);
void apply_color_palette();
void draw_help_overlay();
void draw_arrow(int cx, int cy, int r, float angle_deg, uint16_t fill_col, uint16_t line_col);
void gps_page_toggle();

// ============================================================================
// DISPLAY & PALETTE VARIABLES (Swappable for Night Mode)
// ============================================================================
#define DISP_W       240
#define DISP_H       135

uint16_t BG_COLOR, CARD_COLOR, CARD_BORDER, PANEL_BG, HEADER_COLOR, TEXT_COLOR;
uint16_t DIM_COLOR, DIM2_COLOR, ACCENT_COLOR, TEAL_COLOR, PURPLE_COLOR;
uint16_t ALERT_COLOR, TOAST_COLOR, WARN_COLOR, GRID_COLOR, HILIGHT_BG;
bool night_mode = false;
bool show_help_overlay = false;

unsigned long vol_overlay_start = 0;
bool show_vol_overlay = false;

void apply_color_palette() {
    if (night_mode) {
        BG_COLOR      = lgfx::color565(  8,   0,   0); 
        CARD_COLOR    = lgfx::color565( 25,   0,   0);
        CARD_BORDER   = lgfx::color565( 60,   5,   5);
        PANEL_BG      = lgfx::color565(  8,   0,   0);
        HEADER_COLOR  = lgfx::color565(255,  60,  60);
        TEXT_COLOR    = lgfx::color565(220, 200, 200);
        DIM_COLOR     = lgfx::color565(150,  30,  30);
        DIM2_COLOR    = lgfx::color565( 80,  15,  15);
        ACCENT_COLOR  = lgfx::color565(255,  90,  90);
        TEAL_COLOR    = lgfx::color565(220,  60,  60);
        PURPLE_COLOR  = lgfx::color565(255, 100, 255); 
        ALERT_COLOR   = lgfx::color565(255,  10,  10);
        TOAST_COLOR   = lgfx::color565(255, 140,  20); 
        WARN_COLOR    = lgfx::color565(220, 100,  10);
        GRID_COLOR    = lgfx::color565( 50,   5,   5);
        HILIGHT_BG    = lgfx::color565( 45,   0,   0);
    } else {
        BG_COLOR      = lgfx::color565( 10,  20,  48);
        CARD_COLOR    = lgfx::color565( 18,  36,  80);
        CARD_BORDER   = lgfx::color565( 26,  42,  58);
        PANEL_BG      = lgfx::color565( 10,  20,  48);
        HEADER_COLOR  = lgfx::color565(  0, 238, 255);
        TEXT_COLOR    = lgfx::color565(255, 255, 255);
        DIM_COLOR     = lgfx::color565(  0, 170, 204);
        DIM2_COLOR    = lgfx::color565( 26,  42,  58);
        ACCENT_COLOR  = lgfx::color565( 50, 255, 100);
        TEAL_COLOR    = lgfx::color565(  0, 180, 180);
        PURPLE_COLOR  = lgfx::color565(255, 100, 255); 
        ALERT_COLOR   = lgfx::color565(255,  50,  50);
        TOAST_COLOR   = lgfx::color565(255, 224,   0); 
        WARN_COLOR    = lgfx::color565(255, 165,   0); 
        GRID_COLOR    = lgfx::color565( 26,  42,  58);
        HILIGHT_BG    = lgfx::color565( 18,  36,  80);
    }
}

// ============================================================================
// CONFIGURATION
// ============================================================================
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
#define DWELL_PRIMARY   650  
#define DWELL_SECONDARY 200

#define REDETECT_WINDOW_MS 300000

#define RSSI_TRACK_MAX_DEVICES 16
#define RSSI_TRACK_SAMPLES 5
#define RSSI_TRACK_EXPIRY_MS 15000

#define PERSIST_INTERVAL_MS 60000
#define PERSIST_FILE "/flock_session.dat"
#define TOAST_DURATION_MS 3500

#define LOC_MAX_SAMPLES 40
#define LOC_SAMPLE_INTERVAL 500
#define LOC_PATH_LOSS_N 2.5
#define LOC_MIN_SAMPLES_EST 3
#define LOC_ARROW_RADIUS 28

#define SCORE_DEFINITIVE 100  
#define SCORE_STRONG     60
#define SCORE_WEAK       25   
#define SCORE_BONUS_RSSI 10   
#define SCORE_BONUS_STAT 15   

// RAISED TO 75 TO ELIMINATE LITEON/RING CAMERA FALSE POSITIVES
#define CONFIDENCE_ALARM_THRESHOLD 75
#define CONFIDENCE_HIGH            85
#define CONFIDENCE_CERTAIN         100

#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_CS_PIN       12

// Version string — single source of truth, used in draw_device_info_screen().
#define VERSION_STRING "FLOCK DETECTOR v8.4 [ADV]"

// Compile-time guard: screen name array in draw_header_spr() must stay in sync.
#define NUM_SCREENS 6

// ============================================================================
// GLOBALS & STRUCTS
// ============================================================================
M5Canvas spr(&M5Cardputer.Display);

TaskHandle_t ScannerTaskHandle;
TaskHandle_t GPSTaskHandle; 
SemaphoreHandle_t dataMutex;

static uint8_t current_channel = 1;
static unsigned long last_channel_hop = 0;
static unsigned long channel_lock_until = 0; 
static unsigned long last_ble_scan = 0;
static unsigned long last_buzzer_time = 0;
static NimBLEScan* pBLEScan;
bool sd_available = false;
bool littlefs_available = false;
volatile int trigger_alarm_confidence = 0;
volatile bool is_alarming = false; 

#define SD_LINE_LEN 512
char sd_write_buffer[MAX_LOG_BUFFER][SD_LINE_LEN];
int  sd_write_count = 0;
unsigned long last_sd_flush = 0;

String current_log_file = "/FlockLog.csv";
String current_pcap_file = "/Threats.pcap";

int current_screen = 0;
bool stealth_mode = false;
bool is_muted = false;       
int current_volume = 150;    

long session_wifi = 0;
long session_ble = 0;
long session_flock_wifi = 0;
long session_flock_ble = 0;
long session_raven = 0;
volatile long session_wifi_packets = 0;
volatile long session_ble_packets  = 0;

unsigned long session_start_time = 0;
unsigned long lifetime_seconds = 0;

long lifetime_wifi = 0;
long lifetime_ble = 0;
long lifetime_flock_total = 0;
long lifetime_boots = 0;

#define MAX_SEEN_MACS 200

struct SeenMacEntry {
    char   mac[18];
    unsigned long ts;
    bool   occupied;
};
static SeenMacEntry seen_mac_table[MAX_SEEN_MACS * 2];

static uint32_t hash_mac(const char* mac) {
    uint32_t h = 2166136261u;
    for (const char* p = mac; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

static const int SEEN_MAC_TABLE_SIZE = MAX_SEEN_MACS * 2;

bool is_mac_recently_seen(const String& mac) {
    const char* key = mac.c_str();
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

void add_seen_mac(const String& mac) {
    const char* key = mac.c_str();
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
    int oldest = 0;
    for (int i = 1; i < SEEN_MAC_TABLE_SIZE; i++) {
        if (seen_mac_table[i].ts < seen_mac_table[oldest].ts) oldest = i;
    }
    strncpy(seen_mac_table[oldest].mac, key, 17);
    seen_mac_table[oldest].mac[17] = '\0';
    seen_mac_table[oldest].ts       = millis();
    seen_mac_table[oldest].occupied = true;
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
    char type[16];
    char mac[18];
    char name[65];
    int  rssi;
    int  confidence;
    char time[9];
};
CaptureEntry capture_history[CAPTURE_HISTORY_SIZE];
int capture_history_count = 0;

char toast_text[32]       = "";
unsigned long toast_start = 0;
bool toast_active         = false;

unsigned long last_time_save = 0;
unsigned long last_sd_flush_check = 0;
unsigned long last_persist_save = 0;
unsigned long last_blip_time = 0;

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

TinyGPSPlus gps;
HardwareSerial SerialGPS(2);

static float imu_px = 0.0f;
static float imu_py = 0.0f;
static bool  imu_available = false;

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
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
    neopixelWrite(21, r, g, b); 
#endif
}

// ============================================================================
// BATTERY ENGINE
// ============================================================================
#define BATTERY_SAMPLE_SIZE 15
int16_t battery_buffer[BATTERY_SAMPLE_SIZE] = {0};
uint8_t battery_index = 0;

void update_battery_metrics() {
    battery_buffer[battery_index] = M5Cardputer.Power.getBatteryVoltage();
    battery_index = (battery_index + 1) % BATTERY_SAMPLE_SIZE;
}

static int32_t cached_median_mv = 0;
static uint8_t last_median_battery_index = 255;

int32_t get_median_voltage() {
    if (battery_index == last_median_battery_index && cached_median_mv != 0) {
        return cached_median_mv;
    }
    int16_t sorted[BATTERY_SAMPLE_SIZE];
    memcpy(sorted, battery_buffer, sizeof(battery_buffer));
    std::sort(sorted, sorted + BATTERY_SAMPLE_SIZE);
    cached_median_mv = (sorted[BATTERY_SAMPLE_SIZE / 2] == 0)
        ? M5Cardputer.Power.getBatteryVoltage()
        : (int32_t)sorted[BATTERY_SAMPLE_SIZE / 2];
    last_median_battery_index = battery_index;
    return cached_median_mv;
}

uint8_t get_unified_battery_pct(int16_t mv) {
    const int16_t VOLT_MAX     = 3550; 
    const int16_t VOLT_HIGH    = 3450; 
    const int16_t VOLT_NOMINAL = 3300; 
    const int16_t VOLT_LOW     = 3100; 
    const int16_t VOLT_MIN     = 2800; 

    if (mv >= VOLT_MAX) return 100;
    if (mv <= VOLT_MIN) return 0;
    if (mv > VOLT_HIGH) return map(mv, VOLT_HIGH, VOLT_MAX, 80, 100);
    else if (mv > VOLT_NOMINAL) return map(mv, VOLT_NOMINAL, VOLT_HIGH, 50, 80);
    else if (mv > VOLT_LOW) return map(mv, VOLT_LOW, VOLT_NOMINAL, 20, 50);
    else return map(mv, VOLT_MIN, VOLT_LOW, 0, 20);
}

bool is_device_charging(int32_t current_mv) {
    static bool chg_state = false;
    static int32_t baseline_mv = 0;
    static unsigned long last_check = 0;
    
    if (baseline_mv == 0) baseline_mv = current_mv;
    if (current_mv > 3900) { chg_state = true; baseline_mv = current_mv; }
    
    if (millis() - last_check > 2500) {
        if (current_mv - baseline_mv >= 15) { chg_state = true; baseline_mv = current_mv; }
        else if (baseline_mv - current_mv >= 15) { chg_state = false; baseline_mv = current_mv; }
        else { baseline_mv = (baseline_mv + current_mv) / 2; }
        
        if (current_mv < 3800 && !chg_state && (baseline_mv - current_mv > 0)) chg_state = false; 
        if (current_mv > 3900) chg_state = true;
        
        last_check = millis();
    }
    return chg_state;
}

void dedicated_charging_loop() {
    M5Cardputer.Speaker.stop();
    int display_pct = -1; 
    uint8_t last_led_val = 255;
    unsigned long boot_time = millis();

    const unsigned long CHARGE_AUTO_BOOT_MS = 30UL * 60UL * 1000UL;
    
    while (true) {
        M5Cardputer.update();
        update_battery_metrics(); 
        int32_t current_mv = get_median_voltage();
        int calc_pct = get_unified_battery_pct(current_mv);
        
        if (display_pct == -1) display_pct = calc_pct;
        else if (calc_pct > display_pct) display_pct = calc_pct;

        unsigned long elapsed = millis() - boot_time;

        spr.fillSprite(BG_COLOR);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); 
        spr.setTextSize(3); 
        spr.setCursor(48, 20); spr.print("CHARGING");
        spr.setTextColor(TEXT_COLOR, BG_COLOR); 
        spr.setTextSize(4); 
        spr.setCursor(20, 55); spr.print("USB LINK"); 
        spr.setTextColor(DIM2_COLOR, BG_COLOR); 
        spr.setTextSize(1); 
        spr.setCursor(DISP_W - 40, DISP_H - 10); spr.printf("%dmV", current_mv);

        unsigned long remaining_ms = (elapsed < CHARGE_AUTO_BOOT_MS)
                                     ? (CHARGE_AUTO_BOOT_MS - elapsed) : 0;
        if (remaining_ms < 60000UL) {
            spr.setTextColor(WARN_COLOR, BG_COLOR);
            spr.setCursor(4, DISP_H - 10);
            spr.printf("AUTO-BOOT IN %lus", remaining_ms / 1000UL);
        }
        
        int bar_x = 40, bar_y = 95, bar_w = 160, bar_h = 10;
        spr.drawRect(bar_x, bar_y, bar_w, bar_h, ACCENT_COLOR); 
        int fill_w = (millis() / 15) % (bar_w - 2); 
        if (fill_w > 0) spr.fillRect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, ACCENT_COLOR);
        
        spr.setTextColor(DIM_COLOR, BG_COLOR); 
        spr.setTextSize(1); 
        spr.setCursor(35, 115); spr.print("PRESS ANY KEY TO BOOT DEVICE"); 
        spr.pushSprite(0, 0);

        float t_breath = millis() / 2000.0f * (float)M_PI;
        float breathe = sinf(t_breath);
        breathe = breathe * breathe;
        uint8_t led_val = (uint8_t)(breathe * 80.0f);
        if (led_val != last_led_val) { set_cardputer_led(0, led_val, 0); last_led_val = led_val; }

        if (current_mv < 3200 && elapsed > 3000) { 
            M5Cardputer.Display.fillScreen(BG_COLOR); set_cardputer_led(0,0,0); return; 
        }
        if (elapsed > 1500 && M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            M5Cardputer.Display.fillScreen(BG_COLOR); set_cardputer_led(0,0,0); return; 
        }
        if (elapsed >= CHARGE_AUTO_BOOT_MS) {
            M5Cardputer.Display.fillScreen(BG_COLOR); set_cardputer_led(0,0,0); return;
        }
        delay(15);
    }
}

// ============================================================================
// STRING SCRUBBER (Fixes Apple "Smart Quotes" & UTF-8 Garbage safely)
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
    "flock", "Flock", "FLOCK", "FS Ext Battery", "FS_",
    "Penguin", "Pigvision", "FlockOS", "flocksafety", "OFS_IoT", "PFS_",
    "test_flck"  // CVE-2025-59409: hardcoded dev SSID broadcast during certain firmware states
};
static const int NUM_SSID_PATTERNS = sizeof(wifi_ssid_patterns) / sizeof(wifi_ssid_patterns[0]);

// v8.4: MAC tier corrections
//   PROMOTED to Tier 1 (confirmed Flock hardware, were incorrectly in T2):
//     b4:1e:52 — Flock Safety's own IEEE-registered OUI
//     90:35:ea, f0:82:c0, b4:e3:f9, 04:0d:84 — confirmed FS Ext Battery BLE OUIs
//   DEMOTED to Tier 2 (insufficient specificity for a solo SCORE_STRONG alarm):
//     08:3a:88, d8:f3:bc — Liteon Technology WiFi module OUIs; appear in Flock
//                          captures but are used across laptops/routers/IoT gear
//     cc:cc:cc — not a real IEEE OUI; locally-administered/placeholder pattern
static const char* mac_prefixes_tier1[] = {
    "ec:1b:bd", "58:8e:81", "d8:a0:d8",
    "dc:54:75", "e0:e2:e6", "f0:9f:c2", "68:b6:b3", "a0:b7:65", "24:0a:c4",
    "a4:e5:7c", "78:e3:6d", "fc:f5:c4", "b0:b2:1c",
    // confirmed Flock-registered OUIs (promoted from T2 in v8.4)
    "b4:1e:52", "90:35:ea", "f0:82:c0", "b4:e3:f9", "04:0d:84"
};
static const int NUM_MAC_TIER1 = sizeof(mac_prefixes_tier1) / sizeof(mac_prefixes_tier1[0]);

static const char* mac_prefixes_tier2[] = {
    "48:e7:29", "74:4c:a1", "94:34:69", "38:5b:44",
    "94:08:53", "1c:34:f1", "a4:cf:12",
    "3c:91:80", "80:30:49", "14:5a:fc", "9c:2f:9d", "e4:aa:ea",
    "c8:c9:a3", "70:c9:4e", "d0:39:57",
    "24:b2:b9", "00:f4:8d", "e0:0a:f6",
    // demoted from T1 in v8.4 (Liteon WiFi modules + non-IEEE placeholder)
    "08:3a:88", "d8:f3:bc", "cc:cc:cc"
};
static const int NUM_MAC_TIER2 = sizeof(mac_prefixes_tier2) / sizeof(mac_prefixes_tier2[0]);

static const char* device_name_patterns[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision", "FlockCam", "FS-", "RWLS-"
};
static const int NUM_NAME_PATTERNS = sizeof(device_name_patterns) / sizeof(device_name_patterns[0]);

// ============================================================================
// RAVEN UUID SETS — SPLIT INTO CUSTOM (DEFINITIVE) AND STANDARD SIG (CORROBORATING)
//
// [v8.3 FIX 2] Standard SIG UUIDs (0x180A, 0x1809, 0x1819) are used by
// thousands of non-Flock devices (fitness trackers, thermometers, hearing aids,
// etc.). A single match must NOT set got_raven=true.
//
// Custom UUIDs (0x3100–0x3500) are Flock-proprietary, unregistered with
// Bluetooth SIG, and strongly indicate Raven hardware. Any single custom
// UUID match is sufficient to set got_raven=true.
//
// A device with 2+ total Raven UUIDs (even all standard) also sets got_raven=true,
// since the combination is far more specific than any single standard service.
// ============================================================================
static const char* raven_custom_service_uuids[] = {
    "00003100-0000-1000-8000-00805f9b34fb",  // GPS service (Flock-proprietary)
    "00003200-0000-1000-8000-00805f9b34fb",  // Power service (Flock-proprietary)
    "00003300-0000-1000-8000-00805f9b34fb",  // Network service (Flock-proprietary)
    "00003400-0000-1000-8000-00805f9b34fb",  // Uptime service (Flock-proprietary)
    "00003500-0000-1000-8000-00805f9b34fb",  // Error service (Flock-proprietary)
};
static const int NUM_RAVEN_CUSTOM_UUIDS = sizeof(raven_custom_service_uuids) / sizeof(raven_custom_service_uuids[0]);

static const char* raven_standard_service_uuids[] = {
    "0000180a-0000-1000-8000-00805f9b34fb",  // Device Information (standard SIG)
    "00001809-0000-1000-8000-00805f9b34fb",  // Health Thermometer (standard SIG)
    "00001819-0000-1000-8000-00805f9b34fb",  // Location and Navigation (standard SIG)
};
static const int NUM_RAVEN_STANDARD_UUIDS = sizeof(raven_standard_service_uuids) / sizeof(raven_standard_service_uuids[0]);

// Combined array preserved for firmware version detection logic in ble_process_task.
static const char* raven_service_uuids[] = {
    "0000180a-0000-1000-8000-00805f9b34fb", "00003100-0000-1000-8000-00805f9b34fb", 
    "00003200-0000-1000-8000-00805f9b34fb", "00003300-0000-1000-8000-00805f9b34fb", 
    "00003400-0000-1000-8000-00805f9b34fb", "00003500-0000-1000-8000-00805f9b34fb", 
    "00001809-0000-1000-8000-00805f9b34fb", "00001819-0000-1000-8000-00805f9b34fb"
};
static const int NUM_RAVEN_UUIDS = sizeof(raven_service_uuids) / sizeof(raven_service_uuids[0]);

// NOTE: 0x09C8 is not a registered Bluetooth SIG company ID as of the last
// public assignment list. Verify this value against physical hardware captures
// before treating it as definitive. It may be a Flock-internal/prototype ID.
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
    if (mfg_data.length() < 10) return false;
    for (size_t i = 8; i < mfg_data.length() - 1; i++) {
        if (mfg_data[i] == 'T' && mfg_data[i + 1] == 'N') return true;
    }
    return false;
}

// ============================================================================
// HELPER FUNCTIONS & PCAP
// ============================================================================
void format_time_buf(unsigned long total_sec, char* buf, size_t buf_len) {
    unsigned long h = total_sec / 3600;
    unsigned long m = (total_sec / 60) % 60;
    unsigned long s = total_sec % 60;
    snprintf(buf, buf_len, "%02lu:%02lu:%02lu", h, m, s);
}

const char* confidence_label(int score) {
    if (score >= CONFIDENCE_CERTAIN) return "CERTAIN";
    if (score >= CONFIDENCE_HIGH)    return "HIGH";
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return "MEDIUM";
    return "LOW";
}

uint16_t confidence_color(int score) {
    if (score >= CONFIDENCE_CERTAIN) return ACCENT_COLOR;  
    if (score >= CONFIDENCE_HIGH)    return TOAST_COLOR;   
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return WARN_COLOR; 
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

// ============================================================================
// PERSISTENCE & TRACKING
// ============================================================================
static int flash_write_fail_count = 0;
#define FLASH_FAIL_TOAST_THRESHOLD 3

void save_session_to_flash() {
    if (!littlefs_available) return;
    
    long l_wifi, l_ble, l_sec, l_flock, l_boots;
    int l_vol;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    l_wifi = lifetime_wifi; l_ble = lifetime_ble; l_sec = lifetime_seconds; 
    l_flock = lifetime_flock_total; l_vol = current_volume; l_boots = lifetime_boots;
    xSemaphoreGive(dataMutex);

    bool write_ok = false;
    for (int attempt = 0; attempt < 3 && !write_ok; attempt++) {
        File f = LittleFS.open(PERSIST_FILE, "w");
        if (!f) {
            delay(5);
            continue;
        }
        size_t written = f.printf("%ld\n%ld\n%lu\n%ld\n%d\n%ld\n",
                                  l_wifi, l_ble, l_sec, l_flock, l_vol, l_boots);
        f.close();
        if (written > 10) write_ok = true;
    }

    if (write_ok) {
        flash_write_fail_count = 0;
        last_persist_save = millis();
    } else {
        flash_write_fail_count++;
        if (flash_write_fail_count >= FLASH_FAIL_TOAST_THRESHOLD) {
            strncpy(toast_text, "FLASH WRITE FAIL", sizeof(toast_text) - 1);
            toast_text[sizeof(toast_text) - 1] = '\0';
            toast_start = millis();
            toast_active = true;
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
            int peak_idx = 0;
            for (int j = 1; j < n; j++) if (s[j] > s[peak_idx]) peak_idx = j;
            int range = s[peak_idx] - min(s[0], s[n - 1]);
            if (peak_idx > 0 && peak_idx < n - 1 && range >= 6) {
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
#define radar_cx 66
#define radar_cy 60
#define radar_r  52
#define inner_r  20

#define NUM_RADIAL_BANDS 36
float radial_spikes[NUM_RADIAL_BANDS] = {0};
uint16_t radial_colors[NUM_RADIAL_BANDS] = {0}; 
int radial_rssi[NUM_RADIAL_BANDS] = {0}; 
static float hud_rotation = 0.0f;

#define NUM_PARTICLES 24
static float noise_r[NUM_PARTICLES] = {0};
static float noise_a[NUM_PARTICLES] = {0};
static int noise_life[NUM_PARTICLES] = {0};
static int noise_max_life[NUM_PARTICLES] = {0};

void add_blip(uint16_t blip_color, int rssi) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    last_blip_time = millis(); 
    int band = random(0, NUM_RADIAL_BANDS);
    float strength_mult = constrain(map(rssi, -100, -30, 50, 200), 50, 200) / 100.0f;
    
    radial_spikes[band] = 0.5f + strength_mult; 
    radial_colors[band] = blip_color;
    radial_rssi[band] = rssi;
    
    int prev = (band - 1 + NUM_RADIAL_BANDS) % NUM_RADIAL_BANDS;
    int next = (band + 1) % NUM_RADIAL_BANDS;
    if(radial_spikes[prev] < 0.6f) { radial_spikes[prev] = 0.6f; radial_colors[prev] = blip_color; radial_rssi[prev] = rssi - 10;}
    if(radial_spikes[next] < 0.6f) { radial_spikes[next] = 0.6f; radial_colors[next] = blip_color; radial_rssi[next] = rssi - 10;}
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// LOGGING ENGINE
// ============================================================================
void flush_sd_buffer() {
    static char local_log_buf[MAX_LOG_BUFFER][SD_LINE_LEN];
    static PcapQueueItem local_pcap_buf[MAX_PCAP_BUFFER];

    int log_count  = 0;
    int pcap_count = 0;

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
    
    if (log_count > 0 || pcap_count > 0) {
        last_sd_flush = millis();
    }
}

void trigger_toast(const char* type, const char* name, int confidence) {
    const char* src = (name && name[0] != '\0' && strcmp(name, "Hidden") != 0) ? name : type;
    char pct_str[6];
    snprintf(pct_str, sizeof(pct_str), " %d%%", confidence);
    snprintf(toast_text, sizeof(toast_text), "%.14s%s", src, pct_str);
    toast_start = millis();
    toast_active = true;
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

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool is_new = !is_mac_recently_seen(String(mac));

    if (is_new) {
        add_seen_mac(String(mac));
        uint16_t blip_col = ACCENT_COLOR;
        if (strcmp(proto, "WIFI") == 0) {
            session_wifi++; lifetime_wifi++; session_flock_wifi++; blip_col = TOAST_COLOR;
        } else {
            session_ble++; lifetime_ble++; blip_col = PURPLE_COLOR;
        }
        if (strstr(type, "RAVEN") != NULL) { session_raven++; blip_col = TEAL_COLOR; }
        else if (strcmp(proto, "BLE") == 0) { session_flock_ble++; }
        lifetime_flock_total++;
        add_to_capture_history(type, mac, name, rssi, confidence);
        trigger_toast(type, name, confidence);
        xSemaphoreGive(dataMutex);
        add_blip(blip_col, rssi);
        xSemaphoreTake(dataMutex, portMAX_DELAY);
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

    if (is_new && sd_available && sd_write_count < MAX_LOG_BUFFER) {
        char clean_name[64];
        strncpy(clean_name, name, sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0';
        for (char* p = clean_name; *p; p++) if (*p == ',') *p = ' ';

        char clean_extra[64];
        strncpy(clean_extra, extra_data, sizeof(clean_extra) - 1);
        clean_extra[sizeof(clean_extra) - 1] = '\0';
        for (char* p = clean_extra; *p; p++) if (*p == ',') *p = ' ';

        char gps_fields[80];
        bool gps_fresh = gps.location.isValid() && (gps.location.age() < 2000);
        if (gps_fresh) {
            snprintf(gps_fields, sizeof(gps_fields), "%.6f,%.6f,%.1f,%.1f,%.1f",
                gps.location.lat(), gps.location.lng(),
                gps.speed.isValid()  ? gps.speed.mph()       : 0.0,
                gps.course.isValid() ? gps.course.deg()       : 0.0,
                gps.altitude.isValid()? gps.altitude.meters() : 0.0);
        } else {
            strncpy(gps_fields, "0.000000,0.000000,0.0,0.0,0.0", sizeof(gps_fields));
        }

        snprintf(sd_write_buffer[sd_write_count], SD_LINE_LEN,
            "%lu,%d,%s,%s,%d,%s,%s,%d,%s,%d,%s,%s,%d,%s",
            now_ms, channel, type, proto, rssi, mac, clean_name,
            tx_power, detection_method, confidence,
            confidence_label(confidence), clean_extra, seq_num, gps_fields);
        sd_write_count++;
    }
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// LOCATOR ENGINE
// ============================================================================
double rssi_to_weight(int rssi) { 
    if (rssi > -20) rssi = -20; if (rssi < -100) rssi = -100; return pow(10.0, (double)(rssi + 100) / 20.0); 
}

double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    double dLat = radians(lat2 - lat1); double dLon = radians(lon2 - lon1);
    double a = sin(dLat/2)*sin(dLat/2) + cos(radians(lat1))*cos(radians(lat2))*sin(dLon/2)*sin(dLon/2);
    return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

float bearing_to(double lat1, double lon1, double lat2, double lon2) {
    double dLon = radians(lon2 - lon1); double y = sin(dLon) * cos(radians(lat2));
    double x = cos(radians(lat1)) * sin(radians(lat2)) - sin(radians(lat1)) * cos(radians(lat2)) * cos(dLon);
    float brng = degrees(atan2(y, x)); if (brng < 0) brng += 360.0; return brng;
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
        }
    }
    xSemaphoreGive(dataMutex);
}

void locator_start(const char* mac, const char* name) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = true;
    strncpy(locator_target_mac,  mac,  sizeof(locator_target_mac)  - 1); locator_target_mac[17]  = '\0';
    strncpy(locator_target_name, name, sizeof(locator_target_name) - 1); locator_target_name[64] = '\0';
    locator_sample_count = 0; locator_has_estimate = false; locator_peak_rssi = -120;
    locator_est_distance = 0; locator_bearing = 0; locator_confidence_radius = 0;
    locator_start_time = millis(); locator_newest_sample_ms = 0;
    xSemaphoreGive(dataMutex);
}

void locator_stop() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = false; locator_has_estimate = false; locator_sample_count = 0;
    locator_newest_sample_ms = 0;
    xSemaphoreGive(dataMutex);
}

void draw_arrow(int cx, int cy, int r, float angle_deg, uint16_t fill_col, uint16_t line_col) {
    float a = radians(angle_deg - 90.0f); 
    int tip_x = cx + (int)(r * cosf(a)); int tip_y = cy + (int)(r * sinf(a));
    float a_left  = a + radians(140.0f); float a_right = a - radians(140.0f);
    int left_x  = cx + (int)((r * 0.5f) * cosf(a_left)); int left_y  = cy + (int)((r * 0.5f) * sinf(a_left));
    int right_x = cx + (int)((r * 0.5f) * cosf(a_right)); int right_y = cy + (int)((r * 0.5f) * sinf(a_right));
    spr.fillTriangle(tip_x, tip_y, left_x, left_y, right_x, right_y, fill_col);
    spr.drawTriangle(tip_x, tip_y, left_x, left_y, right_x, right_y, line_col);
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
    uint8_t  payload_snap[64];
    uint16_t payload_snap_len;
    uint16_t orig_len;
    volatile bool ready;
};

static WifiEvent wifi_event_queue[WIFI_EVENT_QUEUE_SIZE];
static volatile uint8_t wifi_eq_write_idx = 0;
static uint8_t          wifi_eq_read_idx  = 0;

void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buff;
    if (ppkt->rx_ctrl.sig_len < 24) return;

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
    ev->payload_snap_len = (ppkt->rx_ctrl.sig_len < 64) ? (uint16_t)ppkt->rx_ctrl.sig_len : 64;
    memcpy(ev->payload_snap, ppkt->payload, ev->payload_snap_len);

    ev->ready = true;
    wifi_eq_write_idx = next;
}

void process_wifi_event_queue() {
    while (wifi_event_queue[wifi_eq_read_idx].ready) {
        WifiEvent* ev = &wifi_event_queue[wifi_eq_read_idx];

        WifiEvent local;
        memcpy(&local, ev, sizeof(WifiEvent));
        ev->ready = false;
        wifi_eq_read_idx = (wifi_eq_read_idx + 1) % WIFI_EVENT_QUEUE_SIZE;

        if (local.rssi < IGNORE_WEAK_RSSI) continue;
        session_wifi_packets++;

        clean_device_name_char(local.ssid);

        int  confidence    = 0;
        char methods[64]   = {0};
        int  mac_score     = check_mac_prefix_tiered(local.mac);
        bool ssid_generic  = (strlen(local.ssid) > 0 && check_ssid_pattern(local.ssid));
        bool ssid_flock_fmt = (strlen(local.ssid) > 0 && is_flock_ssid_format(local.ssid));

        if (ssid_flock_fmt) {
            confidence = SCORE_DEFINITIVE; strncat(methods, "ssid_fmt ", 63);
        } else if (mac_score == 1) {
            confidence = SCORE_STRONG; strncat(methods, "mac_t1 ", 63);
            if (ssid_generic) { confidence = SCORE_DEFINITIVE; strncat(methods, "ssid ", 63); }
        } else {
            if (mac_score == 2) { confidence += SCORE_WEAK; strncat(methods, "mac_t2 ", 63); }
            if (ssid_generic)   { confidence += SCORE_WEAK; strncat(methods, "ssid ", 63); }
        }
        if (confidence > 0 && local.rssi > -50) confidence += SCORE_BONUS_RSSI;

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
            local.mac[0], local.mac[1], local.mac[2],
            local.mac[3], local.mac[4], local.mac[5]);
        const char* name_str       = (strlen(local.ssid) > 0) ? local.ssid : "Hidden";
        const char* frame_type_str = local.is_beacon ? "Beacon" : "ProbeReq";

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            channel_lock_until = millis() + 10000;
            rssi_track_update(mac_str, local.rssi);
            if (rssi_track_is_stationary(mac_str)) confidence += SCORE_BONUS_STAT;
            if (confidence > 100) confidence = 100;
            locator_add_sample(mac_str, local.rssi);

            int mlen = strlen(methods);
            if (mlen > 0 && methods[mlen - 1] == ' ') methods[mlen - 1] = '\0';

            log_detection("FLOCK_WIFI", "WIFI", local.rssi, mac_str, name_str,
                          local.channel, 0, frame_type_str, methods, confidence, local.seq_num);
            write_threat_pcap(local.payload_snap, local.payload_snap_len);

            if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                trigger_alarm_confidence = confidence;
                last_buzzer_time = millis();
            }
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
};

static void ble_process_task(void* pvParameters) {
    BleEventData* ev = (BleEventData*)pvParameters;

    if (ev->rssi < IGNORE_WEAK_RSSI) { free(ev); vTaskDelete(NULL); return; }
    session_ble_packets++;

    int  confidence   = 0;
    char methods[64]  = {0};
    char capture_type[16] = "FLOCK_BLE";
    bool got_name = false, got_mfg = false, got_raven = false;

    // [v8.3 FIX 3] Penguin numeric names are now tracked separately.
    // They contribute SCORE_WEAK as corroborating evidence only, NOT as a
    // definitive signal. got_penguin_name does NOT set got_name=true.
    bool got_penguin_name = false;

    int mac_score = check_mac_prefix_tiered(ev->mac);

    // ── Name check ────────────────────────────────────────────────────────
    char dev_name_char[65];
    strncpy(dev_name_char, ev->dev_name, 64);
    dev_name_char[64] = '\0';
    if (ev->have_name) {
        clean_device_name_char(dev_name_char);
        if (check_device_name_pattern(dev_name_char)) {
            strncat(methods, "name ", 63); got_name = true;
        } else if (is_penguin_numeric_name(dev_name_char)) {
            // Corroborating evidence only — see scoring block below.
            strncat(methods, "penguin_num ", 63); got_penguin_name = true;
        }
    }

    // ── Manufacturer data check ───────────────────────────────────────────
    std::string mfg_std((const char*)ev->mfg_data, ev->mfg_data_len);
    if (ev->have_mfg) {
        if (check_manufacturer_id(mfg_std)) {
            strncat(methods, "mfg_0x09C8 ", 63); got_mfg = true;
            if (has_tn_serial(mfg_std)) strncat(methods, "tn_serial ", 63);
        }
    }

    // ── Raven UUID check — split into custom vs standard SIG ─────────────
    // [v8.3 FIX 2] Only custom UUIDs (0x3100–0x3500) trigger got_raven alone.
    // Standard SIG UUIDs (0x180A, 0x1809, 0x1819) require a second Raven UUID
    // to corroborate before we classify as Raven. Single 0x180A alone is NOT
    // sufficient — it appears on thousands of non-Flock devices.
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
    // Total Raven UUID count preserved for firmware version detection below.
    int raven_uuid_count = raven_custom_count + raven_std_count;

    // got_raven: any custom UUID is sufficient; standard-only requires 2+.
    if (raven_custom_count > 0 || raven_uuid_count >= 2) {
        strncpy(capture_type, "RAVEN_BLE", sizeof(capture_type)); got_raven = true;
        if (raven_uuid_count >= 3)        strncat(methods, "raven_multi ", 63);
        else if (raven_custom_count > 0)  strncat(methods, "raven_custom ", 63);
        else                              strncat(methods, "raven_uuid ", 63);
    }

    // ── Confidence scoring ────────────────────────────────────────────────
    // [v8.3 FIX 1] 0x09C8 manufacturer ID (got_mfg) now routes to
    // SCORE_DEFINITIVE unconditionally. Previously, got_mfg without a TN
    // serial fell into the SCORE_STRONG (60) path — below CONFIDENCE_ALARM_
    // THRESHOLD=75 even with the RSSI bonus. The 0x09C8 ID is more durable
    // than any single OUI prefix and survives firmware name changes.
    //
    // [v8.3 FIX 3] Penguin numeric name alone contributes SCORE_WEAK (25).
    // It escalates to definitive only when paired with a Tier-1 OUI or got_mfg.
    if (got_raven || got_name || got_mfg) {
        // Any definitive-path signal is sufficient to alarm.
        confidence = SCORE_DEFINITIVE;
        if (mac_score == 1) strncat(methods, "mac_t1 ", 63);  // note co-occurrence
    } else if (mac_score == 1) {
        confidence = SCORE_STRONG;
        strncat(methods, "mac_t1 ", 63);
        // Penguin numeric name corroborates a Tier-1 OUI → escalate to definitive.
        if (got_penguin_name) { confidence = SCORE_DEFINITIVE; }
    } else {
        // Accumulate weak signals.
        if (mac_score == 2) { confidence += SCORE_WEAK; strncat(methods, "mac_t2 ", 63); }
        if (got_penguin_name) { confidence += SCORE_WEAK; }  // already in methods from detection
        // [v8.4 FIX 5] static_addr bonus now requires corroboration — it only fires
        // when mac_score==2 or got_penguin_name is also true. A device with a static
        // address alone (no OUI match, no name signal) contributes nothing.
        // Previously the bonus fired on the majority of BLE traffic, since public and
        // random-static addresses are the default for most BLE peripherals.
        if ((mac_score == 2 || got_penguin_name) &&
            (ev->addr_type == 0 || (ev->addr_type == 1 && (ev->mac[0] >> 6) == 0x03))) {
            confidence += SCORE_WEAK; strncat(methods, "static_addr ", 63);
        }
    }
    if (confidence > 0 && ev->rssi > -50) confidence += SCORE_BONUS_RSSI;

    char mac_string[18];
    snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x",
        ev->mac[0], ev->mac[1], ev->mac[2],
        ev->mac[3], ev->mac[4], ev->mac[5]);

    if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
        channel_lock_until = millis() + 10000;
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
            for (int i = 0; i < ev->uuid_count; i++) {
                if (strcasestr(ev->service_uuids[i], "00003100")) has_gps   = true;
                if (strcasestr(ev->service_uuids[i], "00003200")) has_power = true;
                if (strcasestr(ev->service_uuids[i], "00003300")) has_net   = true;
                if (strcasestr(ev->service_uuids[i], "00003400")) has_up    = true;
                if (strcasestr(ev->service_uuids[i], "00003500")) has_err   = true;
            }
            const char* fw = (has_gps && has_power && has_net && has_up && has_err) ? "1.3.x"
                           : (has_gps && has_power && has_net)                      ? "1.2.x"
                           :                                                           "Unknown";
            snprintf(extra_data, sizeof(extra_data), "FW:%s UUIDs:%d", fw, raven_uuid_count);
        }

        int mlen = strlen(methods);
        if (mlen > 0 && methods[mlen - 1] == ' ') methods[mlen - 1] = '\0';

        log_detection(capture_type, "BLE", ev->rssi, mac_string, dev_name_char,
                      0, ev->have_tx_power ? ev->tx_power : 0,
                      extra_data, methods, confidence, -1);

        if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
            trigger_alarm_confidence = confidence;
            last_buzzer_time = millis();
        }
    }

    free(ev);
    vTaskDelete(NULL);
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

        BaseType_t created = xTaskCreatePinnedToCore(
            ble_process_task, "BLEProc", 6144, ev, 1, NULL, 1);
        if (created != pdPASS) {
            free(ev);
        }
    }
};

// ============================================================================
// DEDICATED TASKS (DUAL CORE)
// ============================================================================
void ScannerLoopTask(void* pvParameters) {
    for (;;) {
        unsigned long now = millis();
        if (now > channel_lock_until) {
            bool is_primary = (current_channel == 1 || current_channel == 6 || current_channel == 11);
            unsigned long dwell = is_primary ? DWELL_PRIMARY : DWELL_SECONDARY;
            if (now - last_channel_hop > dwell) {
                current_channel++; 
                if (current_channel > MAX_CHANNEL) current_channel = 1;
                esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); 
                last_channel_hop = now;
            }
        }

        unsigned long ble_interval = (now < channel_lock_until)
            ? BLE_SCAN_INTERVAL_LOCK
            : BLE_SCAN_INTERVAL;

        if (millis() - last_ble_scan >= ble_interval) {
            if (!pBLEScan->isScanning()) { pBLEScan->start(BLE_SCAN_DURATION, false); last_ble_scan = millis(); }
        }
        if (!pBLEScan->isScanning() && (millis() - last_ble_scan > (unsigned long)(BLE_SCAN_DURATION * 1000 + 500))) {
            pBLEScan->clearResults();
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void GPSLoopTask(void* pvParameters) {
    for (;;) {
        if (SerialGPS.available() > 0) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            while (SerialGPS.available() > 0) {
                gps.encode(SerialGPS.read());
            }
            xSemaphoreGive(dataMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void AlarmTask(void* pvParameters) {
    int conf = (int)(intptr_t)pvParameters;
    
    if (conf >= CONFIDENCE_CERTAIN) {
        M5Cardputer.Speaker.tone(880, 60); vTaskDelay(80 / portTICK_PERIOD_MS);
        M5Cardputer.Speaker.tone(1319, 60); vTaskDelay(80 / portTICK_PERIOD_MS);
        M5Cardputer.Speaker.tone(1760, 120); vTaskDelay(120 / portTICK_PERIOD_MS);
    } else if (conf >= CONFIDENCE_HIGH) {
        for (int i = 0; i < 3; i++) {
            M5Cardputer.Speaker.tone(DETECT_FREQ_HIGH, DETECT_BEEP_DURATION);
            vTaskDelay((DETECT_BEEP_DURATION + 50) / portTICK_PERIOD_MS); 
        }
    } else {
        M5Cardputer.Speaker.tone(DETECT_FREQ, DETECT_BEEP_DURATION);
        vTaskDelay((DETECT_BEEP_DURATION + 50) / portTICK_PERIOD_MS); 
    }
    
    is_alarming = false;
    vTaskDelete(NULL);
}

void play_escalated_alarm(int confidence) {
    if (stealth_mode || is_muted || is_alarming) return;
    is_alarming = true;
    xTaskCreate(AlarmTask, "AlarmTask", 2048, (void*)(intptr_t)confidence, 2, NULL);
}

// ============================================================================
// UI RENDERING - BASE COMPONENTS
// ============================================================================
void draw_header_spr(int screen_num) {
    static const char* screen_names[NUM_SCREENS] = {
        "SCANNER", "LOCATOR", "LAST DETECTION", "RECENT DETECTIONS", 
        "GPS", "DEVICE INFO"
    };
    if (screen_num < 0 || screen_num >= NUM_SCREENS) screen_num = 0;

    int32_t med_mv = get_median_voltage(); int raw_bat = get_unified_battery_pct(med_mv); bool chg = is_device_charging(med_mv);
    static int display_bat = -1;
    if (display_bat == -1) display_bat = raw_bat;
    if (abs(raw_bat - display_bat) >= 2 || raw_bat == 100 || raw_bat == 0) display_bat = raw_bat; 

    spr.fillRect(0, 0, DISP_W, 18, BG_COLOR); spr.fillRect(0, 0, 3, 18, HEADER_COLOR);
    spr.setTextColor(HEADER_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(7, 5); spr.print(screen_names[screen_num]);

    uint16_t bcol = chg ? ACCENT_COLOR : (display_bat > 50 ? ACCENT_COLOR : (display_bat > 20 ? WARN_COLOR : ALERT_COLOR));
    if (chg) {
        spr.drawLine(DISP_W - 65, 4, DISP_W - 67, 8, TEXT_COLOR); spr.drawLine(DISP_W - 67, 8, DISP_W - 63, 8, TEXT_COLOR);
        spr.drawLine(DISP_W - 64, 8, DISP_W - 66, 12, TEXT_COLOR); spr.setTextColor(ACCENT_COLOR, BG_COLOR);
    } else { spr.setTextColor(bcol, BG_COLOR); }
    
    if (is_muted) {
        spr.setTextColor(ALERT_COLOR, BG_COLOR);
        spr.setCursor(DISP_W - 85, 5); spr.print("MUTED");
    }

    spr.setTextColor(bcol, BG_COLOR);
    spr.setCursor(DISP_W - 55, 5); spr.printf("%d%%", display_bat);
    spr.drawRect(DISP_W - 26, 3, 22, 11, bcol); spr.fillRect(DISP_W - 4, 6, 2, 5, bcol);
    int bfill = (display_bat * 19) / 100; if (bfill > 0) spr.fillRect(DISP_W - 25, 4, bfill, 9, bcol);
    spr.drawLine(0, 18, DISP_W, 18, GRID_COLOR);
}

void draw_toast_spr() {
    if (!toast_active) return;
    unsigned long elapsed = millis() - toast_start;
    if (elapsed > TOAST_DURATION_MS) { toast_active = false; return; }
    
    int y_pos = DISP_H - 34;
    if (elapsed < 150) y_pos = DISP_H - 10 - (int)((elapsed / 150.0f) * 24);
    else if (elapsed > TOAST_DURATION_MS - 150) y_pos = DISP_H - 34 + (int)(((elapsed - (TOAST_DURATION_MS - 150)) / 150.0f) * 24);

    int t_w = 210; int t_x = (DISP_W - t_w) / 2;
    
    spr.fillRect(t_x, y_pos, t_w, 26, CARD_COLOR);
    spr.drawRect(t_x, y_pos, t_w, 26, CARD_BORDER);
    spr.fillRect(t_x, y_pos, 4, 26, TOAST_COLOR); 
    
    spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(1); 
    spr.setCursor(t_x + 10, y_pos + 9); spr.print("[!]"); 
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); 
    spr.setCursor(t_x + 32, y_pos + 9); spr.print(toast_text);
}

void draw_vol_overlay() {
    if (!show_vol_overlay) return;
    unsigned long elapsed = millis() - vol_overlay_start;
    if (elapsed > 1500) { show_vol_overlay = false; return; }

    const int TARGET_Y = 20;
    const int v_w = 140;
    const int v_x = (DISP_W - v_w) / 2;
    const int v_h = 26;

    int v_y;
    if (elapsed < 120) {
        v_y = -v_h + (int)((float)elapsed / 120.0f * (TARGET_Y + v_h));
    } else if (elapsed > 1380) {
        v_y = TARGET_Y - (int)(((float)(elapsed - 1380) / 120.0f) * (TARGET_Y + v_h));
    } else {
        v_y = TARGET_Y;
    }

    spr.fillRect(v_x + 2, v_y + 2, v_w, v_h, lgfx::color565(2, 4, 10));
    spr.fillRect(v_x, v_y, v_w, v_h, CARD_COLOR);
    spr.drawRect(v_x, v_y, v_w, v_h, CARD_BORDER);
    spr.fillRect(v_x, v_y, 4, v_h, HEADER_COLOR);

    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(v_x + 10, v_y + 11); spr.print("VOL");

    int vol_pct = map(current_volume, 0, 255, 0, 100);
    char vol_str[8]; snprintf(vol_str, sizeof(vol_str), "%d%%", vol_pct);
    spr.setTextColor(TEXT_COLOR, CARD_COLOR);
    spr.setCursor(v_x + 34, v_y + 11); spr.print(vol_str);

    int bar_x = v_x + 62; int bar_y = v_y + 9; int bar_w = v_w - 70; int bar_h = 10;
    spr.drawRect(bar_x, bar_y, bar_w, bar_h, GRID_COLOR);
    int fill = (current_volume * (bar_w - 2)) / 255;
    if (fill > 0) {
        uint16_t bar_col = vol_pct > 80 ? ALERT_COLOR : vol_pct > 40 ? HEADER_COLOR : TEAL_COLOR;
        spr.fillRect(bar_x + 1, bar_y + 1, fill, bar_h - 2, bar_col);
    }
}

void drawCard(int x, int y, int w, int h) {
    spr.fillRect(x, y, w, h, CARD_COLOR); spr.drawRect(x, y, w, h, CARD_BORDER); 
}

void draw_help_overlay() {
    spr.fillRoundRect(10, 15, DISP_W - 20, DISP_H - 30, 6, lgfx::color565(10, 15, 25));
    spr.drawRoundRect(10, 15, DISP_W - 20, DISP_H - 30, 6, HEADER_COLOR);
    spr.setTextColor(HEADER_COLOR); spr.setTextSize(1);
    spr.setCursor(14, 20); spr.print("--- HOTKEYS ---");
    spr.drawLine(10, 30, DISP_W - 10, 30, HEADER_COLOR);

    spr.setTextColor(TEXT_COLOR);
    spr.setCursor(14,  35); spr.print("</> DEL : Prev/Next");
    spr.setCursor(14,  46); spr.print("1-6     : Jump Screen");
    spr.setCursor(14,  57); spr.print("0       : Go Locator");
    spr.setCursor(14,  68); spr.print("m / q   : Mute Toggle");
    spr.setCursor(14,  79); spr.print(";/.     : Vol Up/Down");
    spr.setCursor(14,  90); spr.print("l       : Locate Last");
    spr.setCursor(14, 101); spr.print("t       : Cycle Target");
    spr.setCursor(14, 112); spr.print("s / n   : Stealth/Night");

    spr.setTextColor(DIM_COLOR);
    spr.setCursor(130,  35); spr.print("x: WiFi sim");
    spr.setCursor(130,  46); spr.print("x again: BLE");
    spr.setCursor(130,  57); spr.print("TAB: Help");
    spr.setCursor(130,  68); spr.print("g: GPS page");
}

// ============================================================================
// UI RENDERING - SCREENS 
// ============================================================================
void draw_scanner_screen() {
    int divider_x = 132;
    spr.fillSprite(BG_COLOR);
    draw_header_spr(0);
    spr.setClipRect(0, 19, divider_x, DISP_H - 19);
    
    float TILT = 0.65f;

    int rcx = radar_cx + (int)imu_px;
    int rcy = radar_cy + (int)imu_py;

    int THICKNESS = 10;

    spr.fillEllipse(rcx, rcy + THICKNESS + 2, radar_r, radar_r * TILT, lgfx::color565(4, 8, 16));
    spr.drawEllipse(rcx, rcy + THICKNESS, radar_r, radar_r * TILT, DIM_COLOR);
    for (int i = THICKNESS - 1; i > 0; i--) {
        uint8_t wall_v = 8 + (i * 2);
        spr.drawEllipse(rcx, rcy + i, radar_r, radar_r * TILT,
                        lgfx::color565(wall_v, wall_v * 2, wall_v * 4));
    }

    spr.fillEllipse(rcx, rcy, radar_r, radar_r * TILT, lgfx::color565(14, 26, 52));
    spr.drawEllipse(rcx, rcy, radar_r, radar_r * TILT, HEADER_COLOR);
    spr.drawEllipse(rcx, rcy, inner_r, inner_r * TILT, DIM_COLOR);

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

    const float TRIGGER_ARC  = 0.25f;
    const float TWO_PIf      = (float)M_PI * 2.0f;
    float sweep_norm = fmodf(sweep_rad, TWO_PIf);

    for (int i = 0; i < NUM_PARTICLES; i++) {
        float pa = fmodf(noise_a[i], TWO_PIf);
        float diff = sweep_norm - pa;
        if (diff >  (float)M_PI) diff -= TWO_PIf;
        if (diff < -(float)M_PI) diff += TWO_PIf;
        if (diff >= 0.0f && diff < TRIGGER_ARC) {
            noise_life[i] = noise_max_life[i];
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
            int intensity = (int)(fade * 255.0f * noise_dimming);
            if (intensity < 8) continue;

            int px = rcx + (int)(noise_r[i] * cosf(noise_a[i]));
            int py = rcy + (int)(noise_r[i] * sinf(noise_a[i]) * TILT);

            uint16_t p_col = night_mode
                ? lgfx::color565(intensity, intensity / 4, 0)
                : ((i % 3 == 0)
                    ? lgfx::color565(intensity / 4, intensity, intensity / 3)
                    : lgfx::color565(0, intensity / 2, intensity));
            spr.fillRect(px - 1, py - 1, 3, 3, p_col);

            if (intensity > 80) {
                uint16_t glow = night_mode
                    ? lgfx::color565(intensity / 3, intensity / 8, 0)
                    : ((i % 3 == 0)
                        ? lgfx::color565(0, intensity / 4, 0)
                        : lgfx::color565(0, intensity / 6, intensity / 3));
                spr.drawPixel(px - 2, py,     glow);
                spr.drawPixel(px + 2, py,     glow);
                spr.drawPixel(px,     py - 2, glow);
                spr.drawPixel(px,     py + 2, glow);
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
            if (spike_len < 2) spike_len = 2;

            float dist_ratio = constrain(map(radial_rssi[i], -100, -30, 100, 0), 0, 100) / 100.0f;
            int blip_dist = inner_r + 4 + (int)((radar_r - inner_r - 6) * dist_ratio);

            int base_x = rcx + (int)(blip_dist * cosf(a));
            int base_y = rcy + (int)(blip_dist * sinf(a) * TILT);
            uint16_t line_col = local_colors[i];

            if (is_strong) {
                spr.drawLine(base_x, base_y, base_x, base_y - spike_len, line_col);
                int blip_r = max(2, (int)(local_spikes[i] * 2.0f));
                spr.fillCircle(base_x, base_y - spike_len, blip_r, line_col);
                spr.drawCircle(base_x, base_y - spike_len, blip_r + 1,
                               lgfx::color565(
                                   (line_col >> 11) << 3,
                                   ((line_col >> 5) & 0x3F) << 2,
                                   (line_col & 0x1F) << 3));
            } else {
                int dot_r = max(1, (int)(local_spikes[i] * 2.5f));
                spr.fillCircle(base_x, base_y - 2, dot_r, line_col);
            }
        }
    }
    if (!any_active) {
        float pulse_sin = sinf(millis()/400.0f); int pulse_r = inner_r + 3 + (int)(3 * pulse_sin);
        uint16_t pulse_col = (pulse_sin > 0) ? GRID_COLOR : BG_COLOR;
        spr.drawEllipse(rcx, rcy, pulse_r, pulse_r * TILT, pulse_col);
    }
    hud_rotation += 0.0033f;
    spr.clearClipRect();

    spr.drawLine(divider_x - 4, 20, divider_x - 4, DISP_H, GRID_COLOR);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long sw = session_flock_wifi; long sb = session_flock_ble;
    char l_cap_type[16]; strncpy(l_cap_type, last_cap_type, sizeof(l_cap_type) - 1); l_cap_type[sizeof(l_cap_type)-1] = '\0';
    char l_cap_name[65]; strncpy(l_cap_name, last_cap_name, sizeof(l_cap_name) - 1); l_cap_name[sizeof(l_cap_name)-1] = '\0';
    char l_cap_mac[18];  strncpy(l_cap_mac,  last_cap_mac,  sizeof(l_cap_mac)  - 1); l_cap_mac[sizeof(l_cap_mac)-1]   = '\0';
    int l_cap_conf = last_cap_confidence;
    xSemaphoreGive(dataMutex);
    long wp = session_wifi_packets;
    long bp = session_ble_packets;

    spr.drawFastHLine(4, DISP_H - 14, (divider_x - 4) - 4, GRID_COLOR);
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, DISP_H - 10); spr.print("SESSION:"); 
    spr.setTextColor(TEXT_COLOR, BG_COLOR);
    { char _tbuf[9]; format_time_buf((millis() - session_start_time) / 1000, _tbuf, sizeof(_tbuf)); spr.print(_tbuf); }

    bool ble_scanning = pBLEScan->isScanning();
    int right_text_x = divider_x + 2;

    spr.drawRect(right_text_x, 22, 46, 14, ble_scanning ? DIM2_COLOR : TOAST_COLOR);
    spr.setTextColor(ble_scanning ? DIM_COLOR : TOAST_COLOR, BG_COLOR);
    spr.setCursor(right_text_x + 2, 25); spr.print("WF:"); spr.print(current_channel);
    
    if (millis() < channel_lock_until) {
        spr.fillRect(right_text_x + 34, 23, 10, 12, TOAST_COLOR);
        spr.setTextColor(TFT_BLACK, TOAST_COLOR);
        spr.setCursor(right_text_x + 35, 25); spr.print("L");
    }

    spr.drawRect(right_text_x + 50, 22, 46, 14, ble_scanning ? PURPLE_COLOR : DIM2_COLOR);
    spr.setTextColor(ble_scanning ? PURPLE_COLOR : DIM_COLOR, BG_COLOR);
    spr.setCursor(right_text_x + 52, 25); spr.print("BL:SCN");

    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(right_text_x, 42); spr.print("WIFI HITS");
    spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.setTextSize(2);
    spr.setCursor(right_text_x, 52); spr.print(sw);
    spr.setTextColor(DIM2_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(right_text_x, 65); spr.printf("%ld pkts", wp);

    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(right_text_x, 74); spr.print("BLE HITS");
    spr.setTextColor(PURPLE_COLOR, BG_COLOR); spr.setTextSize(2);
    spr.setCursor(right_text_x, 84); spr.print(sb);
    spr.setTextColor(DIM2_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(right_text_x, 97); spr.printf("%ld pkts", bp);

    spr.setTextSize(1);
    spr.drawLine(right_text_x, 106, DISP_W - 2, 106, GRID_COLOR);
    
    if (l_cap_type[0] != '\0' && strcmp(l_cap_type, "None") != 0) {
        spr.setTextColor(confidence_color(l_cap_conf), BG_COLOR);
        spr.setCursor(right_text_x, 109);
        char dn[14] = "";
        bool use_name = (l_cap_name[0] != '\0' &&
                         strcmp(l_cap_name, "Hidden")  != 0 &&
                         strcmp(l_cap_name, "Unknown") != 0);
        if (use_name) {
            strncpy(dn, l_cap_name, 13); dn[13] = '\0';
        } else {
            const char* sm = (strlen(l_cap_mac) > 8) ? l_cap_mac + 9 : l_cap_mac;
            strncpy(dn, sm, 13); dn[13] = '\0';
        }
        spr.print(dn);
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        char conf_str[24];
        snprintf(conf_str, sizeof(conf_str), "%d%% %s", l_cap_conf, confidence_label(l_cap_conf));
        spr.setCursor(right_text_x, 121); spr.print(conf_str);
    } else {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setCursor(right_text_x, 109); spr.print("-- no hits yet --");
    }
}

void draw_locator_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active=locator_active, has_est=locator_has_estimate;
    char target_mac[18];  strncpy(target_mac,  locator_target_mac,  sizeof(target_mac)  - 1); target_mac[sizeof(target_mac)-1]   = '\0';
    char target_name[65]; strncpy(target_name, locator_target_name, sizeof(target_name) - 1); target_name[sizeof(target_name)-1] = '\0';
    int samples=locator_sample_count, peak=locator_peak_rssi;
    float dist=locator_est_distance, brng=locator_bearing, conf_r=locator_confidence_radius;
    bool gps_valid = gps.course.isValid() && gps.speed.isValid() && gps.speed.mph() > 2.0;
    float gps_course = gps.course.deg();
    unsigned long newest_ms = locator_newest_sample_ms;
    xSemaphoreGive(dataMutex);

    unsigned long est_age_ms = (has_est && newest_ms > 0) ? (millis() - newest_ms) : 0;
    bool est_stale = has_est && (est_age_ms > 60000UL);
    bool est_very_stale = has_est && (est_age_ms > 300000UL);
    
    spr.fillSprite(BG_COLOR); draw_header_spr(1);

    int arrow_cx = 56 + (int)imu_px, arrow_cy = 78 + (int)imu_py;
    const float COMP_TILT  = 0.60f;
    const int   COMP_R     = 42;
    const int   COMP_INNER = 14;
    const int   COMP_THICK = 8;

    uint16_t comp_rim_col  = !active          ? DIM_COLOR
                           : est_very_stale   ? DIM_COLOR
                           : est_stale        ? WARN_COLOR
                           :                   HEADER_COLOR;
    uint16_t comp_face_col = !active          ? lgfx::color565(10, 10, 18)
                           : est_stale        ? lgfx::color565(24, 16, 8)
                           :                   lgfx::color565(10, 22, 50);

    float rel_bearing = brng;
    if (active && has_est && gps_valid) {
        rel_bearing = brng - gps_course; if (rel_bearing < 0) rel_bearing += 360.0f;
    }

    spr.fillEllipse(arrow_cx, arrow_cy + COMP_THICK + 3,
                    COMP_R, (int)(COMP_R * COMP_TILT), lgfx::color565(4, 6, 12));
    spr.drawEllipse(arrow_cx, arrow_cy + COMP_THICK,
                    COMP_R, (int)(COMP_R * COMP_TILT), active ? DIM_COLOR : DIM2_COLOR);
    for (int i = COMP_THICK - 1; i > 0; i--) {
        uint8_t wv = 6 + i * 3;
        spr.drawEllipse(arrow_cx, arrow_cy + i, COMP_R, (int)(COMP_R * COMP_TILT),
                        active ? lgfx::color565(wv / 2, wv, wv * 3)
                               : lgfx::color565(wv / 2, wv / 2, wv));
    }
    spr.fillEllipse(arrow_cx, arrow_cy, COMP_R, (int)(COMP_R * COMP_TILT), comp_face_col);
    spr.drawEllipse(arrow_cx, arrow_cy, COMP_R - 4, (int)((COMP_R - 4) * COMP_TILT),
                    lgfx::color565(20, 35, 70));
    spr.drawEllipse(arrow_cx, arrow_cy, COMP_R, (int)(COMP_R * COMP_TILT), comp_rim_col);
    spr.drawEllipse(arrow_cx, arrow_cy, COMP_INNER, (int)(COMP_INNER * COMP_TILT),
                    active ? lgfx::color565(30, 50, 90) : DIM2_COLOR);

    for (int c = 0; c < 8; c++) {
        float ca = radians(c * 45.0f - 90.0f);
        bool major = (c % 2 == 0);
        int tick_len = major ? 7 : 4;
        int ox = arrow_cx + (int)(COMP_R * cosf(ca));
        int oy = arrow_cy + (int)(COMP_R * sinf(ca) * COMP_TILT);
        int ix = arrow_cx + (int)((COMP_R - tick_len) * cosf(ca));
        int iy = arrow_cy + (int)((COMP_R - tick_len) * sinf(ca) * COMP_TILT);
        spr.drawLine(ix, iy, ox, oy, major ? comp_rim_col : DIM2_COLOR);
    }
    {
        float ca = radians(-90.0f);
        int lx = arrow_cx + (int)(COMP_R * cosf(ca));
        int ly = arrow_cy + (int)(COMP_R * sinf(ca) * COMP_TILT);
        spr.setTextColor(comp_rim_col, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(lx - 3, ly - 11); spr.print("N");
    }

    if (!active || !has_est) {
        float idle_rad = (millis() / 4000.0f) * (float)M_PI * 2.0f;
        for (int t = 1; t <= 16; t++) {
            float ta = idle_rad - t * 0.08f;
            float fade = (16.0f - t) / 16.0f;
            int brightness = (int)(fade * 80.0f);
            uint16_t tc = lgfx::color565(0, brightness / 2, brightness);
            for (int rr = COMP_INNER + 2; rr < COMP_R - 2; rr += 3) {
                spr.drawPixel(arrow_cx + (int)(rr * cosf(ta)),
                              arrow_cy + (int)(rr * sinf(ta) * COMP_TILT), tc);
            }
        }
        spr.drawLine(arrow_cx, arrow_cy,
                     arrow_cx + (int)((COMP_R - 3) * cosf(idle_rad)),
                     arrow_cy + (int)((COMP_R - 3) * sinf(idle_rad) * COMP_TILT),
                     lgfx::color565(0, 100, 140));
    } else {
        float ba = radians(rel_bearing - 90.0f);
        int tip_x = arrow_cx + (int)((COMP_R - 4) * cosf(ba));
        int tip_y = arrow_cy + (int)((COMP_R - 4) * sinf(ba) * COMP_TILT);

        uint8_t line_bright = est_very_stale ? 80 : est_stale ? 150 : 255;
        uint16_t bear_col = est_stale
            ? lgfx::color565(line_bright, line_bright / 2, 0)
            : ALERT_COLOR;

        spr.drawLine(arrow_cx, arrow_cy, tip_x, tip_y, bear_col);
        spr.drawLine(arrow_cx + 1, arrow_cy, tip_x + 1, tip_y, bear_col);
        spr.fillCircle(tip_x, tip_y, 4, bear_col);
        spr.drawCircle(tip_x, tip_y, 5, lgfx::color565(
            (bear_col >> 11) << 3,
            ((bear_col >> 5) & 0x3F) << 2,
            (bear_col & 0x1F) << 3));
        int tail_x = arrow_cx - (int)(10 * cosf(ba));
        int tail_y = arrow_cy - (int)(10 * sinf(ba) * COMP_TILT);
        spr.drawLine(arrow_cx, arrow_cy, tail_x, tail_y, DIM_COLOR);
    }
    
    int rx = 118; spr.drawLine(rx - 2, 20, rx - 2, DISP_H - 14, GRID_COLOR);
    
    char tname[16] = "AWAITING TARGET";
    if (active) {
        bool name_ok = (target_name[0] != '\0' &&
                        strcmp(target_name, "Hidden")  != 0 &&
                        strcmp(target_name, "Unknown") != 0);
        const char* src = name_ok
            ? target_name
            : ((strlen(target_mac) > 8) ? target_mac + 9 : target_mac);
        strncpy(tname, src, 14); tname[14] = '\0';
    }
    spr.setCursor(rx + 2, 22); spr.setTextColor(active ? TEXT_COLOR : DIM_COLOR, BG_COLOR); spr.print(tname);
    
    drawCard(rx + 2, 34, 116, 28);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(rx + 6, 38); spr.print("DISTANCE");
    spr.setTextSize(2); spr.setCursor(rx + 6, 46);
    if (!active || !has_est) {
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.print("--");
    } else {
        if (dist < 20) spr.setTextColor(ALERT_COLOR, CARD_COLOR);
        else if (dist < 60) spr.setTextColor(TOAST_COLOR, CARD_COLOR);
        else spr.setTextColor(TEXT_COLOR, CARD_COLOR);
        char dist_buf[12];
        if (dist < 100) snprintf(dist_buf, sizeof(dist_buf), "%.0fm", dist);
        else            snprintf(dist_buf, sizeof(dist_buf), "%.2fk", dist / 1000.0f);
        spr.print(dist_buf);
    }
    
    drawCard(rx + 2, 66, 116, 28);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(rx + 6, 70); spr.print("BEARING");
    spr.setTextSize(2); spr.setCursor(rx + 6, 78);
    if (!active || !has_est) {
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.print("--");
    } else {
        spr.setTextColor(HEADER_COLOR, CARD_COLOR);
        static const char* cardinals[] = {"N","NE","E","SE","S","SW","W","NW"}; int card_idx = ((int)(brng + 22.5) / 45) % 8;
        spr.print(cardinals[card_idx]);
    }
    
    drawCard(rx + 2, 98, 116, 28);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(rx + 6, 102); spr.print("ACCURACY +/-");
    spr.setTextSize(2); spr.setCursor(rx + 6, 110);
    if (!active || !has_est) {
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.print("--");
    } else {
        spr.setTextColor(TEAL_COLOR, CARD_COLOR);
        char acc_buf[10];
        snprintf(acc_buf, sizeof(acc_buf), "%.0fm", conf_r);
        spr.print(acc_buf);
    }

    spr.setTextSize(1); spr.setCursor(4, DISP_H - 10);
    if (!active) {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.print("Press 'l' to target last match");
    } else if (!has_est) {
        char coll_buf[40];
        snprintf(coll_buf, sizeof(coll_buf), "Collecting: %d/%d  Peak:%ddB",
                 samples, LOC_MIN_SAMPLES_EST, peak);
        spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.print(coll_buf);
    } else if (est_very_stale) {
        unsigned long age_min = est_age_ms / 60000UL;
        char stale_buf[32]; snprintf(stale_buf, sizeof(stale_buf), "EST %lum OLD - MOVE!", age_min);
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.print(stale_buf);
    } else if (est_stale) {
        unsigned long age_sec = est_age_ms / 1000UL;
        char stale_buf[32]; snprintf(stale_buf, sizeof(stale_buf), "EST %lus OLD", age_sec);
        spr.setTextColor(WARN_COLOR, BG_COLOR); spr.print(stale_buf);
    } else {
        if      (dist < 15 && conf_r < 25) { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("** LOOK UP! **"); }
        else if (dist < 30)  { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("Very close"); }
        else if (dist < 75)  { spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.print("Closing in..."); }
        else                 { spr.setTextColor(TEAL_COLOR,  BG_COLOR); spr.print("Keep going..."); }
    }
}

void draw_last_detect_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    char t_type[16]; strncpy(t_type, last_cap_type,           sizeof(t_type)   - 1); t_type[sizeof(t_type)-1]     = '\0';
    char t_time[9];  strncpy(t_time, last_cap_time,           sizeof(t_time)   - 1); t_time[sizeof(t_time)-1]     = '\0';
    char t_mac[18];  strncpy(t_mac,  last_cap_mac,            sizeof(t_mac)    - 1); t_mac[sizeof(t_mac)-1]       = '\0';
    char t_name[65]; strncpy(t_name, last_cap_name,           sizeof(t_name)   - 1); t_name[sizeof(t_name)-1]     = '\0';
    char t_method[64]; strncpy(t_method, last_cap_det_method, sizeof(t_method) - 1); t_method[sizeof(t_method)-1] = '\0';
    int t_rssi = last_cap_rssi;
    int t_conf = last_cap_confidence;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(2);

    if (strcmp(t_type, "None") == 0 || t_type[0] == '\0') {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(20, 65); spr.print("No detections yet");
        return;
    }

    uint16_t ccol = confidence_color(t_conf);

    bool is_active = false;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, t_mac, 17) == 0
            && millis() - rssi_tracker[i].last_seen < RSSI_TRACK_EXPIRY_MS) {
            is_active = true; break;
        }
    }
    xSemaphoreGive(dataMutex);

    spr.fillRect(0, 20, DISP_W, 22, HILIGHT_BG);
    spr.drawLine(0, 41, DISP_W, 41, ccol);
    spr.fillRect(0, 20, 4, 22, ccol);

    spr.setTextColor(ccol, HILIGHT_BG); spr.setTextSize(1);
    spr.setCursor(10, 28); spr.print(t_type);

    uint16_t dot_col = is_active ? ((millis() % 800 < 400) ? ACCENT_COLOR : DIM_COLOR) : DIM2_COLOR;
    spr.fillCircle(DISP_W - 14, 31, 4, dot_col);
    spr.setTextColor(is_active ? ACCENT_COLOR : DIM_COLOR, HILIGHT_BG);
    spr.setCursor(DISP_W - 56, 28); spr.print(is_active ? "LIVE" : "LOST");

    spr.setTextColor(DIM_COLOR, HILIGHT_BG);
    spr.setCursor(80, 28); spr.print("@ "); spr.print(t_time);

    bool use_name = (t_name[0] != '\0' &&
                     strcmp(t_name, "Hidden")  != 0 &&
                     strcmp(t_name, "Unknown") != 0);
    drawCard(4, 46, DISP_W - 8, 22);
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1);
    char dn[33] = "";
    strncpy(dn, use_name ? t_name : t_mac, 32); dn[32] = '\0';
    spr.setCursor(10, 54); spr.print(dn);
    if (use_name) {
        const char* sm = (strlen(t_mac) > 8) ? t_mac + 9 : t_mac;
        spr.setTextColor(DIM_COLOR, CARD_COLOR);
        spr.setCursor(DISP_W - 70, 54); spr.print(sm);
    }

    int cw = (DISP_W - 12) / 2;

    drawCard(4, 72, cw, 46);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(8, 77); spr.print("SIGNAL");
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(3);
    spr.setCursor(8, 87); spr.print(t_rssi);
    spr.setTextSize(1); spr.setTextColor(DIM_COLOR, CARD_COLOR);
    spr.print("dBm");

    drawCard(8 + cw, 72, cw, 46);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(12 + cw, 77); spr.print("CONFIDENCE");
    spr.setTextColor(ccol, CARD_COLOR); spr.setTextSize(3);
    spr.setCursor(12 + cw, 87); spr.print(t_conf);
    spr.setTextSize(1); spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.print("%");
    spr.setTextColor(ccol, CARD_COLOR);
    spr.setCursor(12 + cw, 112); spr.print(confidence_label(t_conf));

    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, DISP_H - 8);
    char mshort[36]; snprintf(mshort, sizeof(mshort), "%.35s", t_method);
    spr.print(mshort);
}

static int history_scroll_offset = 0;

void draw_capture_history_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    CaptureEntry local_history[CAPTURE_HISTORY_SIZE];
    int local_count = capture_history_count;
    for (int i = 0; i < local_count; i++) local_history[i] = capture_history[i];
    xSemaphoreGive(dataMutex);

    int max_scroll = max(0, local_count - 4);
    if (history_scroll_offset > max_scroll) history_scroll_offset = max_scroll;
    if (history_scroll_offset < 0) history_scroll_offset = 0;

    spr.fillSprite(BG_COLOR);
    draw_header_spr(3);

    if (local_count == 0) {
        for (int i = 0; i < 4; i++) {
            int y = 20 + i * 28;
            spr.fillRect(0, y, 3, 27, DIM2_COLOR);
            spr.fillRect(3, y, DISP_W - 3, 27, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setTextColor(DIM2_COLOR, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR); spr.setTextSize(1);
            spr.setCursor(10, y + 9); spr.print("-- Listening...");
        }
        return;
    }

    int rows_shown = 0;
    for (int i = history_scroll_offset; i < local_count && rows_shown < 4; i++, rows_shown++) {
        int y = 20 + rows_shown * 28;
        uint16_t rcol = confidence_color(local_history[i].confidence);
        spr.fillRect(0, y, 4, 27, rcol);
        spr.fillRect(4, y, DISP_W - 4, 27, (rows_shown % 2 == 0) ? HILIGHT_BG : BG_COLOR);
        uint16_t bg = (rows_shown % 2 == 0) ? HILIGHT_BG : BG_COLOR;

        const char* t = "SYS";
        if      (strcmp(local_history[i].type, "FLOCK_WIFI")  == 0) t = "WIFI";
        else if (strcmp(local_history[i].type, "FLOCK_BLE")   == 0) t = "BLE";
        else if (strcmp(local_history[i].type, "RAVEN_BLE")   == 0) t = "RAVN";
        else if (strcmp(local_history[i].type, "SIMULATION")  == 0) t = "TEST";

        char nom[17] = "";
        bool name_ok = (local_history[i].name[0] != '\0' &&
                        strcmp(local_history[i].name, "Hidden")  != 0 &&
                        strcmp(local_history[i].name, "Unknown") != 0);
        if (name_ok) {
            strncpy(nom, local_history[i].name, 16); nom[16] = '\0';
        } else {
            const char* sm = (strlen(local_history[i].mac) > 8) ? local_history[i].mac + 9 : local_history[i].mac;
            strncpy(nom, sm, 16); nom[16] = '\0';
        }

        spr.setTextColor(rcol, bg); spr.setTextSize(1);
        spr.setCursor(10, y + 4); spr.print(t);

        spr.setTextColor(TEXT_COLOR, bg); spr.setTextSize(1);
        spr.setCursor(38, y + 4); spr.print(nom);

        char stat_buf[28];
        snprintf(stat_buf, sizeof(stat_buf), "%d%%  %ddBm  %s",
                 local_history[i].confidence, local_history[i].rssi, local_history[i].time);
        spr.setTextColor(DIM_COLOR, bg);
        spr.setCursor(10, y + 17); spr.print(stat_buf);
    }

    if (local_count > 4) {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(DISP_W - 50, DISP_H - 8);
        char scr_buf[14]; snprintf(scr_buf, sizeof(scr_buf), "%d/%d UP/DN", history_scroll_offset + 1, local_count);
        spr.print(scr_buf);
    }
}

static int gps_page = 0;
void gps_page_toggle() { gps_page = (gps_page + 1) % 2; }

void draw_gps_screen() {
    spr.fillSprite(BG_COLOR);
    draw_header_spr(4);

    bool has_loc, stale, speed_valid, hdg_valid, alt_valid;
    int sats; uint32_t chars;
    double d_lat, d_lng;
    float f_spd, f_hdg, f_alt;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    has_loc     = gps.location.isValid();
    stale       = has_loc && (gps.location.age() > 2000);
    sats        = gps.satellites.isValid() ? gps.satellites.value() : 0;
    chars       = gps.charsProcessed();
    d_lat       = gps.location.lat();
    d_lng       = gps.location.lng();
    speed_valid = gps.speed.isValid();
    f_spd       = gps.speed.mph();
    hdg_valid   = gps.course.isValid();
    f_hdg       = gps.course.deg();
    alt_valid   = gps.altitude.isValid();
    f_alt       = gps.altitude.meters();
    xSemaphoreGive(dataMutex);

    drawCard(4, 22, 114, 108);
    int gx0 = 61, gy0 = 73;
    int gx = gx0 + (int)imu_px;
    int gy = gy0 + (int)imu_py;
    int gr = 26;

    const float TILT_GPS = 0.65f;

    if (!has_loc || stale) {
        const int LEAN = 8;
        int pin_cx  = gx0 - 4 + (int)imu_px;
        int pin_top = gy0 - 28 + (int)imu_py;
        int pin_hr  = 11;
        int pin_hvr = (int)(pin_hr * TILT_GPS);
        int pin_tip_x = gx0 - 4 + LEAN + (int)(imu_px / 2);
        int pin_tip_y = gy0 + 16 + (int)(imu_py / 2);

        spr.fillEllipse(pin_tip_x + 3, pin_tip_y + 4, 6, 2, lgfx::color565(4, 8, 16));

        for (int y = pin_top + pin_hvr - 2; y <= pin_tip_y; y++) {
            float t = (float)(y - (pin_top + pin_hvr - 2))
                    / (float)(pin_tip_y - (pin_top + pin_hvr - 2) + 1);
            int half_w = max(0, (int)(pin_hr * (1.0f - t * t)));
            int cx_here = pin_cx + (int)(LEAN * t);
            uint8_t bv = 20 + (uint8_t)((1.0f - t) * 40);
            uint8_t blended_b = bv * 3 / 2;
            uint16_t seg = night_mode
                ? lgfx::color565(bv, bv / 6, bv / 6)
                : lgfx::color565(bv / 4, bv / 2, blended_b);
            spr.drawLine(cx_here - half_w, y, cx_here + half_w, y, seg);
        }

        spr.fillEllipse(pin_cx, pin_top + 2, pin_hr, pin_hvr, lgfx::color565(4, 12, 28));
        int HEAD_THICK = 5;
        for (int i = HEAD_THICK - 1; i > 0; i--) {
            uint8_t wv = 10 + i * 5;
            spr.drawEllipse(pin_cx, pin_top + i, pin_hr, pin_hvr,
                            night_mode
                            ? lgfx::color565(wv * 2, wv / 4, wv / 4)
                            : lgfx::color565(wv / 4, wv, wv * 2));
        }
        spr.fillEllipse(pin_cx, pin_top, pin_hr, pin_hvr,
                        night_mode ? lgfx::color565(28, 5, 5) : lgfx::color565(10, 28, 65));

        spr.fillEllipse(pin_cx - 4, pin_top - 2, pin_hr / 3, pin_hvr / 2,
                        night_mode ? lgfx::color565(140, 30, 30) : lgfx::color565(80, 150, 240));
        spr.fillEllipse(pin_cx - 5, pin_top - 3, pin_hr / 5, pin_hvr / 3,
                        night_mode ? lgfx::color565(200, 60, 60) : lgfx::color565(160, 210, 255));

        spr.drawEllipse(pin_cx, pin_top, pin_hr, pin_hvr,
                        stale ? WARN_COLOR : HEADER_COLOR);
        spr.drawEllipse(pin_cx, pin_top, pin_hr / 2, pin_hvr / 2,
                        stale ? WARN_COLOR : DIM_COLOR);
        spr.fillCircle(pin_cx, pin_top, 2, stale ? WARN_COLOR : ACCENT_COLOR);

        float phase_pin = millis() / 80.0f;
        for (int i = 0; i < 4; i++) {
            float raw = fmodf(phase_pin + i * 20.0f, 60.0f);
            if (raw < 4.0f) continue;

            float frac = (raw - 4.0f) / 56.0f;
            int   ring_r = pin_hr + 2 + (int)(frac * 36.0f);

            float opacity = sinf(frac * (float)M_PI);
            int   bright  = (int)(opacity * 180.0f);
            if (bright < 10) continue;

            uint16_t ring_col = night_mode
                ? lgfx::color565(bright, bright / 6, bright / 6)
                : lgfx::color565(0, bright / 2, bright);

            spr.drawArc(pin_cx, pin_top, ring_r, ring_r - 1, 210, 330, ring_col);
        }

        spr.setTextColor(stale ? WARN_COLOR : TEAL_COLOR, CARD_COLOR);
        spr.setTextSize(1);
        spr.setCursor(14, gy0 + 36);
        spr.print(stale ? "SIGNAL LOST" : "SEARCHING...");

        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(14, gy0 + 48); spr.print("SATS: ");
        spr.setTextColor(sats > 0 ? TEXT_COLOR : DIM_COLOR, CARD_COLOR); spr.print(sats);

    } else {
        float phase = (millis() / 8000.0f) * (float)M_PI;

        spr.fillCircle(gx, gy, gr, lgfx::color565(4, 10, 30));
        spr.fillCircle(gx + gr/4, gy - gr/4, gr/2, lgfx::color565(14, 30, 70));
        spr.fillCircle(gx + gr/3, gy - gr/3, gr/4, lgfx::color565(22, 45, 100));
        spr.drawCircle(gx, gy, gr, HEADER_COLOR);

        const float AXIS_TILT_RAD = radians(23.0f);
        int ax1x = gx - (int)((gr - 2) * sinf(AXIS_TILT_RAD));
        int ax1y = gy - (int)((gr - 2) * cosf(AXIS_TILT_RAD));
        int ax2x = gx + (int)((gr - 2) * sinf(AXIS_TILT_RAD));
        int ax2y = gy + (int)((gr - 2) * cosf(AXIS_TILT_RAD));
        spr.drawLine(ax1x, ax1y, ax2x, ax2y, DIM2_COLOR);
        spr.fillCircle(ax1x, ax1y, 2, DIM_COLOR);
        spr.fillCircle(ax2x, ax2y, 2, DIM_COLOR);

        const float GLOBE_TILT = 0.35f;
        spr.drawEllipse(gx, gy, gr, (int)(gr * GLOBE_TILT), DIM_COLOR);

        for (int m = 0; m < 2; m++) {
            float mp = phase + m * (float)M_PI / 2.0f;
            int mw = max(1, (int)fabsf(gr * cosf(mp)));
            float ff = (cosf(mp) + 1.0f) / 2.0f;
            uint8_t mv = 18 + (uint8_t)(ff * 50.0f);
            uint16_t mc = night_mode
                ? lgfx::color565(mv, mv / 8, mv / 8)
                : lgfx::color565(mv / 8, mv / 3, mv);
            spr.drawEllipse(gx, gy, mw, gr, mc);
        }

        float sat_phase = (millis() / 2200.0f) * (float)M_PI;
        const float SAT_R   = (float)(gr + 9);
        const float SAT_INC = 0.50f;

        spr.drawEllipse(gx0, gy0, (int)SAT_R, (int)(SAT_R * SAT_INC),
                        lgfx::color565(12, 22, 42));

        for (int tr = 1; tr <= 6; tr++) {
            float ta = sat_phase - tr * 0.18f;
            int tx = gx0 + (int)(SAT_R * cosf(ta));
            int ty = gy0 - (int)(SAT_R * sinf(ta) * SAT_INC);
            bool t_front = sinf(ta) > 0;
            if (!t_front) continue;
            uint8_t tv = (uint8_t)((7 - tr) * 20);
            spr.drawPixel(tx, ty, lgfx::color565(tv, tv * 2, tv));
        }

        int sat_x = gx0 + (int)(SAT_R * cosf(sat_phase));
        int sat_y = gy0 - (int)(SAT_R * sinf(sat_phase) * SAT_INC);
        bool sat_front = sinf(sat_phase) > 0;
        if (sat_front) {
            spr.fillCircle(sat_x, sat_y, 3, ACCENT_COLOR);
            spr.drawCircle(sat_x, sat_y, 4, lgfx::color565(50, 180, 80));
        } else {
            spr.fillCircle(sat_x, sat_y, 2, DIM_COLOR);
        }

        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setTextSize(1);
        spr.setCursor(20, gy0 + 36);
        spr.print("GPS LOCKED");
    }

    spr.setTextColor(DIM_COLOR, CARD_COLOR);
    spr.setTextSize(1);
    spr.fillCircle(112, 124, 3, gps_page == 0 ? HEADER_COLOR : DIM2_COLOR);
    spr.fillCircle(104, 124, 3, gps_page == 1 ? HEADER_COLOR : DIM2_COLOR);
    spr.setCursor(8, 124);
    spr.setTextColor(DIM_COLOR, CARD_COLOR);
    spr.print("g=pg");

    if (gps_page == 0) {
        drawCard(122, 22, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(126, 26); spr.print("SATS:");
        spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setCursor(162, 26); spr.print(sats);

        drawCard(122, 44, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(126, 48); spr.print("LAT:");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR);
        spr.setCursor(152, 48); spr.print(has_loc ? d_lat : 0.0, 5);

        drawCard(122, 66, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(126, 70); spr.print("LON:");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR);
        spr.setCursor(152, 70); spr.print(has_loc ? d_lng : 0.0, 5);

        drawCard(122, 88, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(126, 92); spr.print("SPD:");
        spr.setTextColor(TEXT_COLOR, CARD_COLOR);
        spr.setCursor(152, 92);
        spr.print(speed_valid ? f_spd : 0.0f, 1); spr.print("mph");

        drawCard(122, 110, 114, 11);
        spr.setTextColor(DIM_COLOR, CARD_COLOR);
        spr.setCursor(126, 113); spr.print("press g for HDG/ALT");

    } else {
        drawCard(122, 22, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(126, 26); spr.print("HDG:");
        spr.setTextColor(hdg_valid ? TEXT_COLOR : DIM_COLOR, CARD_COLOR);
        if (hdg_valid) {
            static const char* hdg_cards[] = {"N","NE","E","SE","S","SW","W","NW"};
            int hc = ((int)(f_hdg + 22.5f) / 45) % 8;
            char hdg_buf[9]; snprintf(hdg_buf, sizeof(hdg_buf), "%3.0f %s", f_hdg, hdg_cards[hc]);
            spr.setCursor(150, 26); spr.print(hdg_buf);
        } else { spr.setCursor(150, 26); spr.print("--"); }

        drawCard(122, 44, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(126, 48); spr.print("ALT:");
        spr.setTextColor(alt_valid ? TEXT_COLOR : DIM_COLOR, CARD_COLOR);
        if (alt_valid) {
            char alt_buf[8]; snprintf(alt_buf, sizeof(alt_buf), "%.0fm", f_alt);
            spr.setCursor(152, 48); spr.print(alt_buf);
        } else { spr.setCursor(152, 48); spr.print("--"); }

        drawCard(122, 66, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(126, 70); spr.print("CHARS:");
        spr.setTextColor(TEXT_COLOR, CARD_COLOR);
        spr.setCursor(170, 70); spr.print(chars);

        drawCard(122, 88, 114, 19);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR);
        spr.setCursor(126, 92); spr.print("FIX:");
        spr.setTextColor(has_loc && !stale ? ACCENT_COLOR : (stale ? WARN_COLOR : DIM_COLOR), CARD_COLOR);
        spr.setCursor(152, 92);
        spr.print(has_loc && !stale ? "3D LOCK" : (stale ? "STALE" : "NONE"));

        drawCard(122, 110, 114, 11);
        spr.setTextColor(DIM_COLOR, CARD_COLOR);
        spr.setCursor(126, 113); spr.print("press g for LAT/LON");
    }
}

void draw_device_info_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long lt=lifetime_flock_total;
    long sr=session_raven; long sw=session_flock_wifi; long sb=session_flock_ble;
    long lb = lifetime_boots;
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR);
    draw_header_spr(5);
    
    drawCard(4, 22, DISP_W - 8, 20);
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(10, 27); spr.print(VERSION_STRING);
    
    drawCard(4,  46, 72, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 50); spr.print("BOOTS");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(8, 60);
    spr.print(lb);
    
    drawCard(82,  46, 72, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 50); spr.print("LIFETIME");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 60);
    { char _tbuf[9]; format_time_buf(lifetime_seconds, _tbuf, sizeof(_tbuf)); spr.print(_tbuf); }
    
    drawCard(160, 46, 76, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(164, 50); spr.print("ALL-TIME");
    spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(164, 60); spr.print(lt);
    
    drawCard(4,   88, 72, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 92); spr.print("WIFI SESS");
    spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(8, 102); spr.print(sw);
    
    drawCard(82,  88, 72, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 92); spr.print("BLE SESS");
    spr.setTextColor(PURPLE_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(86, 102); spr.print(sb);
    
    drawCard(160, 88, 76, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(164, 92); spr.print("RAVEN");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(164, 102); spr.print(sr);
}

// ============================================================================
// MAIN UI CONTROLLER
// ============================================================================
void draw_current_screen() {
    switch (current_screen) {
        case 0: draw_scanner_screen();         break;
        case 1: draw_locator_screen();         break;
        case 2: draw_last_detect_screen();     break;
        case 3: draw_capture_history_screen(); break;
        case 4: draw_gps_screen();             break;
        case 5: draw_device_info_screen();     break;
    }
    
    if (show_vol_overlay) draw_vol_overlay();
    if (show_help_overlay) draw_help_overlay();
    draw_toast_spr();
}

void transition_screen(int new_screen, int dir) {
    if (stealth_mode) { current_screen = new_screen; return; }
    if (new_screen == 3) history_scroll_offset = 0;
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
    
    M5Cardputer.Speaker.setVolume(0); 
    M5Cardputer.Display.setRotation(1);
    apply_color_palette();

    delay(100); SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN); delay(100);
    int16_t init_v = M5Cardputer.Power.getBatteryVoltage();
    for(int i = 0; i < BATTERY_SAMPLE_SIZE; i++) battery_buffer[i] = init_v;
    WiFi.mode(WIFI_STA); delay(50);
    
    int32_t start_v = get_median_voltage(); unsigned long trap_start = millis();
    while (millis() - trap_start < 800) { M5Cardputer.update(); update_battery_metrics(); delay(15); }
    int32_t end_v = get_median_voltage();
    if (end_v > 3900 || (end_v - start_v >= 15)) dedicated_charging_loop();

    Serial.begin(115200); delay(200);  
    setCpuFrequencyMhz(240); dataMutex = xSemaphoreCreateMutex();

    memset(seen_mac_table, 0, sizeof(seen_mac_table));

    if (!LittleFS.begin(true)) { littlefs_available = false; } 
    else { 
        littlefs_available = true; 
        load_session_from_flash(); 
    }
    
    lifetime_boots++;
    save_session_to_flash();

    bool mount_success = false;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_CS_PIN);
    for (int i = 0; i < 3; i++) { 
        if (SD.begin(SD_CS_PIN, SPI, 25000000)) { mount_success = true; break; } 
        delay(100); 
    }
    if (mount_success) {
        sd_available = true; current_log_file = "/FlockLog.csv";
        if (!SD.exists(current_log_file.c_str())) {
            File file = SD.open(current_log_file.c_str(), FILE_WRITE);
            if (file) { file.println("Uptime_ms,Channel,Type,Proto,RSSI,MAC,Name,TXPower,Method,Conf,ConfLabel,Extra,SeqNum,Lat,Lon,SpeedMPH,HeadingDeg,AltM"); file.close(); }
        }
        if (!SD.exists(current_pcap_file.c_str())) {
            File pfile = SD.open(current_pcap_file.c_str(), FILE_WRITE);
            if (pfile) {
                uint32_t pcap_header[6] = {0xa1b2c3d4, 0x00040002, 0x00000000, 0x00000000, 0x0000ffff, 0x00000069};
                pfile.write((const uint8_t*)pcap_header, 24); pfile.close();
            }
        }
    }

    session_start_time = millis();

    NimBLEDevice::init(""); NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    pBLEScan = NimBLEDevice::getScan(); pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), false);
    pBLEScan->setActiveScan(true); pBLEScan->setInterval(97); pBLEScan->setWindow(97);

    spr.setColorDepth(16);
    spr.createSprite(DISP_W, DISP_H);

    WiFi.disconnect(); delay(100); esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t filt; filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt); esp_wifi_set_promiscuous(true); 
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler); 
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); delay(100);

    draw_current_screen(); spr.pushSprite(0,0);

    if (!is_muted) {
        int boot_vol = current_volume > 120 ? current_volume : 120;
        M5Cardputer.Speaker.setVolume(boot_vol);
        M5Cardputer.Speaker.tone(1320, 80);  delay(100);
        M5Cardputer.Speaker.tone(880,  80);  delay(100);
        M5Cardputer.Speaker.tone(660,  80);  delay(100);
        M5Cardputer.Speaker.tone(220,  200); delay(260);
        delay(50);
    }
    M5Cardputer.Speaker.setVolume(current_volume);

    last_channel_hop = millis(); last_sd_flush = millis(); last_persist_save = millis();
    xTaskCreatePinnedToCore(ScannerLoopTask, "ScannerTask", 8192, NULL, 1, &ScannerTaskHandle, 0);
    xTaskCreatePinnedToCore(GPSLoopTask, "GPSTask", 4096, NULL, 1, &GPSTaskHandle, 0);
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    M5Cardputer.update(); yield(); update_battery_metrics();
    int32_t loop_mv = get_median_voltage();

    // ── IMU PARALLAX UPDATE ───────────────────────────────────────────────
    {
        static bool imu_checked = false;
        if (!imu_checked) {
            imu_available = (M5.Imu.getType() != m5::imu_none);
            imu_checked = true;
        }
        if (imu_available && M5.Imu.update()) {
            auto d = M5.Imu.getImuData();
            const float ACCEL_SCALE = 6.0f;
            const float MAX_PX     = 3.0f;
            float raw_px =  d.accel.x * ACCEL_SCALE;
            float raw_py = -d.accel.y * ACCEL_SCALE;
            if (raw_px >  MAX_PX) raw_px =  MAX_PX;
            if (raw_px < -MAX_PX) raw_px = -MAX_PX;
            if (raw_py >  MAX_PX) raw_py =  MAX_PX;
            if (raw_py < -MAX_PX) raw_py = -MAX_PX;
            const float ALPHA = 0.08f;
            imu_px += ALPHA * (raw_px - imu_px);
            imu_py += ALPHA * (raw_py - imu_py);
        }
    }

    process_wifi_event_queue();

    if (trigger_alarm_confidence >= 50) {
        int conf = trigger_alarm_confidence; trigger_alarm_confidence = 0; play_escalated_alarm(conf);
    } else { trigger_alarm_confidence = 0; }

    if (M5Cardputer.BtnA.wasClicked() && !stealth_mode) {
        int next_screen = current_screen + 1;
        int dir = (next_screen >= NUM_SCREENS) ? -1 : 1;
        if (next_screen >= NUM_SCREENS) next_screen = 0;
        transition_screen(next_screen, dir);
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        
        if (status.tab && !stealth_mode) { 
            show_help_overlay = !show_help_overlay; 
            draw_current_screen(); spr.pushSprite(0,0); 
        }

        for (auto c : status.word) {
            
            if (c == 'n') { 
                night_mode = !night_mode; apply_color_palette();
                draw_current_screen(); spr.pushSprite(0,0);
            } 
            else if (c == 'q' || c == 'm') { 
                is_muted = !is_muted; draw_current_screen(); spr.pushSprite(0,0);
                if (!is_muted) beep(600, 50); 
            }
            else if (c == '.') { 
                if (current_screen == 3) {
                    history_scroll_offset++; draw_current_screen(); spr.pushSprite(0, 0);
                } else {
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
            }
            else if (c == ';') { 
                if (current_screen == 3) {
                    history_scroll_offset--; if (history_scroll_offset < 0) history_scroll_offset = 0;
                    draw_current_screen(); spr.pushSprite(0, 0);
                } else {
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
                    sim_wifi = !sim_wifi;
                    trigger_alarm_confidence = 100; 
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
                    
                    locator_start(t_mac, t_name);
                    trigger_toast("TARGET", t_name, t_conf);
                    transition_screen(1, 1);
                } else if (!stealth_mode) {
                    trigger_toast("TARGET", "No history", 0);
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
            else if (c >= '1' && c <= '6') { 
                if (!stealth_mode) { 
                    int target = c - '1'; 
                    if (target < NUM_SCREENS) transition_screen(target, (target >= current_screen) ? 1 : -1); 
                } 
            } 
            else if (c == '0') { if (!stealth_mode) transition_screen(1, 1); } 
            else if (c == 's') { stealth_mode = !stealth_mode; if (stealth_mode) { M5Cardputer.Display.setBrightness(0); } else { M5Cardputer.Display.setBrightness(200); draw_current_screen(); spr.pushSprite(0,0); } } 
            else if (c == 'g') {
                gps_page_toggle();
                if (current_screen == 4) { draw_current_screen(); spr.pushSprite(0, 0); }
            }
            else if (c == 'l') { 
                if (locator_active) { 
                    locator_stop(); 
                } else {
                    xSemaphoreTake(dataMutex, portMAX_DELAY);
                    char l_type[16]; strncpy(l_type, last_cap_type, 15); l_type[15] = '\0';
                    char l_mac[18];  strncpy(l_mac,  last_cap_mac,  17); l_mac[17]  = '\0';
                    char l_name[65]; strncpy(l_name, last_cap_name, 64); l_name[64] = '\0';
                    xSemaphoreGive(dataMutex);
                    if (strcmp(l_type, "None") != 0 && l_type[0] != '\0') {
                        locator_start(l_mac, l_name);
                        transition_screen(1, 1); 
                    } 
                } 
            } 
            else if (c == '\n' || c == '\r') {
                if (!stealth_mode) {
                    int next = current_screen + 1;
                    int d = (next >= NUM_SCREENS) ? -1 : 1;
                    if (next >= NUM_SCREENS) next = 0;
                    transition_screen(next, d);
                }
            }
        }
        if (status.del && !stealth_mode) {
            int prev = current_screen - 1;
            int d = (prev < 0) ? 1 : -1;
            if (prev < 0) prev = NUM_SCREENS - 1;
            transition_screen(prev, d);
        }
    }

    if (millis() - last_time_save >= 1000) { xSemaphoreTake(dataMutex, portMAX_DELAY); lifetime_seconds++; xSemaphoreGive(dataMutex); last_time_save = millis(); }
    if (millis() - last_persist_save >= PERSIST_INTERVAL_MS) save_session_to_flash();
    rssi_track_expire();

    if (millis() - last_sd_flush_check >= 500) {
        last_sd_flush_check = millis(); 
        bool should_flush = false; 
        
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (sd_write_count >= MAX_LOG_BUFFER || pcap_write_count >= MAX_PCAP_BUFFER || 
           (millis() - last_sd_flush > SD_FLUSH_INTERVAL && (sd_write_count > 0 || pcap_write_count > 0))) {
            should_flush = true;
        }
        xSemaphoreGive(dataMutex); 
        
        if (should_flush) flush_sd_buffer();
    }

    if (locator_active && locator_has_estimate) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (gps.location.isValid() && gps.location.age() < 2000) {
            locator_est_distance = haversine_m(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
            locator_bearing = bearing_to(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
        }
        xSemaphoreGive(dataMutex);
    }

    if (!stealth_mode) {
        static unsigned long last_fast_anim = 0; static unsigned long last_slow_ui = 0; unsigned long now = millis();
        static uint8_t last_loop_led = 255;
        if (is_device_charging(loop_mv)) {
            float t_breath = now / 2000.0f * (float)M_PI;
            float br = sinf(t_breath); br = br * br;
            uint8_t led_val = (uint8_t)(br * 80.0f);
            if (led_val != last_loop_led) { set_cardputer_led(0, led_val, 0); last_loop_led = led_val; }
        } else { if (last_loop_led != 0) { set_cardputer_led(0, 0, 0); last_loop_led = 0; } }
        
        if (current_screen == 0 || current_screen == 2 || current_screen == 4 || show_vol_overlay || toast_active || (now - last_fast_anim < 30)) { 
            if (now - last_fast_anim >= 15) { draw_current_screen(); spr.pushSprite(0, 0); last_fast_anim = now; } 
        } 
        else { if (now - last_slow_ui >= 100) { draw_current_screen(); spr.pushSprite(0, 0); last_slow_ui = now; } }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
}
