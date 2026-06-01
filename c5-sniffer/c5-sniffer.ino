// ════════════════════════════════════════════════════════════════════════════
// Plume C5 — 5 GHz surveillance sniffer (co-processor for Plume / Cardputer ADV)
//
// Flash this to a Waveshare ESP32-C5-WIFI6-KIT (or any ESP32-C5 dev board). It
// is the 5 GHz "radio ear" the Cardputer's ESP32-S3 can't be: it sniffs 5 GHz
// Wi-Fi management frames, runs the SAME Flock signatures Plume uses on 2.4 GHz,
// and reports each hit as one line over UART. Plume reads those lines on its
// Grove port and folds them into its normal detection pipeline.
//
// Passive only: receive-only promiscuous mode. It never transmits, associates,
// or interacts with any device. No screen, no SD, no BLE, no GPS (GPS lives on
// the Cardputer's hat).
//
// ── Wiring (C5 -> Cardputer Grove) ─────────────────────────────────────────
//   C5 TXD  -> Grove yellow (G2 / S3 RX)        <- the detection stream
//   C5 RXD  <- Grove white  (G1 / S3 TX)        <- optional, unused for now
//   C5 5V   <- Grove red    (or power the C5 from its own USB-C / battery)
//   C5 GND  <- Grove black
//
// ── Build settings (Arduino IDE) ────────────────────────────────────────────
//   Board:            "ESP32C5 Dev Module"  (needs Arduino-ESP32 core >= 3.1.x;
//                     esp_wifi_set_band_mode / WIFI_BAND_MODE_5G_ONLY require it)
//   USB CDC On Boot:  Enabled   <-- REQUIRED. Puts the debug console on USB-C so
//                     UART0 (the TXD/RXD pads) is free for the Cardputer link.
//   Upload/flash over the USB-C port (native USB-Serial/JTAG, the COM5 you saw).
//
// ── Wire protocol (must match Plume's c5_link.h) ──  PROTOCOL_VERSION 1
//   D|mac|name|rssi|ch|conf|methods     a detection
//   H|plume-c5|<ver>                    hello / heartbeat (lights Plume's 5G badge)
//   example:  D|aa:bb:cc:dd:ee:ff|flock-1a2b|-67|149|85|ssid_fmt
// ════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <string.h>
#include <ctype.h>

// ── Protocol / link ─────────────────────────────────────────────────────────
#define PROTOCOL_VERSION   1
#define LINK_BAUD          115200       // must match Plume's C5_BAUD
#define HEARTBEAT_MS       3000         // H|... cadence (Plume's badge times out at 8 s)
#define LinkSerial         Serial0      // UART0 = the C5's TXD/RXD pads -> Cardputer
#define DbgSerial          Serial       // USB CDC on the USB-C port (debug only)

// ── Scoring — MUST MATCH Plume (FlockDetection_Cardputer_ADV.ino) ────────────
#define SCORE_DEFINITIVE   100
#define SCORE_STRONG       60
#define SCORE_WEAK         25
#define SCORE_BONUS_RSSI   10
#define ALARM_THRESHOLD    75           // = CONFIDENCE_ALARM_THRESHOLD in Plume
#define IGNORE_WEAK_RSSI   -80

// ── 5 GHz channels (non-DFS only) ────────────────────────────────────────────
// UNII-1 (36-48) + UNII-3 (149-165). DFS channels 52-144 are intentionally
// omitted: the C5 cannot detect radar, so it must not dwell on DFS channels.
static const uint8_t kChannels[] = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };
static const int     kNumChannels   = sizeof(kChannels) / sizeof(kChannels[0]);
#define CHANNEL_DWELL_MS   300          // per-channel listen time before hopping

// ── Signatures — MUST MATCH Plume ────────────────────────────────────────────
static const char* kSsidPatterns[] = {
    "FS Ext Battery", "Penguin", "Pigvision", "FlockOS",
    "flocksafety", "OFS_IoT", "PFS_"
};
static const int kNumSsidPatterns = sizeof(kSsidPatterns) / sizeof(kSsidPatterns[0]);

static const char* kMacTier1[] = {           // high-confidence Flock OUIs
    "b4:1e:52", "e4:aa:ea", "00:09:01",
    "4c:6e:44", "d8:a0:d8", "a0:b7:65", "f0:82:c0", "b4:e3:f9", "04:0d:84"
};
static const int kNumMacTier1 = sizeof(kMacTier1) / sizeof(kMacTier1[0]);

