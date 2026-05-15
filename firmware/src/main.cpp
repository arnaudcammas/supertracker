// LilyGo T-SIM7000G tracker — plain HTTP via bore.pub tunnel, HMAC-signed.
// Public TCP tunnel: bore.pub:<port> → Pi running `bore local 5056 --to bore.pub`
//                    → Pi-local hmac-proxy:5056 (verify+forward) → Traccar:5055

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include "secrets.h"  // defines TRACCAR_HOST and HMAC_SECRET; gitignored

#define MODEM_PWRKEY      4
#define MODEM_TX          27
#define MODEM_RX          26
#define MODEM_DTR         25
#define BOARD_POWERON     12
#define BAT_ADC           35
#define SOLAR_ADC         36

// ---- USER CONFIG ----
static const char APN[]            = "fast.t-mobile.com";
// TRACCAR_HOST and HMAC_SECRET live in secrets.h (gitignored).
// See secrets.example.h for the template.
static const uint16_t TRACCAR_PORT = 51452;
static const char DEVICE_ID[]      = "tracker-01";

// ---- OTA over WiFi (when at home) ----
// Leave empty strings to disable WiFi OTA entirely. When set, on every wake
// the firmware briefly tries to join the home WiFi and check for a newer
// firmware binary. Failures are silent (cellular cycle proceeds normally).
static const char WIFI_SSID[]      = "";    // e.g. "MyHomeWiFi" — leave empty to disable OTA
static const char WIFI_PASS[]      = "";
static const char OTA_URL[]        = "";    // e.g. "http://192.168.1.10:8090/manifest.json"
static const char OTA_TOKEN[]      = "";    // shared with Pi-side OTA server
static const uint32_t FW_VERSION   = 2;     // bump this each release. Server returns "version" in manifest.

// Timing
static const uint32_t STATIONARY_SLEEP_S    = 300;
static const uint32_t LOW_BATT_SLEEP_S      = 1800;   // 30 min when battery < BATT_LOW_V
static const uint32_t CRIT_BATT_SLEEP_S     = 21600;  // 6 h when battery < BATT_CRIT_V
static const uint32_t GNSS_FIX_TIMEOUT_MS   = 90000;
static const uint32_t NET_REG_TIMEOUT_MS    = 120000;
static const uint32_t MOVING_SAMPLE_MS      = 30000;
static const uint32_t MOVING_REFIX_MS       = 20000;
static const uint32_t TASK_WDT_S            = 240;    // 4 min: if any phase hangs, reboot

// Quality thresholds
static const float    MOTION_DIST_M         = 30.0f;
static const float    MOTION_SPEED_KMH      = 3.0f;
static const float    GNSS_MAX_HDOP         = 5.0f;
static const int      GNSS_MIN_SATS         = 4;

// Battery
static const float BATT_FULL_V  = 4.20f;
static const float BATT_EMPTY_V = 3.30f;
static const float BATT_LOW_V   = 3.45f;
static const float BATT_CRIT_V  = 3.20f;

// Failure tracking → modem soft-reset
static const uint8_t MAX_CONSECUTIVE_FAILS = 2;

RTC_DATA_ATTR struct {
    uint32_t magic;
    float    lat, lon, speed;
    uint32_t boots, fixes;
    uint8_t  consecutiveFails;   // count of cycles with no successful POST
    bool     burstWasStopped;    // hysteresis: previous burst sample was below motion threshold
} rtc;
#define RTC_MAGIC 0xCAFEBABE

#define LOG(...) do { Serial.printf(__VA_ARGS__); Serial.flush(); } while(0)

// ============================================================
// AT command helper
// ============================================================
static String atCmd(const char* cmd, uint32_t timeout = 3000, bool quiet = false) {
    while (Serial1.available()) Serial1.read();
    Serial1.print(cmd); Serial1.print("\r\n");
    String resp; uint32_t deadline = millis() + timeout;
    while (millis() < deadline) {
        while (Serial1.available()) resp += (char)Serial1.read();
        if (resp.indexOf("OK\r\n") >= 0 || resp.indexOf("ERROR") >= 0) break;
    }
    if (!quiet) LOG("> %s\n%s", cmd, resp.c_str());
    return resp;
}

