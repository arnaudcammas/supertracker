// LilyGo T-SIM7000G tracker — PLAIN HTTP via bore.pub tunnel.
// Bypasses the modem's broken HTTPS stack entirely. Public TCP tunnel:
//   bore.pub:<your-port> → Pi (or any host running `bore local 5055 --to bore.pub`)
//   → Traccar localhost:5055

#include <Arduino.h>

#define MODEM_PWRKEY      4
#define MODEM_TX          27
#define MODEM_RX          26
#define MODEM_DTR         25
#define BOARD_POWERON     12
#define BAT_ADC           35
#define SOLAR_ADC         36

static const char APN[]            = "fast.t-mobile.com";
// CHANGE THESE: run `bore local 5055 --to bore.pub` on your Pi, note the port
// it prints, and put it here. Use the IP from `dig +short bore.pub` to skip
// DNS resolution on the modem (more reliable).
static const char TRACCAR_HOST[]   = "BORE_PUB_IP";    // e.g. "161.35.110.36"
static const uint16_t TRACCAR_PORT = 0;                // e.g. 18472 (random each run)
static const char DEVICE_ID[]      = "tracker-01";

static const uint32_t STATIONARY_SLEEP_S    = 300;
static const uint32_t GNSS_FIX_TIMEOUT_MS   = 90000;
static const uint32_t NET_REG_TIMEOUT_MS    = 120000;
static const uint32_t MOVING_SAMPLE_MS      = 30000;     // 30s between samples while moving
static const uint32_t MOVING_BURST_MAX_MS   = 600000;    // up to 10 min in moving mode
static const uint32_t MOVING_REFIX_MS       = 20000;     // GPS fix timeout in burst (hot start)
static const float    MOTION_DIST_M         = 30.0f;
static const float    MOTION_SPEED_KMH      = 3.0f;
static const float    GNSS_MAX_HDOP         = 5.0f;     // skip fix if HDOP > 5
static const int      GNSS_MIN_SATS         = 4;        // skip fix if <4 satellites used

static const float BATT_FULL_V  = 4.20f;
static const float BATT_EMPTY_V = 3.30f;

RTC_DATA_ATTR struct {
    uint32_t magic; float lat, lon, speed; uint32_t boots, fixes;
} rtc;
#define RTC_MAGIC 0xCAFEBABE

#define LOG(...) do { Serial.printf(__VA_ARGS__); Serial.flush(); } while(0)

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

// Parse +CGNSINF: <run>,<fix>,<UTC>,<lat>,<lon>,<alt>,<speed>,<course>,<mode>,<>,<HDOP>,<PDOP>,<VDOP>,<>,<sats_view>,<sats_used>,...
// Returns true and fills outputs only if fix is valid AND quality passes thresholds.
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

// Plain HTTP GET via +HTTPACTION. Returns HTTP status code or -1.
static int httpGet(const char* path) {
    atCmd("AT+HTTPTERM", 2000, true);
    if (atCmd("AT+HTTPINIT", 5000).indexOf("OK") < 0) return -1;
    atCmd("AT+HTTPPARA=\"CID\",1");
    char urlBuf[300];
    snprintf(urlBuf, sizeof(urlBuf),
             "AT+HTTPPARA=\"URL\",\"http://%s:%u%s\"",
             TRACCAR_HOST, (unsigned)TRACCAR_PORT, path);
    atCmd(urlBuf);
    atCmd("AT+HTTPACTION=0");  // 0=GET
    int code = -1;
    String resp; uint32_t deadline = millis() + 30000;
    while (millis() < deadline) {
        while (Serial1.available()) resp += (char)Serial1.read();
        int idx = resp.indexOf("+HTTPACTION:");
        if (idx >= 0) {
            int comma1 = resp.indexOf(',', idx);
            int comma2 = resp.indexOf(',', comma1 + 1);
            if (comma1 > 0 && comma2 > comma1) {
                code = resp.substring(comma1 + 1, comma2).toInt();
                break;
            }
        }
    }
    LOG("[post] HTTP %d\n", code);
    atCmd("AT+HTTPTERM", 3000, true);
    return code;
}

