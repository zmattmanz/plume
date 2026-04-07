// ============================================================================
// FLOCK DETECTOR v6.5-ADV — Tactical Edition (Democracy Polish)
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

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void draw_header_spr(int screen_num);
void draw_toast_spr();
void drawCard(int x, int y, int w, int h);
void draw_current_screen();
void transition_screen(int new_screen, int dir);
void play_escalated_alarm(int confidence);
void dedicated_charging_loop();
void set_cardputer_led(uint8_t r, uint8_t g, uint8_t b);
void beep(int frequency, int duration_ms);
void apply_color_palette();

// ============================================================================
// DISPLAY & PALETTE VARIABLES (Swappable for Night Mode)
// ============================================================================
#define DISP_W       240
#define DISP_H       135

uint16_t BG_COLOR, CARD_COLOR, CARD_BORDER, PANEL_BG, HEADER_COLOR, TEXT_COLOR;
uint16_t DIM_COLOR, DIM2_COLOR, ACCENT_COLOR, TEAL_COLOR, PURPLE_COLOR;
uint16_t ALERT_COLOR, TOAST_COLOR, WARN_COLOR, GRID_COLOR, HILIGHT_BG;
bool night_mode = false;

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
        PURPLE_COLOR  = lgfx::color565(200,  50, 150);
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
        PURPLE_COLOR  = lgfx::color565(190,  50, 255);
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
#define BLE_SCAN_INTERVAL 3000
#define BUZZER_COOLDOWN 60000
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define LOG_UPDATE_DELAY 500
#define IGNORE_WEAK_RSSI -80

#define MAX_LOG_BUFFER 10
#define SD_FLUSH_INTERVAL 10000
#define DWELL_PRIMARY   650  
#define DWELL_SECONDARY 200

#define REDETECT_WINDOW_MS 300000

#define RSSI_TRACK_MAX_DEVICES 16
#define RSSI_TRACK_SAMPLES 5
#define RSSI_TRACK_EXPIRY_MS 15000

#define PERSIST_INTERVAL_MS 60000
#define PERSIST_FILE "/flock_session.dat"
#define TOAST_DURATION_MS 2500

#define LOC_MAX_SAMPLES 40
#define LOC_SAMPLE_INTERVAL 500
#define LOC_TX_POWER_DEFAULT -59
#define LOC_PATH_LOSS_N 2.5
#define LOC_MIN_SAMPLES_EST 3
#define LOC_ARROW_RADIUS 28

#define SCORE_DEFINITIVE 100  
#define SCORE_STRONG     75   
#define SCORE_WEAK       25   
#define SCORE_BONUS_RSSI 10   
#define SCORE_BONUS_STAT 15   

#define CONFIDENCE_ALARM_THRESHOLD 50
#define CONFIDENCE_HIGH    75
#define CONFIDENCE_CERTAIN 100

#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_CS_PIN       12

// ============================================================================
// GLOBALS & STRUCTS
// ============================================================================
M5Canvas spr(&M5Cardputer.Display);
bool spr_ready = false;

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

#define SD_LINE_LEN 220
char sd_write_buffer[MAX_LOG_BUFFER][SD_LINE_LEN];
int  sd_write_count = 0;
unsigned long last_sd_flush = 0;

String current_log_file = "/FlockLog.csv";
String current_pcap_file = "/Threats.pcap";

int current_screen = 0;
#define NUM_SCREENS 10
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

#define MAX_SEEN_MACS 200
String seen_macs[MAX_SEEN_MACS];
unsigned long seen_mac_timestamps[MAX_SEEN_MACS];
int seen_macs_write_idx = 0;

String last_cap_type = "None";
String last_cap_mac = "--:--:--:--:--:--";
String last_cap_name = "";
int last_cap_rssi = 0;
int last_cap_confidence = 0;
String last_cap_time = "00:00:00";
String last_cap_det_method = "";
int last_cap_seq_num = -1; 

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
String live_logs[7] = {"", "", "", "", "", "", ""};

unsigned long last_time_save = 0;
unsigned long last_log_update = 0;
unsigned long last_sd_flush_check = 0;
unsigned long last_persist_save = 0;
unsigned long last_stats_update = 0; 

#define CHART_BARS 25
int activity_history[CHART_BARS] = {0};
unsigned long last_chart_update = 0;
long last_total_dets = 0;

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
String locator_target_mac = "";
String locator_target_name = "";
LocSample locator_samples[LOC_MAX_SAMPLES];
int locator_sample_count = 0;
unsigned long locator_last_sample = 0;
unsigned long locator_start_time = 0;

double locator_est_lat = 0.0;
double locator_est_lng = 0.0;
float locator_est_distance = 0.0;
float locator_bearing = 0.0;
float locator_confidence_radius = 0.0;
bool locator_has_estimate = false;
int locator_peak_rssi = -120;

TinyGPSPlus gps;
HardwareSerial SerialGPS(2);

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

int32_t get_median_voltage() {
    int16_t sorted[BATTERY_SAMPLE_SIZE];
    memcpy(sorted, battery_buffer, sizeof(battery_buffer));
    std::sort(sorted, sorted + BATTERY_SAMPLE_SIZE);
    if (sorted[BATTERY_SAMPLE_SIZE / 2] == 0) return M5Cardputer.Power.getBatteryVoltage();
    return sorted[BATTERY_SAMPLE_SIZE / 2];
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
    
    while (true) {
        M5Cardputer.update();
        update_battery_metrics(); 
        int32_t current_mv = get_median_voltage();
        int calc_pct = get_unified_battery_pct(current_mv);
        
        if (display_pct == -1) display_pct = calc_pct;
        else if (calc_pct > display_pct) display_pct = calc_pct;

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
        
        int bar_x = 40, bar_y = 95, bar_w = 160, bar_h = 10;
        spr.drawRect(bar_x, bar_y, bar_w, bar_h, ACCENT_COLOR); 
        int fill_w = (millis() / 15) % (bar_w - 2); 
        if (fill_w > 0) spr.fillRect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, ACCENT_COLOR);
        
        spr.setTextColor(DIM_COLOR, BG_COLOR); 
        spr.setTextSize(1); 
        spr.setCursor(35, 115); spr.print("PRESS ANY KEY TO BOOT DEVICE"); 
        spr.pushSprite(0, 0);

        float breathe = (exp(sin(millis() / 1000.0f * PI)) - 0.36787944f) / 2.35040238f;
        uint8_t led_val = (uint8_t)(breathe * 60.0f); 
        if (led_val != last_led_val) { set_cardputer_led(0, led_val, 0); last_led_val = led_val; }

        if (current_mv < 3200 && millis() - boot_time > 3000) { 
            M5Cardputer.Display.fillScreen(BG_COLOR); set_cardputer_led(0,0,0); return; 
        }
        if ((millis() - boot_time > 1500) && M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            M5Cardputer.Display.fillScreen(BG_COLOR); set_cardputer_led(0,0,0); return; 
        }
        delay(15);
    }
}

// ============================================================================
// SIGNATURE DATABASE
// ============================================================================
static const char* wifi_ssid_patterns[] = {
    "flock", "Flock", "FLOCK", "FS Ext Battery", "FS_", 
    "Penguin", "Pigvision", "FlockOS", "flocksafety", "OFS_IoT", "PFS_"
};
static const int NUM_SSID_PATTERNS = sizeof(wifi_ssid_patterns) / sizeof(wifi_ssid_patterns[0]);

static const char* mac_prefixes_tier1[] = {
    "ec:1b:bd", "58:8e:81", "d8:a0:d8", "08:3a:88", "d8:f3:bc", "cc:cc:cc"
};
static const int NUM_MAC_TIER1 = sizeof(mac_prefixes_tier1) / sizeof(mac_prefixes_tier1[0]);

static const char* mac_prefixes_tier2[] = {
    "48:e7:29", "74:4c:a1", "94:34:69", "38:5b:44", "90:35:ea", 
    "f0:82:c0", "b4:e3:f9", "94:08:53", "1c:34:f1", "a4:cf:12", 
    "3c:91:80", "80:30:49", "14:5a:fc", "9c:2f:9d", "e4:aa:ea", 
    "c8:c9:a3", "70:c9:4e", "04:0d:84"
};
static const int NUM_MAC_TIER2 = sizeof(mac_prefixes_tier2) / sizeof(mac_prefixes_tier2[0]);

static const char* device_name_patterns[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision", "FlockCam", "FS-", "RWLS-"
};
static const int NUM_NAME_PATTERNS = sizeof(device_name_patterns) / sizeof(device_name_patterns[0]);

static const char* raven_service_uuids[] = {
    "0000180a-0000-1000-8000-00805f9b34fb", "00003100-0000-1000-8000-00805f9b34fb", 
    "00003200-0000-1000-8000-00805f9b34fb", "00003300-0000-1000-8000-00805f9b34fb", 
    "00003400-0000-1000-8000-00805f9b34fb", "00003500-0000-1000-8000-00805f9b34fb", 
    "00001809-0000-1000-8000-00805f9b34fb", "00001819-0000-1000-8000-00805f9b34fb"
};
static const int NUM_RAVEN_UUIDS = sizeof(raven_service_uuids) / sizeof(raven_service_uuids[0]);
#define FLOCK_MFG_COMPANY_ID 0x09C8

