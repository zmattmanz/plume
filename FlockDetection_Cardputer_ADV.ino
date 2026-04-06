// ============================================================================
// FLOCK DETECTOR v6.1-ADV — M5Stack Cardputer ADV Port
// ============================================================================
// v6.1 Optimizations:
//   - Removed redundant includes: WiFi.h, HardwareSerial.h, math.h, vector
//   - WiFi + BLE callbacks: String→char[] eliminates heap malloc in hot path
//   - RSSITrack struct: String mac→char[18], no heap per tracked device
//   - SD write buffer: std::vector<String>→fixed char[10][220] array
//   - log_detection CSV: String concat→snprintf, writes to buffer not SD directly  
//   - flush_sd_buffer: works with fixed array, single SD open per flush cycle
//   - beep(): removed blocking delay(), no more packet blindness during alarms
//   - BLE callback: bool flags replace methods.indexOf() string scans
//   - Battery: global_smoothed_mv EMA already computed once per loop tick
//
// Hardware changes vs original:
//   Display : SSD1306 128x64 OLED  →  ST7789 240x135 TFT (via M5Cardputer lib)
//   Input   : Single button (D1)   →  Cardputer keyboard matrix
//   Audio   : Piezo tone() on A3   →  M5Cardputer speaker (M5Cardputer.Speaker)
//   SD      : SPI on D2 CS         →  M5Cardputer built-in SD (G12 CS)
//   GPS     : HardwareSerial(D6/D7)→  HardwareSerial on Cardputer EXT pins
//             (G1=RX, G2=TX via Grove/EXT header)
//
// Key mappings:
//   G0 (side button) = cycle screens forward
//   Backspace        = cycle screens backward  
//   's'              = toggle stealth mode (display off)
//   'l'              = toggle locator on locator screen (replaces double-press)
//   Enter            = same as G0 (cycle forward)
//
// Libraries required (install via Arduino Library Manager):
//   M5Cardputer       (M5Stack official)
//   NimBLE-Arduino    
//   TinyGPSPlus       
//   ArduinoJson       (v6)
//
// Build target: M5Stack-Cardputer (Arduino IDE board manager)
// ============================================================================

#include <M5Cardputer.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <TinyGPSPlus.h>

// ============================================================================
// DISPLAY ABSTRACTION
// ============================================================================
// The Cardputer ADV uses a 240x135 TFT (landscape). We map the original
// 128x64 OLED coordinate space to the TFT by scaling 2x and centering.
// All draw_ functions use M5Cardputer.Display (LovyanGFX / M5GFX).
//
// Scale factors: x*1.875 (240/128), y*2.1 (135/64) — rounded to x*2, y*2
// with a small left/top margin so content stays readable.
//
// Colour palette:
//   TEXT_COLOR   = WHITE (TFT_WHITE)
//   BG_COLOR     = BLACK (TFT_BLACK)
//   ALERT_COLOR  = RED   (TFT_RED)
//   HEADER_COLOR = CYAN  (TFT_CYAN)
//   TOAST_COLOR  = YELLOW(TFT_YELLOW)

#define DISP_W       240
#define DISP_H       135
// ── Washer-detector color palette ──
// lgfx::color565(r,g,b) is a constexpr function — safe as compile-time #define
#define BG_COLOR      lgfx::color565( 10,  10,  24)  // near-black navy
#define CARD_COLOR    lgfx::color565( 14,  16,  34)  // card background
#define CARD_BORDER   lgfx::color565( 26,  42,  58)  // card border
#define PANEL_BG      lgfx::color565( 14,  20,  32)  // panel bg
#define HEADER_COLOR  lgfx::color565(  0, 238, 255)  // bright cyan
#define TEXT_COLOR    lgfx::color565(255, 255, 255)  // white
#define DIM_COLOR     lgfx::color565( 51,  68,  85)  // muted label
#define DIM2_COLOR    lgfx::color565( 34,  51,  68)  // darker muted
#define ACCENT_COLOR  lgfx::color565(  0, 238, 100)  // bright green
#define TEAL_COLOR    lgfx::color565(  0, 153, 187)  // teal
#define CYAN2_COLOR   lgfx::color565(  0, 170, 204)  // mid cyan
#define ALERT_COLOR   lgfx::color565(255,   0,   0)  // red
#define TOAST_COLOR   lgfx::color565(255, 224,   0)  // yellow
#define WARN_COLOR    lgfx::color565(255, 140,   0)  // orange
#define GRID_COLOR    lgfx::color565( 16,  20,  36)  // dark grid
#define HILIGHT_BG    lgfx::color565( 26,  42,  58)  // highlight panels

// Coordinate mapping from original 128x64 space
// We use a simple scale: multiply x by ~1.85, y by ~2.0
// and add a small left margin (4px) and top margin (2px)
// Scaled font size (original used textSize(1) = 6x8 pixels)
// At scale 2 that is effectively textSize(2) on the TFT

// ============================================================================
// PIN DEFINITIONS — Cardputer ADV
// ============================================================================
// SD card CS is handled by M5Cardputer library (G12)
// GPS via EXT header: G1=RX into ESP, G2=TX from ESP (connect to GPS module TX/RX)
#define GPS_RX_PIN   1    // ESP receives from GPS TX
#define GPS_TX_PIN   2    // ESP transmits to GPS RX  
#define GPS_BAUD     9600

// SD CS pin for Cardputer (used with SD.begin)
#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_CS_PIN       12

// ============================================================================
// CONFIGURATION (unchanged from v3.3)
// ============================================================================
#define LOW_FREQ  200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define DETECT_FREQ_HIGH 1200
#define DETECT_FREQ_CERTAIN 1500
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150

#define MAX_CHANNEL 13
#define BLE_SCAN_DURATION 2
#define BLE_SCAN_INTERVAL 3000
#define BUZZER_COOLDOWN 60000
#define LOG_UPDATE_DELAY 500
#define IGNORE_WEAK_RSSI -80

#define MAX_LOG_BUFFER 10
#define SD_FLUSH_INTERVAL 10000

#define DWELL_PRIMARY   500
#define DWELL_SECONDARY 200

#define REDETECT_WINDOW_MS 300000

#define RSSI_TRACK_MAX_DEVICES 16
#define RSSI_TRACK_SAMPLES 5
#define RSSI_TRACK_EXPIRY_MS 15000
#define CONF_BONUS_STATIONARY 15

#define PERSIST_INTERVAL_MS 60000
#define PERSIST_FILE "/flock_session.dat"

#define TZ_OFFSET_HOURS -4   // EDT. Change to -5 for EST.

#define TOAST_DURATION_MS 2500

#define LOC_MAX_SAMPLES 40
#define LOC_SAMPLE_INTERVAL 500
#define LOC_TX_POWER_DEFAULT -59
#define LOC_PATH_LOSS_N 2.5
#define LOC_MIN_SAMPLES_EST 3
#define LOC_ARROW_RADIUS 28    // Larger for bigger screen

// ============================================================================
// CONFIDENCE SCORING (unchanged)
// ============================================================================
#define CONF_MAC_TIER1        40
#define CONF_MAC_TIER2        20
#define CONF_SSID_PATTERN     50
#define CONF_SSID_FLOCK_FMT   65
#define CONF_BLE_NAME         45
#define CONF_MFG_ID           60
#define CONF_RAVEN_UUID       70
#define CONF_RAVEN_MULTI_UUID 90
#define CONF_PENGUIN_SERIAL   80

#define CONF_BONUS_STRONG_RSSI     10
#define CONF_BONUS_MULTI_METHOD    20
#define CONF_BONUS_BLE_STATIC_ADDR 10

#define CONFIDENCE_ALARM_THRESHOLD 50
#define CONFIDENCE_HIGH    70
#define CONFIDENCE_CERTAIN 85

// ============================================================================
// GLOBAL VARIABLES (unchanged from v3.3)
// ============================================================================
TaskHandle_t ScannerTaskHandle;
SemaphoreHandle_t dataMutex;

static uint8_t current_channel = 1;
static unsigned long last_channel_hop = 0;
static unsigned long last_ble_scan = 0;
static unsigned long last_buzzer_time = 0;
static NimBLEScan* pBLEScan;
bool sd_available = false;
bool littlefs_available = false;
volatile int trigger_alarm_confidence = 0;

// Fixed SD write buffer — no heap allocation, no std::vector
char sd_write_buffer[MAX_LOG_BUFFER][220];
int  sd_write_count = 0;
unsigned long last_sd_flush = 0;

String current_log_file = "/FlockLog.csv";

#define NUM_SCREENS 10
int current_screen = 0;
bool stealth_mode = false;

long session_wifi = 0;
long session_ble = 0;
unsigned long session_start_time = 0;
long lifetime_wifi = 0;
long lifetime_ble = 0;
unsigned long lifetime_seconds = 0;
long lifetime_flock_total = 0;

#define MAX_SEEN_MACS 200
struct SeenMAC {
    String mac;
    unsigned long timestamp;
};
SeenMAC seen_macs[MAX_SEEN_MACS];
int seen_macs_count = 0;
int seen_macs_write_idx = 0;

String last_cap_type = "None";
String last_cap_mac = "--:--:--:--:--:--";
String last_cap_name = "";
int last_cap_rssi = 0;
int last_cap_confidence = 0;
String last_cap_time = "00:00:00";
String last_cap_det_method = "";

#define CAPTURE_HISTORY_SIZE 5
struct CaptureEntry {
    String type;
    String mac;
    String name;
    int rssi;
    int confidence;
    String time;
};
CaptureEntry capture_history[CAPTURE_HISTORY_SIZE];
int capture_history_count = 0;

String toast_text = "";
unsigned long toast_start = 0;
bool toast_active = false;

String live_logs[6] = {"", "", "", "", "", ""};

unsigned long last_uptime_update = 0;
unsigned long last_anim_update = 0;
unsigned long last_stats_update = 0;
unsigned long last_time_save = 0;
unsigned long last_log_update = 0;
unsigned long last_persist_save = 0;
int scan_line_x = 0;

#define CHART_BARS 25
int activity_history[CHART_BARS] = {0};
unsigned long last_chart_update = 0;
long last_total_dets = 0;

long session_flock_wifi = 0;
long session_flock_ble = 0;
long session_raven = 0;

struct RSSITrack {
    char mac[18];  // Fixed char array — no heap allocation
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
String locator_target_mac = "";
String locator_target_name = "";
LocSample locator_samples[LOC_MAX_SAMPLES];
int locator_sample_count = 0;
unsigned long locator_last_sample = 0;
double locator_est_lat = 0.0;
double locator_est_lng = 0.0;
float locator_est_distance = 0.0;
float locator_bearing = 0.0;
float locator_confidence_radius = 0.0;
bool locator_has_estimate = false;
int locator_peak_rssi = -120;
unsigned long locator_start_time = 0;

// GPS
TinyGPSPlus gps;
// ============================================================================
// SPRITE BUFFER — eliminates flicker on all screen draws
// ============================================================================
// We use a single full-screen sprite as an off-screen buffer.
// All draw calls go to spr, then spr.pushSprite(0,0) in one shot.
M5Canvas spr(&M5Cardputer.Display);
bool spr_ready = false;

// Convenience wrappers so existing code compiles unchanged
// (we'll call spr. directly in the new draw functions)


HardwareSerial SerialGPS(1);

// ============================================================================
// DETECTION SIGNATURE DATABASE (unchanged from v3.3)
// ============================================================================
static const char* wifi_ssid_patterns[] = {
    "flock", "Flock", "FLOCK",
    "FS Ext Battery", "FS_",
    "Penguin", "Pigvision",
    "FlockOS", "flocksafety",
    "OFS_IoT",
    "PFS_",
};
static const int NUM_SSID_PATTERNS = sizeof(wifi_ssid_patterns) / sizeof(wifi_ssid_patterns[0]);

static const char* mac_prefixes_tier1[] = {
    "ec:1b:bd",
    "58:8e:81",
    "d8:a0:d8",
    "08:3a:88",
    "d8:f3:bc",
    "cc:cc:cc",
};
static const int NUM_MAC_TIER1 = sizeof(mac_prefixes_tier1) / sizeof(mac_prefixes_tier1[0]);

static const char* mac_prefixes_tier2[] = {
    "48:e7:29", "74:4c:a1", "94:34:69", "38:5b:44",
    "90:35:ea", "f0:82:c0", "b4:e3:f9", "94:08:53",
    "1c:34:f1", "a4:cf:12", "3c:91:80", "80:30:49",
    "14:5a:fc", "9c:2f:9d", "e4:aa:ea", "c8:c9:a3",
    "70:c9:4e", "04:0d:84",
};
static const int NUM_MAC_TIER2 = sizeof(mac_prefixes_tier2) / sizeof(mac_prefixes_tier2[0]);

static const char* device_name_patterns[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision", "FlockCam", "FS-",
    "RWLS-",
};
static const int NUM_NAME_PATTERNS = sizeof(device_name_patterns) / sizeof(device_name_patterns[0]);

static const char* raven_service_uuids[] = {
    "0000180a-0000-1000-8000-00805f9b34fb",
    "00003100-0000-1000-8000-00805f9b34fb",
    "00003200-0000-1000-8000-00805f9b34fb",
    "00003300-0000-1000-8000-00805f9b34fb",
    "00003400-0000-1000-8000-00805f9b34fb",
    "00003500-0000-1000-8000-00805f9b34fb",
    "00001809-0000-1000-8000-00805f9b34fb",
    "00001819-0000-1000-8000-00805f9b34fb",
};
static const int NUM_RAVEN_UUIDS = sizeof(raven_service_uuids) / sizeof(raven_service_uuids[0]);

#define FLOCK_MFG_COMPANY_ID 0x09C8

// ============================================================================
// AUDIO — Cardputer ADV speaker via M5Cardputer.Speaker
// ============================================================================
void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b) {
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
    neopixelWrite(21, r, g, b);
#endif
}