// ============================================================
// Battery + GPS helpers
// ============================================================
static float readDividerV(int pin) {
    uint32_t acc = 0;
    for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(pin);
    return (acc / 16.0f) * 2.0f / 1000.0f;
}
static int batteryPercent(float v) {
    if (v >= BATT_FULL_V)  return 100;
    if (v <= BATT_EMPTY_V) return 0;
    return (int)((v - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f);
}
static float haversineMeters(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371000.0f, toRad = 3.14159265f / 180.0f;
    float dLat = (lat2-lat1)*toRad, dLon = (lon2-lon1)*toRad;
    float a = sinf(dLat/2)*sinf(dLat/2)
            + cosf(lat1*toRad)*cosf(lat2*toRad)*sinf(dLon/2)*sinf(dLon/2);
    return 2*R*atan2f(sqrtf(a), sqrtf(1-a));
}

// Parse +CGNSINF and apply HDOP/sats quality thresholds. Returns true on qualified fix.
static bool parseAndQualifyGNSS(const String& r, float* lat, float* lon, float* speed, float* hdop, int* sats) {
    int p[20] = {0};
    int n = 0;
    int start = r.indexOf(':');
    if (start < 0) return false;
    p[n++] = start;
    int idx = start;
    while (n < 20) {
        int c = r.indexOf(',', idx + 1);
        if (c < 0) break;
        p[n++] = c;
        idx = c;
    }
    if (n < 16) return false;
    int fix = r.substring(p[0]+1, p[1]).toInt();
    if (fix != 1) return false;
    *lat   = r.substring(p[3]+1, p[4]).toFloat();
    *lon   = r.substring(p[4]+1, p[5]).toFloat();
    *speed = r.substring(p[6]+1, p[7]).toFloat();
    *hdop  = r.substring(p[10]+1, p[11]).toFloat();
    *sats  = r.substring(p[15]+1, p[16]).toInt();
    if (*lat == 0.0f && *lon == 0.0f) return false;
    if (*sats < GNSS_MIN_SATS) return false;
    if (*hdop > GNSS_MAX_HDOP) return false;
    return true;
}

// CSQ → RSSI dBm (approx). Returns -999 on unknown.
static int readCSQ() {
    String r = atCmd("AT+CSQ", 1000, true);
    int idx = r.indexOf("+CSQ:");
    if (idx < 0) return -999;
    int comma = r.indexOf(',', idx);
    if (comma < 0) return -999;
    int raw = r.substring(idx+6, comma).toInt();
    if (raw == 99) return -999;             // unknown
    return -113 + 2 * raw;                  // 0→-113, 31→-51
}

// ============================================================
// HMAC-SHA256 of "id|lat|lon|boot|fixes" → 16-hex-char signature.
// Server verifies + checks (boot,fixes) strictly greater than previous.
// ============================================================
static void hmacSig(const char* msg, char outHex[17]) {
    uint8_t mac[32];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)HMAC_SECRET, strlen(HMAC_SECRET));
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg));
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    for (int i = 0; i < 8; i++) sprintf(outHex + 2*i, "%02x", mac[i]);  // first 8 bytes = 16 hex chars
    outHex[16] = 0;
}

// Build the request path including HMAC signature. msgFmt receives sprintf-style.
// Path layout: /?id=...&lat=..&lon=..&...&boot=N&fixes=N&sig=XXXX
static void buildSignedPath(char* outPath, size_t outSize,
                            float lat, float lon, float speed, int valid,
                            int pct, float vbat, float vsol, bool charging,
                            float hdop, int sats, int csq)
{
    char sigInput[128];
    snprintf(sigInput, sizeof(sigInput), "%s|%.6f|%.6f|%u|%u",
             DEVICE_ID, lat, lon, (unsigned)rtc.boots, (unsigned)rtc.fixes);
    char sig[17];
    hmacSig(sigInput, sig);
    snprintf(outPath, outSize,
        "/?id=%s&lat=%.6f&lon=%.6f&speed=%.2f&batt=%d&vbat=%.2f&solar=%.2f&charge=%d"
        "&hdop=%.1f&sat=%d&csq=%d&valid=%d&boot=%u&fixes=%u&sig=%s",
        DEVICE_ID, lat, lon, speed, pct, vbat, vsol, charging ? 1 : 0,
        hdop, sats, csq, valid, (unsigned)rtc.boots, (unsigned)rtc.fixes, sig);
}