int check_mac_prefix_tiered(const uint8_t* mac) {
    char mac_str[9]; 
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
    for (int i = 0; i < NUM_MAC_TIER1; i++) {
        if (strncasecmp(mac_str, mac_prefixes_tier1[i], 8) == 0) return 1;
    }
    for (int i = 0; i < NUM_MAC_TIER2; i++) {
        if (strncasecmp(mac_str, mac_prefixes_tier2[i], 8) == 0) return 2;
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

String classify_raven_firmware(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return "Unknown";
    bool has_gps = false, has_power = false, has_network = false, has_upload = false, has_error = false;
    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string uuid = device->getServiceUUID(i).toString();
        if (strcasestr(uuid.c_str(), "00003100")) has_gps = true;
        if (strcasestr(uuid.c_str(), "00003200")) has_power = true;
        if (strcasestr(uuid.c_str(), "00003300")) has_network = true;
        if (strcasestr(uuid.c_str(), "00003400")) has_upload = true;
        if (strcasestr(uuid.c_str(), "00003500")) has_error = true;
    }
    if (has_gps && has_power && has_network && has_upload && has_error) return "1.3.x";
    if (has_gps && has_power && has_network) return "1.2.x";
    return "Unknown";
}

int count_raven_uuids(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return 0;
    int matched = 0;
    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string uuid = device->getServiceUUID(i).toString();
        for (int j = 0; j < NUM_RAVEN_UUIDS; j++) {
            if (strcasecmp(uuid.c_str(), raven_service_uuids[j]) == 0) { 
                matched++; break; 
            }
        }
    }
    return matched;
}

// ============================================================================
// HELPER FUNCTIONS & PCAP
// ============================================================================
String format_time(unsigned long total_sec) {
    unsigned long m = (total_sec / 60) % 60;
    unsigned long h = (total_sec / 3600);
    unsigned long s = total_sec % 60;
    char buf[12]; 
    sprintf(buf, "%02lu:%02lu:%02lu", h, m, s);
    return String(buf);
}

String short_mac(const String& mac) {
    if (mac.length() > 8) return mac.substring(9);
    return mac;
}

String bytesToHexStr(const std::string& data) {
    static const char hex_chars[] = "0123456789ABCDEF";
    String res = ""; 
    res.reserve(data.length() * 2);
    for (uint8_t b : data) { res += hex_chars[b >> 4]; res += hex_chars[b & 0x0F]; }
    return res;
}

const char* confidence_label(int score) {
    if (score >= CONFIDENCE_CERTAIN) return "CERTAIN";
    if (score >= CONFIDENCE_HIGH)    return "HIGH";
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return "MEDIUM";
    return "LOW";
}

uint16_t confidence_color(int score) {
    if (score >= CONFIDENCE_CERTAIN) return ALERT_COLOR;
    if (score >= CONFIDENCE_HIGH)    return WARN_COLOR;
    if (score >= CONFIDENCE_ALARM_THRESHOLD) return TOAST_COLOR;
    return TEXT_COLOR;
}

struct pcap_packet_header { 
    uint32_t ts_sec; 
    uint32_t ts_usec; 
    uint32_t incl_len; 
    uint32_t orig_len; 
};

void write_threat_pcap(const uint8_t* payload, uint32_t length) {
    if (!sd_available) return;
    uint32_t capture_len = (length > 256) ? 256 : length; 
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    File pfile = SD.open(current_pcap_file.c_str(), FILE_APPEND);
    if (pfile) {
        unsigned long now = millis();
        pcap_packet_header pph;
        pph.ts_sec = session_start_time + (now / 1000); 
        pph.ts_usec = (now % 1000) * 1000;
        pph.incl_len = capture_len; 
        pph.orig_len = length; 
        pfile.write((const uint8_t*)&pph, sizeof(pph)); 
        pfile.write(payload, capture_len); 
        pfile.close();
    }
    xSemaphoreGive(dataMutex);
}

// ============================================================================
// PERSISTENCE & TRACKING
// ============================================================================
void save_session_to_flash() {
    if (!littlefs_available) return;
    File f = LittleFS.open(PERSIST_FILE, "w");
    if (!f) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    f.printf("%ld\n%ld\n%lu\n%ld\n%d\n", lifetime_wifi, lifetime_ble, lifetime_seconds, lifetime_flock_total, current_volume);
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
    line = f.readStringUntil('\n'); if (line.length() > 0) current_volume = line.toInt(); 
    f.close();
}

bool is_mac_recently_seen(const String& mac) {
    unsigned long now = millis();
    for (int i = 0; i < MAX_SEEN_MACS; i++) {
        if (seen_macs[i] == mac) {
            if ((now - seen_mac_timestamps[i]) < REDETECT_WINDOW_MS) return true;
            else { seen_mac_timestamps[i] = now; return false; }
        }
    }
    return false;
}

void add_seen_mac(const String& mac) {
    seen_macs[seen_macs_write_idx] = mac; 
    seen_mac_timestamps[seen_macs_write_idx] = millis();
    seen_macs_write_idx = (seen_macs_write_idx + 1) % MAX_SEEN_MACS;
}

void rssi_track_update(const char* mac, int rssi) {
    unsigned long now = millis();
    for (int i = 0; i < rssi_tracker_count; i++) {
        if (strncmp(rssi_tracker[i].mac, mac, 17) == 0) {
            if (rssi_tracker[i].sample_count < RSSI_TRACK_SAMPLES) {
                rssi_tracker[i].samples[rssi_tracker[i].sample_count++] = rssi;
            } else {
                for (int j = 0; j < RSSI_TRACK_SAMPLES - 1; j++) rssi_tracker[i].samples[j] = rssi_tracker[i].samples[j + 1];
                rssi_tracker[i].samples[RSSI_TRACK_SAMPLES - 1] = rssi;
            }
            rssi_tracker[i].last_seen = now; 
            return;
        }
    }
    if (rssi_tracker_count >= RSSI_TRACK_MAX_DEVICES) return; 
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
            int n = rssi_tracker[i].sample_count; 
            int* s = rssi_tracker[i].samples; 
            int peak_idx = 0;
            for (int j = 1; j < n; j++) if (s[j] > s[peak_idx]) peak_idx = j;
            int range = s[peak_idx] - min(s[0], s[n - 1]);
            if (peak_idx > 0 && peak_idx < n - 1 && range >= 6) { 
                rssi_tracker[i].scored = true; return true; 
            }
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
// THREAT EQUALIZER DATA
// ============================================================================
#define radar_cx 62
#define radar_cy 74
#define radar_r  50
#define inner_r  20
#define NUM_RADIAL_BANDS 36
float radial_spikes[NUM_RADIAL_BANDS] = {0};
uint16_t radial_colors[NUM_RADIAL_BANDS] = {0}; 
static float hud_rotation = 0.0f;
#define NUM_PARTICLES 24 // Increased by 25%
static float noise_r[NUM_PARTICLES] = {0};
static float noise_a[NUM_PARTICLES] = {0};
static int noise_life[NUM_PARTICLES] = {0};
static int noise_max_life[NUM_PARTICLES] = {0};

void add_blip(uint16_t blip_color) {
    int band = random(0, NUM_RADIAL_BANDS);
    radial_spikes[band] = 1.0f + (random(0, 50) / 100.0f); 
    radial_colors[band] = blip_color;
    int prev = (band - 1 + NUM_RADIAL_BANDS) % NUM_RADIAL_BANDS;
    int next = (band + 1) % NUM_RADIAL_BANDS;
    if(radial_spikes[prev] < 0.6f) { radial_spikes[prev] = 0.6f; radial_colors[prev] = blip_color; }
    if(radial_spikes[next] < 0.6f) { radial_spikes[next] = 0.6f; radial_colors[next] = blip_color; }
}

// ============================================================================
// LOGGING ENGINE
// ============================================================================
void flush_sd_buffer() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (sd_write_count == 0 || !sd_available) { xSemaphoreGive(dataMutex); return; }
    int count = sd_write_count;
    char local_buf[MAX_LOG_BUFFER][SD_LINE_LEN];
    for (int i = 0; i < count; i++) {
        strncpy(local_buf[i], sd_write_buffer[i], SD_LINE_LEN - 1);
        local_buf[i][SD_LINE_LEN - 1] = '\0';
    }
    sd_write_count = 0;
    xSemaphoreGive(dataMutex);
    File file = SD.open(current_log_file.c_str(), FILE_APPEND);
    if (file) {
        for (int i = 0; i < count; i++) file.println(local_buf[i]);
        file.close(); last_sd_flush = millis();
    }
}

void trigger_toast(const String& type, const String& name, int confidence) {
    toast_text = (name.length() > 0 && name != "Hidden") ? name : type;
    if (toast_text.length() > 14) toast_text = toast_text.substring(0, 14);
    toast_text += " " + String(confidence) + "%";
    toast_start = millis(); toast_active = true;
}

void add_to_capture_history(const char* type, const char* mac, const String& name, int rssi, int confidence) {
    for (int i = CAPTURE_HISTORY_SIZE - 1; i > 0; i--) capture_history[i] = capture_history[i - 1];
    capture_history[0].type = String(type); capture_history[0].mac = String(mac);
    capture_history[0].name = name; capture_history[0].rssi = rssi; capture_history[0].confidence = confidence;
    capture_history[0].time = format_time((millis() - session_start_time) / 1000);
    if (capture_history_count < CAPTURE_HISTORY_SIZE) capture_history_count++;
}