void beep(int frequency, int duration_ms) {
    if (!stealth_mode) {
        M5Cardputer.Speaker.tone(frequency, duration_ms);
        // No blocking delay — tone() handles its own duration.
        // Prevents dropping WiFi packets during alarm playback.
    }
}

void speaker_off() {
    M5Cardputer.Speaker.stop();
}

// ============================================================================
// HELPER FUNCTIONS (unchanged logic)
// ============================================================================
String format_time(unsigned long total_sec) {
    unsigned long m = (total_sec / 60) % 60;
    unsigned long h = (total_sec / 3600);
    if (h > 99) return String(h) + "h " + String(m) + "m";
    unsigned long s = total_sec % 60;
    char timeStr[10];
    sprintf(timeStr, "%02lu:%02lu:%02lu", h, m, s);
    return String(timeStr);
}

String short_mac(const String& mac) {
    if (mac.length() > 8) return mac.substring(9);
    return mac;
}

String bytesToHexStr(const std::string& data) {
    String res = "";
    for (size_t i = 0; i < data.length(); i++) {
        char buf[4]; sprintf(buf, "%02X", (uint8_t)data[i]); res += buf;
    }
    return res;
}

String get_gps_datetime() {
    if (!gps.date.isValid() || !gps.time.isValid()) return "No_GPS_Time";
    int year = gps.date.year(), month = gps.date.month(), day = gps.date.day();
    int hour = gps.time.hour() + TZ_OFFSET_HOURS;
    int minute = gps.time.minute(), second = gps.time.second();
    if (hour < 0) {
        hour += 24; day--;
        if (day < 1) {
            month--;
            if (month < 1) { month = 12; year--; }
            int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (month == 2 && (year % 4 == 0)) dim[1] = 29;
            day = dim[month - 1];
        }
    } else if (hour >= 24) {
        hour -= 24; day++;
        int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (month == 2 && (year % 4 == 0)) dim[1] = 29;
        if (day > dim[month - 1]) { day = 1; month++; if (month > 12) { month = 1; year++; } }
    }
    char dt[24];
    sprintf(dt, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return String(dt);
}

const char* confidence_label(int score) {
    if (score >= CONFIDENCE_CERTAIN) return "CERTAIN";
    if (score >= CONFIDENCE_HIGH)    return "HIGH";
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return "MEDIUM";
    return "LOW";
}

uint16_t confidence_color(int score) {
    if (score >= CONFIDENCE_CERTAIN) return TFT_RED;
    if (score >= CONFIDENCE_HIGH)    return TFT_ORANGE;
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return TFT_YELLOW;
    return TFT_WHITE;
}

// ============================================================================
// SESSION PERSISTENCE (LittleFS — unchanged)
// ============================================================================
void save_session_to_flash() {
    if (!littlefs_available) return;
    File f = LittleFS.open(PERSIST_FILE, "w");
    if (!f) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    f.printf("%ld\n%ld\n%lu\n%ld\n", lifetime_wifi, lifetime_ble, lifetime_seconds, lifetime_flock_total);
    xSemaphoreGive(dataMutex);
    f.close();
    last_persist_save = millis();
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
    f.close();
}

// ============================================================================
// TIME-WINDOWED MAC DEDUPLICATION (unchanged)
// ============================================================================
bool is_mac_recently_seen(const String& mac) {
    unsigned long now = millis();
    int limit = min(seen_macs_count, MAX_SEEN_MACS);
    for (int i = 0; i < limit; i++) {
        if (seen_macs[i].mac == mac) {
            if ((now - seen_macs[i].timestamp) < REDETECT_WINDOW_MS) return true;
            else { seen_macs[i].timestamp = now; return false; }
        }
    }
    return false;
}

void add_seen_mac(const String& mac) {
    seen_macs[seen_macs_write_idx].mac = mac;
    seen_macs[seen_macs_write_idx].timestamp = millis();
    seen_macs_write_idx = (seen_macs_write_idx + 1) % MAX_SEEN_MACS;
    if (seen_macs_count < MAX_SEEN_MACS) seen_macs_count++;
}

// ============================================================================
// RSSI TREND TRACKING (unchanged)
// ============================================================================
void rssi_track_update(const char* mac, int rssi) {
    unsigned long now = millis();
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, mac, 17) == 0) {
            if (rssi_tracker[i].sample_count < RSSI_TRACK_SAMPLES) {
                rssi_tracker[i].samples[rssi_tracker[i].sample_count++] = rssi;
            } else {
                for (int j = 0; j < RSSI_TRACK_SAMPLES - 1; j++)
                    rssi_tracker[i].samples[j] = rssi_tracker[i].samples[j + 1];
                rssi_tracker[i].samples[RSSI_TRACK_SAMPLES - 1] = rssi;
            }
            rssi_tracker[i].last_seen = now; return;
        }
    }
    if (rssi_tracker_count >= RSSI_TRACK_MAX_DEVICES) {
        int oldest_idx = 0; unsigned long oldest_time = rssi_tracker[0].last_seen;
        for (int i = 1; i < rssi_tracker_count; i++) {
            if (rssi_tracker[i].last_seen < oldest_time) { oldest_time = rssi_tracker[i].last_seen; oldest_idx = i; }
        }
        strncpy(rssi_tracker[oldest_idx].mac, mac, 17);
        rssi_tracker[oldest_idx].mac[17] = '\0';
        rssi_tracker[oldest_idx].samples[0] = rssi;
        rssi_tracker[oldest_idx].sample_count = 1;
        rssi_tracker[oldest_idx].last_seen = now;
        rssi_tracker[oldest_idx].scored = false;
        return;
    }
    strncpy(rssi_tracker[rssi_tracker_count].mac, mac, 17);
    rssi_tracker[rssi_tracker_count].mac[17] = '\0';
    rssi_tracker[rssi_tracker_count].samples[0] = rssi;
    rssi_tracker[rssi_tracker_count].sample_count = 1;
    rssi_tracker[rssi_tracker_count].last_seen = now;
    rssi_tracker[rssi_tracker_count].scored = false;
    rssi_tracker_count++;
}

bool rssi_track_is_stationary(const char* mac) {
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, mac, 17) == 0 && rssi_tracker[i].sample_count >= 3 && !rssi_tracker[i].scored) {
            int n = rssi_tracker[i].sample_count; int* s = rssi_tracker[i].samples;
            int peak_idx = 0;
            for (int j = 1; j < n; j++) if (s[j] > s[peak_idx]) peak_idx = j;
            int range = s[peak_idx] - min(s[0], s[n - 1]);
            if (peak_idx > 0 && peak_idx < n - 1 && range >= 6) { rssi_tracker[i].scored = true; return true; }
            return false;
        }
    }
    return false;
}

void rssi_track_expire() {
    unsigned long now = millis();
    for (int i = rssi_tracker_count - 1; i >= 0; i--) {
        if ((now - rssi_tracker[i].last_seen) > RSSI_TRACK_EXPIRY_MS) {
            for (int j = i; j < rssi_tracker_count - 1; j++) rssi_tracker[j] = rssi_tracker[j + 1];
            rssi_tracker_count--;
        }
    }
}

// ============================================================================
// WIFI SSID VALIDATION (unchanged)
// ============================================================================
bool is_flock_ssid_format(const char* ssid) {
    if (!ssid) return false;
    if (strncmp(ssid, "Flock-", 6) != 0 && strncmp(ssid, "flock-", 6) != 0) return false;
    const char* suffix = ssid + 6; int len = strlen(suffix);
    if (len < 2 || len > 12) return false;
    for (int i = 0; i < len; i++) if (!isxdigit(suffix[i])) return false;
    return true;
}

// ============================================================================
// PENGUIN / RAVEN HELPERS (unchanged)
// ============================================================================
bool is_penguin_numeric_name(const char* name) {
    if (!name) return false; int len = strlen(name);
    if (len < 8 || len > 12) return false;
    for (int i = 0; i < len; i++) if (!isdigit(name[i])) return false;
    return true;
}

bool has_tn_serial(const std::string& mfg_data) {
    if (mfg_data.length() < 10) return false;
    for (size_t i = 8; i < mfg_data.length() - 1; i++)
        if (mfg_data[i] == 'T' && mfg_data[i + 1] == 'N') return true;
    return false;
}

String classify_raven_firmware(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return "Unknown";
    bool has_health=false, has_location=false, has_gps=false;
    bool has_power=false, has_network=false, has_upload=false, has_error=false;
    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string uuid = device->getServiceUUID(i).toString();
        if (strcasestr(uuid.c_str(), "00001809")) has_health = true;
        if (strcasestr(uuid.c_str(), "00001819")) has_location = true;
        if (strcasestr(uuid.c_str(), "00003100")) has_gps = true;
        if (strcasestr(uuid.c_str(), "00003200")) has_power = true;
        if (strcasestr(uuid.c_str(), "00003300")) has_network = true;
        if (strcasestr(uuid.c_str(), "00003400")) has_upload = true;
        if (strcasestr(uuid.c_str(), "00003500")) has_error = true;
    }
    if (has_gps && has_power && has_network && has_upload && has_error) return "1.3.x";
    if (has_gps && has_power && has_network) return "1.2.x";
    if (has_health || has_location) return "1.1.x";
    return "Unknown";
}

int count_raven_uuids(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return 0;
    int matched = 0, count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string uuid = device->getServiceUUID(i).toString();
        for (int j = 0; j < NUM_RAVEN_UUIDS; j++)
            if (strcasecmp(uuid.c_str(), raven_service_uuids[j]) == 0) { matched++; break; }
    }
    return matched;
}

// ============================================================================
// PATTERN MATCHING (unchanged)
// ============================================================================
int check_mac_prefix_tiered(const uint8_t* mac) {
    char mac_str[9]; snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
    for (int i = 0; i < NUM_MAC_TIER1; i++) if (strncasecmp(mac_str, mac_prefixes_tier1[i], 8) == 0) return CONF_MAC_TIER1;
    for (int i = 0; i < NUM_MAC_TIER2; i++) if (strncasecmp(mac_str, mac_prefixes_tier2[i], 8) == 0) return CONF_MAC_TIER2;
    return 0;
}

bool check_ssid_pattern(const char* ssid) {
    if (!ssid || strlen(ssid) == 0) return false;
    for (int i = 0; i < NUM_SSID_PATTERNS; i++) if (strcasestr(ssid, wifi_ssid_patterns[i])) return true;
    return false;
}

bool check_device_name_pattern(const char* name) {
    if (!name || strlen(name) == 0) return false;
    for (int i = 0; i < NUM_NAME_PATTERNS; i++) if (strcasestr(name, device_name_patterns[i])) return true;
    return false;
}

bool check_manufacturer_id(const std::string& mfg_data) {
    if (mfg_data.length() >= 2) {
        uint16_t mfg_id = (uint8_t)mfg_data[0] | ((uint8_t)mfg_data[1] << 8);
        if (mfg_id == FLOCK_MFG_COMPANY_ID) return true;
    }
    return false;
}

// ============================================================================
// SD CARD (unchanged logic, SD init handled in setup via M5Cardputer)
// ============================================================================
void flush_sd_buffer() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (sd_write_count == 0 || !sd_available) { xSemaphoreGive(dataMutex); return; }
    // Copy count and snapshot locally, clear buffer under mutex
    int count = sd_write_count;
    char local_buf[MAX_LOG_BUFFER][220];
    for (int i = 0; i < count; i++) {
        strncpy(local_buf[i], sd_write_buffer[i], 219);
        local_buf[i][219] = '\0';
    }
    sd_write_count = 0;
    xSemaphoreGive(dataMutex);
    File file = SD.open(current_log_file.c_str(), FILE_APPEND);
    if (file) {
        for (int i = 0; i < count; i++) file.println(local_buf[i]);
        file.close();
        last_sd_flush = millis();
    }
}

// ============================================================================
// TOAST NOTIFICATION
// ============================================================================
void trigger_toast(const String& type, const String& name, int confidence) {
    String display_name = name;
    if (display_name.length() > 14) display_name = display_name.substring(0, 14);
    if (display_name == "Hidden" || display_name == "Unknown" || display_name == "")
        display_name = type;
    toast_text = display_name + " " + String(confidence) + "%";
    toast_start = millis();
    toast_active = true;
}