// ============================================================
// OTA over WiFi
// ============================================================
// Try to connect to home WiFi briefly. Returns true if associated + IP'd.
static bool wifiTryConnect(uint32_t timeout_ms = 6000) {
    if (WIFI_SSID[0] == 0) return false;
    LOG("[ota] trying WiFi '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t deadline = millis() + timeout_ms;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(200);
        esp_task_wdt_reset();
    }
    bool ok = WiFi.status() == WL_CONNECTED;
    if (ok) LOG("[ota] WiFi up: %s\n", WiFi.localIP().toString().c_str());
    else    LOG("[ota] WiFi not available\n");
    return ok;
}

static void wifiOff() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}

// Check for newer firmware via the OTA manifest. If newer, download and apply.
// Manifest format (JSON): {"version": <int>, "url": "http://.../firmware.bin"}
// Both the manifest fetch and binary fetch include "X-OTA-Token" header.
static void otaCheck() {
    if (OTA_URL[0] == 0 || OTA_TOKEN[0] == 0) return;
    if (!wifiTryConnect(6000)) return;

    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(OTA_URL)) { LOG("[ota] manifest begin fail\n"); wifiOff(); return; }
    http.addHeader("X-OTA-Token", OTA_TOKEN);
    int code = http.GET();
    if (code != 200) {
        LOG("[ota] manifest HTTP %d\n", code);
        http.end(); wifiOff(); return;
    }
    String body = http.getString();
    http.end();

    // Crude JSON parse (avoids pulling in ArduinoJson)
    int vIdx = body.indexOf("\"version\"");
    int uIdx = body.indexOf("\"url\"");
    if (vIdx < 0 || uIdx < 0) { LOG("[ota] bad manifest: %s\n", body.c_str()); wifiOff(); return; }
    int vColon = body.indexOf(':', vIdx);
    int vEnd   = body.indexOf(',', vColon);
    if (vEnd < 0) vEnd = body.indexOf('}', vColon);
    uint32_t serverVer = body.substring(vColon+1, vEnd).toInt();

    int uQuote1 = body.indexOf('"', body.indexOf(':', uIdx));
    int uQuote2 = body.indexOf('"', uQuote1 + 1);
    String url = body.substring(uQuote1+1, uQuote2);

    LOG("[ota] local v%u  server v%u  url=%s\n", (unsigned)FW_VERSION, (unsigned)serverVer, url.c_str());
    if (serverVer <= FW_VERSION) { LOG("[ota] up to date\n"); wifiOff(); return; }

    LOG("[ota] downloading + applying...\n");
    httpUpdate.setLedPin(BOARD_POWERON, HIGH);   // re-uses our power-on LED as activity indicator
    httpUpdate.rebootOnUpdate(true);
    // Long task; pet the watchdog by extending its timeout for this phase
    esp_task_wdt_reset();

    WiFiClient client;
    HTTPUpdateResult ret = httpUpdate.update(client, url, "", [](HTTPClient* h){
        h->addHeader("X-OTA-Token", OTA_TOKEN);
    });
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            LOG("[ota] FAILED: %s\n", httpUpdate.getLastErrorString().c_str()); break;
        case HTTP_UPDATE_NO_UPDATES:
            LOG("[ota] no updates\n"); break;
        case HTTP_UPDATE_OK:
            LOG("[ota] success (will not reach here, device reboots)\n"); break;
    }
    wifiOff();
}

// ============================================================
// Modem power + cycle
// ============================================================
static void modemPowerOn() {
    pinMode(BOARD_POWERON, OUTPUT); digitalWrite(BOARD_POWERON, HIGH);
    pinMode(MODEM_DTR, OUTPUT); digitalWrite(MODEM_DTR, LOW);
    pinMode(MODEM_PWRKEY, OUTPUT);
    digitalWrite(MODEM_PWRKEY, HIGH); delay(100);
    digitalWrite(MODEM_PWRKEY, LOW); delay(1000);
    digitalWrite(MODEM_PWRKEY, HIGH);
}

static void cycleReset(uint32_t s) {
    LOG("[sleep] entering deep sleep for %us\n", (unsigned)s);
    delay(50);
    atCmd("AT+CPOWD=1", 5000, true);
    digitalWrite(BOARD_POWERON, LOW);
    Serial1.end();
    esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
    esp_deep_sleep_start();
}