void log_detection(const char* type, const char* proto, int rssi, const char* mac, const String& name, int channel, int tx_power, const String& extra_data, const char* detection_method, int confidence, int seq_num) {
    String mac_str = String(mac); 
    unsigned long now_ms = millis();
    String current_time_formatted = format_time((now_ms - session_start_time) / 1000);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool is_new = !is_mac_recently_seen(mac_str);
    
    if (is_new) {
        add_seen_mac(mac_str);
        uint16_t blip_col = ACCENT_COLOR;
        if (strcmp(proto, "WIFI") == 0) { 
            session_wifi++; lifetime_wifi++; session_flock_wifi++; blip_col = HEADER_COLOR; 
        } else { 
            session_ble++; lifetime_ble++; blip_col = PURPLE_COLOR; 
        }
        if (strstr(type, "RAVEN") != NULL) { session_raven++; blip_col = TEAL_COLOR; }
        else if (strcmp(proto, "BLE") == 0) { session_flock_ble++; }
        lifetime_flock_total++;
        add_to_capture_history(type, mac, name, rssi, confidence);
        trigger_toast(String(type), name, confidence); 
        add_blip(blip_col); 
    }
    
    last_cap_type = String(type); last_cap_mac = mac_str; last_cap_name = name;
    last_cap_rssi = rssi; last_cap_confidence = confidence; last_cap_time = current_time_formatted; 
    last_cap_det_method = String(detection_method);
    last_cap_seq_num = seq_num; 

    if (is_new && sd_available && sd_write_count < MAX_LOG_BUFFER) {
        char clean_name[64]; strncpy(clean_name, name.c_str(), sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0'; for (char* p = clean_name; *p; p++) if (*p == ',') *p = ' ';
        char clean_extra[64]; strncpy(clean_extra, extra_data.c_str(), sizeof(clean_extra) - 1);
        clean_extra[sizeof(clean_extra) - 1] = '\0'; for (char* p = clean_extra; *p; p++) if (*p == ',') *p = ' ';

        char gps_fields[80];
        bool gps_fresh = gps.location.isValid() && (gps.location.age() < 2000);
        if (gps_fresh) {
            snprintf(gps_fields, sizeof(gps_fields), "%.6f,%.6f,%.1f,%.1f,%.1f",
                gps.location.lat(), gps.location.lng(), gps.speed.isValid() ? gps.speed.mph() : 0.0,
                gps.course.isValid() ? gps.course.deg() : 0.0, gps.altitude.isValid() ? gps.altitude.meters(): 0.0);
        } else { strncpy(gps_fields, "0.000000,0.000000,0.0,0.0,0.0", sizeof(gps_fields)); }

        snprintf(sd_write_buffer[sd_write_count], SD_LINE_LEN, "%lu,%d,%s,%s,%d,%s,%s,%d,%s,%d,%s,%s,%d,%s",
            now_ms, channel, type, proto, rssi, mac, clean_name, tx_power, detection_method, confidence, 
            confidence_label(confidence), clean_extra, seq_num, gps_fields);
        sd_write_count++;
    }
    xSemaphoreGive(dataMutex);

    if (millis() - last_log_update > 500) {
        String cleanName = (name != "Hidden" && name != "Unknown" && name != "") ? name : String(proto);
        if (cleanName.length() > 9) cleanName = cleanName.substring(0, 9);
        char log_buf[64]; sprintf(log_buf, "%s | %ddBm | %s", cleanName.c_str(), rssi, short_mac(mac_str).c_str());
        bool is_duplicate = false;
        for (int i = 0; i < 7; i++) { 
            if (live_logs[i].indexOf(cleanName) >= 0 && cleanName != "WIFI" && cleanName != "BLE") { is_duplicate = true; break; } 
        }
        if (!is_duplicate) {
            for (int i = 6; i > 0; i--) live_logs[i] = live_logs[i - 1];
            char prefix = (strcmp(proto, "WIFI") == 0) ? 'W' : 'B';
            live_logs[0] = String(prefix) + "!" + String(confidence) + "%|" + String(log_buf); 
            last_log_update = millis();
        }
    }
}

// ============================================================================
// LOCATOR ENGINE
// ============================================================================
double rssi_to_weight(int rssi) { 
    if (rssi > -20) rssi = -20; if (rssi < -100) rssi = -100; return pow(10.0, (double)(rssi + 100) / 20.0); 
}

float rssi_to_distance(int rssi, int tx_power) {
    if (rssi >= tx_power) return 0.5; 
    float d = pow(10.0, (double)(tx_power - rssi) / (10.0 * LOC_PATH_LOSS_N)); 
    if (d > 500.0) d = 500.0; return d;
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
    if (!locator_active || strncmp(mac, locator_target_mac.c_str(), 17) != 0) return;
    if (millis() - locator_last_sample < LOC_SAMPLE_INTERVAL) return;
    if (!gps.location.isValid() || gps.location.age() > 2000) return;
    
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int idx = locator_sample_count;
    if (idx >= LOC_MAX_SAMPLES) {
        int weakest = 0;
        for (int i = 1; i < LOC_MAX_SAMPLES; i++) if (locator_samples[i].rssi < locator_samples[weakest].rssi) weakest = i;
        if (rssi > locator_samples[weakest].rssi) { idx = weakest; } 
        else { xSemaphoreGive(dataMutex); return; }
    } else { locator_sample_count++; }
    
    locator_samples[idx] = {gps.location.lat(), gps.location.lng(), rssi, millis()};
    locator_last_sample = millis();
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

void locator_start(const String& mac, const String& name) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = true; locator_target_mac = mac; locator_target_name = name;
    locator_sample_count = 0; locator_has_estimate = false; locator_peak_rssi = -120;
    locator_est_distance = 0; locator_bearing = 0; locator_confidence_radius = 0; locator_start_time = millis();
    xSemaphoreGive(dataMutex);
}

void locator_stop() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    locator_active = false; locator_has_estimate = false; locator_sample_count = 0;
    xSemaphoreGive(dataMutex);
}

void draw_arrow(int cx, int cy, int r, float angle_deg) {
    float a = radians(angle_deg - 90.0); 
    int tip_x = cx + (int)(r * cos(a)); int tip_y = cy + (int)(r * sin(a));
    float a_left  = a + radians(140); float a_right = a - radians(140);
    int left_x  = cx + (int)((r * 0.5) * cos(a_left)); int left_y  = cy + (int)((r * 0.5) * sin(a_left));
    int right_x = cx + (int)((r * 0.5) * cos(a_right)); int right_y = cy + (int)((r * 0.5) * sin(a_right));
    spr.fillTriangle(tip_x, tip_y, left_x, left_y, right_x, right_y, ALERT_COLOR);
    spr.drawTriangle(tip_x, tip_y, left_x, left_y, right_x, right_y, TEXT_COLOR);
}

// ============================================================================
// WIFI PACKET HANDLER 
// ============================================================================
typedef struct { 
    unsigned frame_ctrl:16; unsigned duration_id:16; uint8_t addr1[6]; 
    uint8_t addr2[6]; uint8_t addr3[6]; unsigned sequence_ctrl:16; uint8_t addr4[6]; 
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
    bool is_beacon    = (frame_subtype == 8); bool is_probe_req = (frame_subtype == 4);
    if (!is_beacon && !is_probe_req) return;

    char ssid[33] = {0}; 
    uint8_t* frame_body = (uint8_t*)ipkt + 24; uint8_t* tagged_params; int remaining;
    if (is_beacon) {
        if (ppkt->rx_ctrl.sig_len < 24 + 12 + 2) return;
        tagged_params = frame_body + 12; remaining = ppkt->rx_ctrl.sig_len - 24 - 12 - 4;
    } else {
        tagged_params = frame_body; remaining = ppkt->rx_ctrl.sig_len - 24 - 4;
    }
    if (remaining > 2 && tagged_params[0] == 0 && tagged_params[1] <= 32 && tagged_params[1] <= remaining - 2) {
        memcpy(ssid, &tagged_params[2], tagged_params[1]); ssid[tagged_params[1]] = '\0';
    }

    int confidence = 0; 
    char methods[64] = {0};
    int mac_score    = check_mac_prefix_tiered(hdr->addr2); 
    bool ssid_generic   = (strlen(ssid) > 0 && check_ssid_pattern(ssid));
    bool ssid_flock_fmt = (strlen(ssid) > 0 && is_flock_ssid_format(ssid));
    
    if (ssid_flock_fmt) { 
        confidence = SCORE_DEFINITIVE; strncat(methods, "ssid_fmt ", 63); 
    } else if (mac_score == 1) { 
        confidence = SCORE_STRONG; strncat(methods, "mac_t1 ", 63); 
        if (ssid_generic) { confidence = SCORE_DEFINITIVE; strncat(methods, "ssid ", 63); }
    } else {
        if (mac_score == 2) { confidence += SCORE_WEAK; strncat(methods, "mac_t2 ", 63); }
        if (ssid_generic) { confidence += SCORE_WEAK; strncat(methods, "ssid ", 63); }
    }
    
    if (confidence > 0 && ppkt->rx_ctrl.rssi > -50) confidence += SCORE_BONUS_RSSI;

    char mac_str[18]; 
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", 
        hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5]);
    const char* name_str       = strlen(ssid) > 0 ? ssid : "Hidden";
    const char* frame_type_str = is_beacon ? "Beacon" : "ProbeReq";
    int seq_num                = hdr->sequence_ctrl >> 4; 

    if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
        channel_lock_until = millis() + 10000; 
        
        rssi_track_update(mac_str, ppkt->rx_ctrl.rssi);
        if (rssi_track_is_stationary(mac_str)) confidence += SCORE_BONUS_STAT;
        if (confidence > 100) confidence = 100;
        locator_add_sample(mac_str, ppkt->rx_ctrl.rssi);
        
        int mlen = strlen(methods); if (mlen > 0 && methods[mlen-1] == ' ') methods[mlen-1] = '\0';
        log_detection("FLOCK_WIFI", "WIFI", ppkt->rx_ctrl.rssi, mac_str, String(name_str),
                      ppkt->rx_ctrl.channel, 0, String(frame_type_str), methods, confidence, seq_num);
        write_threat_pcap(ppkt->payload, ppkt->rx_ctrl.sig_len);
        if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) { 
            trigger_alarm_confidence = confidence; last_buzzer_time = millis(); 
        }
    } else if (ppkt->rx_ctrl.rssi > IGNORE_WEAK_RSSI) {
        if (millis() - last_log_update > LOG_UPDATE_DELAY) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            String cleanName = (strlen(ssid) > 0) ? String(ssid) : "WiFi";
            if (cleanName.length() > 9) cleanName = cleanName.substring(0, 9);
            bool is_duplicate = false;
            for (int i = 0; i < 7; i++) { 
                if (live_logs[i].indexOf(cleanName) >= 0 && cleanName != "WiFi") { is_duplicate = true; break; } 
            }
            if (!is_duplicate) {
                char log_buf[64]; sprintf(log_buf, "%s | %ddBm | %s", cleanName.c_str(), ppkt->rx_ctrl.rssi, &mac_str[9]);
                for (int i = 6; i > 0; i--) live_logs[i] = live_logs[i - 1];
                live_logs[0] = "W." + String(log_buf); last_log_update = millis();
            }
            xSemaphoreGive(dataMutex);
        }
    }
}