static const char* kMacTier2[] = {           // component-vendor OUIs (weaker)
    "74:4c:a1", "94:34:69", "38:5b:44", "94:08:53", "1c:34:f1", "a4:cf:12",
    "3c:91:80", "80:30:49", "14:5a:fc", "9c:2f:9d", "c8:c9:a3", "70:c9:4e",
    "24:b2:b9", "00:f4:8d", "08:3a:88", "d8:f3:bc", "ec:1b:bd", "58:8e:81",
    "90:35:ea", "b8:35:32", "c0:35:32", "f4:6a:dd", "f8:a2:d6", "e8:d0:fc",
    "e0:4f:43", "b8:1e:a4", "70:08:94", "3c:71:bf", "58:00:e3", "5c:93:a2",
    "64:6e:69", "48:27:ea", "82:6b:f2", "d0:39:57", "e0:0a:f6", "e8:2a:44",
    "30:d1:6b", "b8:ee:65", "a4:db:30", "40:f0:2f", "30:52:cb", "94:97:4f"
};
static const int kNumMacTier2 = sizeof(kMacTier2) / sizeof(kMacTier2[0]);

// ── 802.11 management frame header (24 bytes) ────────────────────────────────
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];   // receiver / destination
    uint8_t  addr2[6];   // transmitter
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} __attribute__((packed)) mac_hdr_t;

// ── Raw-event ring buffer ────────────────────────────────────────────────────
// The promiscuous callback runs in the Wi-Fi driver task; keep it fast. It only
// copies metadata + a frame-body snapshot here. All parsing/scoring/sending is
// deferred to loop() (same pattern Plume uses with process_wifi_event_queue).
struct RawEvent {
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    int8_t   rssi;
    uint8_t  channel;
    bool     is_beacon;
    bool     is_probe_req;
    uint8_t  body[128];     // frame body (fixed params + tagged params)
    uint16_t body_len;
    volatile bool ready;
};
#define EVENT_QUEUE_SIZE 16
static RawEvent          g_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t g_write_idx = 0;
static uint32_t          g_read_idx  = 0;

// ── Dedup table — suppress repeats of the same device within a cooldown ──────
#define DEDUP_SIZE        48
#define DEDUP_COOLDOWN_MS 30000     // re-report a given MAC at most every 30 s
struct DedupEntry { uint8_t mac[6]; uint32_t last_ms; bool used; };
static DedupEntry g_dedup[DEDUP_SIZE];

// ───────────────────────────── matching (mirrors Plume) ─────────────────────
static int mac_prefix_tier(const uint8_t* mac) {   // 1=tier1, 2=tier2, 0=none
    char s[9];
    snprintf(s, sizeof(s), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
    for (int i = 0; i < kNumMacTier1; i++) if (strncmp(s, kMacTier1[i], 8) == 0) return 1;
    for (int i = 0; i < kNumMacTier2; i++) if (strncmp(s, kMacTier2[i], 8) == 0) return 2;
    return 0;
}
static bool ssid_pattern_match(const char* ssid) {
    if (!ssid || ssid[0] == '\0') return false;
    for (int i = 0; i < kNumSsidPatterns; i++) if (strcasestr(ssid, kSsidPatterns[i])) return true;
    return false;
}
static bool is_flock_ssid_format(const char* ssid) {
    if (!ssid) return false;
    if (strncmp(ssid, "Flock-", 6) != 0 && strncmp(ssid, "flock-", 6) != 0) return false;
    const char* sfx = ssid + 6;
    int n = (int)strlen(sfx);
    if (n < 2 || n > 12) return false;
    for (int i = 0; i < n; i++) if (!isxdigit((unsigned char)sfx[i])) return false;
    return true;
}

// Pull the SSID out of a mgmt frame body. Beacons carry 12 fixed bytes
// (timestamp+interval+caps) before the tagged params; probe requests carry
// none. SSID is element id 0.
static void extract_ssid(const uint8_t* body, uint16_t len, bool is_beacon,
                         char* out, size_t out_sz) {
    out[0] = '\0';
    uint16_t off = is_beacon ? 12 : 0;
    while (off + 2 <= len) {
        uint8_t id = body[off];
        uint8_t ln = body[off + 1];
        if (off + 2 + ln > len) break;
        if (id == 0) {                               // SSID element
            uint8_t copy = (ln < out_sz - 1) ? ln : (uint8_t)(out_sz - 1);
            memcpy(out, &body[off + 2], copy);
            out[copy] = '\0';
            return;
        }
        off += 2 + ln;
    }
}

// ───────────────────────────── sniffer callback ─────────────────────────────
static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < sizeof(mac_hdr_t)) return;

    const mac_hdr_t* hdr = (const mac_hdr_t*)pkt->payload;
    uint8_t ftype = (hdr->frame_ctrl & 0x0C) >> 2;
    uint8_t fsub  = (hdr->frame_ctrl & 0xF0) >> 4;
    if (ftype != 0) return;                          // management frames only
    bool is_beacon    = (fsub == 8);
    bool is_probe_req = (fsub == 4);
    if (!is_beacon && !is_probe_req) return;

    uint32_t idx = g_write_idx % EVENT_QUEUE_SIZE;
    if (g_queue[idx].ready) return;                  // queue full — drop

    RawEvent* e = &g_queue[idx];
    memcpy(e->addr1, hdr->addr1, 6);
    memcpy(e->addr2, hdr->addr2, 6);
    e->rssi         = pkt->rx_ctrl.rssi;
    e->channel      = pkt->rx_ctrl.channel;
    e->is_beacon    = is_beacon;
    e->is_probe_req = is_probe_req;

    uint16_t body_len = (pkt->rx_ctrl.sig_len > sizeof(mac_hdr_t))
                      ? (uint16_t)(pkt->rx_ctrl.sig_len - sizeof(mac_hdr_t)) : 0;
    if (body_len > sizeof(e->body)) body_len = sizeof(e->body);
    memcpy(e->body, pkt->payload + sizeof(mac_hdr_t), body_len);
    e->body_len = body_len;

    e->ready = true;
    g_write_idx++;
}