void draw_toast_overlay() {
    // Legacy stub — all new screens use draw_toast_spr() instead
    draw_toast_spr();
}

// ============================================================================
// LOGGING (unchanged logic)
// ============================================================================
void add_to_capture_history(const char* type, const char* mac, const String& name,
                             int rssi, int confidence) {
    for (int i = CAPTURE_HISTORY_SIZE - 1; i > 0; i--) capture_history[i] = capture_history[i - 1];
    capture_history[0].type = String(type);
    capture_history[0].mac = String(mac);
    capture_history[0].name = name;
    capture_history[0].rssi = rssi;
    capture_history[0].confidence = confidence;
    capture_history[0].time = format_time((millis() - session_start_time) / 1000);
    if (capture_history_count < CAPTURE_HISTORY_SIZE) capture_history_count++;
}

void log_detection(const char* type, const char* proto, int rssi, const char* mac,
                   const String& name, int channel, int tx_power, const String& extra_data,
                   const char* detection_method, int confidence) {
    String mac_str = String(mac);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool is_new = !is_mac_recently_seen(mac_str);
    if (is_new) add_seen_mac(mac_str);
    if (is_new) {
        if (strcmp(proto, "WIFI") == 0) { session_wifi++; lifetime_wifi++; session_flock_wifi++; }
        else { session_ble++; lifetime_ble++; }
        if (strstr(type, "RAVEN") != NULL) session_raven++;
        else if (strcmp(proto, "BLE") == 0) session_flock_ble++;
        lifetime_flock_total++;
    }
    last_cap_type = String(type); last_cap_mac = mac_str; last_cap_name = name;
    last_cap_rssi = rssi; last_cap_confidence = confidence;
    last_cap_time = format_time((millis() - session_start_time) / 1000);
    last_cap_det_method = String(detection_method);
    if (is_new) add_to_capture_history(type, mac, name, rssi, confidence);
    if (is_new) {
        trigger_toast(String(type), name, confidence);
        add_blip();  // Add radar blip on new detection
    }

    String logEntry;
    if (name != "Hidden" && name != "Unknown" && name != "") {
        String cleanName = name; if (cleanName.length() > 14) cleanName = cleanName.substring(0, 14);
        logEntry = "!" + cleanName + " " + String(confidence) + "%";
    } else {
        logEntry = "!" + String(proto) + " " + short_mac(mac_str) + " " + String(confidence) + "%";
    }
    if (millis() - last_log_update > LOG_UPDATE_DELAY) {
        for (int i = 5; i > 0; i--) live_logs[i] = live_logs[i - 1];
        live_logs[0] = logEntry; last_log_update = millis();
    }
    if (is_new && sd_available && sd_write_count < MAX_LOG_BUFFER) {
        // Build CSV with snprintf — no heap allocation, no String objects
        char clean_name[64];
        strncpy(clean_name, name.c_str(), sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0';
        // Replace commas with spaces in name
        for (char* p = clean_name; *p; p++) { if (*p == ',') *p = ' '; }

        char clean_extra[64];
        strncpy(clean_extra, extra_data.c_str(), sizeof(clean_extra) - 1);
        clean_extra[sizeof(clean_extra) - 1] = '\0';
        for (char* p = clean_extra; *p; p++) { if (*p == ',') *p = ' '; }

        bool gps_fresh = gps.location.isValid() && (gps.location.age() < 2000);
        char gps_fields[80];
        if (gps_fresh) {
            snprintf(gps_fields, sizeof(gps_fields), "%.6f,%.6f,%.1f,%.1f,%.1f",
                gps.location.lat(), gps.location.lng(),
                gps.speed.isValid()   ? gps.speed.mph()    : 0.0,
                gps.course.isValid()  ? gps.course.deg()   : 0.0,
                gps.altitude.isValid()? gps.altitude.meters(): 0.0);
        } else {
            strncpy(gps_fields, "0.000000,0.000000,0.0,0.0,0.0", sizeof(gps_fields));
        }

        snprintf(sd_write_buffer[sd_write_count], 220,
            "%lu,%s,%d,%s,%s,%d,%s,%s,%d,%s,%d,%s,%s,%s",
            millis(), get_gps_datetime().c_str(), channel, type, proto,
            rssi, mac, clean_name, tx_power, detection_method,
            confidence, confidence_label(confidence), clean_extra, gps_fields);
        sd_write_count++;
    }
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// CORE 0 SCANNER TASK (unchanged)
// ============================================================================
void ScannerLoopTask(void* pvParameters) {
    for (;;) {
        unsigned long now = millis();
        bool is_primary = (current_channel == 1 || current_channel == 6 || current_channel == 11);
        unsigned long dwell = is_primary ? DWELL_PRIMARY : DWELL_SECONDARY;
        if (now - last_channel_hop > dwell) {
            current_channel++; if (current_channel > MAX_CHANNEL) current_channel = 1;
            esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
            last_channel_hop = now;
        }
        if (millis() - last_ble_scan >= BLE_SCAN_INTERVAL) {
            if (!pBLEScan->isScanning()) { pBLEScan->start(BLE_SCAN_DURATION, false); last_ble_scan = millis(); }
        }
        if (!pBLEScan->isScanning() && (millis() - last_ble_scan > (unsigned long)(BLE_SCAN_DURATION * 1000 + 500)))
            pBLEScan->clearResults();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// WIFI PACKET HANDLER (unchanged)
// ============================================================================
typedef struct {
    unsigned frame_ctrl:16; unsigned duration_id:16;
    uint8_t addr1[6]; uint8_t addr2[6]; uint8_t addr3[6];
    unsigned sequence_ctrl:16; uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;
typedef struct { wifi_ieee80211_mac_hdr_t hdr; uint8_t payload[0]; } wifi_ieee80211_packet_t;

void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buff;
    if (ppkt->rx_ctrl.sig_len < 24) return;
    const wifi_ieee80211_packet_t* ipkt = (wifi_ieee80211_packet_t*)ppkt->payload;
    const wifi_ieee80211_mac_hdr_t* hdr = &ipkt->hdr;
    uint8_t frame_type    = (hdr->frame_ctrl & 0x0C) >> 2;
    uint8_t frame_subtype = (hdr->frame_ctrl & 0xF0) >> 4;
    if (frame_type != 0) return;
    bool is_beacon   = (frame_subtype == 8);
    bool is_probe_req = (frame_subtype == 4);
    if (!is_beacon && !is_probe_req) return;

    char ssid[33] = {0};
    uint8_t* frame_body = (uint8_t*)ipkt + 24;
    uint8_t* tagged_params; int remaining;
    if (is_beacon) {
        if (ppkt->rx_ctrl.sig_len < 24 + 12 + 2) return;
        tagged_params = frame_body + 12;
        remaining = ppkt->rx_ctrl.sig_len - 24 - 12 - 4;
    } else {
        tagged_params = frame_body;
        remaining = ppkt->rx_ctrl.sig_len - 24 - 4;
    }
    if (remaining > 2 && tagged_params[0] == 0 && tagged_params[1] <= 32 && tagged_params[1] <= remaining - 2) {
        memcpy(ssid, &tagged_params[2], tagged_params[1]);
        ssid[tagged_params[1]] = '\0';
    }

    int confidence = 0;
    char methods[64] = {0};  // Fixed buffer — no heap allocation in callback
    int mac_score   = check_mac_prefix_tiered(hdr->addr2);
    bool ssid_generic   = (strlen(ssid) > 0 && check_ssid_pattern(ssid));
    bool ssid_flock_fmt = (strlen(ssid) > 0 && is_flock_ssid_format(ssid));
    if (ssid_flock_fmt)    { confidence += CONF_SSID_FLOCK_FMT; strncat(methods, "ssid_fmt ", sizeof(methods) - strlen(methods) - 1); }
    else if (ssid_generic) { confidence += CONF_SSID_PATTERN;   strncat(methods, "ssid ", sizeof(methods) - strlen(methods) - 1); }
    if (mac_score > 0)     { confidence += mac_score; strncat(methods, (mac_score == CONF_MAC_TIER1) ? "mac_t1 " : "mac_t2 ", sizeof(methods) - strlen(methods) - 1); }
    int wifi_methods = 0;
    if (ssid_flock_fmt || ssid_generic) wifi_methods++;
    if (mac_score > 0) wifi_methods++;
    if (wifi_methods >= 2) confidence += CONF_BONUS_MULTI_METHOD;
    if (ppkt->rx_ctrl.rssi > -50) confidence += CONF_BONUS_STRONG_RSSI;

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             hdr->addr2[0], hdr->addr2[1], hdr->addr2[2],
             hdr->addr2[3], hdr->addr2[4], hdr->addr2[5]);
    // Use const char* — no String heap allocation
    const char* name_str = strlen(ssid) > 0 ? ssid : "Hidden";
    const char* frame_type_str = is_beacon ? "Beacon" : "ProbeReq";

    if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
        rssi_track_update(mac_str, ppkt->rx_ctrl.rssi);
        if (rssi_track_is_stationary(mac_str)) confidence += CONF_BONUS_STATIONARY;
        if (confidence > 100) confidence = 100;
        locator_add_sample(mac_str, ppkt->rx_ctrl.rssi);
        // Trim trailing space from methods
        int mlen = strlen(methods);
        if (mlen > 0 && methods[mlen-1] == ' ') methods[mlen-1] = '\0';
        log_detection("FLOCK_WIFI", "WIFI", ppkt->rx_ctrl.rssi, mac_str, String(name_str),
                      ppkt->rx_ctrl.channel, 0, String(frame_type_str), methods, confidence);
        if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
            trigger_alarm_confidence = confidence; last_buzzer_time = millis();
        }
    } else if (ppkt->rx_ctrl.rssi > IGNORE_WEAK_RSSI) {
        if (millis() - last_log_update > LOG_UPDATE_DELAY) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            char logEntry[48];
            if (strlen(ssid) > 0) {
                char cn[17]; strncpy(cn, ssid, 16); cn[16] = '\0';
                snprintf(logEntry, sizeof(logEntry), "%s (%d)", cn, ppkt->rx_ctrl.rssi);
            } else {
                snprintf(logEntry, sizeof(logEntry), "WiFi %s (%d)", &mac_str[9], ppkt->rx_ctrl.rssi);
            }
            for (int i = 5; i > 0; i--) live_logs[i] = live_logs[i - 1];
            live_logs[0] = String(logEntry); last_log_update = millis();
            xSemaphoreGive(dataMutex);
        }
    }
}