// ============================================================================
// BLE CALLBACK 
// ============================================================================
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        NimBLEAddress addr = advertisedDevice->getAddress(); 
        uint8_t mac[6];
        sscanf(addr.toString().c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", 
            (unsigned int*)&mac[0], (unsigned int*)&mac[1], (unsigned int*)&mac[2], 
            (unsigned int*)&mac[3], (unsigned int*)&mac[4], (unsigned int*)&mac[5]);
            
        int confidence = 0; char methods[64] = {0}; char capture_type[16] = "FLOCK_BLE";
        bool got_name = false, got_mfg = false, got_raven = false;
        
        int mac_score = check_mac_prefix_tiered(mac);
        String dev_name = advertisedDevice->haveName() ? String(advertisedDevice->getName().c_str()) : "Unknown";
        if (advertisedDevice->haveName()) {
            if (check_device_name_pattern(advertisedDevice->getName().c_str())) { 
                strncat(methods, "name ", 63); got_name = true;
            } else if (is_penguin_numeric_name(advertisedDevice->getName().c_str())) { 
                strncat(methods, "penguin_num ", 63); got_name = true;
            }
        }
        
        if (advertisedDevice->haveManufacturerData()) {
            std::string mfg = advertisedDevice->getManufacturerData();
            if (check_manufacturer_id(mfg)) {
                strncat(methods, "mfg_0x09C8 ", 63); got_mfg = true;
                if (has_tn_serial(mfg)) { 
                    strncat(methods, "tn_serial ", 63);
                }
            }
        }
        
        int raven_uuid_count = count_raven_uuids(advertisedDevice);
        if (raven_uuid_count > 0) {
            strncpy(capture_type, "RAVEN_BLE", sizeof(capture_type)); got_raven = true;
            if (raven_uuid_count >= 3) strncat(methods, "raven_multi ", 63);
            else strncat(methods, "raven_uuid ", 63);
        }

        if (got_raven || got_name || (got_mfg && has_tn_serial(advertisedDevice->getManufacturerData()))) {
            confidence = SCORE_DEFINITIVE;
        } else if (mac_score == 1 || got_mfg) {
            confidence = SCORE_STRONG;
            if (mac_score == 1) strncat(methods, "mac_t1 ", 63);
        } else {
            if (mac_score == 2) { confidence += SCORE_WEAK; strncat(methods, "mac_t2 ", 63); }
            uint8_t addr_type = addr.getType();
            if (addr_type == 0 || (addr_type == 1 && (mac[0] >> 6) == 0x03)) { 
                confidence += SCORE_WEAK; strncat(methods, "static_addr ", 63); 
            }
        }
        
        if (confidence > 0 && advertisedDevice->getRSSI() > -50) confidence += SCORE_BONUS_RSSI;
        
        char mac_string[18]; snprintf(mac_string, sizeof(mac_string), "%s", addr.toString().c_str());
        
        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            channel_lock_until = millis() + 10000; 
            
            rssi_track_update(mac_string, advertisedDevice->getRSSI());
            if (rssi_track_is_stationary(mac_string)) confidence += SCORE_BONUS_STAT;
            locator_add_sample(mac_string, advertisedDevice->getRSSI());
        }
        if (confidence > 100) confidence = 100;
        
        if (confidence >= CONFIDENCE_ALARM_THRESHOLD) {
            int tx_power = advertisedDevice->haveTXPower() ? advertisedDevice->getTXPower() : 0;
            String extra_data = advertisedDevice->haveManufacturerData() ? bytesToHexStr(advertisedDevice->getManufacturerData()) : "";
            if (strcmp(capture_type, "RAVEN_BLE") == 0) { 
                String fw = classify_raven_firmware(advertisedDevice); extra_data = "FW:" + fw + " UUIDs:" + String(raven_uuid_count); 
            }
            int mlen = strlen(methods); if (mlen > 0 && methods[mlen-1] == ' ') methods[mlen-1] = '\0';
            log_detection(capture_type, "BLE", advertisedDevice->getRSSI(), mac_string, dev_name, 0, tx_power, extra_data, methods, confidence, -1);
            if (millis() - last_buzzer_time > BUZZER_COOLDOWN || last_buzzer_time == 0) { 
                trigger_alarm_confidence = confidence; last_buzzer_time = millis(); 
            }
        } else if (advertisedDevice->getRSSI() > IGNORE_WEAK_RSSI) {
            if (millis() - last_log_update > LOG_UPDATE_DELAY) {
                xSemaphoreTake(dataMutex, portMAX_DELAY);
                String cleanName = (dev_name != "Unknown" && dev_name != "") ? dev_name : "BLE";
                if (cleanName.length() > 9) cleanName = cleanName.substring(0, 9);
                bool is_duplicate = false;
                for (int i = 0; i < 7; i++) { 
                    if (live_logs[i].indexOf(cleanName) >= 0 && cleanName != "BLE") { is_duplicate = true; break; } 
                }
                if (!is_duplicate) {
                    char log_buf[64]; sprintf(log_buf, "%s | %ddBm | %s", cleanName.c_str(), advertisedDevice->getRSSI(), &mac_string[9]);
                    for (int i = 6; i > 0; i--) live_logs[i] = live_logs[i - 1];
                    live_logs[0] = "B." + String(log_buf); last_log_update = millis();
                }
                xSemaphoreGive(dataMutex);
            }
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
        
        if (millis() - last_ble_scan >= BLE_SCAN_INTERVAL) { 
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
        while (SerialGPS.available() > 0) gps.encode(SerialGPS.read()); 
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// UI RENDERING - BASE COMPONENTS
// ============================================================================
void draw_header_spr(int screen_num) {
    static const char* screen_names[] = {
        "SCANNER", "STATS", "LAST HIT", "HISTORY", 
        "LIVE FEED", "GPS", "ACTIVITY", "PROXIMITY", "DEVICE INFO", "LOCATOR"
    };
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
    if (millis() - toast_start > TOAST_DURATION_MS) { toast_active = false; return; }
    spr.fillRect(0, DISP_H - 18, DISP_W, 18, TOAST_COLOR);
    spr.setTextColor(TFT_BLACK, TOAST_COLOR); spr.setTextSize(2); spr.setCursor(4, DISP_H - 16); 
    spr.print("! "); spr.print(toast_text);
}

void drawCard(int x, int y, int w, int h) {
    spr.fillRect(x, y, w, h, CARD_COLOR); spr.drawRect(x, y, w, h, CARD_BORDER); 
}

// ============================================================================
// UI RENDERING - SCREENS 
// ============================================================================
void draw_scanner_screen() {
    spr.fillRect(116, 19, DISP_W - 116, DISP_H - 19, BG_COLOR); spr.fillRect(0, 19, 116, DISP_H - 19, BG_COLOR); 
    draw_header_spr(0); spr.setClipRect(0, 19, 116, DISP_H - 19);
    
    // TILT INCREASED FOR DEEPER 3D PERSPECTIVE
    float TILT = 0.65f; 

    // 3D DROP SHADOW / RIM RINGS
    spr.drawEllipse(radar_cx, radar_cy + 2, radar_r, radar_r * TILT, DIM2_COLOR);
    spr.drawEllipse(radar_cx, radar_cy + 2, inner_r, inner_r * TILT, DIM2_COLOR);
    
    // MAIN RINGS
    spr.drawEllipse(radar_cx, radar_cy, radar_r, radar_r * TILT, GRID_COLOR);
    spr.drawEllipse(radar_cx, radar_cy, inner_r, inner_r * TILT, GRID_COLOR);
    spr.drawEllipse(radar_cx, radar_cy, inner_r - 5, (inner_r - 5) * TILT, DIM2_COLOR);

    // STAGGERED PARTICLE INIT
    static bool p_init = false;
    if (!p_init) {
        for(int i=0; i<NUM_PARTICLES; i++) {
            noise_r[i] = random(inner_r + 2, radar_r - 2);
            noise_a[i] = random(0, 360) * 0.0174533f;
            noise_max_life[i] = random(200, 600);
            noise_life[i] = random(0, noise_max_life[i]); // Asynchronous start ages
        }
        p_init = true;
    }

    for(int i = 0; i < NUM_PARTICLES; i++) {
        if (noise_life[i] <= 0) {
            noise_r[i] = random(inner_r + 2, radar_r - 2); noise_a[i] = random(0, 360) * 0.0174533f;
            noise_max_life[i] = random(200, 600); noise_life[i] = noise_max_life[i];
        } else {
            noise_life[i]--; float life_ratio = (float)noise_life[i] / (float)noise_max_life[i]; float peak = 0.5f; int intensity;
            if (life_ratio > peak) intensity = (int)(255.0f * (1.0f - life_ratio) / (1.0f - peak));
            else intensity = (int)(255.0f * (life_ratio / peak));
            intensity = (int)(255.0f * pow((float)intensity/255.0f, 2));
            if (intensity < 0) intensity = 0; if (intensity > 255) intensity = 255;
            uint16_t p_col = night_mode ? lgfx::color565(intensity, 0, 0) : ((i % 3 == 0) ? lgfx::color565(intensity, intensity, intensity) : lgfx::color565(0, intensity, intensity)); 
            int px = radar_cx + noise_r[i] * cos(noise_a[i]); int py = radar_cy + noise_r[i] * sin(noise_a[i]) * TILT;
            spr.fillRect(px - 1, py - 1, 2, 2, p_col);
        }
    }

    uint16_t modulated_col = (sin(millis()/500.0f) > 0) ? HEADER_COLOR : DIM_COLOR;
    float breathe = sin(millis() / 500.0f); int dynamic_r = radar_r + (int)(4 * breathe);
    float cos_rot = cos(hud_rotation); float sin_rot = sin(hud_rotation);
    spr.drawLine(radar_cx - dynamic_r * cos_rot, radar_cy - dynamic_r * sin_rot * TILT, radar_cx + dynamic_r * cos_rot, radar_cy + dynamic_r * sin_rot * TILT, modulated_col);
    spr.drawLine(radar_cx - dynamic_r * -sin_rot, radar_cy - dynamic_r * cos_rot * TILT, radar_cx + dynamic_r * -sin_rot, radar_cy + dynamic_r * cos_rot * TILT, modulated_col);

    float sweep_deg = (millis() / 1500.0f) * 6.28318f;
    for (int i = 1; i <= 24; i++) {
        float trail_angle = sweep_deg - (i * 0.05f); float ratio = (24.0f - i) / 24.0f; 
        int tr = 10 + (0 - 10) * ratio; int tg = 20 + (238 - 20) * ratio; int tb = 48 + (255 - 48) * ratio;
        uint16_t fade_col = night_mode ? lgfx::color565(tb, 0, 0) : lgfx::color565(tr, tg, tb);
        for (int r_step = inner_r + 2; r_step < radar_r; r_step += 4) {
            spr.drawPixel(radar_cx + r_step * cos(trail_angle), radar_cy + r_step * sin(trail_angle) * TILT, fade_col);
        }
    }
    spr.drawLine(radar_cx, radar_cy, radar_cx + (radar_r - 1) * cos(sweep_deg), radar_cy + (radar_r - 1) * sin(sweep_deg) * TILT, HEADER_COLOR);

    float angle_step = 6.28318f / NUM_RADIAL_BANDS; bool any_active = false;
    for (int i = 0; i < NUM_RADIAL_BANDS; i++) {
        float a = i * angle_step;
        if (radial_spikes[i] > 0.1f) any_active = true;
        if (radial_spikes[i] > 0) {
            int spike_len = (int)((radar_r - inner_r) * radial_spikes[i] * 0.6f); if (spike_len < 2) spike_len = 2; 
            int base_x = radar_cx + radar_r * cos(a); int base_y = radar_cy + radar_r * sin(a) * TILT;
            uint16_t line_col = radial_colors[i];
            spr.drawLine(base_x, base_y, base_x, base_y - spike_len, line_col); spr.drawPixel(base_x, base_y - spike_len, TEXT_COLOR);
            int blip_r = (int)(radial_spikes[i] * 2.5f);
            spr.fillCircle(radar_cx + (radar_r - 8) * cos(a), radar_cy + (radar_r - 8) * sin(a) * TILT, blip_r, line_col);
            radial_spikes[i] -= 0.0015f; if (radial_spikes[i] < 0) radial_spikes[i] = 0; // HANGS MUCH LONGER NOW
        }
    }
    if (!any_active) {
        float pulse_sin = sin(millis()/400.0f); int pulse_r = inner_r + 3 + (int)(3 * pulse_sin);
        uint16_t pulse_col = (pulse_sin > 0) ? GRID_COLOR : BG_COLOR;
        spr.drawEllipse(radar_cx, radar_cy, pulse_r, pulse_r * TILT, pulse_col);
    }
    hud_rotation += 0.01f; spr.clearClipRect(); 

    int px = radar_cx + radar_r + 8;
    spr.drawLine(px - 2, 20, px - 2, DISP_H, GRID_COLOR);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long sw = session_flock_wifi; long sb = session_flock_ble;
    xSemaphoreGive(dataMutex);

    spr.drawFastHLine(4, DISP_H - 14, radar_cx*2 + radar_r - 4, GRID_COLOR);
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, DISP_H - 10); spr.print("UPTIME:"); 
    spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(format_time((millis() - session_start_time) / 1000));

    bool ble_scanning = pBLEScan->isScanning();
    spr.drawRect(px + 2, 22, 54, 14, ble_scanning ? DIM2_COLOR : TOAST_COLOR);
    spr.setTextColor(ble_scanning ? DIM_COLOR : TOAST_COLOR, BG_COLOR);
    spr.setCursor(px + 4, 25); spr.print("WF:CH"); spr.print(current_channel);
    
    if (millis() < channel_lock_until) {
        spr.fillRect(px + 40, 23, 15, 12, TOAST_COLOR);
        spr.setTextColor(TFT_BLACK, TOAST_COLOR);
        spr.setCursor(px + 42, 25); spr.print("LK");
    }

    spr.drawRect(px + 58, 22, 54, 14, ble_scanning ? PURPLE_COLOR : DIM2_COLOR);
    spr.setTextColor(ble_scanning ? PURPLE_COLOR : DIM_COLOR, BG_COLOR);
    spr.setCursor(px + 60, 25); spr.print("BLE:SCN");

    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(px + 2, 42); spr.print("WIFI HITS");
    spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.setTextSize(2);
    spr.setCursor(px + 2, 52); spr.print(sw);
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(px + 2, 70); spr.print("BLE HITS");
    spr.setTextColor(PURPLE_COLOR, BG_COLOR); spr.setTextSize(2);
    spr.setCursor(px + 2, 80); spr.print(sb);

    spr.setTextSize(1);
    spr.drawLine(px + 2, 100, DISP_W - 2, 100, GRID_COLOR);
    if (last_cap_type != "None") {
        spr.setTextColor(confidence_color(last_cap_confidence), BG_COLOR);
        spr.setCursor(px + 2, 104);
        String dn = (last_cap_name != "" && last_cap_name != "Hidden" && last_cap_name != "Unknown") ? last_cap_name : short_mac(last_cap_mac);
        if (dn.length() > 13) dn = dn.substring(0, 13);
        spr.print(dn);
        spr.setTextColor(TEXT_COLOR, BG_COLOR);
        spr.setCursor(px + 2, 116); spr.print(String(last_cap_confidence) + "% " + confidence_label(last_cap_confidence));
    } else {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setCursor(px + 2, 104); spr.print("-- no hits yet --");
    }
    draw_toast_spr();
}

void draw_stats_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    long tw = session_flock_wifi, tb = session_flock_ble, tr = session_raven;
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR);
    draw_header_spr(1);
    drawCard(4, 30, 72, 52);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(10, 34); spr.print("WIFI SESS");
    spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(3); spr.setCursor(10, 46); spr.print(tw);
    drawCard(82, 30, 72, 52);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(88, 34); spr.print("BLE SESS");
    spr.setTextColor(PURPLE_COLOR, CARD_COLOR); spr.setTextSize(3); spr.setCursor(88, 46); spr.print(tb);
    drawCard(160, 30, 72, 52);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(166, 34); spr.print("RAVEN");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(3); spr.setCursor(166, 46); spr.print(tr);
    drawCard(67, 95, 106, 32);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(85, 100); spr.print("SESSION TIME");
    spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(77, 110);
    spr.print(format_time((millis() - session_start_time) / 1000));
    draw_toast_spr();
}