// Soft reset of the radio without power-cycling — recovers from wedged states.
static void modemSoftReset() {
    LOG("[modem] soft reset (CFUN 0→1)\n");
    atCmd("AT+CFUN=0", 10000);
    delay(1000);
    atCmd("AT+CFUN=1", 10000);
    delay(3000);
}

// ============================================================
// HTTP GET with single retry on failure
// ============================================================
static int httpGetOnce(const char* path) {
    atCmd("AT+HTTPTERM", 2000, true);
    if (atCmd("AT+HTTPINIT", 5000).indexOf("OK") < 0) return -1;
    atCmd("AT+HTTPPARA=\"CID\",1");
    char urlBuf[420];
    snprintf(urlBuf, sizeof(urlBuf),
             "AT+HTTPPARA=\"URL\",\"http://%s:%u%s\"",
             TRACCAR_HOST, (unsigned)TRACCAR_PORT, path);
    atCmd(urlBuf);
    atCmd("AT+HTTPACTION=0");
    int code = -1;
    String resp; uint32_t deadline = millis() + 30000;
    while (millis() < deadline) {
        while (Serial1.available()) resp += (char)Serial1.read();
        int idx = resp.indexOf("+HTTPACTION:");
        if (idx >= 0) {
            int c1 = resp.indexOf(',', idx);
            int c2 = resp.indexOf(',', c1 + 1);
            if (c1 > 0 && c2 > c1) {
                code = resp.substring(c1 + 1, c2).toInt();
                break;
            }
        }
    }
    atCmd("AT+HTTPTERM", 3000, true);
    return code;
}

// Returns HTTP code (200 = success). Retries once on non-2xx.
static int httpGet(const char* path) {
    int code = httpGetOnce(path);
    LOG("[post] HTTP %d\n", code);
    if (code >= 200 && code < 300) return code;
    LOG("[post] retry once...\n");
    delay(2000);
    code = httpGetOnce(path);
    LOG("[post] HTTP %d (retry)\n", code);
    return code;
}

// ============================================================
// Offline store-and-forward queue (LittleFS)
//
// Each line is one fully-built, HMAC-signed request path. The signature has
// no timestamp, so a buffered path stays valid indefinitely, and the server's
// (boot,fixes) replay guard makes re-sending a record harmless. Records are
// appended in fixes-increasing order and always flushed oldest-first, so the
// server sees a monotonic sequence even after an offline stretch.
// ============================================================
static const char   QUEUE_FN[]      = "/q.log";
static const char   QUEUE_TMP[]     = "/q.tmp";
static const size_t MAX_QUEUE_BYTES = 400000;   // ~1200 records; SPIFFS partition is ~1.5 MB
static bool         fsReady         = false;

static void queueInit() {
    fsReady = LittleFS.begin(true);   // format on first boot / corruption
    if (!fsReady) { LOG("[queue] LittleFS mount FAILED — buffering disabled\n"); return; }
    size_t n = 0;
    File f = LittleFS.open(QUEUE_FN, "r");
    if (f) { n = f.size(); f.close(); }
    LOG("[queue] ready, %u bytes pending\n", (unsigned)n);
}

// Monotonic boot counter persisted to flash. RTC RAM alone is wiped on power
// loss, which would send the server's replay guard backwards and lock us out.
static const char BOOTCNT_FN[] = "/boot.cnt";
static uint32_t bumpBootCount() {
    uint32_t n = 0;
    if (fsReady) {
        File f = LittleFS.open(BOOTCNT_FN, "r");
        if (f) { n = (uint32_t)f.readString().toInt(); f.close(); }
    }
    n++;
    if (fsReady) {
        File f = LittleFS.open(BOOTCNT_FN, "w");
        if (f) { f.print(n); f.close(); }
    }
    LOG("[boot] persistent boot count = %u\n", (unsigned)n);
    return n;
}

static void queueAppend(const char* path) {
    if (!fsReady) return;
    File f = LittleFS.open(QUEUE_FN, "a");
    if (!f) { LOG("[queue] append failed to open\n"); return; }
    if (f.size() > MAX_QUEUE_BYTES) {
        LOG("[queue] FULL (%u B) — dropping this fix\n", (unsigned)f.size());
        f.close();
        return;
    }
    f.println(path);
    size_t n = f.size();
    f.close();
    LOG("[queue] buffered fix (%u B pending)\n", (unsigned)n);
}