// ============================================================================
// BLE CALLBACK (unchanged)
// ============================================================================
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        NimBLEAddress addr = advertisedDevice->getAddress();
        uint8_t mac[6];
        sscanf(addr.toString().c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               (unsigned int*)&mac[0], (unsigned int*)&mac[1], (unsigned int*)&mac[2],
               (unsigned int*)&mac[3], (unsigned int*)&mac[4], (unsigned int*)&mac[5]);

        int confidence = 0;
        char methods[64] = {0};  // Fixed buffer — no heap allocation
        char capture_type[16] = "FLOCK_BLE";
        // Bool flags replace methods.indexOf() calls
        bool got_name = false, got_mfg = false, got_raven = false;

        int mac_score = check_mac_prefix_tiered(mac);
        if (mac_score > 0) {
            confidence += mac_score;
            strncat(methods, (mac_score == CONF_MAC_TIER1) ? "mac_t1 " : "mac_t2 ", sizeof(methods) - strlen(methods) - 1);
        }

        String dev_name = advertisedDevice->haveName() ? String(advertisedDevice->getName().c_str()) : "Unknown";
        if (advertisedDevice->haveName()) {
            if (check_device_name_pattern(advertisedDevice->getName().c_str())) {
                confidence += CONF_BLE_NAME;
                strncat(methods, "name ", sizeof(methods) - strlen(methods) - 1);
                got_name = true;
            } else if (is_penguin_numeric_name(advertisedDevice->getName().c_str())) {
                confidence += 15;
                strncat(methods, "penguin_num ", sizeof(methods) - strlen(methods) - 1);
                got_name = true;
            }
        }

        if (advertisedDevice->haveManufacturerData()) {
            std::string mfg = advertisedDevice->getManufacturerData();
            if (check_manufacturer_id(mfg)) {
                confidence += CONF_MFG_ID;
                strncat(methods, "mfg_0x09C8 ", sizeof(methods) - strlen(methods) - 1);
                got_mfg = true;
                if (has_tn_serial(mfg)) {
                    confidence += (CONF_PENGUIN_SERIAL - CONF_MFG_ID);
                    strncat(methods, "tn_serial ", sizeof(methods) - strlen(methods) - 1);
                }
            }
        }

        int raven_uuid_count = count_raven_uuids(advertisedDevice);
        if (raven_uuid_count > 0) {
            strncpy(capture_type, "RAVEN_BLE", sizeof(capture_type));
            got_raven = true;
            if (raven_uuid_count >= 3) {
                confidence += CONF_RAVEN_MULTI_UUID;
                strncat(methods, "raven_multi ", sizeof(methods) - strlen(methods) - 1);
            } else {
                confidence += CONF_RAVEN_UUID;
                strncat(methods, "raven_uuid ", sizeof(methods) - strlen(methods) - 1);
            }
        }

        uint8_t addr_type = addr.getType();
        if (addr_type == 0) {
            confidence += CONF_BONUS_BLE_STATIC_ADDR;
            strncat(methods, "pub_addr ", sizeof(methods) - strlen(methods) - 1);
        } else if (addr_type == 1) {
            if ((mac[0] >> 6) == 0x03) {
                confidence += CONF_BONUS_BLE_STATIC_ADDR;
                strncat(methods, "static_addr ", sizeof(methods) - strlen(methods) - 1);
            }
        }

        // Use bool flags instead of indexOf() string scanning
        int method_count = 0;
        if (mac_score > 0) method_count++;
        if (got_name)      method_count++;
        if (got_mfg)       method_count++;
        if (got_raven)     method_count++;
        if (method_count >= 2) confidence += CONF_BONUS_MULTI_METHOD;
        if (advertisedDevice->getRSSI() > -50) confidence += CONF_BONUS_STRONG_RSSI;

        char mac_string[18];
        snprintf(mac_string, sizeof(mac_string), "%s", addr.toString().c_str());

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            rssi_track_update(mac_string, advertisedDevice->getRSSI());
            if (rssi_track_is_stationary(mac_string)) confidence += CONF_BONUS_STATIONARY;
            locator_add_sample(mac_string, advertisedDevice->getRSSI());
        }
        if (confidence > 100) confidence = 100;

        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            int tx_power = advertisedDevice->haveTXPower() ? advertisedDevice->getTXPower() : 0;
            String mfg_hex = advertisedDevice->haveManufacturerData() ?
                bytesToHexStr(advertisedDevice->getManufacturerData()) : "";
            String extra_data = mfg_hex;
            if (strcmp(capture_type, "RAVEN_BLE") == 0) {
                String fw = classify_raven_firmware(advertisedDevice);
                extra_data = "FW:" + fw + " UUIDs:" + String(raven_uuid_count);
            }
            // Trim trailing space
            int mlen = strlen(methods);
            if (mlen > 0 && methods[mlen-1] == ' ') methods[mlen-1] = '\0';
            log_detection(capture_type, "BLE", advertisedDevice->getRSSI(),
                          mac_string, dev_name, 0, tx_power, extra_data,
                          methods, confidence);
            if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) {
                trigger_alarm_confidence = confidence; last_buzzer_time = millis();
            }
        } else if (advertisedDevice->getRSSI() > IGNORE_WEAK_RSSI) {
            if (millis() - last_log_update > LOG_UPDATE_DELAY) {
                xSemaphoreTake(dataMutex, portMAX_DELAY);
                char logEntry[48];
                if (dev_name != "Unknown" && dev_name != "") {
                    char cn[17]; strncpy(cn, dev_name.c_str(), 16); cn[16] = '\0';
                    snprintf(logEntry, sizeof(logEntry), "%s (%d)", cn, advertisedDevice->getRSSI());
                } else {
                    snprintf(logEntry, sizeof(logEntry), "BLE %s (%d)", &mac_string[9], advertisedDevice->getRSSI());
                }
                for (int i = 5; i > 0; i--) live_logs[i] = live_logs[i - 1];
                live_logs[0] = String(logEntry); last_log_update = millis();
                xSemaphoreGive(dataMutex);
            }
        }
    }
};

// ============================================================================
// LOCATOR ENGINE (unchanged logic, larger arrow for TFT)
// ============================================================================
double rssi_to_weight(int rssi) {
    if (rssi > -20) rssi = -20; if (rssi < -100) rssi = -100;
    return pow(10.0, (double)(rssi + 100) / 20.0);
}

float rssi_to_distance(int rssi, int tx_power) {
    if (rssi >= tx_power) return 0.5;
    float d = pow(10.0, (double)(tx_power - rssi) / (10.0 * LOC_PATH_LOSS_N));
    if (d > 500.0) d = 500.0;
    return d;
}

double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    double dLat = radians(lat2 - lat1), dLon = radians(lon2 - lon1);
    double a = sin(dLat/2)*sin(dLat/2) + cos(radians(lat1))*cos(radians(lat2))*sin(dLon/2)*sin(dLon/2);
    return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

float bearing_to(double lat1, double lon1, double lat2, double lon2) {
    double dLon = radians(lon2 - lon1);
    double y = sin(dLon) * cos(radians(lat2));
    double x = cos(radians(lat1)) * sin(radians(lat2)) - sin(radians(lat1)) * cos(radians(lat2)) * cos(dLon);
    float brng = degrees(atan2(y, x));
    if (brng < 0) brng += 360.0;
    return brng;
}

void locator_add_sample(const char* mac, int rssi) {
    if (!locator_active || strncmp(mac, locator_target_mac.c_str(), 17) != 0) return;
    if (millis() - locator_last_sample < LOC_SAMPLE_INTERVAL) return;
    if (!gps.location.isValid() || gps.location.age() > 2000) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int idx = locator_sample_count;
    if (idx >= LOC_MAX_SAMPLES) {
        int weakest = 0;
        for (int i = 1; i < LOC_MAX_SAMPLES; i++)
            if (locator_samples[i].rssi < locator_samples[weakest].rssi) weakest = i;
        if (rssi > locator_samples[weakest].rssi) idx = weakest;
        else { xSemaphoreGive(dataMutex); return; }
    } else { locator_sample_count++; }
    locator_samples[idx] = {gps.location.lat(), gps.location.lng(), rssi, millis()};
    locator_last_sample = millis();
    if (rssi > locator_peak_rssi) locator_peak_rssi = rssi;
    if (locator_sample_count >= LOC_MIN_SAMPLES_EST) {
        double sum_wlat=0, sum_wlng=0, sum_w=0;
        for (int i = 0; i < locator_sample_count; i++) {
            double w = rssi_to_weight(locator_samples[i].rssi);
            sum_wlat += locator_samples[i].lat * w;
            sum_wlng += locator_samples[i].lng * w;
            sum_w += w;
        }
        if (sum_w > 0) {
            locator_est_lat = sum_wlat / sum_w; locator_est_lng = sum_wlng / sum_w;
            locator_has_estimate = true;
            if (gps.location.isValid()) {
                locator_est_distance = haversine_m(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
                locator_bearing = bearing_to(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
            }
            double sum_wd2 = 0;
            for (int i = 0; i < locator_sample_count; i++) {
                double d = haversine_m(locator_samples[i].lat, locator_samples[i].lng, locator_est_lat, locator_est_lng);
                double w = rssi_to_weight(locator_samples[i].rssi);
                sum_wd2 += w * d * d;
            }
            locator_confidence_radius = sqrt(sum_wd2 / sum_w);
        }
    }
    xSemaphoreGive(dataMutex);
}

void locator_start(const String& mac, const String& name) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active=true; locator_target_mac=mac; locator_target_name=name;
    locator_sample_count=0; locator_has_estimate=false; locator_peak_rssi=-120;
    locator_est_distance=0; locator_bearing=0; locator_confidence_radius=0;
    locator_start_time=millis();
    xSemaphoreGive(dataMutex);
}

void locator_stop() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active=false; locator_has_estimate=false; locator_sample_count=0;
    xSemaphoreGive(dataMutex);
}

// Draw compass arrow on TFT
void draw_arrow(int cx, int cy, int r, float angle_deg) {
    float a = radians(angle_deg - 90.0);
    int tip_x = cx + (int)(r * cos(a));
    int tip_y = cy + (int)(r * sin(a));
    float a_left  = a + radians(140);
    float a_right = a - radians(140);
    int left_x  = cx + (int)((r * 0.5) * cos(a_left));
    int left_y  = cy + (int)((r * 0.5) * sin(a_left));
    int right_x = cx + (int)((r * 0.5) * cos(a_right));
    int right_y = cy + (int)((r * 0.5) * sin(a_right));
    spr.fillTriangle(tip_x, tip_y, left_x, left_y, right_x, right_y, ALERT_COLOR);
    spr.drawTriangle(tip_x, tip_y, left_x, left_y, right_x, right_y, TEXT_COLOR);
}

// ============================================================================
// UI — BATTERY HELPER
// ============================================================================
// Returns battery percentage 0-100 using M5Cardputer power API
// ============================================================================
// BATTERY & CHARGE DETECTION ENGINE
// ============================================================================
// HARDWARE FACT: M5Cardputer/ADV isCharging() and getBatteryCurrent() are BROKEN.
// The only reliable method is direct ADC on GPIO10 (ADC1_CH9).
// When USB is plugged in, the charge circuit holds voltage at/above ~4.28V.
// A LiPo under WiFi+BLE load physically cannot sustain above 4.25V on its own.
// Source: https://github.com/screenfluent/cardputer_charge_mode
// ============================================================================

#define BAT_ADC_PIN     10      // GPIO10 = ADC1_CH9 — direct battery voltage tap
#define BAT_ADC_SAMPLES  8      // Samples to average per read

float global_smoothed_mv = 0.0f;

// Read battery voltage directly from GPIO10 ADC (8-sample average)
// GPIO10 on Cardputer is on a 1:2 voltage divider -> multiply ADC mV by 2
int32_t read_battery_mv_direct() {
    uint32_t sum = 0;
    for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
        sum += analogReadMilliVolts(BAT_ADC_PIN);
        delayMicroseconds(250);
    }
    int32_t adc_mv = (int32_t)(sum / BAT_ADC_SAMPLES);
    int32_t bat_mv = adc_mv * 2;
    // Sanity: if result is garbage, fall back to M5 API
    if (bat_mv < 2800 || bat_mv > 5200) {
        bat_mv = M5Cardputer.Power.getBatteryVoltage();
    }
    return bat_mv;
}

void update_battery_metrics() {
    int32_t raw_mv = read_battery_mv_direct();
    // Snap on first call, EMA blend after
    if (global_smoothed_mv < 2800.0f) {
        global_smoothed_mv = (float)raw_mv;
    } else {
        global_smoothed_mv = (global_smoothed_mv * 0.90f) + ((float)raw_mv * 0.10f);
    }
}

int get_unified_battery_pct() {
    // LiPo curve: 3500mV = 0%, 4200mV = 100%
    int pct = (int)(((global_smoothed_mv - 3500.0f) / 700.0f) * 100.0f);
    return constrain(pct, 0, 100);
}

bool is_device_charging() {
    static float mv_window_start = 0.0f;
    static unsigned long window_ts = 0;
    static bool chg_state = false;

    float mv = global_smoothed_mv;
    if (mv_window_start < 2800.0f) { mv_window_start = mv; window_ts = millis(); }

    // HARD RULE: Above 4.28V = USB rail holding it up. Definitely charging.
    if (mv >= 4280.0f) {
        chg_state = true;
        mv_window_start = mv;
        window_ts = millis();
        return true;
    }

    // TREND RULE: Check every 5 seconds
    if (millis() - window_ts >= 5000) {
        float delta = mv - mv_window_start;
        if (delta >= 12.0f) {
            chg_state = true;           // Rising voltage = charging
        } else if (delta <= -10.0f) {
            chg_state = false;          // Falling voltage = discharging
        }
        // Near-zero delta: keep previous state (handles fully-charged plateau)
        mv_window_start = mv;
        window_ts = millis();
    }
    return chg_state;
}