void draw_last_capture_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    String t_type=last_cap_type, t_time=last_cap_time, t_mac=last_cap_mac;
    String t_name=last_cap_name, t_method=last_cap_det_method;
    int t_rssi=last_cap_rssi, t_conf=last_cap_confidence, t_seq = last_cap_seq_num;
    xSemaphoreGive(dataMutex);
    
    spr.fillSprite(BG_COLOR); draw_header_spr(2);
    
    // GHOST WIREFRAME FOR EMPTY STATE
    if (t_type == "None") {
        spr.fillRect(4, 22, DISP_W - 8, 20, HILIGHT_BG);
        spr.drawRect(4, 22, DISP_W - 8, 20, CARD_BORDER);
        spr.setTextColor(DIM_COLOR, HILIGHT_BG); spr.setTextSize(1); spr.setCursor(8, 26); spr.print("WAITING FOR SIGNAL");
        
        drawCard(4, 46, DISP_W - 8, 22);
        spr.setTextColor(DIM2_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 50); spr.print("--:--:--:--:--:--");
        
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 74); spr.print("RSSI");
        spr.setTextColor(DIM2_COLOR, BG_COLOR); spr.setTextSize(2); spr.setCursor(4, 82); spr.print("-- dBm");
        
        int bar_x = 135, bar_w = DISP_W - 139;
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(bar_x, 74); spr.print("CONFIDENCE");
        spr.drawRect(bar_x, 84, bar_w, 10, GRID_COLOR);
        
        spr.drawLine(0, 108, DISP_W, 108, GRID_COLOR);
    } else {
        uint16_t ccol = confidence_color(t_conf);
        spr.fillRect(4, 22, DISP_W - 8, 20, HILIGHT_BG); spr.drawRect(4, 22, DISP_W - 8, 20, ccol);
        spr.setTextColor(ccol, HILIGHT_BG); spr.setTextSize(1); spr.setCursor(8, 26); spr.print(t_type);
        spr.setTextColor(TEXT_COLOR, HILIGHT_BG); spr.setCursor(DISP_W - 70, 26); spr.print("@ "); spr.print(t_time);
        
        drawCard(4, 46, DISP_W - 8, 22);
        spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 50);
        String dn = (t_name != "" && t_name != "Hidden" && t_name != "Unknown") ? t_name : t_mac;
        if (dn.length() > 36) dn = dn.substring(0, 36); spr.print(dn);
        
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 74); spr.print("RSSI");
        spr.setTextColor(HEADER_COLOR, BG_COLOR); spr.setTextSize(2); spr.setCursor(4, 82); spr.print(t_rssi); spr.print("dBm");
        
        if (t_seq >= 0) {
            spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(75, 74); spr.print("SEQ ID");
            spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(2); spr.setCursor(75, 82); spr.print(t_seq);
        }
        
        int bar_x = 135, bar_w = DISP_W - 139;
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(bar_x, 74); spr.print("CONFIDENCE");
        spr.drawRect(bar_x, 84, bar_w, 10, GRID_COLOR);
        int cfill = (t_conf * (bar_w - 2)) / 100; if (cfill > 0) spr.fillRect(bar_x + 1, 85, cfill, 8, ccol);
        spr.setTextColor(ccol, BG_COLOR); spr.setCursor(bar_x + 10, 96);
        char cpct[6]; sprintf(cpct, "%d%%", t_conf); spr.print(cpct);
        
        spr.drawLine(0, 108, DISP_W, 108, GRID_COLOR);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 111); spr.print("METHOD: ");
        spr.setTextColor(TEAL_COLOR, BG_COLOR);
        String m = t_method; if (m.length() > 28) m = m.substring(0, 28); spr.print(m);
        
        spr.setTextColor(ccol, BG_COLOR); spr.setTextSize(2); spr.setCursor(140, 118); spr.print(confidence_label(t_conf));
    }
    draw_toast_spr();
}

