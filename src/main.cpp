
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TinyGPS++.h>

// ── Configuration ────────────────────────────────────────────────────────────
constexpr char     WIFI_SSID[]    = "ALHN-F6C0-J";
constexpr char     WIFI_PASS[]    = "00000000";
constexpr char     SERVER_URL[]   = "http://0.0.0.0:3000/data";
constexpr uint8_t  GPS_RX        = 34;
constexpr uint8_t  GPS_TX        = 33;
constexpr uint8_t  BAT_ADC_PIN   = 1;
constexpr uint8_t  WIFI_LED_PIN  = 21;
constexpr uint32_t GPS_BAUD      = 115200;
constexpr uint32_t SEND_INTERVAL = 10000;   // ms between HTTP posts
constexpr uint32_t DBG_INTERVAL  = 2000;    // ms between debug prints

// Battery voltage mapping (×100 to stay in integer math)
constexpr int BAT_MIN_MV_x100 = 330;  // 3.30 V  → 0 %
constexpr int BAT_MAX_MV_x100 = 420;  // 4.20 V  → 100 %
// ─────────────────────────────────────────────────────────────────────────────

TinyGPSPlus    gps;
HardwareSerial SerialGPS(1);
HTTPClient     http;                   // reused – avoids repeated heap allocs

// ── Battery ──────────────────────────────────────────────────────────────────
// Pure integer math; no floats, no map() overflow risk.
static inline int getBatteryPercent() {
    // ADC is 12-bit (0-4095), Vref = 3.3 V, resistor divider ×2
    // voltage_mV = raw * 3300 * 2 / 4095  →  simplified: raw * 6600 / 4095
    int mv_x100 = (int)(analogRead(BAT_ADC_PIN)) * 660 / 4095 * 10;
    int pct      = (mv_x100 - BAT_MIN_MV_x100) * 100 / (BAT_MAX_MV_x100 - BAT_MIN_MV_x100);
    return constrain(pct, 0, 100);
}

// ── WiFi reconnect (non-blocking) ────────────────────────────────────────────
static void ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.print("[WiFi] Reconnecting");
    WiFi.disconnect(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 20) {
        delay(500);
        Serial.print('.');
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " FAILED");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Drive LED / power pin low immediately
    pinMode(WIFI_LED_PIN, OUTPUT);
    digitalWrite(WIFI_LED_PIN, LOW);

    // Single attenuation call covers all ADC channels
    analogSetAttenuation(ADC_11db);

    SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

    WiFi.setAutoReconnect(true);      // let SDK handle background reconnects
    WiFi.persistent(false);           // don't wear flash with repeated writes
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
    Serial.println("\n[WiFi] Connected – " + WiFi.localIP().toString());
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // ── 1. Feed GPS parser (highest priority, no blocking) ──────────────────
    while (SerialGPS.available()) gps.encode(SerialGPS.read());

    const uint32_t now = millis();    // single timestamp for the whole frame

    // ── 2. Debug heartbeat ──────────────────────────────────────────────────
    static uint32_t lastDbg = 0;
    if (now - lastDbg >= DBG_INTERVAL) {
        lastDbg = now;
        Serial.printf("[DBG] Sats:%u  Bat:%d%%  WiFi:%s\n",
                      gps.satellites.value(),
                      getBatteryPercent(),
                      WiFi.status() == WL_CONNECTED ? "OK" : "LOST");
    }

    
    static uint32_t lastSend = 0;
    if (now - lastSend >= SEND_INTERVAL) {
        lastSend = now;

        ensureWiFi();
        if (WiFi.status() != WL_CONNECTED) return;

        // Build payload with snprintf – zero heap fragmentation vs String +
        char payload[72];   // {"lat":-xx.xxxxxx,"lng":-xxx.xxxxxx,"battery":100}
        snprintf(payload, sizeof(payload),
                 "{\"lat\":%.6f,\"lng\":%.6f,\"battery\":%d}",
                 gps.location.isValid() ? gps.location.lat() : 0.0,
                 gps.location.isValid() ? gps.location.lng() : 0.0,
                 getBatteryPercent());

        http.begin(SERVER_URL);
        http.addHeader("Content-Type", "application/json", false, true); // replace any existing Content-Type header
        int code = http.POST(payload);
        http.end();

        Serial.printf("[HTTP] %s  →  %d\n", payload, code);
    }
}