// ───────────────────────────── scoring (mirrors Plume) ──────────────────────
// Fills methods[] and returns confidence 0-100. report_mac receives the MAC to
// report (normally the transmitter, but addr1 for a sleeping-device hit).
static int score_event(const RawEvent* e, const char* ssid,
                       char* methods, size_t msz, uint8_t report_mac[6]) {
    methods[0] = '\0';
    memcpy(report_mac, e->addr2, 6);

    bool ssid_present = (ssid[0] != '\0');
    bool is_random    = (e->addr2[0] & 0x02) != 0;       // locally-administered MAC
    int  mac_score    = is_random ? 0 : mac_prefix_tier(e->addr2);
    bool ssid_generic = ssid_present && ssid_pattern_match(ssid);
    bool ssid_fmt     = ssid_present && is_flock_ssid_format(ssid);
    int  conf         = 0;

    // CVE-2025-59409 hardcoded probe SSID — definitive, Flock-only.
    if (e->is_probe_req && ssid_present && strcmp(ssid, "test_flck") == 0) {
        conf = SCORE_DEFINITIVE; strlcat(methods, "test_flck_cve ", msz);
    }

    if (ssid_fmt) {
        conf = SCORE_DEFINITIVE; strlcat(methods, "ssid_fmt ", msz);
    } else if (mac_score == 1) {
        conf = SCORE_STRONG; strlcat(methods, "mac_t1 ", msz);
        if (ssid_generic) { conf = SCORE_DEFINITIVE; strlcat(methods, "ssid ", msz); }
    } else {
        if (mac_score == 2) { conf += SCORE_WEAK; strlcat(methods, "mac_t2 ", msz); }
        if (ssid_generic)   { conf += SCORE_WEAK; strlcat(methods, "ssid ", msz); }
    }

    // Wildcard probe (empty SSID) from a known OUI.
    if (e->is_probe_req && mac_score > 0 && !ssid_present) {
        if (mac_score == 1) { conf = SCORE_DEFINITIVE; strlcat(methods, "wildcard_probe ", msz); }
        else                { conf = SCORE_STRONG;     strlcat(methods, "wildcard_probe_t2 ", msz); }
    }

    if (conf > 0 && e->rssi > -50) conf += SCORE_BONUS_RSSI;

    // Sleeping-device addr1 (receiver) OUI check.
    bool a1_mc  = (e->addr1[0] & 0x01);
    bool a1_rnd = (e->addr1[0] & 0x02);
    bool a1_bc  = (e->addr1[0] == 0xFF && e->addr1[1] == 0xFF);
    if (!a1_mc && !a1_rnd && !a1_bc) {
        int a1 = mac_prefix_tier(e->addr1);
        if (a1 > 0 && mac_score == 0) {
            if (a1 == 1) { conf = SCORE_STRONG; strlcat(methods, "addr1_t1 ", msz); }
            else         { conf += SCORE_WEAK;  strlcat(methods, "addr1_t2 ", msz); }
            memcpy(report_mac, e->addr1, 6);             // key off the device MAC
        }
    }

    if (conf > 100) conf = 100;
    int ml = (int)strlen(methods);
    if (ml > 0 && methods[ml - 1] == ' ') methods[ml - 1] = '\0';
    return conf;
}