void draw_capture_history_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    CaptureEntry local_history[CAPTURE_HISTORY_SIZE];
    int local_count = capture_history_count;
    for (int i = 0; i < local_count; i++) local_history[i] = capture_history[i];
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR);
    draw_header_spr(3);
    
    // GHOST WIREFRAME FOR EMPTY STATE
    if (local_count == 0) {
        for(int i=0; i<5; i++) {
            int y = 22 + (i*21);
            spr.fillRect(0, y, 3, 20, DIM2_COLOR);
            spr.fillRect(3, y, DISP_W - 3, 20, (i%2==0) ? HILIGHT_BG : BG_COLOR);
            spr.setTextColor(DIM2_COLOR); spr.setTextSize(1); 
            spr.setCursor(8, y+6); spr.print("--");
            spr.setCursor(30, y+6); spr.print("Listening...");
        }
    } else {
        int y = 22;
        for (int i = 0; i < local_count && i < CAPTURE_HISTORY_SIZE; i++) {
            uint16_t rcol = confidence_color(local_history[i].confidence);
            spr.fillRect(0, y, 3, 20, rcol); 
            spr.fillRect(3, y, DISP_W - 3, 20, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            String t = local_history[i].type;
            if (t == "FLOCK_WIFI") t = "FW"; else if (t == "FLOCK_BLE") t = "FB"; else if (t == "RAVEN_BLE") t = "RV";
            String nom;
            if (local_history[i].name != "" && local_history[i].name != "Hidden" && local_history[i].name != "Unknown") {
                nom = local_history[i].name; if (nom.length() > 14) nom = nom.substring(0, 14);
            } else { nom = short_mac(local_history[i].mac); }
            spr.setTextColor(rcol, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR); spr.setTextSize(1);
            spr.setCursor(8, y + 6); spr.print(t);
            spr.setTextColor(TEXT_COLOR, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setCursor(30, y + 6); spr.print(nom);
            char cpct[5]; sprintf(cpct, "%d%%", local_history[i].confidence);
            spr.setTextColor(rcol, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setCursor(DISP_W - 50, y + 6); spr.print(cpct);
            spr.setTextColor(TEXT_COLOR, (i % 2 == 0) ? HILIGHT_BG : BG_COLOR);
            spr.setCursor(DISP_W - 35, y + 6); spr.print(local_history[i].time);
            y += 21;
        }
    }
    draw_toast_spr();
}

void draw_live_log_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    String t_logs[7]; for (int i = 0; i < 7; i++) t_logs[i] = live_logs[i];
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR);
    draw_header_spr(4);
    int y = 19;
    for (int i = 0; i < 7; i++) {
        if (t_logs[i] == "") { y += 16; continue; }
        
        char proto_char = t_logs[i].charAt(0);
        char hit_char = t_logs[i].charAt(1);
        bool is_hit = (hit_char == '!');
        String clean_line = t_logs[i].substring(2); 
        
        uint16_t row_bg = is_hit ? HILIGHT_BG : BG_COLOR;
        uint16_t txt_color = (proto_char == 'W') ? HEADER_COLOR : PURPLE_COLOR; // Text stays Cyan or Purple!
        
        spr.fillRect(0, y, DISP_W, 15, row_bg);
        if (is_hit) spr.fillRect(0, y, 3, 15, TOAST_COLOR); // Left bar indicates threshold cross
        
        spr.setTextColor(txt_color, row_bg); spr.setTextSize(1.2); 
        spr.setCursor(6, y + 3);
        if (clean_line.length() > 38) clean_line = clean_line.substring(0, 38);
        spr.print(clean_line);
        
        spr.drawLine(0, y + 15, DISP_W, y + 15, GRID_COLOR);
        y += 16;
    }
}

void draw_gps_screen() {
    spr.fillSprite(BG_COLOR);
    draw_header_spr(5);
    bool has_loc = gps.location.isValid();
    bool stale   = has_loc && (gps.location.age() > 2000);
    int sats     = gps.satellites.isValid() ? gps.satellites.value() : 0;
    uint32_t chars = gps.charsProcessed();
    uint32_t passed = gps.passedChecksum();
    uint32_t failed = gps.failedChecksum();

    drawCard(4, 22, 80, 40);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(10, 26); spr.print("SATELLITES");
    spr.setTextColor(sats >= 4 ? TEXT_COLOR : WARN_COLOR, CARD_COLOR); spr.setTextSize(3); spr.setCursor(10, 36); spr.print(sats);

    drawCard(90, 22, 146, 40);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(96, 26); spr.print("STATUS");
    int anim_cx = 215, anim_cy = 42;
    if (has_loc && !stale) {
        spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(96, 36); spr.print("LOCKED");
        int r = (millis() / 80) % 15;
        spr.drawCircle(anim_cx, anim_cy, r, ACCENT_COLOR); spr.fillCircle(anim_cx, anim_cy, 3, ACCENT_COLOR);
    } else if (has_loc && stale) {
        spr.setTextColor(WARN_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(96, 36); spr.print("LOST");
        spr.fillCircle(anim_cx, anim_cy, 4, WARN_COLOR);
    } else {
        spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(96, 36); spr.print("SEARCH...");
        float a = millis() / 250.0f;
        spr.drawCircle(anim_cx, anim_cy, 12, GRID_COLOR);
        spr.drawLine(anim_cx, anim_cy, anim_cx + 12*cos(a), anim_cy + 12*sin(a), WARN_COLOR);
        spr.fillCircle(anim_cx, anim_cy, 2, ALERT_COLOR);
    }

    if (has_loc && !stale) {
        drawCard(4, 68, 112, 30);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 72); spr.print("LATITUDE");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setCursor(8, 82); spr.print(gps.location.lat(), 5);
        drawCard(120, 68, 116, 30);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(124, 72); spr.print("LONGITUDE");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setCursor(124, 82); spr.print(gps.location.lng(), 5);
        drawCard(4, 102, 80, 28);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 106); spr.print("SPEED");
        spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setCursor(8, 116);
        char spd[12]; sprintf(spd, "%.1f mph", gps.speed.isValid() ? gps.speed.mph() : 0.0); spr.print(spd);
        drawCard(90, 102, 80, 28);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(94, 106); spr.print("HEADING");
        spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setCursor(94, 116);
        char hdg[8]; sprintf(hdg, "%d deg", gps.course.isValid() ? (int)gps.course.deg() : 0); spr.print(hdg);
        if (gps.altitude.isValid()) {
            drawCard(176, 102, 60, 28);
            spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(180, 106); spr.print("ALT");
            spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setCursor(180, 116);
            char alt[8]; sprintf(alt, "%dm", (int)gps.altitude.meters()); spr.print(alt);
        }
    } else {
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 76);
        spr.print("RAW DATA: "); spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(chars); spr.print(" bytes");
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setCursor(4, 88);
        spr.print("VALID NMEA: "); spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(passed);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setCursor(4, 100);
        spr.print("CHECKSUM FAILS: "); spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(failed);
        if (chars < 10) { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.setCursor(4, 116); spr.print("NO DATA: CHECK TX/RX PINS"); } 
        else if (passed == 0) { spr.setTextColor(WARN_COLOR, BG_COLOR); spr.setCursor(4, 116); spr.print("WAITING FOR SATELLITE LOCK"); }
    }
    draw_toast_spr();
}

void draw_chart_screen() {
    spr.fillSprite(BG_COLOR);
    draw_header_spr(6);
    int max_val = 1;
    for (int i = 0; i < CHART_BARS; i++) if (activity_history[i] > max_val) max_val = activity_history[i];
    int chart_top = 28, chart_h = DISP_H - chart_top - 16, bar_w = DISP_W / CHART_BARS;
    
    spr.drawRect(0, chart_top, DISP_W, chart_h, DIM2_COLOR);
    for (int g = 1; g < 4; g++) {
        int gy = chart_top + chart_h - (g * chart_h / 4);
        spr.drawLine(1, gy, DISP_W-1, gy, GRID_COLOR);
    }
    for (int v = 1; v <= 5; v++) {
        int vx = v * (DISP_W / 5);
        spr.drawLine(vx, chart_top+1, vx, chart_top + chart_h - 1, GRID_COLOR);
    }
    
    int prev_x = -1, prev_y = -1;
    for (int i = 0; i < CHART_BARS; i++) {
        int val_y = chart_top + chart_h - ((activity_history[i] * chart_h) / max_val);
        int val_x = i * bar_w + (bar_w / 2);
        if (prev_x != -1) {
            spr.drawLine(prev_x, prev_y, val_x, val_y, TOAST_COLOR);
            spr.drawLine(prev_x, prev_y + 1, val_x, val_y + 1, TOAST_COLOR);
        }
        spr.fillCircle(val_x, val_y, 2, HEADER_COLOR);
        prev_x = val_x; prev_y = val_y;
    }
    
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1);
    spr.setCursor(4, DISP_H - 12); spr.print("25s ago");
    spr.setCursor(DISP_W - 30, DISP_H - 12); spr.print("now");
    spr.setCursor(DISP_W / 2 - 20, DISP_H - 12); spr.print("MAX: "); 
    spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(max_val);
}