// Send queued fixes oldest-first. Stops at the first transient failure and
// keeps that record plus all newer ones. A 403 (replay / bad sig) can never
// succeed on retry, so it is dropped. Returns true if the queue is now empty.
static bool queueFlush() {
    if (!fsReady) return true;
    if (!LittleFS.exists(QUEUE_FN)) return true;
    File in = LittleFS.open(QUEUE_FN, "r");
    if (!in) return true;
    if (in.size() == 0) { in.close(); LittleFS.remove(QUEUE_FN); return true; }

    LOG("[queue] flushing %u bytes...\n", (unsigned)in.size());
    int sent = 0, dropped = 0;
    bool stalled = false;
    File out;

    while (in.available()) {
        esp_task_wdt_reset();
        String line = in.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        if (!stalled) {
            int code = httpGet(line.c_str());
            if (code >= 200 && code < 300) { sent++; continue; }
            if (code == 403)               { dropped++; continue; }
            stalled = true;
            out = LittleFS.open(QUEUE_TMP, "w");
            if (!out) {                       // can't stage remainder — leave queue intact
                in.close();
                LOG("[queue] temp open failed — keeping queue as-is\n");
                return false;
            }
        }
        out.println(line);
    }
    in.close();

    if (stalled) {
        out.close();
        LittleFS.remove(QUEUE_FN);
        LittleFS.rename(QUEUE_TMP, QUEUE_FN);
        size_t rem = 0;
        File q = LittleFS.open(QUEUE_FN, "r");
        if (q) { rem = q.size(); q.close(); }
        LOG("[queue] sent %d, dropped %d, %u B still pending\n", sent, dropped, (unsigned)rem);
        return false;
    }
    LittleFS.remove(QUEUE_FN);
    LOG("[queue] sent %d, dropped %d, queue empty\n", sent, dropped);
    return true;
}

// Flush any backlog, then send `path`. If `enqueueOnFail` and `path` is not
// delivered (and not permanently rejected), it is buffered for a later cycle.
// Returns true only if `path` itself was delivered (2xx) on this call.
static bool sendWithQueue(const char* path, bool enqueueOnFail) {
    bool drained = queueFlush();
    if (!drained) {
        if (enqueueOnFail) queueAppend(path);   // keep ordering: behind the backlog
        return false;
    }
    int code = httpGet(path);
    bool ok = (code >= 200 && code < 300);
    if (!ok && enqueueOnFail && code != 403) queueAppend(path);
    return ok;
}