// ───────────────────────────── dedup ────────────────────────────────────────
// True if this MAC should be reported now (first sight, or cooldown elapsed).
static bool should_report(const uint8_t* mac) {
    uint32_t now = millis();
    int  free_slot = -1, oldest = 0;
    uint32_t oldest_ms = 0xFFFFFFFFUL;
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (g_dedup[i].used && memcmp(g_dedup[i].mac, mac, 6) == 0) {
            if (now - g_dedup[i].last_ms < DEDUP_COOLDOWN_MS) return false;
            g_dedup[i].last_ms = now;
            return true;
        }
        if (!g_dedup[i].used && free_slot < 0) free_slot = i;
        if (g_dedup[i].used && g_dedup[i].last_ms < oldest_ms) {
            oldest_ms = g_dedup[i].last_ms; oldest = i;
        }
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;    // evict oldest if full
    memcpy(g_dedup[slot].mac, mac, 6);
    g_dedup[slot].last_ms = now;
    g_dedup[slot].used    = true;
    return true;
}

// ───────────────────────────── reporting ────────────────────────────────────
static void send_detection(const uint8_t* mac, const char* name, int rssi,
                           int ch, int conf, const char* methods) {
    char safe[33];
    strlcpy(safe, (name && name[0]) ? name : "Hidden", sizeof(safe));
    for (char* p = safe; *p; p++)                        // keep the line parseable
        if (*p == '|' || *p == '\n' || *p == '\r') *p = '_';

    char line[200];
    snprintf(line, sizeof(line),
             "D|%02x:%02x:%02x:%02x:%02x:%02x|%s|%d|%d|%d|%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             safe, rssi, ch, conf, methods);
    LinkSerial.println(line);
    DbgSerial.printf("[HIT] %s\n", line);
}

// ───────────────────────────── channel hopping ──────────────────────────────
static int      g_ch_idx   = 0;
static uint32_t g_last_hop = 0;
static void hop_channel() {
    g_ch_idx = (g_ch_idx + 1) % kNumChannels;
    esp_wifi_set_channel(kChannels[g_ch_idx], WIFI_SECOND_CHAN_NONE);
}

// ───────────────────────────── setup / loop ─────────────────────────────────
void setup() {
    DbgSerial.begin(115200);                             // USB-C debug console
    LinkSerial.begin(LINK_BAUD, SERIAL_8N1);             // UART0 -> Cardputer
    delay(300);
    DbgSerial.println("\n[C5] Plume 5GHz sniffer booting...");

    WiFi.mode(WIFI_MODE_STA);                            // brings up the Wi-Fi stack
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Force the single radio onto 5 GHz (the C5 can't do both bands at once).
    esp_err_t berr = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    DbgSerial.printf("[C5] set_band_mode(5G_ONLY): %s\n", esp_err_to_name(berr));

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_cb);
    esp_wifi_set_channel(kChannels[0], WIFI_SECOND_CHAN_NONE);

    DbgSerial.println("[C5] sniffing 5 GHz; reporting over UART0 (TXD).");
}

void loop() {
    uint32_t now = millis();

    if (now - g_last_hop >= CHANNEL_DWELL_MS) { hop_channel(); g_last_hop = now; }

    static uint32_t last_hb = 0;
    if (now - last_hb >= HEARTBEAT_MS) {
        last_hb = now;
        LinkSerial.printf("H|plume-c5|%d\n", PROTOCOL_VERSION);
    }

    // Drain raw events: parse SSID, score, dedup, report.
    while (g_queue[g_read_idx % EVENT_QUEUE_SIZE].ready) {
        RawEvent* e = &g_queue[g_read_idx % EVENT_QUEUE_SIZE];

        if (e->rssi >= IGNORE_WEAK_RSSI) {
            char ssid[33];
            extract_ssid(e->body, e->body_len, e->is_beacon, ssid, sizeof(ssid));

            char    methods[64];
            uint8_t report_mac[6];
            int conf = score_event(e, ssid, methods, sizeof(methods), report_mac);

            if (conf >= ALARM_THRESHOLD && should_report(report_mac)) {
                const char* name = (ssid[0] != '\0') ? ssid : "Hidden";
                send_detection(report_mac, name, e->rssi, e->channel, conf, methods);
            }
        }

        e->ready = false;
        g_read_idx++;
    }
}
