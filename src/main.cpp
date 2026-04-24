// ============================================================================
// FLOCK-YOU (PC-only): WiFi Wildcard Probe Detector
// ============================================================================
// Detection logic (per requirement):
//   - Source MAC OUI matches upstream OUI list
//   - Frame is an 802.11 Management Probe Request
//   - SSID tagged parameter (tag 0) has length == 0 ("Wildcard SSID")
//
// Interaction:
//   - No SoftAP / no on-device dashboard
//   - Emits line-delimited JSON over USB serial for the PC Flask app
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ============================================================================
// CONFIG
// ============================================================================

#define BUZZER_PIN 3
#define MAX_DETECTIONS 200

// Tuning knobs (reflash to change)
// - FY_FIXED_CHANNEL: 0 = hop; 1..11 = lock to channel
// - FY_HOP_DWELL_MS: dwell time per channel when hopping
#define FY_FIXED_CHANNEL 0
#define FY_HOP_DWELL_MS 800

// Buzzer / range timing (same as colonelpanichacks/flock-you loop(): 10s heartbeat cadence, 30s idle -> reset)
#define FY_HEARTBEAT_INTERVAL_MS 10000UL
#define FY_INRANGE_TIMEOUT_MS 30000UL

static const uint8_t FY_HOP_CHANNELS[] = {1,2,3,4,5,6,7,8,9,10,11};
static uint8_t fyHopIdx = 0;
static uint8_t fyHopChannel = (FY_FIXED_CHANNEL >= 1 && FY_FIXED_CHANNEL <= 11) ? (uint8_t)FY_FIXED_CHANNEL : FY_HOP_CHANNELS[0];
static unsigned long fyLastHop = 0;

// ============================================================================
// OUI LIST (authoritative upstream: colonelpanichacks/flock-you commit 6c6930b)
// ============================================================================
// Source: datasets/NitekryDPaul_wifi_ouis.md (30 prefixes, lowercase, colon-separated)

static const uint8_t fyProbeRequestOUIs[][3] = {
    {0x70, 0xc9, 0x4e},
    {0x3c, 0x91, 0x80},
    {0xd8, 0xf3, 0xbc},
    {0x80, 0x30, 0x49},
    {0xb8, 0x35, 0x32},
    {0x14, 0x5a, 0xfc},
    {0x74, 0x4c, 0xa1},
    {0x08, 0x3a, 0x88},
    {0x9c, 0x2f, 0x9d},
    {0xc0, 0x35, 0x32},
    {0x94, 0x08, 0x53},
    {0xe4, 0xaa, 0xea},
    {0xf4, 0x6a, 0xdd},
    {0xf8, 0xa2, 0xd6},
    {0x24, 0xb2, 0xb9},
    {0x00, 0xf4, 0x8d},
    {0xd0, 0x39, 0x57},
    {0xe8, 0xd0, 0xfc},
    {0xe0, 0x4f, 0x43},
    {0xb8, 0x1e, 0xa4},
    {0x70, 0x08, 0x94},
    {0x58, 0x8e, 0x81},
    {0xec, 0x1b, 0xbd},
    {0x3c, 0x71, 0xbf},
    {0x58, 0x00, 0xe3},
    {0x90, 0x35, 0xea},
    {0x5c, 0x93, 0xa2},
    {0x64, 0x6e, 0x69},
    {0x48, 0x27, 0xea},
    {0xa4, 0xcf, 0x12},
};

// ============================================================================
// DETECTION STORAGE
// ============================================================================

struct FYDetection {
    char mac[18];
    int rssi;
    int channel;
    char method[32];
    unsigned long firstSeen;
    unsigned long lastSeen;
    int count;
    unsigned long lastEmit;  // throttle serial emits per MAC
};

static FYDetection fyDet[MAX_DETECTIONS];
static int fyDetCount = 0;
static SemaphoreHandle_t fyMutex = NULL;

static bool fyBuzzerOn = true;
static bool fyTriggered = false;
static bool fyDeviceInRange = false;
static unsigned long fyLastDetTime = 0;
static unsigned long fyLastHB = 0;

// ============================================================================
// PROMISC EVENT QUEUE (keep WiFi RX callback minimal)
// ============================================================================