// ============================================================
// Setup
// ============================================================
void setup() {
    Serial.begin(115200); delay(2000);
    LOG("\n[boot] T-SIM7000G tracker via bore.pub:%u\n", (unsigned)TRACCAR_PORT);

    // Task watchdog: if we hang for TASK_WDT_S seconds, reboot.
    esp_task_wdt_init(TASK_WDT_S, true);
    esp_task_wdt_add(NULL);

    if (rtc.magic != RTC_MAGIC) {
        rtc = {0}; rtc.magic = 0;
        rtc.consecutiveFails = 0;
        rtc.burstWasStopped = false;
    }
    queueInit();   // mount LittleFS — needed for the queue AND the boot counter

    // The boot counter feeds the server's replay guard, which demands a
    // strictly-increasing (boot,fixes). RTC RAM resets to 0 on every power
    // loss (no battery = every unplug), so persist it to flash instead.
    rtc.boots = bumpBootCount();
    LOG("[boot] wake #%u  cachedFixes=%u  consecutiveFails=%u\n",
        (unsigned)rtc.boots,
        rtc.magic == RTC_MAGIC ? (unsigned)rtc.fixes : 0u,
        (unsigned)rtc.consecutiveFails);

    // Battery / solar — the divider network is unpowered until BOARD_POWERON
    // is HIGH; reading before that floats both ADCs to the same bogus ~0.28 V.
    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);
    delay(500);
    analogReadResolution(12);
    float vbat = readDividerV(BAT_ADC);
    float vsol = readDividerV(SOLAR_ADC);
    int pct = batteryPercent(vbat);
    bool charging = vsol > vbat + 0.20f;
    bool batteryPresent = vbat >= 2.5f;
    LOG("[batt] V=%.3fV (%d%%) solar=%.3fV %s\n",
        vbat, pct, vsol, charging ? "CHARGING" : (batteryPresent ? "discharging" : "no-battery/USB"));

    // Critical-battery guard — only kicks in if a real battery is actually present.
    if (batteryPresent && vbat < BATT_CRIT_V && !charging) {
        LOG("[batt] CRITICAL — sleeping %us, skipping cycle\n", (unsigned)CRIT_BATT_SLEEP_S);
        cycleReset(CRIT_BATT_SLEEP_S);
    }

    // Check for OTA update over home WiFi BEFORE powering up the modem.
    // If new firmware is found, this call reboots the device.
    otaCheck();

    modemPowerOn();
    delay(3000);
    Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    esp_task_wdt_reset();

    // Bring up AT
    int tries = 0;
    while (atCmd("AT", 1000, true).indexOf("OK") < 0) {
        if (++tries > 20) { LOG("[modem] no AT\n"); cycleReset(60); }
    }
    LOG("[modem] AT ok\n");
    atCmd("ATE0", 1000, true);
    atCmd("AT+CMEE=2", 1000, true);
    atCmd("AT+CFUN=1", 10000);

    // If last cycle(s) failed, soft-reset the radio first
    if (rtc.consecutiveFails >= MAX_CONSECUTIVE_FAILS) {
        LOG("[modem] %u failures in a row — soft-resetting radio\n", (unsigned)rtc.consecutiveFails);
        modemSoftReset();
        rtc.consecutiveFails = 0;
    }

    atCmd("AT+CMNB=1");
    atCmd("AT+CNMP=38");
    char apnCmd[80];
    snprintf(apnCmd, sizeof(apnCmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    atCmd(apnCmd);

    // GNSS with antenna LDO enabled
    atCmd("AT+CGPIO=0,48,1,1");
    atCmd("AT+CGNSPWR=1");
    LOG("[gnss] waiting for fix...\n");
    float lat = 0, lon = 0, speed = 0, hdop = 99.0f;
    int sats = 0;
    bool gotFix = false;
    uint32_t gstart = millis();
    while (millis() - gstart < GNSS_FIX_TIMEOUT_MS) {
        esp_task_wdt_reset();
        String r = atCmd("AT+CGNSINF", 2000, true);
        if (parseAndQualifyGNSS(r, &lat, &lon, &speed, &hdop, &sats)) {
            gotFix = true; break;
        }
        delay(2000);
    }
    atCmd("AT+CGNSPWR=0");
    atCmd("AT+CGPIO=0,48,1,0");
    if (gotFix) {
        LOG("[gnss] FIX %.6f,%.6f speed=%.2f hdop=%.1f sats=%d\n", lat, lon, speed, hdop, sats);
        rtc.lat = lat; rtc.lon = lon; rtc.speed = speed;
        rtc.fixes++; rtc.magic = RTC_MAGIC;
    } else {
        LOG("[gnss] no qualified fix this wake\n");
    }

    // Network registration
    esp_task_wdt_reset();
    LOG("[net] waiting for registration...\n");
    uint32_t regDeadline = millis() + NET_REG_TIMEOUT_MS;
    bool registered = false;
    while (millis() < regDeadline) {
        esp_task_wdt_reset();
        String r = atCmd("AT+CEREG?", 1000, true);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { registered = true; break; }
        delay(2000);
    }
    if (!registered) {
        LOG("[net] reg timeout\n");
        rtc.consecutiveFails++;
        cycleReset(STATIONARY_SLEEP_S);
    }
    int csq = readCSQ();
    LOG("[net] reg ok  CSQ=%d dBm\n", csq);

    atCmd("AT+CGATT=1", 10000);
    snprintf(apnCmd, sizeof(apnCmd), "AT+CNACT=1,\"%s\"", APN);
    atCmd(apnCmd, 15000);
    atCmd("AT+CNACT?");

    // SAPBR bearer — +HTTPACTION on SIM7000 needs this, not just CNACT.
    atCmd("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
    snprintf(apnCmd, sizeof(apnCmd), "AT+SAPBR=3,1,\"APN\",\"%s\"", APN);
    atCmd(apnCmd);
    atCmd("AT+SAPBR=1,1", 15000);
    atCmd("AT+SAPBR=2,1");

    esp_task_wdt_reset();

    // POST — signed. Real fixes go through the offline queue (buffered to
    // flash and retried on a later cycle if the send fails); heartbeats are
    // best-effort but still trigger a flush so any backlog drains once we're up.
    char path[512];
    bool delivered;
    if (gotFix) {
        buildSignedPath(path, sizeof(path), lat, lon, speed, 1,
                        pct, vbat, vsol, charging, hdop, sats, csq);
        delivered = sendWithQueue(path, true);
    } else if (rtc.magic == RTC_MAGIC) {
        buildSignedPath(path, sizeof(path), rtc.lat, rtc.lon, 0.0f, 0,
                        pct, vbat, vsol, charging, 99.0f, 0, csq);
        delivered = sendWithQueue(path, false);
    } else {
        buildSignedPath(path, sizeof(path), 0.0f, 0.0f, 0.0f, 0,
                        pct, vbat, vsol, charging, 99.0f, 0, csq);
        delivered = sendWithQueue(path, false);
    }

    if (delivered) {
        rtc.consecutiveFails = 0;
    } else {
        rtc.consecutiveFails++;
    }

    // ---- Motion detection ----
    bool moving = false;
    if (gotFix && rtc.fixes >= 2) {
        float dist = haversineMeters(rtc.lat, rtc.lon, lat, lon);
        if (dist > MOTION_DIST_M || speed > MOTION_SPEED_KMH) moving = true;
        LOG("[motion] dist=%.1fm speed=%.2fkmh -> %s\n",
            dist, speed, moving ? "MOVING" : "stationary");
    }

    // ---- Burst mode with hysteresis: need 2 consecutive low samples to exit
    if (moving) {
        LOG("[motion] burst mode (indefinite while moving)\n");
        rtc.burstWasStopped = false;
        uint32_t lastSample = millis();
        int missedFixes = 0;
        while (true) {
            esp_task_wdt_reset();
            if (millis() - lastSample < MOVING_SAMPLE_MS) { delay(500); continue; }
            lastSample = millis();
            atCmd("AT+CGPIO=0,48,1,1", 1000, true);
            atCmd("AT+CGNSPWR=1", 2000, true);
            float bl = 0, bn = 0, bs = 0, bh = 99.0f;
            int bsats = 0;
            bool got = false;
            uint32_t fstart = millis();
            while (millis() - fstart < MOVING_REFIX_MS) {
                esp_task_wdt_reset();
                String r = atCmd("AT+CGNSINF", 2000, true);
                if (parseAndQualifyGNSS(r, &bl, &bn, &bs, &bh, &bsats)) { got = true; break; }
                delay(2000);
            }
            atCmd("AT+CGNSPWR=0", 2000, true);
            atCmd("AT+CGPIO=0,48,1,0", 1000, true);
            if (got) {
                missedFixes = 0;
                float dist = haversineMeters(rtc.lat, rtc.lon, bl, bn);
                rtc.lat = bl; rtc.lon = bn; rtc.speed = bs; rtc.fixes++;
                float vb = readDividerV(BAT_ADC);
                int pp = batteryPercent(vb);
                int bcsq = readCSQ();
                char p2[512];
                buildSignedPath(p2, sizeof(p2), bl, bn, bs, 1, pp, vb, vsol, charging, bh, bsats, bcsq);
                bool bdelivered = sendWithQueue(p2, true);
                if (bdelivered) rtc.consecutiveFails = 0;
                else rtc.consecutiveFails++;
                // Hysteresis: only exit burst after TWO consecutive low samples
                bool low = (bs < MOTION_SPEED_KMH && dist < MOTION_DIST_M);
                if (low && rtc.burstWasStopped) {
                    LOG("[motion] stopped (2 consecutive low), exit burst\n");
                    break;
                }
                rtc.burstWasStopped = low;
            } else {
                missedFixes++;
                LOG("[motion] burst sample missed fix (%d/3)\n", missedFixes);
                if (missedFixes >= 3) { LOG("[motion] bad GPS, exit burst\n"); break; }
            }
        }
    }

    atCmd("AT+CNACT=0,0", 5000);

    // ---- Adaptive sleep based on battery ----
    uint32_t sleepS = STATIONARY_SLEEP_S;
    float vEnd = readDividerV(BAT_ADC);
    if (batteryPresent && vEnd < BATT_LOW_V && !charging) {
        sleepS = LOW_BATT_SLEEP_S;
        LOG("[batt] LOW V=%.3f — extending sleep to %us\n", vEnd, sleepS);
    }
    cycleReset(sleepS);
}

void loop() {}