void setup() {
    Serial.begin(115200); delay(2000);
    LOG("\n[boot] T-SIM7000G tracker via bore.pub:%u\n", (unsigned)TRACCAR_PORT);

    if (rtc.magic != RTC_MAGIC) { rtc = {0}; rtc.magic = 0; }
    rtc.boots++;
    LOG("[boot] wake #%u  cachedFixes=%u\n",
        (unsigned)rtc.boots, rtc.magic == RTC_MAGIC ? (unsigned)rtc.fixes : 0u);

    analogReadResolution(12);
    float vbat = readDividerV(BAT_ADC);
    float vsol = readDividerV(SOLAR_ADC);
    int pct = batteryPercent(vbat);
    bool charging = vsol > vbat + 0.20f;
    bool batteryPresent = vbat >= 2.5f;
    LOG("[batt] V=%.3fV (%d%%) solar=%.3fV %s\n",
        vbat, pct, vsol, charging ? "CHARGING" : (batteryPresent ? "discharging" : "no-battery/USB"));

    modemPowerOn();
    delay(3000);
    Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);

    int tries = 0;
    while (atCmd("AT", 1000, true).indexOf("OK") < 0) {
        if (++tries > 20) { LOG("[modem] no AT\n"); cycleReset(60); }
    }
    LOG("[modem] AT ok\n");
    atCmd("ATE0", 1000, true);
    atCmd("AT+CMEE=2", 1000, true);
    atCmd("AT+CFUN=1", 10000);
    atCmd("AT+CMNB=1");
    atCmd("AT+CNMP=38");

    char apnCmd[80];
    snprintf(apnCmd, sizeof(apnCmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    atCmd(apnCmd);

    // GNSS with antenna LDO enabled
    modem_gnss:
    atCmd("AT+CGPIO=0,48,1,1");
    atCmd("AT+CGNSPWR=1");
    LOG("[gnss] waiting for fix...\n");
    float lat = 0, lon = 0, speed = 0, hdop = 99.0f;
    int sats = 0;
    bool gotFix = false;
    uint32_t gstart = millis();
    while (millis() - gstart < GNSS_FIX_TIMEOUT_MS) {
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
        LOG("[gnss] no qualified fix this wake (hdop=%.1f sats=%d)\n", hdop, sats);
    }

    // Network registration
    LOG("[net] waiting for registration...\n");
    uint32_t regDeadline = millis() + NET_REG_TIMEOUT_MS;
    bool registered = false;
    while (millis() < regDeadline) {
        String r = atCmd("AT+CEREG?", 1000, true);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { registered = true; break; }
        delay(2000);
    }
    if (!registered) { LOG("[net] reg timeout\n"); cycleReset(STATIONARY_SLEEP_S); }
    LOG("[net] reg ok\n");
    atCmd("AT+CGATT=1", 10000);
    snprintf(apnCmd, sizeof(apnCmd), "AT+CNACT=1,\"%s\"", APN);
    atCmd(apnCmd, 15000);
    atCmd("AT+CNACT?");

    // Also open SAPBR bearer — +HTTPACTION on SIM7000 needs this, not just CNACT.
    atCmd("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
    snprintf(apnCmd, sizeof(apnCmd), "AT+SAPBR=3,1,\"APN\",\"%s\"", APN);
    atCmd(apnCmd);
    atCmd("AT+SAPBR=1,1", 15000);
    atCmd("AT+SAPBR=2,1");

    // POST
    char path[320];
    if (gotFix) {
        snprintf(path, sizeof(path),
            "/?id=%s&lat=%.6f&lon=%.6f&speed=%.2f&batt=%d&vbat=%.2f&solar=%.2f&charge=%d&hdop=%.1f&sat=%d&valid=1&boot=%u&fixes=%u",
            DEVICE_ID, lat, lon, speed, pct, vbat, vsol, charging ? 1 : 0, hdop, sats,
            (unsigned)rtc.boots, (unsigned)rtc.fixes);
        httpGet(path);
    } else if (rtc.magic == RTC_MAGIC) {
        snprintf(path, sizeof(path),
            "/?id=%s&lat=%.6f&lon=%.6f&speed=0.00&batt=%d&vbat=%.2f&solar=%.2f&charge=%d&valid=0&boot=%u&fixes=%u",
            DEVICE_ID, rtc.lat, rtc.lon, pct, vbat, vsol, charging ? 1 : 0,
            (unsigned)rtc.boots, (unsigned)rtc.fixes);
        httpGet(path);
    } else {
        snprintf(path, sizeof(path),
            "/?id=%s&lat=0&lon=0&batt=%d&vbat=%.2f&solar=%.2f&charge=%d&valid=0&boot=%u",
            DEVICE_ID, pct, vbat, vsol, charging ? 1 : 0, (unsigned)rtc.boots);
        httpGet(path);   // heartbeat with no fix
    }

    // ---- Motion detection: did we move since last wake? ----
    bool moving = false;
    if (gotFix && rtc.fixes >= 2) {
        float dist = haversineMeters(rtc.lat, rtc.lon, lat, lon);
        if (dist > MOTION_DIST_M || speed > MOTION_SPEED_KMH) moving = true;
        LOG("[motion] dist=%.1fm speed=%.2fkmh -> %s\n",
            dist, speed, moving ? "MOVING" : "stationary");
    }

    // ---- Burst mode: sample every 30s as long as we're moving.
    //      Exits on: stop (low speed + small distance), or 3 consecutive bad fixes.
    if (moving) {
        LOG("[motion] burst mode (indefinite while moving)\n");
        uint32_t lastSample = millis();
        int missedFixes = 0;
        while (true) {
            if (millis() - lastSample < MOVING_SAMPLE_MS) { delay(500); continue; }
            lastSample = millis();
            atCmd("AT+CGPIO=0,48,1,1", 1000, true);
            atCmd("AT+CGNSPWR=1", 2000, true);
            float bl = 0, bn = 0, bs = 0, bh = 99.0f;
            int bsats = 0;
            bool got = false;
            uint32_t fstart = millis();
            while (millis() - fstart < MOVING_REFIX_MS) {
                String r = atCmd("AT+CGNSINF", 2000, true);
                if (parseAndQualifyGNSS(r, &bl, &bn, &bs, &bh, &bsats)) {
                    got = true; break;
                }
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
                char p2[320];
                snprintf(p2, sizeof(p2),
                    "/?id=%s&lat=%.6f&lon=%.6f&speed=%.2f&batt=%d&vbat=%.2f&hdop=%.1f&sat=%d&valid=1&boot=%u&fixes=%u",
                    DEVICE_ID, bl, bn, bs, pp, vb, bh, bsats, (unsigned)rtc.boots, (unsigned)rtc.fixes);
                httpGet(p2);
                if (bs < MOTION_SPEED_KMH && dist < MOTION_DIST_M) {
                    LOG("[motion] stopped, exit burst\n");
                    break;
                }
            } else {
                missedFixes++;
                LOG("[motion] burst sample missed fix (%d/3)\n", missedFixes);
                if (missedFixes >= 3) { LOG("[motion] bad GPS, exit burst\n"); break; }
            }
        }
    }

    atCmd("AT+CNACT=0,0", 5000);
    cycleReset(STATIONARY_SLEEP_S);
}

void loop() {}