struct FYProbeHit {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t channel;
    uint32_t tsMs;
};

static QueueHandle_t fyHitQueue = nullptr;

// #region agent log
// Host capture: pipe serial to /repos/flock-you/.cursor/debug-69ade4.log (see README reproduction).
static volatile uint32_t fyAgentQueueFullISR = 0;

static void fyAgentNdjson(const char* hypothesisId, const char* location, const char* message, int d1, int d2, int d3) {
    Serial.printf(
        "FYDBG {\"sessionId\":\"69ade4\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\","
        "\"data\":{\"d1\":%d,\"d2\":%d,\"d3\":%d},\"timestamp\":%lu}\n",
        hypothesisId, location, message, d1, d2, d3, (unsigned long)millis());
}
// #endregion

// ============================================================================
// AUDIO
// ============================================================================

static void fyCaw(int startFreq, int endFreq, int durationMs, int warbleHz) {
    if (!fyBuzzerOn) return;
    int steps = durationMs / 8;
    if (steps < 1) steps = 1;
    float fStep = (float)(endFreq - startFreq) / steps;
    for (int i = 0; i < steps; i++) {
        int f = startFreq + (int)(fStep * i);
        if (warbleHz > 0 && (i % 3 == 0)) f += ((i % 6 < 3) ? warbleHz : -warbleHz);
        if (f < 100) f = 100;
        tone(BUZZER_PIN, f, 10);
        delay(8);
    }
    noTone(BUZZER_PIN);
}

static void fyBootBeep() {
    if (!fyBuzzerOn) return;
    fyCaw(850, 380, 180, 40);
    delay(100);
    fyCaw(780, 350, 150, 50);
    delay(100);
    fyCaw(820, 280, 220, 60);
    delay(80);
    tone(BUZZER_PIN, 600, 25); delay(40);
    tone(BUZZER_PIN, 550, 25); delay(40);
    noTone(BUZZER_PIN);
}

static void fyDetectBeep() {
    if (!fyBuzzerOn) return;
    // Alarm crow: two sharp ascending chirps then a caw (colonelpanichacks/flock-you)
    fyCaw(400, 900, 100, 30);
    delay(60);
    fyCaw(450, 950, 100, 30);
    delay(60);
    fyCaw(900, 350, 200, 50);
}

static void fyHeartbeat() {
    if (!fyBuzzerOn) return;
    // Soft double coo — distant crow (colonelpanichacks/flock-you)
    fyCaw(500, 400, 80, 20);
    delay(120);
    fyCaw(480, 380, 80, 20);
}

// ============================================================================
// HELPERS
// ============================================================================

static bool fyOUIEq(const uint8_t* mac, const uint8_t oui[3]) {
    return mac[0] == oui[0] && mac[1] == oui[1] && mac[2] == oui[2];
}

static bool fyMatchAnyOUI(const uint8_t* mac) {
    for (size_t i = 0; i < sizeof(fyProbeRequestOUIs) / sizeof(fyProbeRequestOUIs[0]); i++) {
        if (fyOUIEq(mac, fyProbeRequestOUIs[i])) return true;
    }
    return false;
}