void draw_proximity_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int rssi=last_cap_rssi; String cap_type=last_cap_type; int conf=last_cap_confidence;
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR);
    draw_header_spr(7);
    if (cap_type == "None") {
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2); spr.setCursor(20, 65); spr.print("No detections yet");
    } else {
        int pct = constrain(map(rssi, -100, -30, 0, 100), 0, 100);
        uint16_t ccol = confidence_color(conf);
        drawCard(4, 22, 110, 48);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(10, 26); spr.print("SIGNAL STRENGTH");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(3); spr.setCursor(10, 36); spr.print(rssi);
        spr.setTextSize(1); spr.print("dBm");
        drawCard(120, 22, 116, 48);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(126, 26); spr.print("CONFIDENCE");
        spr.setTextColor(ccol, CARD_COLOR); spr.setTextSize(3); spr.setCursor(126, 36); spr.print(conf);
        spr.setTextSize(1); spr.print("%");
        
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 76); spr.print("TRK: ");
        spr.setTextColor(TEXT_COLOR, BG_COLOR); 
        String dn = (last_cap_name != "" && last_cap_name != "Hidden" && last_cap_name != "Unknown") ? last_cap_name : short_mac(last_cap_mac);
        if (dn.length() > 18) dn = dn.substring(0, 18);
        spr.print(dn);

        int bar_x = 4, bar_y = 88, bar_w = DISP_W - 8;
        spr.drawRect(bar_x, bar_y, bar_w, 14, GRID_COLOR);
        int bfill = (pct * (bar_w - 2)) / 100;
        if (bfill > 0) {
            uint16_t pcol = pct > 75 ? ALERT_COLOR : pct > 50 ? WARN_COLOR : pct > 25 ? TOAST_COLOR : TEAL_COLOR;
            spr.fillRect(bar_x + 1, bar_y + 1, bfill, 12, pcol);
        }
        spr.setTextSize(2); spr.setCursor(4, 106);
        if      (pct > 75) { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print(">> VERY CLOSE <<"); }
        else if (pct > 50) { spr.setTextColor(WARN_COLOR,  BG_COLOR); spr.print("> NEARBY <"); }
        else if (pct > 25) { spr.setTextColor(TEAL_COLOR,  BG_COLOR); spr.print("Moderate range"); }
        else               { spr.setTextColor(DIM_COLOR,   BG_COLOR); spr.print("Weak / distant"); }
    }
    draw_toast_spr();
}

void draw_device_info_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    unsigned long ls=lifetime_seconds; long lt=lifetime_flock_total;
    long sr=session_raven; long sw=session_flock_wifi; long sb=session_flock_ble;
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR);
    draw_header_spr(8);
    drawCard(4, 22, DISP_W - 8, 20);
    spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(10, 27); spr.print("FLOCK DETECTOR v6.5 [ADV]");
    drawCard(4,  46, 72, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 50); spr.print("SESSION");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 60);
    spr.print(format_time((millis() - session_start_time) / 1000));
    drawCard(82,  46, 72, 38);
    spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 50); spr.print("LIFETIME");
    spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(86, 60); spr.print(format_time(ls));
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
    spr.drawLine(0, 130, DISP_W, 130, GRID_COLOR);
    spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 133);
    if (sd_available) { spr.print("SD: "); spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print("PCAP LOGGING ON"); }
    else { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("NO SD CARD"); }
    draw_toast_spr();
}

