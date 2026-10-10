/*
 * SafeSignal Watch - UNIHIKER K10 (ESP32-S3) version, Arduino C++.
 *
 * Port of the Python UniHiker version / web watch. Same behaviour:
 *   - Three choices: I'M OK / I'M LOST / HELP, sent as JSON POST to the Base44 endpoint
 *   - 2.5 s cooldown against repeated presses
 *   - Offline queue saved to flash (latest status / LOW_BATTERY / GPS kept), retried every 5 s
 *   - HEARTBEAT every 15 s, connectivity tracking, CONNECTIVITY_RESTORED event
 *   - LOW_BATTERY (once per dip), GPS_LOST / GPS_RESTORED events
 *   - Caregiver acknowledgements in any response are shown full screen
 *
 * HOW TO USE THE K10 (it has two buttons, A and B - no touch input is used):
 *   Button A = move the highlight  OK -> LOST -> HELP -> OK ...   (highlight = white frame + "> LABEL <")
 *   Button B = SEND the highlighted choice
 *
 * Arduino IDE setup:
 *   1. Add the DFRobot UNIHIKER board package (Boards Manager -> "UNIHIKER"), select board "UNIHIKER K10".
 *   2. Fill in WIFI_SSID / WIFI_PASSWORD below, then Upload. Open Serial Monitor at 115200 baud.
 *
 * Dev/test commands (type a letter in the Serial Monitor, no hardware needed):
 *   b = battery 18%     B = battery 78%     g = GPS lost     G = GPS restored
 *   o = internet off    O = internet on     1/2/3 = show caregiver acks (safe / on my way / nearby help)
 *   r = reset all simulations
 *
 * FILE LAYOUT
 *   1. CONFIG ........ endpoint, IDs, timings, Wi-Fi, location source
 *   2. HARDWARE LAYER  the ONLY code that touches the K10 library (screen, buttons, battery).
 *                      If a K10 call does not compile for your library version, fix it here.
 *   3. Location, outbox/queue, network task, monitors, UI, setup/loop.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include "unihiker_k10.h"

// =====================================================================
// 1. CONFIG  (edit this section)
// =====================================================================
static const char *WIFI_SSID     = "YOUR_WIFI_NAME";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

static const char *API_ENDPOINT = "https://safesignal-caregiver-to-watch-acrn.base44.app/functions/watchStatus";
static const char *CHILD_ID     = "child_001";
static const char *DEVICE_ID    = "safesignal_watch_001";
// Extra header if Base44 needs one (leave EXTRA_HEADER_NAME empty for none), e.g. "api_key" / "xxxx"
static const char *EXTRA_HEADER_NAME  = "";
static const char *EXTRA_HEADER_VALUE = "";

// TLS: the prototype does NOT verify the server certificate (setInsecure). Traffic is encrypted but a
// man-in-the-middle is not detected. For a real product load the CA certificate instead.
static const bool TLS_INSECURE = true;

static const uint32_t COOLDOWN_MS         = 2500;   // ignore presses for this long after a press
static const uint32_t RETRY_INTERVAL_MS   = 5000;   // retry unsent events this often
static const uint32_t HEARTBEAT_MS        = 15000;  // "I'm alive" ping; Base44 flags the watch if several are missed
static const int      LOW_BATTERY_PERCENT = 20;     // LOW_BATTERY event when level drops below this
static const uint32_t BATTERY_CHECK_MS    = 10000;
static const uint32_t GPS_CHECK_MS        = 5000;   // how often location is sampled
static const uint32_t GPS_LOSS_MS         = 30000;  // no valid location for this long => GPS_LOST
static const uint32_t REQUEST_TIMEOUT_MS  = 8000;
static const uint32_t WIFI_RETRY_MS       = 8000;

// Time zone for timestamps (Singapore = UTC+8). Time comes from NTP once Wi-Fi is up.
static const int TZ_OFFSET_MIN = 8 * 60;

// The K10 has no GPS. Choose where location comes from:
enum LocationSource { LOC_FIXED, LOC_SERIAL_NMEA, LOC_NONE };
static const LocationSource LOCATION_SOURCE = LOC_FIXED;
static const double FIXED_LAT = 1.3521;
static const double FIXED_LNG = 103.8198;
// For LOC_SERIAL_NMEA: a UART GPS module on these pins (set to the K10 pins you wired it to).
static const int GPS_RX_PIN = 44;
static const int GPS_TX_PIN = 43;
static const long GPS_BAUD  = 9600;

// =====================================================================
// 2. HARDWARE LAYER  (K10 specific - keep all unihiker_k10.h calls here)
// =====================================================================
UNIHIKER_K10 k10;

#define COL_BLACK  0x000000
#define COL_WHITE  0xFFFFFF
#define COL_GREEN  0x1FAA3C
#define COL_ORANGE 0xF08A00
#define COL_RED    0xD61F1F
#define COL_BLUE   0x1769C9
#define COL_GREY   0x444444
#define COL_LGREY  0xCCCCCC
#define COL_CONN   0x2ECC40
#define COL_OFF    0xFF3B30

static const int SCREEN_W = 240, SCREEN_H = 320;

static void hal_init() {
  k10.begin();
  k10.initScreen(2);                 // 2 = portrait, 240x320
  k10.creatCanvas();
  k10.setScreenBackground(COL_BLACK);
}
static void hal_clear() { k10.canvas->canvasClear(); }
// filled rectangle
static void hal_rect(int x, int y, int w, int h, uint32_t color) {
  k10.canvas->canvasRectangle(x, y, w, h, color, color, true);
}
static void hal_circle(int x, int y, int r, uint32_t color) {
  k10.canvas->canvasCircle(x, y, r, color, color, true);
}
// size 16 or 24 px font; (x, y) = top-left of the text
static void hal_text(const char *text, int x, int y, uint32_t color, int size) {
  k10.canvas->canvasText(text, x, y, color,
                         size >= 24 ? k10.canvas->eCNAndENFont24 : k10.canvas->eCNAndENFont16,
                         50, false);
}
static void hal_present() { k10.canvas->updateCanvas(); }
static bool hal_buttonA() { return k10.buttonA->isPressed(); }
static bool hal_buttonB() { return k10.buttonB->isPressed(); }
// Battery percent 0-100, or -1 if this board/library cannot report it.
// TODO: replace with the K10 battery-capacity call from DFRobot's "Battery capacity" example.
static int hal_batteryPercent() { return -1; }

// Rough text width so we can centre text (the canvas API only takes the top-left corner).
static int textW(const char *s, int size) { return (int)strlen(s) * (size >= 24 ? 13 : 9); }
static void textCentered(const char *s, int y, uint32_t color, int size) {
  int x = (SCREEN_W - textW(s, size)) / 2;
  if (x < 2) x = 2;
  hal_text(s, x, y, color, size);
}
// Centre text and wrap onto 2 lines at a space if too wide. Returns y after the text.
static int textWrapped(const String &s, int y, uint32_t color, int size) {
  int lineH = size + 6;
  if (textW(s.c_str(), size) <= SCREEN_W - 12) { textCentered(s.c_str(), y, color, size); return y + lineH; }
  int mid = s.length() / 2, cut = -1;
  for (int d = 0; d < (int)s.length(); d++) {
    if (mid + d < (int)s.length() && s[mid + d] == ' ') { cut = mid + d; break; }
    if (mid - d >= 0 && s[mid - d] == ' ') { cut = mid - d; break; }
  }
  if (cut < 0) { textCentered(s.c_str(), y, color, size); return y + lineH; }
  String a = s.substring(0, cut), b = s.substring(cut + 1);
  textCentered(a.c_str(), y, color, size);
  textCentered(b.c_str(), y + lineH, color, size);
  return y + 2 * lineH;
}

// =====================================================================
// 3. SHARED STATE
// =====================================================================
struct Sim { volatile int battery = -1; volatile bool gpsLost = false; volatile bool offline = false; };
static Sim SIM;

static SemaphoreHandle_t gMutex;
static Preferences prefs;

static volatile int8_t gOnline = -1;       // -1 unknown, 0 offline, 1 online
static volatile int8_t gGpsOk  = -1;       // -1 unknown, 0 lost, 1 ok
static volatile int    gBattery = -1;      // percent or -1 (n/a)
static String gOfflineSince;

static String timestampNow() {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
  char zone[8];
  int off = TZ_OFFSET_MIN, a = off < 0 ? -off : off;
  snprintf(zone, sizeof(zone), "%c%02d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
  return String(buf) + zone;
}

// =====================================================================
// 4. LOCATION  (never blocks long; get() always returns something)
// =====================================================================
struct Fix { bool ok; double lat; double lng; };

static bool   gHaveLastKnown = false;
static double gLastLat = 0, gLastLng = 0;
static uint32_t gLastSavedAt = 0;
static double gSavedLat = 0, gSavedLng = 0;

// NMEA line parsing ($xxRMC / $xxGGA) for LOC_SERIAL_NMEA
static String   nmeaLine;
static Fix      nmeaFix = {false, 0, 0};
static uint32_t nmeaFixAt = 0;

static bool parseNmea(const String &line, double &lat, double &lng) {
  if (!line.startsWith("$")) return false;
  int star = line.indexOf('*');
  String body = star >= 0 ? line.substring(0, star) : line;
  String f[15];
  int n = 0, start = 0;
  for (int i = 0; i <= (int)body.length() && n < 15; i++) {
    if (i == (int)body.length() || body[i] == ',') { f[n++] = body.substring(start, i); start = i + 1; }
  }
  if (n < 7) return false;
  String kind = f[0].substring(f[0].length() - 3);
  String la, ns, lo, ew;
  if (kind == "RMC" && f[2] == "A") { la = f[3]; ns = f[4]; lo = f[5]; ew = f[6]; }
  else if (kind == "GGA" && f[6] != "" && f[6] != "0") { la = f[2]; ns = f[3]; lo = f[4]; ew = f[5]; }
  else return false;
  if (la.length() < 4 || lo.length() < 5) return false;
  lat = la.substring(0, 2).toDouble() + la.substring(2).toDouble() / 60.0;
  lng = lo.substring(0, 3).toDouble() + lo.substring(3).toDouble() / 60.0;
  if (ns == "S") lat = -lat;
  if (ew == "W") lng = -lng;
  return true;
}

static void pollNmea() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      double la, lo;
      if (parseNmea(nmeaLine, la, lo)) { nmeaFix = {true, la, lo}; nmeaFixAt = millis(); }
      nmeaLine = "";
    } else if (c != '\r' && nmeaLine.length() < 120) {
      nmeaLine += c;
    }
  }
}

static void rememberValid(double lat, double lng) {
  gLastLat = lat; gLastLng = lng; gHaveLastKnown = true;
  // Save to flash only when we moved or every 10 min (flash wear).
  bool moved = fabs(lat - gSavedLat) > 0.0005 || fabs(lng - gSavedLng) > 0.0005;
  if (moved || millis() - gLastSavedAt > 600000UL || gLastSavedAt == 0) {
    xSemaphoreTake(gMutex, portMAX_DELAY);
    prefs.putDouble("lkLat", lat);
    prefs.putDouble("lkLng", lng);
    xSemaphoreGive(gMutex);
    gSavedLat = lat; gSavedLng = lng; gLastSavedAt = millis() ? millis() : 1;
  }
}

static Fix getFix() {
  Fix f = {false, 0, 0};
  if (SIM.gpsLost) return f;
  if (LOCATION_SOURCE == LOC_FIXED) {
    f = {true, FIXED_LAT, FIXED_LNG};
  } else if (LOCATION_SOURCE == LOC_SERIAL_NMEA) {
    pollNmea();
    if (nmeaFix.ok && millis() - nmeaFixAt < 10000UL) f = nmeaFix;
  }
  if (f.ok) rememberValid(f.lat, f.lng);
  return f;
}

// =====================================================================
// 5. PAYLOADS (JSON built by hand - no library needed)
// =====================================================================
static String jnum(bool ok, double v) { return ok ? String(v, 6) : String("null"); }

static String buildStatusPayload(const char *status, const Fix &f) {
  String s = "{\"childId\":\""; s += CHILD_ID;
  s += "\",\"deviceId\":\""; s += DEVICE_ID;
  s += "\",\"status\":\""; s += status;
  s += "\",\"latitude\":"; s += jnum(f.ok, f.lat);
  s += ",\"longitude\":"; s += jnum(f.ok, f.lng);
  s += ",\"timestamp\":\""; s += timestampNow(); s += "\"}";
  return s;
}

// extra = already-formatted fields starting with a comma, e.g. ",\"batteryLevel\":18"
static String buildEventPayload(const char *eventType, const String &extra) {
  String s = "{\"childId\":\""; s += CHILD_ID;
  s += "\",\"deviceId\":\""; s += DEVICE_ID;
  s += "\",\"eventType\":\""; s += eventType; s += "\"";
  s += extra;
  s += ",\"timestamp\":\""; s += timestampNow(); s += "\"}";
  return s;
}

// =====================================================================
// 6. OUTBOX / OFFLINE QUEUE  (latest event of each kind is kept, saved in flash)
// =====================================================================
enum SlotId { SLOT_STATUS = 0, SLOT_BATT = 1, SLOT_GPS = 2, SLOT_COUNT = 3 };   // priority order
static const char *SLOT_KEYS[SLOT_COUNT] = {"q0", "q1", "q2"};

struct Slot { bool used = false; String payload; uint32_t seq = 0; };
static Slot gSlots[SLOT_COUNT];
static uint32_t gSeqCounter = 0;
static volatile bool gKick = false;                 // try to send right away
static volatile bool gConnRestoredPending = false;

// UI notifications from the network task
enum UiEvt { UE_PRESS_SENT, UE_PRESS_FAILED, UE_DELIVERED_LATER, UE_ACK };
struct UiMsg { UiEvt type; int arg; };
static UiMsg gUiQ[8];
static int gUiHead = 0, gUiTail = 0;

static void pushUi(UiEvt t, int arg = 0) {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  int next = (gUiHead + 1) % 8;
  if (next != gUiTail) { gUiQ[gUiHead] = {t, arg}; gUiHead = next; }
  xSemaphoreGive(gMutex);
}
static bool popUi(UiMsg &m) {
  bool ok = false;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  if (gUiTail != gUiHead) { m = gUiQ[gUiTail]; gUiTail = (gUiTail + 1) % 8; ok = true; }
  xSemaphoreGive(gMutex);
  return ok;
}

// Queue an event for sending. Returns its sequence number.
static uint32_t submit(SlotId slot, const String &payload) {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  gSlots[slot].used = true;
  gSlots[slot].payload = payload;
  gSlots[slot].seq = ++gSeqCounter;
  prefs.putString(SLOT_KEYS[slot], payload);
  uint32_t seq = gSlots[slot].seq;
  xSemaphoreGive(gMutex);
  gKick = true;
  Serial.printf("[SafeSignal] queued %s\n", payload.c_str());
  return seq;
}

static void loadQueueFromFlash() {
  for (int i = 0; i < SLOT_COUNT; i++) {
    String p = prefs.getString(SLOT_KEYS[i], "");
    if (p.length() > 2) { gSlots[i].used = true; gSlots[i].payload = p; gSlots[i].seq = ++gSeqCounter; }
  }
}

static int pendingCount() {
  int n = 0;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  for (int i = 0; i < SLOT_COUNT; i++) if (gSlots[i].used) n++;
  xSemaphoreGive(gMutex);
  return n;
}

// =====================================================================
// 7. NETWORK  (runs in its own FreeRTOS task so the screen never freezes)
// =====================================================================
enum SendResult { R_SENT, R_QUEUED, R_DROPPED };

static void setOnline(bool on) {
  int8_t prev = gOnline;
  if ((int8_t)on == prev) return;
  gOnline = on ? 1 : 0;
  if (!on) gOfflineSince = timestampNow();
  else if (prev == 0) gConnRestoredPending = true;     // offline -> online
  Serial.printf("[SafeSignal] connection: %s\n", on ? "ONLINE" : "OFFLINE");
}

// ---- caregiver acknowledgement: {"acknowledgement":"ON_MY_WAY","ackId":"..."} in any response ----
static String jsonGetString(const String &body, const char *key) {
  String k = String("\"") + key + "\"";
  int i = body.indexOf(k);
  if (i < 0) return "";
  i = body.indexOf(':', i + k.length());
  if (i < 0) return "";
  int q1 = body.indexOf('"', i + 1);
  if (q1 < 0) return "";
  // value must start right after the colon (allow whitespace) - reject numbers/objects
  for (int j = i + 1; j < q1; j++) if (body[j] != ' ' && body[j] != '\n' && body[j] != '\r' && body[j] != '\t') return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

static String gLastAckKey;
static int ackCode(const String &a) {
  if (a == "CHILD_IS_SAFE") return 1;
  if (a == "ON_MY_WAY") return 2;
  if (a == "NEED_NEARBY_HELP") return 3;
  return 0;
}
static void handleAck(const String &body) {
  String a = jsonGetString(body, "acknowledgement");
  if (a == "") a = jsonGetString(body, "ack");
  int code = ackCode(a);
  if (!code) return;
  String key = a + ":" + jsonGetString(body, "ackId");
  if (key == gLastAckKey) return;           // already shown
  gLastAckKey = key;
  pushUi(UE_ACK, code);
}

// Low-level POST. Returns false if the backend could not be reached (network failure).
// Any HTTP answer (even 4xx/5xx) counts as reachable.
static bool doPost(const String &body, int &code, String &resp) {
  code = 0;
  if (SIM.offline || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  if (TLS_INSECURE) client.setInsecure();
  HTTPClient http;
  http.setTimeout(REQUEST_TIMEOUT_MS);
  http.setConnectTimeout(REQUEST_TIMEOUT_MS);
  if (!http.begin(client, API_ENDPOINT)) return false;
  http.addHeader("Content-Type", "application/json");
  if (EXTRA_HEADER_NAME[0]) http.addHeader(EXTRA_HEADER_NAME, EXTRA_HEADER_VALUE);
  code = http.POST(body);
  if (code <= 0) { http.end(); code = 0; return false; }
  resp = http.getString();
  http.end();
  return true;
}

// slot < 0 = never queued (HEARTBEAT, CONNECTIVITY_RESTORED).
// Status buttons are always kept and retried. System events are kept on network/5xx/408/429, dropped on other 4xx.
static int gLastHttpCode = 0;                        // 0 = network failure on the last attempt
static SendResult sendPayload(const String &payload, int slot, bool quiet) {
  int code; String resp;
  bool reached = doPost(payload, code, resp);
  gLastHttpCode = reached ? code : 0;
  if (!reached) {
    if (gOnline == 1) setOnline(false);              // (unknown state stays unknown until first contact)
    return slot >= 0 ? R_QUEUED : R_DROPPED;
  }
  setOnline(true);
  if (!quiet || code < 200 || code >= 300)
    Serial.printf("[SafeSignal] POST -> %d %s\n", code, resp.substring(0, 160).c_str());
  if (code >= 200 && code < 300) { handleAck(resp); return R_SENT; }
  if (slot >= 0 && (slot == SLOT_STATUS || code >= 500 || code == 408 || code == 429)) return R_QUEUED;
  return R_DROPPED;
}

static uint32_t gPressSeq = 0;                       // seq of the status event the child just sent
static volatile bool gPressReported = false;

// Deliver everything queued, highest priority first.
static void flushOutbox() {
  for (int i = 0; i < SLOT_COUNT; i++) {
    String payload; uint32_t seq;
    xSemaphoreTake(gMutex, portMAX_DELAY);
    bool used = gSlots[i].used;
    payload = gSlots[i].payload; seq = gSlots[i].seq;
    xSemaphoreGive(gMutex);
    if (!used) continue;

    SendResult r = sendPayload(payload, i, false);
    bool isPress = (i == SLOT_STATUS && seq == gPressSeq);
    if (r == R_QUEUED) {
      if (isPress && !gPressReported) { gPressReported = true; pushUi(UE_PRESS_FAILED); }
      break;                                          // still failing - try again next tick
    }
    xSemaphoreTake(gMutex, portMAX_DELAY);
    if (gSlots[i].used && gSlots[i].seq == seq) { gSlots[i].used = false; prefs.remove(SLOT_KEYS[i]); }
    xSemaphoreGive(gMutex);
    if (r == R_SENT && i == SLOT_STATUS) {
      if (isPress && !gPressReported) { gPressReported = true; pushUi(UE_PRESS_SENT); }
      else pushUi(UE_DELIVERED_LATER);
    }
  }
}

static void networkTask(void *) {
  uint32_t lastHb = 0, lastTry = 0, lastWifi = 0, lastRestored = 0;
  bool ntpStarted = false;
  for (;;) {
    uint32_t now = millis();

    if (SIM.offline && gOnline == 1) setOnline(false);   // dev: simulated "internet off"

    // Wi-Fi up / reconnect
    if (WiFi.status() != WL_CONNECTED) {
      if (gOnline == 1) setOnline(false);
      if (now - lastWifi > WIFI_RETRY_MS || lastWifi == 0) {
        lastWifi = now ? now : 1;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    } else if (!ntpStarted) {
      ntpStarted = true;
      configTime(TZ_OFFSET_MIN * 60, 0, "pool.ntp.org", "time.google.com");
    }

    // Queued events: right away when new, otherwise every RETRY_INTERVAL_MS
    if (pendingCount() > 0 && (gKick || now - lastTry >= RETRY_INTERVAL_MS)) {
      gKick = false; lastTry = now;
      flushOutbox();
    }

    // Back online: after the queue is delivered, tell Base44 the connection returned
    if (gConnRestoredPending && pendingCount() == 0 && gOnline == 1 && now - lastRestored >= 2000) {
      lastRestored = now;
      String extra = ",\"offlineSince\":\"" + gOfflineSince + "\"";
      SendResult r = sendPayload(buildEventPayload("CONNECTIVITY_RESTORED", extra), -1, false);
      if (r == R_SENT || gLastHttpCode > 0) gConnRestoredPending = false;   // done, or backend answered (even a 4xx)
    }

    // Heartbeat. An offline watch cannot report that it is offline, so Base44 treats
    // several missed beats (about 45 s) as "connectivity lost".
    if (now - lastHb >= HEARTBEAT_MS || lastHb == 0) {
      lastHb = now ? now : 1;
      sendPayload(buildEventPayload("HEARTBEAT", ""), -1, true);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =====================================================================
// 8. MONITORS  (battery, GPS)  - called from loop()
// =====================================================================
static bool gLowSent = false;
static bool gGpsLostSent = false;
static uint32_t gLastValidFix = 0, gLastBattCheck = 0, gLastGpsCheck = 0;

static void evalBattery() {
  int level = SIM.battery >= 0 ? SIM.battery : hal_batteryPercent();
  gBattery = level;
  if (level < 0) return;                              // not supported -> shows n/a, nothing sent
  if (level < LOW_BATTERY_PERCENT) {
    if (!gLowSent) {                                  // only once per dip below the threshold
      gLowSent = true;
      submit(SLOT_BATT, buildEventPayload("LOW_BATTERY", ",\"batteryLevel\":" + String(level)));
    }
  } else {
    gLowSent = false;                                 // recovered -> arm for the next dip
  }
}

static void gpsTick() {
  Fix f = getFix();
  if (f.ok) {
    gLastValidFix = millis();
    gGpsOk = 1;
    if (gGpsLostSent) {
      gGpsLostSent = false;
      submit(SLOT_GPS, buildEventPayload("GPS_RESTORED",
             ",\"latitude\":" + jnum(true, f.lat) + ",\"longitude\":" + jnum(true, f.lng)));
    }
  } else {
    if (gGpsOk == -1) gGpsOk = 0;
    if (millis() - gLastValidFix >= GPS_LOSS_MS && !gGpsLostSent) {
      gGpsLostSent = true;
      gGpsOk = 0;
      submit(SLOT_GPS, buildEventPayload("GPS_LOST",
             ",\"lastKnownLatitude\":" + jnum(gHaveLastKnown, gLastLat) +
             ",\"lastKnownLongitude\":" + jnum(gHaveLastKnown, gLastLng)));
    }
  }
}

// =====================================================================
// 9. USER INTERFACE  (240 x 320, buttons A / B)
// =====================================================================
struct Choice { const char *status; const char *label; uint32_t color; };
static const Choice CHOICES[3] = {
  {"OK",   "I'M OK",   COL_GREEN},
  {"LOST", "I'M LOST", COL_ORANGE},
  {"HELP", "HELP",     COL_RED},
};
static int gSel = 0;                                   // highlighted choice

// Full-screen message
static bool     gOverlay = false;
static uint32_t gOverlayUntil = 0;
static uint32_t gOverlayColor = COL_GREY;
static String   gOvTitle, gOvL1, gOvL2;
static bool     gDirty = true;

static uint32_t gCooldownUntil = 0;
static bool     gAwaitingPress = false;                // overlay "Sending..." until the network task reports
static uint32_t gAwaitingSince = 0;

static void showOverlay(uint32_t color, const String &t, const String &l1, const String &l2, uint32_t ms) {
  gOverlay = true; gOverlayColor = color; gOvTitle = t; gOvL1 = l1; gOvL2 = l2;
  gOverlayUntil = millis() + ms; gDirty = true;
}

static void drawMain() {
  hal_clear();
  textCentered("SafeSignal", 4, COL_WHITE, 24);
  textCentered("Tap how you feel", 34, COL_LGREY, 16);
  bool on = (gOnline == 1);
  textCentered(on ? "Connected" : "Offline", 54, on ? COL_CONN : COL_OFF, 16);

  int y = 78, h = 62, gap = 6;
  for (int i = 0; i < 3; i++) {
    if (i == gSel) hal_rect(2, y - 4, SCREEN_W - 4, h + 8, COL_WHITE);       // selection frame
    hal_rect(8, y, SCREEN_W - 16, h, CHOICES[i].color);
    // the highlighted choice is shown as "> LABEL <" (A moves it, B sends it)
    String label = (i == gSel) ? String("> ") + CHOICES[i].label + " <" : String(CHOICES[i].label);
    textCentered(label.c_str(), y + (h - 24) / 2, COL_WHITE, 24);
    y += h + gap + 4;
  }

  char line[40];
  int b = gBattery;
  if (b < 0) snprintf(line, sizeof(line), "Batt:n/a"); else snprintf(line, sizeof(line), "Batt:%d%%", b);
  hal_text(line, 4, 300, COL_LGREY, 16);
  snprintf(line, sizeof(line), "GPS:%s", gGpsOk == 1 ? "OK" : gGpsOk == 0 ? "X" : "...");
  hal_text(line, 100, 300, COL_LGREY, 16);
  hal_circle(224, 309, 7, gOnline == 1 ? COL_CONN : gOnline == 0 ? COL_OFF : 0x777777);
  hal_present();
}

static void drawOverlay() {
  hal_clear();
  hal_rect(0, 0, SCREEN_W, SCREEN_H, gOverlayColor);
  int y = textWrapped(gOvTitle, 100, COL_WHITE, 24);
  if (gOvL1.length()) y = textWrapped(gOvL1, y + 14, COL_WHITE, 16);
  if (gOvL2.length()) textWrapped(gOvL2, y + 8, COL_WHITE, 16);
  hal_present();
}

static void pressChoice(int idx) {
  if (millis() < gCooldownUntil || gOverlay) return;   // duplicate-press protection
  gCooldownUntil = millis() + COOLDOWN_MS;
  Fix f = getFix();                                    // never fails; nulls in payload if no GPS
  String payload = buildStatusPayload(CHOICES[idx].status, f);
  Serial.printf("[SafeSignal] Sending SafeSignal event:\n%s\n", payload.c_str());
  gPressReported = false;
  gPressSeq = submit(SLOT_STATUS, payload);
  gAwaitingPress = true; gAwaitingSince = millis();
  showOverlay(CHOICES[idx].color, "Sending...", "", "", 12000);
}

static void handleUiEvents() {
  UiMsg m;
  while (popUi(m)) {
    const Choice &c = CHOICES[gSel];
    switch (m.type) {
      case UE_PRESS_SENT:
        gAwaitingPress = false;
        if (gSel == 0)      showOverlay(c.color, "You're safe", "Caregiver updated", "", 2500);
        else if (gSel == 1) showOverlay(c.color, "Caregiver notified", "", "Caregiver updated", 2500);
        else                showOverlay(c.color, "HELP SENT", "Caregiver notified", "", 2500);
        break;
      case UE_PRESS_FAILED:
        gAwaitingPress = false;
        showOverlay(COL_GREY, "Unable to send", "retrying...", "", 2500);
        break;
      case UE_DELIVERED_LATER:
        showOverlay(COL_GREEN, "Caregiver updated", "", "", 2500);
        break;
      case UE_ACK:
        if (m.arg == 1)      showOverlay(COL_BLUE, "You're safe", "Caregiver says OK", "", 6000);
        else if (m.arg == 2) showOverlay(COL_BLUE, "Help is coming", "Caregiver is on the way", "", 6000);
        else                 showOverlay(COL_BLUE, "Help is coming", "Someone nearby is helping", "", 6000);
        break;
    }
  }
}

// =====================================================================
// 10. DEV COMMANDS (Serial Monitor)
// =====================================================================
static void devCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'b': SIM.battery = 18; evalBattery(); Serial.println("DEV: battery 18%"); break;
      case 'B': SIM.battery = 78; evalBattery(); Serial.println("DEV: battery 78%"); break;
      case 'g': SIM.gpsLost = true;  gLastValidFix = millis() - GPS_LOSS_MS; gpsTick(); Serial.println("DEV: GPS lost"); break;
      case 'G': SIM.gpsLost = false; gpsTick(); Serial.println("DEV: GPS restored"); break;
      case 'o': SIM.offline = true;  Serial.println("DEV: internet OFF (simulated)"); break;
      case 'O': SIM.offline = false; gKick = true; Serial.println("DEV: internet ON"); break;
      case '1': pushUi(UE_ACK, 1); break;
      case '2': pushUi(UE_ACK, 2); break;
      case '3': pushUi(UE_ACK, 3); break;
      case 'r': SIM.battery = -1; SIM.gpsLost = false; SIM.offline = false; Serial.println("DEV: reset simulations"); break;
      default: break;
    }
  }
}

// =====================================================================
// 11. setup() / loop()
// =====================================================================
static bool gPrevA = false, gPrevB = false;
static int8_t gDrawnOnline = -2, gDrawnGps = -2; static int gDrawnBatt = -2;

void setup() {
  Serial.begin(115200);
  gMutex = xSemaphoreCreateMutex();
  prefs.begin("safesignal", false);
  gHaveLastKnown = prefs.isKey("lkLat");
  if (gHaveLastKnown) { gLastLat = prefs.getDouble("lkLat", 0); gLastLng = prefs.getDouble("lkLng", 0); gSavedLat = gLastLat; gSavedLng = gLastLng; }
  loadQueueFromFlash();                                // unsent events survive a reboot

  hal_init();
  if (LOCATION_SOURCE == LOC_SERIAL_NMEA) Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  gLastValidFix = millis();
  xTaskCreatePinnedToCore(networkTask, "net", 12288, nullptr, 1, nullptr, 0);

  Serial.printf("[SafeSignal] K10 watch running. Endpoint: %s\n", API_ENDPOINT);
  Serial.println("DEV keys: b/B battery, g/G gps, o/O internet, 1/2/3 acks, r reset");
  evalBattery();
}

void loop() {
  uint32_t now = millis();
  devCommands();

  if (now - gLastBattCheck >= BATTERY_CHECK_MS) { gLastBattCheck = now; evalBattery(); }
  if (now - gLastGpsCheck >= GPS_CHECK_MS)      { gLastGpsCheck = now;  gpsTick(); }

  // Buttons (act on the press edge)
  bool a = hal_buttonA(), b = hal_buttonB();
  if (a && !gPrevA && !gOverlay) { gSel = (gSel + 1) % 3; gDirty = true; }
  if (b && !gPrevB) {
    if (gOverlay && !gAwaitingPress) gOverlayUntil = 0;            // B dismisses a message
    else pressChoice(gSel);
  }
  gPrevA = a; gPrevB = b;

  handleUiEvents();

  if (gAwaitingPress && now - gAwaitingSince > REQUEST_TIMEOUT_MS + 4000) {   // network task never answered
    gAwaitingPress = false;
    showOverlay(COL_GREY, "Unable to send", "retrying...", "", 2500);
  }
  if (gOverlay && (int32_t)(now - gOverlayUntil) >= 0) { gOverlay = false; gAwaitingPress = false; gDirty = true; }

  // Redraw only when something changed (updateCanvas is slow)
  if (gOnline != gDrawnOnline || gGpsOk != gDrawnGps || gBattery != gDrawnBatt) {
    gDrawnOnline = gOnline; gDrawnGps = gGpsOk; gDrawnBatt = gBattery; if (!gOverlay) gDirty = true;
  }
  if (gDirty) {
    gDirty = false;
    if (gOverlay) drawOverlay(); else drawMain();
  }
  delay(20);
}