static void fyFormatMAC(const uint8_t* mac, char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool fyIsProbeRequest(const uint8_t* frame, size_t len) {
    if (len < 24) return false;

    // Frame Control is first 2 bytes, little-endian.
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (fc >> 2) & 0x3;
    uint8_t subtype = (fc >> 4) & 0xF;
    return (type == 0 && subtype == 4);
}

static bool fyParseWildcardSSID(const uint8_t* frame, size_t len, bool& outFoundSSID, uint8_t& outSSIDLen) {
    outFoundSSID = false;
    outSSIDLen = 0;
    if (!fyIsProbeRequest(frame, len)) return false;

    // Tagged parameters begin immediately after 24-byte mgmt header for Probe Request.
    size_t pos = 24;
    while (pos + 2 <= len) {
        uint8_t tag = frame[pos];
        uint8_t tlen = frame[pos + 1];
        pos += 2;
        if (pos + tlen > len) break;
        if (tag == 0) {
            outFoundSSID = true;
            outSSIDLen = tlen;
            return (tlen == 0);
        }
        pos += tlen;
    }
    return false;
}

static int fyAddDetection(const char* mac, int rssi, int channel, const char* method) {
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

    for (int i = 0; i < fyDetCount; i++) {
        if (strcasecmp(fyDet[i].mac, mac) == 0) {
            fyDet[i].count++;
            fyDet[i].lastSeen = millis();
            fyDet[i].rssi = rssi;
            fyDet[i].channel = channel;
            xSemaphoreGive(fyMutex);
            return i;
        }
    }

    if (fyDetCount < MAX_DETECTIONS) {
        FYDetection& d = fyDet[fyDetCount];
        memset(&d, 0, sizeof(d));
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        d.rssi = rssi;
        d.channel = channel;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.firstSeen = millis();
        d.lastSeen = millis();
        d.count = 1;
        d.lastEmit = 0;
        int idx = fyDetCount++;
        xSemaphoreGive(fyMutex);
        return idx;
    }

    xSemaphoreGive(fyMutex);
    return -1;
}

static void fyMaybeEmitDetection(int idx) {
    if (idx < 0) return;
    unsigned long now = millis();
    const unsigned long minGapMs = 1500;

    FYDetection snap;
    memset(&snap, 0, sizeof(snap));

    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (idx >= fyDetCount) { xSemaphoreGive(fyMutex); return; }

    if (fyDet[idx].lastEmit != 0 && (now - fyDet[idx].lastEmit) < minGapMs) {
        xSemaphoreGive(fyMutex);
        return;
    }
    fyDet[idx].lastEmit = now;
    snap = fyDet[idx];
    xSemaphoreGive(fyMutex);

    char jsonBuf[256];
    int n = snprintf(jsonBuf, sizeof(jsonBuf),
        "{\"event\":\"detection\",\"detection_method\":\"%s\","
        "\"protocol\":\"wifi\",\"mac_address\":\"%s\",\"ssid\":\"\","
        "\"rssi\":%d,\"channel\":%d}\n",
        snap.method[0] ? snap.method : "probe_request",
        snap.mac, snap.rssi, snap.channel);
    if (n > 0) {
        Serial.write((const uint8_t*)jsonBuf, (size_t)n);
    }
}

// ============================================================================
// WIFI PROMISCUOUS
// ============================================================================

static void fyWifiPromiscCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT || !buf) return;

    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    const wifi_pkt_rx_ctrl_t& rx = p->rx_ctrl;
    const uint8_t* frame = p->payload;
    size_t len = rx.sig_len;

    if (len < 24) return;
    if (!fyIsProbeRequest(frame, len)) return;

    // Parse wildcard SSID. Some capture paths include a 4-byte FCS trailer.
    bool ssidFound = false;
    uint8_t ssidLen = 0;
    bool isWildcard = fyParseWildcardSSID(frame, len, ssidFound, ssidLen);
    if (!ssidFound && len > 28) {
        // Retry assuming 4-byte FCS trailer.
        bool ssidFound2 = false;
        uint8_t ssidLen2 = 0;
        bool isWildcard2 = fyParseWildcardSSID(frame, len - 4, ssidFound2, ssidLen2);
        if (ssidFound2) {
            ssidFound = true;
            ssidLen = ssidLen2;
            isWildcard = isWildcard2;
            len -= 4;
        }
    }

    if (!ssidFound) return;
    if (ssidLen != 0) return;
    if (!isWildcard) return; // defensive (should be true if ssidLen==0)

    // Source/Transmitter address is addr2 at offset 10 for management frames.
    const uint8_t* sa = frame + 10;
    if (!fyMatchAnyOUI(sa)) return;

    if (!fyHitQueue) return;
    FYProbeHit hit;
    memcpy(hit.mac, sa, 6);
    hit.rssi = (int8_t)rx.rssi;
    hit.channel = (uint8_t)rx.channel;
    hit.tsMs = (uint32_t)millis();

    BaseType_t woken = pdFALSE;
    // Callback context can be WiFi task or ISR-like; FromISR is safest.
    if (xQueueSendFromISR(fyHitQueue, &hit, &woken) != pdTRUE) {
        // #region agent log
        fyAgentQueueFullISR++;
        // #endregion
    }
    if (woken) portYIELD_FROM_ISR();
}