void draw_locator_screen() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active=locator_active, has_est=locator_has_estimate;
    String target_mac=locator_target_mac, target_name=locator_target_name;
    int samples=locator_sample_count, peak=locator_peak_rssi;
    float dist=locator_est_distance, brng=locator_bearing, conf_r=locator_confidence_radius;
    xSemaphoreGive(dataMutex);
    spr.fillSprite(BG_COLOR); draw_header_spr(9);

    if (!active) {
        if (last_cap_type == "None") {
            spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setTextSize(2);
            spr.setCursor(4, 40); spr.print("No target device."); spr.setCursor(4, 64); spr.print("Detect first,"); spr.setCursor(4, 88); spr.print("then press 'l'");
        } else {
            drawCard(4, 22, DISP_W - 8, 20); spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 27); spr.print("TARGET");
            drawCard(4, 46, DISP_W - 8, 24); spr.setTextColor(TEXT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 54);
            String dn = (last_cap_name != "" && last_cap_name != "Hidden" && last_cap_name != "Unknown") ? last_cap_name : last_cap_mac;
            if (dn.length() > 36) dn = dn.substring(0, 36); spr.print(dn);
            spr.setTextColor(TEAL_COLOR, BG_COLOR); spr.setTextSize(1); 
            spr.setCursor(4, 76); spr.print("Press 'l' to start locating");
            spr.setTextColor(PURPLE_COLOR, BG_COLOR);
            spr.setCursor(4, 90); spr.print("Press 't' to cycle targets");
        }
    } else if (!has_est) {
        drawCard(4, 22, DISP_W - 8, 20); spr.setTextColor(TOAST_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(8, 27); spr.print("TRACKING...");
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setTextSize(2); spr.setCursor(4, 50);
        String tname = target_name;
        if (tname == "" || tname == "Hidden" || tname == "Unknown") tname = short_mac(target_mac);
        else if (tname.length() > 18) tname = tname.substring(0, 18); spr.print(tname);
        spr.setTextColor(ACCENT_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(4, 78); spr.print("Collecting samples:");
        spr.drawRect(4, 88, DISP_W - 8, 12, GRID_COLOR);
        int sfill = min(samples, LOC_MIN_SAMPLES_EST) * (DISP_W - 10) / LOC_MIN_SAMPLES_EST;
        if (sfill > 0) spr.fillRect(5, 89, sfill, 10, TEAL_COLOR);
        spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.setCursor(4, 106); spr.print(samples); spr.print("/"); spr.print(LOC_MIN_SAMPLES_EST); spr.print("  Peak: "); spr.print(peak); spr.print("dBm");
        spr.setTextColor(DIM_COLOR, BG_COLOR); spr.setCursor(4, 120); spr.print("Drive around area to collect...");
    } else {
        int arrow_cx = 55, arrow_cy = 82; float rel_bearing = brng;
        if (gps.course.isValid() && gps.speed.isValid() && gps.speed.mph() > 2.0) { rel_bearing = brng - gps.course.deg(); if (rel_bearing < 0) rel_bearing += 360.0; }
        spr.drawCircle(arrow_cx, arrow_cy, LOC_ARROW_RADIUS + 3, GRID_COLOR); spr.drawCircle(arrow_cx, arrow_cy, LOC_ARROW_RADIUS + 1, CARD_BORDER);
        draw_arrow(arrow_cx, arrow_cy, LOC_ARROW_RADIUS, rel_bearing);
        float na = radians(-90.0); int nx = arrow_cx + (int)((LOC_ARROW_RADIUS + 8) * cos(na)); int ny = arrow_cy + (int)((LOC_ARROW_RADIUS + 8) * sin(na));
        spr.setTextColor(HEADER_COLOR, BG_COLOR); spr.setTextSize(1); spr.setCursor(nx - 3, ny - 4); spr.print("N");
        int rx = 118; spr.drawLine(rx - 2, 20, rx - 2, DISP_H, GRID_COLOR);
        String tname = target_name; if (tname == "" || tname == "Hidden" || tname == "Unknown") tname = short_mac(target_mac);
        else if (tname.length() > 15) tname = tname.substring(0, 15);
        spr.setCursor(rx + 2, 22); spr.setTextColor(TEXT_COLOR, BG_COLOR); spr.print(tname);
        
        drawCard(rx + 2, 34, 116, 28);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(rx + 6, 38); spr.print("DISTANCE");
        spr.setTextSize(2); spr.setCursor(rx + 6, 48);
        
        if (dist < 20) spr.setTextColor(ALERT_COLOR, CARD_COLOR);
        else if (dist < 60) spr.setTextColor(TOAST_COLOR, CARD_COLOR);
        else spr.setTextColor(TEXT_COLOR, CARD_COLOR);
        
        if (dist < 100) spr.print(String(dist, 0) + "m"); else spr.print(String(dist / 1000.0, 2) + "km");
        
        drawCard(rx + 2, 66, 116, 28);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(rx + 6, 70); spr.print("BEARING");
        spr.setTextColor(HEADER_COLOR, CARD_COLOR); spr.setTextSize(2); spr.setCursor(rx + 6, 80);
        const char* cardinals[] = {"N","NE","E","SE","S","SW","W","NW"}; int card_idx = ((int)(brng + 22.5) / 45) % 8;
        spr.print(String((int)brng) + " "); spr.print(cardinals[card_idx]);
        
        drawCard(rx + 2, 98, 116, 22);
        spr.setTextColor(ACCENT_COLOR, CARD_COLOR); spr.setTextSize(1); spr.setCursor(rx + 6, 102); spr.print("ACCURACY +/-");
        spr.setTextColor(TEAL_COLOR, CARD_COLOR); spr.setCursor(rx + 6, 112); spr.print(String(conf_r, 0) + "m");
        spr.setTextSize(1); spr.setCursor(4, DISP_H - 10);
        if      (dist < 15 && conf_r < 25) { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("** LOOK UP! **"); }
        else if (dist < 30)  { spr.setTextColor(ALERT_COLOR, BG_COLOR); spr.print("Very close"); }
        else if (dist < 75)  { spr.setTextColor(TOAST_COLOR, BG_COLOR); spr.print("Closing in..."); }
        else                 { spr.setTextColor(TEAL_COLOR,  BG_COLOR); spr.print("Keep going..."); }
    }
    if (active) {
        spr.fillRect(0, DISP_H - 14, DISP_W, 14, HILIGHT_BG); spr.setTextColor(TEAL_COLOR, HILIGHT_BG); spr.setTextSize(1); spr.setCursor(4, DISP_H - 10);
        spr.print(String(samples) + " pts | press 'l' to stop");
    }
}

// ============================================================================
// MAIN UI CONTROLLER
// ============================================================================
void draw_current_screen() {
    switch (current_screen) {
        case 0: draw_scanner_screen();         break;
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

void transition_screen(int new_screen, int dir) {
    if (stealth_mode) { current_screen = new_screen; return; }
    current_screen = new_screen;
    draw_current_screen(); 
    if (dir > 0) { for (int x = DISP_W; x >= 0; x -= 30) { spr.pushSprite(x, 0); delay(5); } } 
    else { for (int x = -DISP_W; x <= 0; x += 30) { spr.pushSprite(x, 0); delay(5); } }
    spr.pushSprite(0, 0); last_stats_update = millis();
}

void play_escalated_alarm(int confidence) {
    if (stealth_mode || is_muted) return;
    if (confidence >= CONFIDENCE_CERTAIN) {
        // High-pitched "Jackpot" Alert for 100% hits
        M5Cardputer.Display.invertDisplay(true); beep(987, 80); delay(80);
        M5Cardputer.Display.invertDisplay(false); beep(1318, 80); delay(80);
        M5Cardputer.Display.invertDisplay(true); beep(1568, 150); delay(150);
        M5Cardputer.Display.invertDisplay(false);
    } else if (confidence >= CONFIDENCE_HIGH) {
        for (int i = 0; i < 3; i++) {
            M5Cardputer.Display.invertDisplay(true); beep(DETECT_FREQ_HIGH, DETECT_BEEP_DURATION); M5Cardputer.Display.invertDisplay(false); if (i < 2) delay(50);
        }
    } else {
        M5Cardputer.Display.invertDisplay(true); beep(DETECT_FREQ, DETECT_BEEP_DURATION); M5Cardputer.Display.invertDisplay(false);
    }
}

// ============================================================================
// SYSTEM SETUP
// ============================================================================
// ============================================================================
// SYSTEM SETUP
// ============================================================================
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    
    // 1. KILL THE HARDWARE POWER-ON POP INSTANTLY
    M5Cardputer.Speaker.setVolume(0); 
    
    M5Cardputer.Display.setRotation(1);
    
    apply_color_palette();
    
    spr.setColorDepth(16);
    spr.createSprite(DISP_W, DISP_H);

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

    if (!LittleFS.begin(true)) { littlefs_available = false; } 
    else { littlefs_available = true; load_session_from_flash(); }

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
    WiFi.disconnect(); delay(100); esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t filt; filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt); esp_wifi_set_promiscuous(true); 
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler); 
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE); delay(100);

    NimBLEDevice::init(""); NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    pBLEScan = NimBLEDevice::getScan(); pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), false);
    pBLEScan->setActiveScan(true); pBLEScan->setInterval(97); pBLEScan->setWindow(97);

    // 2. FUN, GENTLE BOOT CHIME (Nintendo "Coin" Ping)
    if (!is_muted) {
        // Cap boot volume at 60 (out of 255) so it never blasts you on startup
        int boot_vol = (current_volume > 60) ? 60 : current_volume;
        M5Cardputer.Speaker.setVolume(boot_vol);
        
        // B5 -> E6 Happy Ping
        M5Cardputer.Speaker.tone(987, 80);   delay(100); 
        M5Cardputer.Speaker.tone(1319, 250); delay(300);
    }
    
    // 3. Restore master volume for actual threat alarms
    M5Cardputer.Speaker.setVolume(current_volume);

    last_channel_hop = millis(); last_sd_flush = millis(); last_persist_save = millis();
    xTaskCreatePinnedToCore(ScannerLoopTask, "ScannerTask", 8192, NULL, 1, &ScannerTaskHandle, 0);
    xTaskCreatePinnedToCore(GPSLoopTask, "GPSTask", 4096, NULL, 1, &GPSTaskHandle, 0);

    draw_current_screen(); spr.pushSprite(0,0);
}
// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
    M5Cardputer.update(); yield(); update_battery_metrics();
    int32_t loop_mv = get_median_voltage();

    if (millis() - last_chart_update >= 1000) {
        last_chart_update = millis(); xSemaphoreTake(dataMutex, portMAX_DELAY);
        long current_total = session_wifi + session_ble; int new_dets = current_total - last_total_dets;
        last_total_dets = current_total; xSemaphoreGive(dataMutex);
        for (int i = 0; i < CHART_BARS - 1; i++) activity_history[i] = activity_history[i + 1];
        activity_history[CHART_BARS - 1] = new_dets;
    }

    if (trigger_alarm_confidence >= 50) {
        int conf = trigger_alarm_confidence; trigger_alarm_confidence = 0; play_escalated_alarm(conf);
    } else { trigger_alarm_confidence = 0; }

    if (M5Cardputer.BtnA.wasClicked() && !stealth_mode) {
        int next_screen = current_screen + 1; if (next_screen >= NUM_SCREENS) next_screen = 0; transition_screen(next_screen, 1);
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        for (auto c : status.word) {
            
            if (c == 'n') { 
                night_mode = !night_mode; apply_color_palette();
                draw_current_screen(); spr.pushSprite(0,0);
            } 
            else if (c == 'q' || c == 'm') { 
                is_muted = !is_muted; draw_current_screen(); spr.pushSprite(0,0);
                if (!is_muted) beep(600, 50); 
            }
            else if (c == '-' || c == '_') { 
                current_volume -= 15; if (current_volume < 0) current_volume = 0;
                M5Cardputer.Speaker.setVolume(current_volume); beep(400, 50);
            }
            else if (c == '=' || c == '+') { 
                current_volume += 15; if (current_volume > 255) current_volume = 255;
                M5Cardputer.Speaker.setVolume(current_volume); beep(800, 50);
            }
            else if (c == 't') { // MULTI-TARGET SELECTOR
                if (!stealth_mode && capture_history_count > 0) {
                    static int target_select_idx = -1;
                    target_select_idx = (target_select_idx + 1) % capture_history_count;
                    xSemaphoreTake(dataMutex, portMAX_DELAY);
                    String t_mac = capture_history[target_select_idx].mac;
                    String t_name = capture_history[target_select_idx].name;
                    int t_conf = capture_history[target_select_idx].confidence;
                    xSemaphoreGive(dataMutex);
                    
                    locator_start(t_mac, t_name);
                    trigger_toast("TARGET", t_name, t_conf);
                    transition_screen(9, 1);
                } else if (!stealth_mode) {
                    trigger_toast("TARGET", "No history", 0);
                }
            }
            else if (c == '/' || c == '>') { if (!stealth_mode) { int next = current_screen + 1; if(next >= NUM_SCREENS) next = 0; transition_screen(next, 1); } } 
            else if (c == ',' || c == '<') { if (!stealth_mode) { int prev = current_screen - 1; if(prev < 0) prev = NUM_SCREENS - 1; transition_screen(prev, -1); } } 
            else if (c >= '1' && c <= '9') { if (!stealth_mode) { int target = c - '1'; if (target < NUM_SCREENS) transition_screen(target, (target >= current_screen) ? 1 : -1); } } 
            else if (c == '0') { if (!stealth_mode) transition_screen(9, 1); } 
            else if (c == 's') { stealth_mode = !stealth_mode; if (stealth_mode) { M5Cardputer.Display.setBrightness(0); } else { M5Cardputer.Display.setBrightness(200); draw_current_screen(); spr.pushSprite(0,0); } } 
            else if (c == 'l') { if (locator_active) { locator_stop(); } else if (last_cap_type != "None") { locator_start(last_cap_mac, last_cap_name); transition_screen(9, 1); } } 
            else if (c == '\n' || c == '\r') { if (!stealth_mode) { int next = current_screen + 1; if(next >= NUM_SCREENS) next = 0; transition_screen(next, 1); } }
        }
        if (status.del && !stealth_mode) { int prev = current_screen - 1; if(prev < 0) prev = NUM_SCREENS - 1; transition_screen(prev, -1); }
    }

    if (millis() - last_time_save >= 1000) { xSemaphoreTake(dataMutex, portMAX_DELAY); lifetime_seconds++; xSemaphoreGive(dataMutex); last_time_save = millis(); }
    if (millis() - last_persist_save >= PERSIST_INTERVAL_MS) save_session_to_flash();
    rssi_track_expire();

    if (millis() - last_sd_flush_check >= 500) {
        last_sd_flush_check = millis(); bool should_flush = false; xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (sd_write_count >= MAX_LOG_BUFFER || (millis() - last_sd_flush > SD_FLUSH_INTERVAL && sd_write_count > 0)) should_flush = true;
        xSemaphoreGive(dataMutex); if (should_flush) flush_sd_buffer();
    }

    if (locator_active && locator_has_estimate && gps.location.isValid() && gps.location.age() < 2000) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        locator_est_distance = haversine_m(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
        locator_bearing = bearing_to(gps.location.lat(), gps.location.lng(), locator_est_lat, locator_est_lng);
        xSemaphoreGive(dataMutex);
    }

    if (!stealth_mode) {
        static unsigned long last_fast_anim = 0; static unsigned long last_slow_ui = 0; unsigned long now = millis();
        static uint8_t last_loop_led = 255;
        if (is_device_charging(loop_mv)) {
            float breathe = (exp(sin(now / 1000.0f * PI)) - 0.36787944f) / 2.35040238f; uint8_t led_val = (uint8_t)(breathe * 60.0f); 
            if (led_val != last_loop_led) { set_cardputer_led(0, led_val, 0); last_loop_led = led_val; }
        } else { if (last_loop_led != 0) { set_cardputer_led(0, 0, 0); last_loop_led = 0; } }
        
        if (current_screen == 0) { if (now - last_fast_anim >= 15) { draw_current_screen(); spr.pushSprite(0, 0); last_fast_anim = now; } } 
        else { if (now - last_slow_ui >= 100) { draw_current_screen(); spr.pushSprite(0, 0); last_slow_ui = now; } }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
}