// ============================================================================
// DEDICATED CHARGING SCREEN LOOP
// ============================================================================
void dedicated_charging_loop() {
    M5Cardputer.Speaker.stop();
    int display_pct = -1;
    uint8_t last_led_val = 255;
    unsigned long boot_time = millis();

    while (true) {
        M5Cardputer.update();
        update_battery_metrics();

        int calc_pct = get_unified_battery_pct();
        if (display_pct == -1) display_pct = calc_pct;
        else if (calc_pct > display_pct) display_pct = calc_pct;

        spr.fillSprite(BG_COLOR);

        // Title
        spr.setTextColor(HEADER_COLOR, BG_COLOR);
        spr.setTextSize(1);
        spr.setCursor(8, 8); spr.print("FLOCK DETECTOR");

        // Big charging label
        spr.setTextColor(ACCENT_COLOR, BG_COLOR);
        spr.setTextSize(2);
        int cx = (DISP_W - 96) / 2;
        spr.setCursor(cx, 28); spr.print("CHARGING");

        // Animated lightning bolt
        int bx = cx - 18, by = 26;
        spr.drawLine(bx + 5, by,     bx + 2, by + 7,  TEXT_COLOR);
        spr.drawLine(bx + 2, by + 7, bx + 7, by + 7,  TEXT_COLOR);
        spr.drawLine(bx + 7, by + 7, bx + 3, by + 14, TEXT_COLOR);
        spr.fillTriangle(bx+5,by, bx+2,by+7, bx+7,by+7, TEXT_COLOR);
        spr.fillTriangle(bx+7,by+7, bx+3,by+14, bx+8,by+14, TEXT_COLOR);

        // Big percentage
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setTextSize(4);
        String pctStr = String(display_pct) + "%";
        int pw = pctStr.length() * 24;
        spr.setCursor((DISP_W - pw) / 2, 52); spr.print(pctStr);

        // Animated progress bar (sweeping fill)
        int bar_x = 20, bar_y = 98, bar_w = DISP_W - 40, bar_h = 12;
        spr.drawRect(bar_x, bar_y, bar_w, bar_h, ACCENT_COLOR);
        // Real fill based on actual percentage
        int real_fill = (display_pct * (bar_w - 2)) / 100;
        if (real_fill > 0) spr.fillRect(bar_x + 1, bar_y + 1, real_fill, bar_h - 2, ACCENT_COLOR);
        // Animated shimmer on top
        int shimmer_pos = (millis() / 8) % (bar_w - 2);
        if (shimmer_pos < real_fill)
            spr.fillRect(bar_x + 1 + shimmer_pos, bar_y + 1, 8, bar_h - 2, TEXT_COLOR);

        // mV readout
        spr.setTextColor(DIM2_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(DISP_W - 52, DISP_H - 10);
        spr.printf("%dmV", (int)global_smoothed_mv);

        // Prompt
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(24, DISP_H - 10);
        spr.print("PRESS ANY KEY TO BOOT");

        spr.pushSprite(0, 0);

        // Breathing green LED
        float breathe = (exp(sin(millis() / 1000.0f * PI)) - 0.36787944f) / 2.35040238f;
        uint8_t led_val = (uint8_t)(breathe * 60.0f);
        if (led_val != last_led_val) {
            set_cardputer_led(0, led_val, 0);
            last_led_val = led_val;
        }

        // 1.5s lockout to ignore power-switch transients, then any key exits
        if ((millis() - boot_time > 1500) &&
            M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            M5Cardputer.Display.fillScreen(BG_COLOR);
            set_cardputer_led(0, 0, 0);
            return;
        }

        delay(15);
    }
}

// Draw a rounded card into the sprite
void drawCard(int x, int y, int w, int h) {
    spr.fillRoundRect(x, y, w, h, 4, CARD_COLOR);
    spr.drawRoundRect(x, y, w, h, 4, CARD_BORDER);
}

// Screen name arrays — declared before draw_header_spr
static const char* screen_names[] = {
    "SCANNER", "STATS", "LAST HIT", "HISTORY",
    "LIVE FEED", "GPS", "ACTIVITY", "PROXIMITY", "DEVICE", "LOCATOR"
};

// Draw header into sprite — uses GPIO10-based battery detection
void draw_header_spr(int screen_num) {
    int bat = get_unified_battery_pct();
    bool chg = is_device_charging();

    spr.fillRect(0, 0, DISP_W, 18, HILIGHT_BG);
    spr.fillRect(0, 0, 3, 18, HEADER_COLOR);
    spr.setTextColor(HEADER_COLOR, HILIGHT_BG);
    spr.setTextSize(1);
    spr.setCursor(7, 5);
    spr.print(screen_names[screen_num]);

    // Battery icon — animated when charging
    uint16_t bcol = chg ? ACCENT_COLOR : (bat > 50 ? ACCENT_COLOR : (bat > 20 ? WARN_COLOR : ALERT_COLOR));

    if (chg) {
        // Lightning bolt
        spr.drawLine(DISP_W-65, 4,  DISP_W-67, 9,  TEXT_COLOR);
        spr.drawLine(DISP_W-67, 9,  DISP_W-63, 9,  TEXT_COLOR);
        spr.drawLine(DISP_W-63, 9,  DISP_W-66, 14, TEXT_COLOR);
        // Percentage
        spr.setTextColor(ACCENT_COLOR, HILIGHT_BG);
        spr.setCursor(DISP_W - 55, 5);
        spr.printf("%d%%", bat);
        // Animated battery fill
        int anim_step = (millis() / 400) % 5;
        int bfill = (anim_step * 19) / 4;
        spr.drawRect(DISP_W-26, 3, 22, 11, ACCENT_COLOR);
        spr.fillRect(DISP_W- 4, 6,  2,  5, ACCENT_COLOR);
        if (bfill > 0) spr.fillRect(DISP_W-25, 4, bfill, 9, ACCENT_COLOR);
    } else {
        spr.setTextColor(bcol, HILIGHT_BG);
        spr.setCursor(DISP_W - 55, 5);
        spr.printf("%d%%", bat);
        spr.drawRect(DISP_W-26, 3, 22, 11, bcol);
        spr.fillRect(DISP_W- 4, 6,  2,  5, bcol);
        int bfill = max(1, (bat * 19) / 100);
        spr.fillRect(DISP_W-25, 4, bfill, 9, bcol);
    }

    spr.drawLine(0, 18, DISP_W, 18, GRID_COLOR);
}

// Toast overlay into sprite
void draw_toast_spr() {
    if (!toast_active) return;
    if (millis() - toast_start > TOAST_DURATION_MS) { toast_active = false; return; }
    spr.fillRect(0, DISP_H - 18, DISP_W, 18, TOAST_COLOR);
    spr.setTextColor(TFT_BLACK, TOAST_COLOR);
    spr.setTextSize(2);
    spr.setCursor(4, DISP_H - 16);
    spr.print("! "); spr.print(toast_text);
}

void draw_header(int screen_num) {
    draw_header_spr(screen_num);
}

// ============================================================================
// UI — BOOT ANIMATION
// ============================================================================
// Slick animated boot: radar sweep reveal + typewriter system check lines

static int boot_anim_frame = 0;

void draw_boot_logo() {
    // Draw the "FLOCK" logo large, centered, with scan-line reveal effect
    M5Cardputer.Display.setTextColor(HEADER_COLOR, BG_COLOR);
    M5Cardputer.Display.setTextSize(3);
    M5Cardputer.Display.setCursor(28, 8);
    M5Cardputer.Display.print("FLOCK");
    M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(118, 36);
    M5Cardputer.Display.print("DETECTOR v3.3");
}

// Small battery icon for boot screen: outline + fill based on pct, bolt if charging
void draw_battery_icon(int x, int y, int pct, bool charging) {
    uint16_t col = charging ? ACCENT_COLOR : (pct > 50 ? ACCENT_COLOR : (pct > 20 ? WARN_COLOR : ALERT_COLOR));
    // Outer rectangle
    M5Cardputer.Display.drawRect(x, y, 22, 10, col);
    // Terminal nub
    M5Cardputer.Display.fillRect(x + 22, y + 3, 2, 4, col);
    // Fill bar
    int fill = (pct * 19) / 100;
    if (fill > 0) M5Cardputer.Display.fillRect(x + 1, y + 1, fill, 8, col);
    // Charging bolt overlay
    if (charging) {
        M5Cardputer.Display.setTextColor(BG_COLOR, col);
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setCursor(x + 7, y + 1);
        M5Cardputer.Display.print("+");
    }
}

void draw_boot_screen(const char* status_line, int progress_pct) {
    M5Cardputer.Display.fillScreen(BG_COLOR);

    // Draw subtle grid lines for the "hacker terminal" feel
    for (int y = 0; y < DISP_H; y += 12)
        M5Cardputer.Display.drawLine(0, y, DISP_W, y, GRID_COLOR);
    for (int x = 0; x < DISP_W; x += 20)
        M5Cardputer.Display.drawLine(x, 0, x, DISP_H, GRID_COLOR);

    draw_boot_logo();

    // Separator
    M5Cardputer.Display.drawLine(0, 52, DISP_W, 52, HEADER_COLOR);
    M5Cardputer.Display.drawLine(0, 53, DISP_W, 53, GRID_COLOR);

    // Status line with blinking cursor effect
    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(4, 58);
    M5Cardputer.Display.print("> ");
    M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
    M5Cardputer.Display.print(status_line);

    // Progress bar — segmented style
    int bar_x = 4, bar_y = 76, bar_w = DISP_W - 8, bar_h = 10;
    M5Cardputer.Display.drawRect(bar_x, bar_y, bar_w, bar_h, DIM_COLOR);
    int fill_w = (progress_pct * (bar_w - 2)) / 100;
    // Draw segmented fill
    uint16_t bar_col = (progress_pct == 100) ? ACCENT_COLOR : HEADER_COLOR;
    for (int i = 0; i < fill_w; i += 4) {
        int seg_w = min(3, fill_w - i);
        M5Cardputer.Display.fillRect(bar_x + 1 + i, bar_y + 1, seg_w, bar_h - 2, bar_col);
    }
    // Percentage text
    M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(bar_x + bar_w + 2, bar_y + 1);
    char pct_str[5]; sprintf(pct_str, "%d%%", progress_pct);
    M5Cardputer.Display.print(pct_str);

    // Battery status at bottom during boot
    int bat = get_unified_battery_pct();
    bool chg = is_device_charging();
    M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(4, 96);
    M5Cardputer.Display.print("BAT:");
    draw_battery_icon(30, 93, bat, chg);
    char bat_str[6]; sprintf(bat_str, " %d%%", bat);
    M5Cardputer.Display.setTextColor(bat > 20 ? ACCENT_COLOR : ALERT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(56, 96);
    M5Cardputer.Display.print(bat_str);

    // Lifetime detections at bottom right
    if (lifetime_flock_total > 0) {
        M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
        M5Cardputer.Display.setCursor(140, 96);
        M5Cardputer.Display.print("LIFETIME:");
        M5Cardputer.Display.setTextColor(TOAST_COLOR, BG_COLOR);
        M5Cardputer.Display.print(lifetime_flock_total);
    }
}

void run_boot_sequence() {
    // Animated intro: flash cyan border then settle
    M5Cardputer.Display.fillScreen(BG_COLOR);
    for (int i = 0; i < 3; i++) {
        M5Cardputer.Display.drawRect(i, i, DISP_W - i*2, DISP_H - i*2, HEADER_COLOR);
    }
    delay(120);
    M5Cardputer.Display.fillScreen(BG_COLOR);
    delay(60);
    // Scan line wipe reveal
    for (int y = 0; y < DISP_H; y += 2) {
        M5Cardputer.Display.drawLine(0, y, DISP_W, y, GRID_COLOR);
        if (y % 10 == 0) {
            M5Cardputer.Display.drawLine(0, y, DISP_W, y, HEADER_COLOR);
            delay(8);
        }
    }
    delay(100);

    // Now run the system check sequence
    draw_boot_screen("System init...", 5);   delay(250);
    draw_boot_screen(littlefs_available ? "Flash storage  [OK]" : "Flash storage  [--]", 18); delay(180);
    draw_boot_screen(sd_available ? "SD card        [OK]" : "SD card        [--]", 36); delay(180);
    draw_boot_screen("WiFi promisc   [OK]", 54); delay(180);
    draw_boot_screen("BLE scanner    [OK]", 72); delay(180);
    int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    char gps_msg[32];
    if (sats > 0) sprintf(gps_msg, "GPS           [%dSV]", sats);
    else sprintf(gps_msg, "GPS        [SEARCH]");
    draw_boot_screen(gps_msg, 90); delay(180);
    draw_boot_screen("ARMED. SCANNING.", 100);
    // Final flash
    delay(200);
    M5Cardputer.Display.invertDisplay(true); delay(80);
    M5Cardputer.Display.invertDisplay(false);
    delay(400);
}

// ============================================================================
// UI — SCREEN 0: SCANNER  (sprite-buffered radar)
// ============================================================================
// All drawing goes to spr (off-screen), then pushed in one call — no flicker.

static float radar_angle = 0.0f;
static const int radar_cx = 62;
static const int radar_cy = 77;
static const int radar_r  = 50;

// Persistent blip positions (set on detection, fade over time)
#define MAX_BLIPS 8
struct Blip { int x, y; unsigned long ts; };
static Blip blips[MAX_BLIPS];
static int blip_count = 0;

void add_blip() {
    // Place blip at random position within radar circle
    float ang = random(0, 628) / 100.0f;
    float dist = (float)random(10, radar_r - 6);
    int bx = radar_cx + (int)(dist * cos(ang));
    int by = radar_cy + (int)(dist * sin(ang));
    blips[blip_count % MAX_BLIPS] = {bx, by, millis()};
    blip_count++;
}

void update_animation() {
    if (current_screen != 0 || !spr_ready) return;

    // Draw radar into sprite — full repaint each frame (sprite handles flicker)
    // Clear radar area only
    spr.fillRect(0, 20, radar_cx + radar_r + 4, DISP_H - 20, BG_COLOR);

    // Grid rings — dark teal
    spr.drawCircle(radar_cx, radar_cy, radar_r,     GRID_COLOR);
    spr.drawCircle(radar_cx, radar_cy, radar_r*2/3, GRID_COLOR);
    spr.drawCircle(radar_cx, radar_cy, radar_r/3,   GRID_COLOR);
    // Crosshairs
    spr.drawLine(radar_cx - radar_r, radar_cy, radar_cx + radar_r, radar_cy, GRID_COLOR);
    spr.drawLine(radar_cx, radar_cy - radar_r, radar_cx, radar_cy + radar_r, GRID_COLOR);

    // Sweep trail — 12 steps fading from cyan to invisible
    for (int t = 11; t >= 0; t--) {
        float ta = radar_angle - t * 0.08f;
        int tx2 = radar_cx + (int)(radar_r * cos(ta));
        int ty2 = radar_cy + (int)(radar_r * sin(ta));
        // Fade: t=0 is oldest (dimmest), t=11 is newest (brightest)
        uint16_t trail_col;
        if      (t >= 10) trail_col = HEADER_COLOR;   // bright cyan tip
        else if (t >= 7)  trail_col = 0x0698;         // medium cyan
        else if (t >= 4)  trail_col = 0x0410;         // dim cyan
        else              trail_col = GRID_COLOR;      // almost invisible
        spr.drawLine(radar_cx, radar_cy, tx2, ty2, trail_col);
    }

    // Blips — green dots, fade out over 8 seconds
    unsigned long now = millis();
    for (int i = 0; i < min(blip_count, MAX_BLIPS); i++) {
        unsigned long age = now - blips[i].ts;
        if (age < 8000) {
            uint16_t bcol = (age < 2000) ? ACCENT_COLOR :
                            (age < 5000) ? 0x0320 : GRID_COLOR;
            spr.fillCircle(blips[i].x, blips[i].y, 2, bcol);
        }
    }

    // Push only the radar area to avoid full-screen push lag
    spr.pushSprite(0, 0);

    radar_angle += 0.10f;
    if (radar_angle > 6.28318f) radar_angle -= 6.28318f;
}

void draw_scanner_screen() {
    if (!spr_ready) return;
    if (millis() - last_uptime_update > 800) {

        // Draw entire screen into sprite
        spr.fillSprite(BG_COLOR);

        // ── Header ──
        spr.fillRect(0, 0, DISP_W, 18, BG_COLOR);
        spr.fillRect(0, 0, 3, 18, HEADER_COLOR);
        spr.setTextColor(HEADER_COLOR, BG_COLOR);
        spr.setTextSize(1);
        spr.setCursor(7, 5);
        spr.print("[~] SCANNER");

        int bat = get_unified_battery_pct();
        bool chg = is_device_charging();
        int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
        // Battery icon in sprite
        uint16_t bcol = chg ? ACCENT_COLOR : (bat > 50 ? ACCENT_COLOR : (bat > 20 ? WARN_COLOR : ALERT_COLOR));
        spr.drawRect(DISP_W - 26, 3, 22, 11, bcol);
        spr.fillRect(DISP_W - 4, 6, 2, 5, bcol);
        int bfill = (bat * 19) / 100;
        if (bfill > 0) spr.fillRect(DISP_W - 25, 4, bfill, 9, bcol);
        // Sats
        spr.setTextColor(sats >= 4 ? ACCENT_COLOR : DIM_COLOR, BG_COLOR);
        spr.setCursor(DISP_W - 52, 5);
        char sat_buf[4]; sprintf(sat_buf, "%dS", sats);
        spr.print(sat_buf);
        // SD dot
        spr.fillCircle(DISP_W - 59, 9, 3, sd_available ? ACCENT_COLOR : ALERT_COLOR);
        // Screen num
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setCursor(DISP_W - 72, 5); spr.print("1");
        // Divider
        spr.drawLine(0, 18, DISP_W, 18, GRID_COLOR);

        // ── Radar base (will be animated on top) ──
        spr.drawCircle(radar_cx, radar_cy, radar_r,     GRID_COLOR);
        spr.drawCircle(radar_cx, radar_cy, radar_r*2/3, GRID_COLOR);
        spr.drawCircle(radar_cx, radar_cy, radar_r/3,   GRID_COLOR);
        spr.drawLine(radar_cx - radar_r, radar_cy, radar_cx + radar_r, radar_cy, GRID_COLOR);
        spr.drawLine(radar_cx, radar_cy - radar_r, radar_cx, radar_cy + radar_r, GRID_COLOR);

        // Uptime under radar
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(1);
        spr.setCursor(4, DISP_H - 10);
        spr.print(format_time((millis() - session_start_time) / 1000));

        // ── Right panel ──
        int px = radar_cx + radar_r + 6;
        spr.drawLine(px - 2, 20, px - 2, DISP_H, GRID_COLOR);

        xSemaphoreTake(dataMutex, portMAX_DELAY);
        long sw = session_flock_wifi, sb = session_flock_ble;
        xSemaphoreGive(dataMutex);

        // Scan mode badge
        bool ble_scanning = pBLEScan->isScanning();
        spr.fillRoundRect(px + 2, 22, 95, 14, 3, ble_scanning ? 0x000C : 0x000C);
        spr.drawRoundRect(px + 2, 22, 95, 14, 3, ble_scanning ? TFT_MAGENTA : HEADER_COLOR);
        spr.setTextColor(ble_scanning ? TFT_MAGENTA : HEADER_COLOR, BG_COLOR);
        spr.setTextSize(1);
        spr.setCursor(px + 6, 25);
        if (ble_scanning) spr.print("BLE SCANNING");
        else { spr.print("WiFi  CH:"); spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.print(current_channel); }

        // WiFi count
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(1);
        spr.setCursor(px + 2, 42); spr.print("WIFI HITS");
        spr.setTextColor(TOAST_COLOR, BG_COLOR);
        spr.setTextSize(2);
        spr.setCursor(px + 2, 52); spr.print(sw);

        // BLE count
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setTextSize(1);
        spr.setCursor(px + 2, 70); spr.print("BLE HITS");
        spr.setTextColor(TFT_MAGENTA, BG_COLOR);
        spr.setTextSize(2);
        spr.setCursor(px + 2, 80); spr.print(sb);

        // Last hit
        spr.setTextSize(1);
        spr.drawLine(px + 2, 100, DISP_W - 2, 100, GRID_COLOR);
        if (last_cap_type != "None") {
            spr.setTextColor(confidence_color(last_cap_confidence), BG_COLOR);
            spr.setCursor(px + 2, 104);
            String dn = (last_cap_name != "" && last_cap_name != "Hidden" && last_cap_name != "Unknown")
                          ? last_cap_name : short_mac(last_cap_mac);
            if (dn.length() > 13) dn = dn.substring(0, 13);
            spr.print(dn);
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setCursor(px + 2, 116);
            spr.print(String(last_cap_confidence) + "% " + confidence_label(last_cap_confidence));
        } else {
            spr.setTextColor(DIM_COLOR, BG_COLOR);
            spr.setCursor(px + 2, 104);
            spr.print("-- no hits yet --");
        }

        spr.pushSprite(0, 0);
        last_uptime_update = millis();
    }
}

// ============================================================================
// UI — SCREEN 1: STATS
// ============================================================================
void draw_stats_screen() {
    if (!spr_ready || millis() - last_stats_update < 500) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long tw=session_flock_wifi, tb=session_flock_ble, tr=session_raven;
    long lw=lifetime_wifi, lb=lifetime_ble;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(1);

    // Three stat cards
    // Card 1: Session WiFi
    drawCard(4, 22, 72, 52);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(10, 26); spr.print("WIFI SESS");
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(3);
    spr.setCursor(10, 38); spr.print(tw);

    // Card 2: Session BLE
    drawCard(82, 22, 72, 52);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(88, 26); spr.print("BLE SESS");
    spr.setTextColor(TFT_MAGENTA, CARD_COLOR); spr.setTextSize(3);
    spr.setCursor(88, 38); spr.print(tb);

    // Card 3: Raven
    drawCard(160, 22, 72, 52);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(166, 26); spr.print("RAVEN");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(3);
    spr.setCursor(166, 38); spr.print(tr);

    // Lifetime row
    spr.drawLine(0, 80, DISP_W, 80, GRID_COLOR);
    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, 84); spr.print("LIFETIME:");
    spr.setTextColor(CYAN2_COLOR, BG_COLOR); spr.setTextSize(2);
    spr.setCursor(4, 94);
    spr.print("WiFi:"); spr.print(lw);
    spr.setCursor(4, 114);
    spr.print("BLE: "); spr.print(lb);

    // Uptime card
    drawCard(130, 82, 106, 46);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(136, 86); spr.print("SESSION TIME");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2);
    spr.setCursor(136, 98);
    spr.print(format_time((millis() - session_start_time) / 1000));

    draw_toast_spr();
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 2: LAST CAPTURE
// ============================================================================
void draw_last_capture_screen() {
    if (!spr_ready || millis() - last_stats_update < 400) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    String t_type=last_cap_type, t_time=last_cap_time;
    String t_mac=last_cap_mac, t_name=last_cap_name;
    String t_method=last_cap_det_method;
    int t_rssi=last_cap_rssi, t_conf=last_cap_confidence;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(2);

    if (t_type == "None") {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(20, 65); spr.print("No detections yet");
    } else {
        uint16_t ccol = confidence_color(t_conf);
        // Type badge
        spr.fillRoundRect(4, 22, DISP_W - 8, 20, 3, HILIGHT_BG);
        spr.drawRoundRect(4, 22, DISP_W - 8, 20, 3, ccol);
        spr.setTextColor(ccol, HILIGHT_BG); spr.setTextSize(1);
        spr.setCursor(8, 26); spr.print(t_type);
        spr.setTextColor(DIM_COLOR, HILIGHT_BG);
        spr.setCursor(DISP_W - 70, 26); spr.print("@ "); spr.print(t_time);

        // Name/MAC card
        drawCard(4, 46, DISP_W - 8, 22);
        spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(8, 50);
        String dn = (t_name != "" && t_name != "Hidden" && t_name != "Unknown") ? t_name : t_mac;
        if (dn.length() > 36) dn = dn.substring(0, 36);
        spr.print(dn);

        // RSSI + Confidence bar
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(4, 74); spr.print("RSSI");
        spr.setTextColor(CYAN2_COLOR, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(4, 82); spr.print(t_rssi); spr.print("dBm");

        // Confidence bar
        int bar_x = 90, bar_y = 74, bar_w = DISP_W - 94;
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(bar_x, 74); spr.print("CONFIDENCE");
        spr.drawRoundRect(bar_x, 84, bar_w, 10, 2, GRID_COLOR);
        int cfill = (t_conf * (bar_w - 2)) / 100;
        if (cfill > 0) spr.fillRoundRect(bar_x + 1, 85, cfill, 8, 1, ccol);
        spr.setTextColor(ccol, BG_COLOR);
        spr.setCursor(bar_x + bar_w + 2, 84);
        char cpct[6]; sprintf(cpct, "%d%%", t_conf); spr.print(cpct);

        // Method
        spr.drawLine(0, 100, DISP_W, 100, GRID_COLOR);
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(4, 104); spr.print("METHOD: ");
        spr.setTextColor(TEAL_COLOR, BG_COLOR);
        String m = t_method; if (m.length() > 28) m = m.substring(0, 28);
        spr.print(m);

        // Confidence label
        spr.setTextColor(ccol, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(4, 116); spr.print(confidence_label(t_conf));
    }
    draw_toast_spr();
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 3: CAPTURE HISTORY
// ============================================================================
void draw_capture_history_screen() {
    if (!spr_ready || millis() - last_stats_update < 400) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    CaptureEntry local_history[CAPTURE_HISTORY_SIZE];
    int local_count = capture_history_count;
    for (int i = 0; i < local_count; i++) local_history[i] = capture_history[i];
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(3);

    if (local_count == 0) {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(20, 65); spr.print("No detections yet");
    } else {
        int y = 22;
        for (int i = 0; i < local_count && i < CAPTURE_HISTORY_SIZE; i++) {
            // Row card
            uint16_t rcol = confidence_color(local_history[i].confidence);
            spr.fillRect(0, y, 3, 20, rcol);  // confidence stripe
            spr.fillRect(3, y, DISP_W - 3, 20, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);

            String t = local_history[i].type;
            if (t == "FLOCK_WIFI") t = "FW";
            else if (t == "FLOCK_BLE") t = "FB";
            else if (t == "RAVEN_BLE") t = "RV";

            String nom;
            if (local_history[i].name != "" && local_history[i].name != "Hidden" && local_history[i].name != "Unknown") {
                nom = local_history[i].name;
                if (nom.length() > 14) nom = nom.substring(0, 14);
            } else { nom = short_mac(local_history[i].mac); }

            spr.setTextColor(rcol, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setTextSize(1);
            spr.setCursor(8, y + 6);
            spr.print(t);
            spr.setTextColor(TEXT_COLOR, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setCursor(30, y + 6);
            spr.print(nom);
            // Confidence pill right side
            char cpct[5]; sprintf(cpct, "%d%%", local_history[i].confidence);
            spr.setTextColor(rcol, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setCursor(DISP_W - 50, y + 6); spr.print(cpct);
            spr.setTextColor(DIM_COLOR, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setCursor(DISP_W - 35, y + 6); spr.print(local_history[i].time);
            y += 21;
        }
    }
    draw_toast_spr();
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 4: LIVE FEED
// ============================================================================
void draw_live_log_screen() {
    if (!spr_ready || millis() - last_stats_update < 100) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    String t_logs[6]; for (int i = 0; i < 6; i++) t_logs[i] = live_logs[i];
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(4);

    int y = 21;
    for (int i = 0; i < 6; i++) {
        if (t_logs[i] == "") { y += 19; continue; }
        bool is_hit = t_logs[i].startsWith("!");
        uint16_t row_bg = is_hit ? HILIGHT_BG : BG_COLOR;
        spr.fillRect(0, y, DISP_W, 18, row_bg);
        if (is_hit) spr.fillRect(0, y, 3, 18, TOAST_COLOR);
        spr.setTextColor(is_hit ? TOAST_COLOR : TEAL_COLOR, row_bg);
        spr.setTextSize(1);
        spr.setCursor(6, y + 5);
        String line = t_logs[i]; if (line.length() > 38) line = line.substring(0, 38);
        spr.print(line);
        spr.drawLine(0, y + 18, DISP_W, y + 18, GRID_COLOR);
        y += 19;
    }
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 5: GPS
// ============================================================================
void draw_gps_screen() {
    if (!spr_ready || millis() - last_stats_update < 500) return;
    spr.fillSprite(BG_COLOR);
    draw_header_spr(5);

    bool has_loc = gps.location.isValid();
    bool stale   = has_loc && (gps.location.age() > 2000);
    int sats     = gps.satellites.isValid() ? gps.satellites.value() : 0;

    // Satellite count card (top)
    drawCard(4, 22, 80, 40);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(10, 26); spr.print("SATELLITES");
    spr.setTextColor(sats >= 4 ? ACCENT_COLOR : WARN_COLOR, CARD_COLOR); spr.setTextSize(3);
    spr.setCursor(10, 36); spr.print(sats);

    // Status card
    drawCard(90, 22, 146, 40);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(96, 26); spr.print("STATUS");
    if (has_loc && !stale) {
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(96, 36); spr.print("GPS LOCKED");
    } else if (has_loc && stale) {
        spr.setTextColor(WARN_COLOR, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(96, 36); spr.print("SIGNAL LOST");
    } else {
        spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(96, 36); spr.print("SEARCHING...");
    }

    if (has_loc && !stale) {
        // Lat/Lon cards
        drawCard(4, 68, 112, 30);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(8, 72); spr.print("LATITUDE");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR);
        spr.setCursor(8, 82); spr.print(gps.location.lat(), 5);

        drawCard(120, 68, 116, 30);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(124, 72); spr.print("LONGITUDE");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR);
        spr.setCursor(124, 82); spr.print(gps.location.lng(), 5);

        // Speed / Heading
        drawCard(4, 102, 80, 28);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(8, 106); spr.print("SPEED");
        spr.setTextColor(TEAL_COLOR, CARD_COLOR);
        spr.setCursor(8, 116);
        char spd[12]; sprintf(spd, "%.1f mph", gps.speed.isValid() ? gps.speed.mph() : 0.0);
        spr.print(spd);

        drawCard(90, 102, 80, 28);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(94, 106); spr.print("HEADING");
        spr.setTextColor(TEAL_COLOR, CARD_COLOR);
        spr.setCursor(94, 116);
        char hdg[8]; sprintf(hdg, "%d deg", gps.course.isValid() ? (int)gps.course.deg() : 0);
        spr.print(hdg);

        if (gps.altitude.isValid()) {
            drawCard(176, 102, 60, 28);
            spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
            spr.setCursor(180, 106); spr.print("ALT");
            spr.setTextColor(TEAL_COLOR, CARD_COLOR);
            spr.setCursor(180, 116);
            char alt[8]; sprintf(alt, "%dm", (int)gps.altitude.meters());
            spr.print(alt);
        }
    } else {
        // No fix — show chars processed
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(4, 76);
        char rxbuf[32]; sprintf(rxbuf, "Rx: %lu bytes", gps.charsProcessed());
        spr.print(rxbuf);
    }

    draw_toast_spr();
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 6: ACTIVITY CHART
// ============================================================================
void draw_chart_screen() {
    if (!spr_ready || millis() - last_stats_update < 500) return;
    spr.fillSprite(BG_COLOR);
    draw_header_spr(6);

    int max_val = 1;
    for (int i = 0; i < CHART_BARS; i++) if (activity_history[i] > max_val) max_val = activity_history[i];
    int chart_top = 24, chart_h = DISP_H - chart_top - 14;
    int bar_w = DISP_W / CHART_BARS;

    // Grid lines
    for (int g = 0; g <= 4; g++) {
        int gy = chart_top + chart_h - (g * chart_h / 4);
        spr.drawLine(0, gy, DISP_W, gy, GRID_COLOR);
    }

    for (int i = 0; i < CHART_BARS; i++) {
        int bar_h = (activity_history[i] * chart_h) / max_val;
        if (bar_h > 0) {
            // Color most recent bar differently
            uint16_t col = (i == CHART_BARS - 1) ? TOAST_COLOR :
                           (activity_history[i] > 0) ? TEAL_COLOR : GRID_COLOR;
            spr.fillRect(i * bar_w, chart_top + chart_h - bar_h, bar_w - 1, bar_h, col);
        }
    }

    // Bottom labels
    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, DISP_H - 10); spr.print("25s ago");
    spr.setCursor(DISP_W - 30, DISP_H - 10); spr.print("now");
    spr.setCursor(DISP_W / 2 - 15, DISP_H - 10); spr.print("MAX:"); spr.print(max_val);

    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 7: PROXIMITY
// ============================================================================
void draw_proximity_screen() {
    if (!spr_ready || millis() - last_stats_update < 200) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int rssi=last_cap_rssi; String cap_type=last_cap_type; int conf=last_cap_confidence;
    String cap_name=last_cap_name, cap_mac=last_cap_mac;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(7);

    if (cap_type == "None") {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(20, 65); spr.print("No detections yet");
    } else {
        int pct = constrain(map(rssi, -100, -30, 0, 100), 0, 100);
        uint16_t ccol = confidence_color(conf);

        // RSSI card
        drawCard(4, 22, 110, 48);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(10, 26); spr.print("SIGNAL STRENGTH");
        spr.setTextColor(CYAN2_COLOR, CARD_COLOR); spr.setTextSize(3);
        spr.setCursor(10, 36); spr.print(rssi);
        spr.setTextSize(1); spr.print("dBm");

        // Confidence card
        drawCard(120, 22, 116, 48);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(126, 26); spr.print("CONFIDENCE");
        spr.setTextColor(ccol, CARD_COLOR); spr.setTextSize(3);
        spr.setCursor(126, 36); spr.print(conf);
        spr.setTextSize(1); spr.print("%");

        // Proximity bar
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(4, 76); spr.print("PROXIMITY");
        int bar_x = 4, bar_y = 86, bar_w = DISP_W - 8;
        spr.drawRoundRect(bar_x, bar_y, bar_w, 14, 3, GRID_COLOR);
        int bfill = (pct * (bar_w - 2)) / 100;
        if (bfill > 0) {
            uint16_t pcol = pct > 75 ? ALERT_COLOR : pct > 50 ? WARN_COLOR : pct > 25 ? TOAST_COLOR : TEAL_COLOR;
            spr.fillRoundRect(bar_x + 1, bar_y + 1, bfill, 12, 2, pcol);
        }

        // Label
        spr.setTextSize(2);
        spr.setCursor(4, 106);
        if      (pct > 75) { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print(">> VERY CLOSE <<"); }
        else if (pct > 50) { spr.setTextColor(WARN_COLOR,  BG_COLOR); spr.print("> NEARBY <"); }
        else if (pct > 25) { spr.setTextColor(TEAL_COLOR,  BG_COLOR); spr.print("Moderate range"); }
        else               { spr.setTextColor(DIM_COLOR,   BG_COLOR); spr.print("Weak / distant"); }
    }
    draw_toast_spr();
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 8: DEVICE INFO
// ============================================================================
void draw_device_info_screen() {
    if (!spr_ready || millis() - last_stats_update < 1000) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    unsigned long ls=lifetime_seconds; long lt=lifetime_flock_total;
    long sr=session_raven; long sw=session_flock_wifi; long sb=session_flock_ble;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(8);

    // Firmware card
    drawCard(4, 22, DISP_W - 8, 20);
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(1);
    spr.setCursor(10, 27); spr.print("FLOCK DETECTOR v3.3-ADV  |  M5Stack Cardputer ADV");

    // Stats grid
    // Row 1
    drawCard(4,  46, 72, 38);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 50); spr.print("SESSION");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 60);
    spr.print(format_time((millis() - session_start_time) / 1000));

    drawCard(82,  46, 72, 38);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 50); spr.print("LIFETIME");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 60);
    spr.print(format_time(ls));

    drawCard(160, 46, 76, 38);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(164, 50); spr.print("ALL-TIME");
    spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(164, 60);
    spr.print(lt);

    // Row 2
    drawCard(4,   88, 72, 38);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 92); spr.print("WIFI SESS");
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(8, 102); spr.print(sw);

    drawCard(82,  88, 72, 38);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 92); spr.print("BLE SESS");
    spr.setTextColor(TFT_MAGENTA, CARD_COLOR); spr.setTextSize(2); spr.setCursor(86, 102); spr.print(sb);

    drawCard(160, 88, 76, 38);
    spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(164, 92); spr.print("RAVEN");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(164, 102); spr.print(sr);

    // SD card info bottom
    spr.drawLine(0, 130, DISP_W, 130, GRID_COLOR);
    spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, 133);
    if (sd_available) { spr.print("SD: "); spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.print(current_log_file.substring(1)); }
    else { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("NO SD CARD"); }

    draw_toast_spr();
    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// UI — SCREEN 9: LOCATOR
// ============================================================================
void draw_locator_screen() {
    if (!spr_ready || millis() - last_stats_update < 250) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active=locator_active, has_est=locator_has_estimate;
    String target_mac=locator_target_mac, target_name=locator_target_name;
    int samples=locator_sample_count, peak=locator_peak_rssi;
    float dist=locator_est_distance, brng=locator_bearing, conf_r=locator_confidence_radius;
    xSemaphoreGive(dataMutex);

    spr.fillSprite(BG_COLOR);
    draw_header_spr(9);

    if (!active) {
        if (last_cap_type == "None") {
            spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2);
            spr.setCursor(4, 40); spr.print("No target device.");
            spr.setCursor(4, 64); spr.print("Detect first,");
            spr.setCursor(4, 88); spr.print("then press 'l'");
        } else {
            drawCard(4, 22, DISP_W - 8, 20);
            spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
            spr.setCursor(8, 27); spr.print("TARGET");
            drawCard(4, 46, DISP_W - 8, 24);
            spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1);
            spr.setCursor(8, 54);
            String dn = (last_cap_name != "" && last_cap_name != "Hidden" && last_cap_name != "Unknown")
                          ? last_cap_name : last_cap_mac;
            if (dn.length() > 36) dn = dn.substring(0, 36);
            spr.print(dn);
            spr.setTextColor(TEAL_COLOR, BG_COLOR); spr.setTextSize(1);
            spr.setCursor(4, 82); spr.print("Press 'l' to start locating");
        }
    } else if (!has_est) {
        drawCard(4, 22, DISP_W - 8, 20);
        spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(8, 27); spr.print("TRACKING...");
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(2);
        spr.setCursor(4, 50);
        String tname = target_name;
        if (tname == "" || tname == "Hidden" || tname == "Unknown") tname = short_mac(target_mac);
        else if (tname.length() > 18) tname = tname.substring(0, 18);
        spr.print(tname);
        // Sample progress bar
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(4, 78); spr.print("Collecting samples:");
        spr.drawRoundRect(4, 88, DISP_W - 8, 12, 2, GRID_COLOR);
        int sfill = min(samples, LOC_MIN_SAMPLES_EST) * (DISP_W - 10) / LOC_MIN_SAMPLES_EST;
        if (sfill > 0) spr.fillRoundRect(5, 89, sfill, 10, 1, TEAL_COLOR);
        spr.setCursor(4, 106); spr.print(samples); spr.print("/"); spr.print(LOC_MIN_SAMPLES_EST);
        spr.print("  Peak: "); spr.print(peak); spr.print("dBm");
        spr.setTextColor(DIM_COLOR, BG_COLOR);
        spr.setCursor(4, 120); spr.print("Drive around area to collect...");
    } else {
        // Arrow compass (left half)
        int arrow_cx = 55, arrow_cy = 82;
        float rel_bearing = brng;
        if (gps.course.isValid() && gps.speed.isValid() && gps.speed.mph() > 2.0) {
            rel_bearing = brng - gps.course.deg();
            if (rel_bearing < 0) rel_bearing += 360.0;
        }
        spr.drawCircle(arrow_cx, arrow_cy, LOC_ARROW_RADIUS + 3, GRID_COLOR);
        spr.drawCircle(arrow_cx, arrow_cy, LOC_ARROW_RADIUS + 1, CARD_BORDER);
        draw_arrow(arrow_cx, arrow_cy, LOC_ARROW_RADIUS, rel_bearing);
        float na = radians(-90.0);
        int nx = arrow_cx + (int)((LOC_ARROW_RADIUS + 8) * cos(na));
        int ny = arrow_cy + (int)((LOC_ARROW_RADIUS + 8) * sin(na));
        spr.setTextColor(HEADER_COLOR, BG_COLOR); spr.setTextSize(1);
        spr.setCursor(nx - 3, ny - 4); spr.print("N");

        // Right panel
        int rx = 118;
        spr.drawLine(rx - 2, 20, rx - 2, DISP_H, GRID_COLOR);
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1);
        String tname = target_name;
        if (tname == "" || tname == "Hidden" || tname == "Unknown") tname = short_mac(target_mac);
        else if (tname.length() > 15) tname = tname.substring(0, 15);
        spr.setCursor(rx + 2, 22); spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(tname);

        drawCard(rx + 2, 34, 116, 28);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(rx + 6, 38); spr.print("DISTANCE");
        spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(rx + 6, 48);
        if (dist < 100) spr.print(String(dist, 0) + "m");
        else spr.print(String(dist / 1000.0, 2) + "km");

        drawCard(rx + 2, 66, 116, 28);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(rx + 6, 70); spr.print("BEARING");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(2);
        spr.setCursor(rx + 6, 80);
        const char* cardinals[] = {"N","NE","E","SE","S","SW","W","NW"};
        int card_idx = ((int)(brng + 22.5) / 45) % 8;
        spr.print(String((int)brng) + " "); spr.print(cardinals[card_idx]);

        drawCard(rx + 2, 98, 116, 22);
        spr.setTextColor(DIM_COLOR, CARD_COLOR); spr.setTextSize(1);
        spr.setCursor(rx + 6, 102); spr.print("ACCURACY +/-");
        spr.setTextColor(TEAL_COLOR, CARD_COLOR);
        spr.setCursor(rx + 6, 112); spr.print(String(conf_r, 0) + "m");

        // Guidance
        spr.setTextSize(1); spr.setCursor(4, DISP_H - 10);
        if      (dist < 15 && conf_r < 25) { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("** LOOK UP! **"); }
        else if (dist < 30)  { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("Very close"); }
        else if (dist < 75)  { spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.print("Closing in..."); }
        else                 { spr.setTextColor(TEAL_COLOR,  BG_COLOR); spr.print("Keep going..."); }
    }

    if (active) {
        spr.fillRect(0, DISP_H - 14, DISP_W, 14, HILIGHT_BG);
        spr.setTextColor(TEAL_COLOR, HILIGHT_BG); spr.setTextSize(1);
        spr.setCursor(4, DISP_H - 10);
        spr.print(String(samples) + " pts | press 'l' to stop");
    }

    spr.pushSprite(0, 0);
    last_stats_update = millis();
}

// ============================================================================
// ALARM ESCALATION (speaker replaces buzzer)
// ============================================================================
void play_escalated_alarm(int confidence) {
    if (stealth_mode) return;
    if (confidence >= CONFIDENCE_CERTAIN) {
        for (int i = 0; i < 5; i++) {
            M5Cardputer.Display.invertDisplay(true);
            beep(DETECT_FREQ_CERTAIN, 100);
            M5Cardputer.Display.invertDisplay(false);
            if (i < 4) delay(30);
        }
    } else if (confidence >= CONFIDENCE_HIGH) {
        for (int i = 0; i < 3; i++) {
            M5Cardputer.Display.invertDisplay(true);
            beep(DETECT_FREQ_HIGH, DETECT_BEEP_DURATION);
            M5Cardputer.Display.invertDisplay(false);
            if (i < 2) delay(50);
        }
    } else {
        M5Cardputer.Display.invertDisplay(true);
        beep(DETECT_FREQ, DETECT_BEEP_DURATION);
        M5Cardputer.Display.invertDisplay(false);
    }
}

// ============================================================================
// SCREEN REFRESH
// ============================================================================
void refresh_screen_layout() {
    if (stealth_mode) return;
    M5Cardputer.Display.fillScreen(BG_COLOR);
    // Reset animation state on screen change
    last_uptime_update = 0;
    last_stats_update  = 0;
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    // ---- Step 1: M5Cardputer init FIRST ----
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BG_COLOR);
    spr.setColorDepth(16);
    spr.createSprite(DISP_W, DISP_H);
    spr_ready = true;

    // ---- Step 1b: BOOT CHARGE TRAP ----
    // Sample voltage before WiFi load, then after 600ms with WiFi on.
    // If voltage is above 4.28V (USB rail) or rises by 20mV+, go to charging screen.
    analogSetAttenuation(ADC_11db);    // Full 0-3.3V range on all ADC pins
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    update_battery_metrics();
    float v_before = global_smoothed_mv;

    WiFi.mode(WIFI_STA);   // Apply load so battery voltage sags if on battery
    delay(100);
    unsigned long trap_t = millis();
    while (millis() - trap_t < 500) { update_battery_metrics(); delay(20); }
    float v_after = global_smoothed_mv;

    // USB present = voltage held up above 4.28V, OR voltage ROSE under load
    if (v_after >= 4280.0f || (v_after - v_before) >= 15.0f) {
        dedicated_charging_loop();
        // Returns when user presses a key to boot normally
    }
    M5Cardputer.Display.setTextColor(HEADER_COLOR, BG_COLOR);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(10, 55);
    M5Cardputer.Display.print("FLOCK DETECTOR");
    M5Cardputer.Display.setCursor(10, 80);
    M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
    M5Cardputer.Display.print("v3.3-ADV  boot...");
    M5Cardputer.Speaker.setVolume(100);

    Serial.begin(115200);
    delay(200);  // Let serial settle

    // ---- Step 2: GPS (LoRa/GPS cap on EXT header) ----
    SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    // ---- Step 3: CPU + mutex ----
    setCpuFrequencyMhz(240);
    dataMutex = xSemaphoreCreateMutex();

    // ---- Step 4: LittleFS for session persistence ----
    if (!LittleFS.begin(true)) { littlefs_available = false; }
    else { littlefs_available = true; load_session_from_flash(); }

    // ---- Step 5: SD card ----
    bool mount_success = false;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_CS_PIN);
    for (int i = 0; i < 3; i++) { if (SD.begin(SD_CS_PIN, SPI, 25000000)) { mount_success = true; break; } delay(100); }
    if (mount_success) {
        sd_available = true;
        current_log_file = "/FlockLog.csv";
        if (!SD.exists(current_log_file.c_str())) {
            File file = SD.open(current_log_file.c_str(), FILE_WRITE);
            if (file) {
                file.println("Uptime_ms,Date_Time,Channel,Capture_Type,Protocol,RSSI,MAC_Address,Device_Name,TX_Power,Detection_Method,Confidence,Confidence_Label,Extra_Data,Latitude,Longitude,Speed_MPH,Heading_Deg,Altitude_M");
                file.close();
            }
        }
    }

    session_start_time = millis();

    // ---- Step 6: WiFi promiscuous mode ----
    // Must start WiFi driver properly before enabling promiscuous mode
    WiFi.mode(WIFI_STA);
    delay(100);  // Let WiFi driver initialize
    WiFi.disconnect();
    delay(100);
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t filt;
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_promiscuous(true);  // Enable AFTER setting callback
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
    delay(100);

    // ---- Step 7: BLE ----
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(97);
    pBLEScan->setWindow(97);

    // ---- Step 8: Boot sequence + beeps ----
    run_boot_sequence();
    beep(LOW_FREQ, BOOT_BEEP_DURATION);
    beep(HIGH_FREQ, BOOT_BEEP_DURATION);

    last_channel_hop = millis(); last_sd_flush = millis(); last_persist_save = millis();

    // ---- Step 9: Start scanner task on Core 0 ----
    xTaskCreatePinnedToCore(ScannerLoopTask, "ScannerTask", 8192, NULL, 1, &ScannerTaskHandle, 0);

    refresh_screen_layout();
    Serial.println(F("=== Flock Detector v3.3-ADV ==="));
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    M5Cardputer.update();
    update_battery_metrics();

    // global_smoothed_mv is already updated by update_battery_metrics() above.
    // Reference it directly everywhere instead of calling get_median_voltage() repeatedly.

    // GPS
    while (SerialGPS.available() > 0) { gps.encode(SerialGPS.read()); yield(); }

    // Activity chart
    if (millis() - last_chart_update >= 1000) {
        last_chart_update = millis();
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        long current_total = session_wifi + session_ble;
        xSemaphoreGive(dataMutex);
        int new_dets = current_total - last_total_dets;
        last_total_dets = current_total;
        for (int i = 0; i < CHART_BARS - 1; i++) activity_history[i] = activity_history[i + 1];
        activity_history[CHART_BARS - 1] = new_dets;
    }

    // Escalated alarm
    if (trigger_alarm_confidence > 0) {
        int conf = trigger_alarm_confidence;
        trigger_alarm_confidence = 0;
        play_escalated_alarm(conf);
    }

    // -----------------------------------------------------------------------
    // KEYBOARD INPUT
    // Replaces the original single-button logic.
    //   G0 (side button) or Enter = cycle screens forward
    //   Backspace                 = cycle screens backward
    //   's'                       = toggle stealth mode
    //   'l' on locator screen     = toggle locator on/off
    // -----------------------------------------------------------------------
    // G0 side button
    if (M5Cardputer.BtnA.wasClicked()) {
        if (!stealth_mode) {
            current_screen++; if (current_screen >= NUM_SCREENS) current_screen = 0;
            refresh_screen_layout();
        }
    }

    // ── KEYBOARD INPUT ──────────────────────────────────────────────────────
    // G0 button  = next screen
    // Enter / '>' = next screen
    // Backspace ('<') = prev screen
    // ';' = prev screen  '.' = next screen  (Meshtastic-style nav)
    // 's' = stealth toggle   'l' = locator toggle
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        for (auto c : status.word) {
            if (c == '.' || c == '>') {
                // Next screen
                if (!stealth_mode) {
                    current_screen++; if (current_screen >= NUM_SCREENS) current_screen = 0;
                    refresh_screen_layout();
                }
            } else if (c == ';' || c == '<') {
                // Prev screen
                if (!stealth_mode) {
                    current_screen--; if (current_screen < 0) current_screen = NUM_SCREENS - 1;
                    refresh_screen_layout();
                }
            } else if (c == 's') {
                stealth_mode = !stealth_mode;
                if (stealth_mode) M5Cardputer.Display.setBrightness(0);
                else { M5Cardputer.Display.setBrightness(200); refresh_screen_layout(); }
            } else if (c == 'l') {
                if (locator_active) { locator_stop(); }
                else if (last_cap_type != "None") {
                    locator_start(last_cap_mac, last_cap_name);
                    current_screen = 9;
                    refresh_screen_layout();
                }
            } else if (c == '\n' || c == '\r') {
                if (!stealth_mode) {
                    current_screen++; if (current_screen >= NUM_SCREENS) current_screen = 0;
                    refresh_screen_layout();
                }
            }
        }
        if (status.del) {
            // Backspace = prev screen
            if (!stealth_mode) {
                current_screen--; if (current_screen < 0) current_screen = NUM_SCREENS - 1;
                refresh_screen_layout();
            }
        }
    }

    // Lifetime timer
    if (millis() - last_time_save >= 1000) { lifetime_seconds++; last_time_save = millis(); }

    // Session persistence
    if (millis() - last_persist_save >= PERSIST_INTERVAL_MS) save_session_to_flash();

    // RSSI tracker expiry
    rssi_track_expire();

    // SD flush
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool should_flush = (sd_write_count >= MAX_LOG_BUFFER ||
                         (millis() - last_sd_flush > SD_FLUSH_INTERVAL && sd_write_count > 0));
    xSemaphoreGive(dataMutex);
    if (should_flush) flush_sd_buffer();

    // Update locator bearing from GPS
    if (locator_active && locator_has_estimate && gps.location.isValid() && gps.location.age() < 2000) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        locator_est_distance = haversine_m(gps.location.lat(), gps.location.lng(),
                                            locator_est_lat, locator_est_lng);
        locator_bearing = bearing_to(gps.location.lat(), gps.location.lng(),
                                      locator_est_lat, locator_est_lng);
        xSemaphoreGive(dataMutex);
    }

    // Screen rendering
    if (!stealth_mode) {
        switch (current_screen) {
            case 0:
                draw_scanner_screen();
                if (millis() - last_anim_update > 40) { update_animation(); last_anim_update = millis(); }
                break;
            case 1: draw_stats_screen();           break;
            case 2: draw_last_capture_screen();    break;
            case 3: draw_capture_history_screen(); break;
            case 4: draw_live_log_screen();        break;
            case 5: draw_gps_screen();             break;
            case 6: draw_chart_screen();           break;
            case 7: draw_proximity_screen();       break;
            case 8: draw_device_info_screen();     break;
            case 9: draw_locator_screen();         break;
        }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
}