static void fyWifiInitPromisc() {
    // Bring up WiFi driver in STA mode and enable promiscuous RX.
    // If promisc callback never fires, the usual cause is WiFi not started.
    WiFi.mode(WIFI_STA);
    delay(50);

    // Ensure the driver is started (Arduino core usually does this, but be explicit).
    (void)esp_wifi_start();
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(&fyWifiPromiscCallback);
    (void)esp_wifi_set_channel(fyHopChannel, WIFI_SECOND_CHAN_NONE);
    (void)esp_wifi_set_promiscuous(true);
}

// ============================================================================
// ARDUINO
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(200);

    fyBuzzerOn = true;
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    fyMutex = xSemaphoreCreateMutex();
    fyHitQueue = xQueueCreate(64, sizeof(FYProbeHit));

    printf("\n========================================\n");
    printf("  FLOCK-YOU WiFi Probe Detector (PC-only)\n");
    printf("  Rule: OUI match + wildcard probe request\n");
    printf("========================================\n");

    fyBootBeep();
    fyWifiInitPromisc();

    printf("[FLOCK-YOU] Serial JSON output at 115200 baud\n");
    // #region agent log
    fyAgentNdjson("H4", "setup", "boot_reset_reason", (int)esp_reset_reason(), 0, 0);
    // #endregion
}

void loop() {
    unsigned long now = millis();

    // Drain queued WiFi hits (ISR only enqueues; same role as BLE callbacks feeding state upstream)
    if (fyHitQueue) {
        FYProbeHit hit;
        // #region agent log
        static uint32_t sLastReportedDrops = 0;
        uint32_t drops = fyAgentQueueFullISR;
        if (drops != sLastReportedDrops) {
            fyAgentNdjson("H5", "loop", "isr_queue_full_total", (int)drops, (int)(drops - sLastReportedDrops), 0);
            sLastReportedDrops = drops;
        }
        // #endregion
        while (xQueueReceive(fyHitQueue, &hit, 0) == pdTRUE) {
            unsigned long t = millis();
            char macStr[18];
            fyFormatMAC(hit.mac, macStr);
            int idx = fyAddDetection(macStr, (int)hit.rssi, (int)hit.channel, "probe_request");
            fyMaybeEmitDetection(idx);

            // colonelpanichacks/flock-you: high-confidence detect -> in-range + reset HB clock; alert if !fyTriggered
            fyDeviceInRange = true;
            fyLastDetTime = t;
            fyLastHB = t;
            if (!fyTriggered) {
                fyTriggered = true;
                fyDetectBeep();
                // #region agent log
                fyAgentNdjson("H1", "loop", "detect_alert", 0, 0, 0);
                // #endregion
            }
        }
    }

    // Heartbeat tracking (same structure as colonelpanichacks/flock-you loop())
    if (fyDeviceInRange) {
        if (millis() - fyLastHB >= FY_HEARTBEAT_INTERVAL_MS) {
            fyHeartbeat();
            fyLastHB = millis();
            // #region agent log
            fyAgentNdjson("H1", "loop", "heartbeat_play", 0, 0, 0);
            // #endregion
        }
        if (millis() - fyLastDetTime >= FY_INRANGE_TIMEOUT_MS) {
            // #region agent log
            fyAgentNdjson("H1", "loop", "range_exit_30s", 0, 0, 0);
            // #endregion
            fyDeviceInRange = false;
            fyTriggered = false;
        }
    }

    // Channel control
    if (!(FY_FIXED_CHANNEL >= 1 && FY_FIXED_CHANNEL <= 11)) {
        if (now - fyLastHop >= FY_HOP_DWELL_MS) {
            fyLastHop = now;
            fyHopIdx = (uint8_t)((fyHopIdx + 1) % (sizeof(FY_HOP_CHANNELS) / sizeof(FY_HOP_CHANNELS[0])));
            fyHopChannel = FY_HOP_CHANNELS[fyHopIdx];
            esp_wifi_set_channel(fyHopChannel, WIFI_SECOND_CHAN_NONE);
        }
    }
    // Upstream uses delay(100) in a BLE + web loop; shorter here to drain promisc hits promptly.
    delay(10);
}

