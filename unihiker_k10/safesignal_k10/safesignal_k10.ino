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
 * PAIRING WITH THE CAREGIVER APP:
 *   This watch has a FIXED Device ID and Pairing Code (see section 1: DEVICE_ID and FIXED_PAIR_CODE).
 *   They are shown on the watch's pairing screen and printed in the Serial Monitor. In the caregiver app's
 *   "Connect Your SafeSignal Device" form type  Device ID = SAFESIGNAL-001  and  Pairing Code = 482731
 *   (or whatever you set). The watch then shows "Paired" and from then on all events are sent for that
 *   child with a private device token (header X-Device-Token).
 *   Pairing screen: B = skip for now.  To pair again later: hold A + B for 3 seconds.
 *
 * Arduino IDE setup:
 *   1. Add the DFRobot UNIHIKER board package (Boards Manager -> "UNIHIKER"), select board "UNIHIKER K10".
 *   2. Fill in WIFI_SSID / WIFI_PASSWORD below, then Upload. Open Serial Monitor at 115200 baud.
 *
 * Dev/test commands (type a letter in the Serial Monitor, no hardware needed):
 *   b = battery 18%     B = battery 78%     g = GPS lost     G = GPS restored
 *   o = internet off    O = internet on     1/2/3 = show caregiver acks (safe / on my way / nearby help)
 *   r = reset all simulations     p = fake "paired" reply     u = unpair
 *   c = live camera ON now     x = live camera OFF (back to main screen)
 *   t = SERVER TEST: sends the same PAIR_REQUEST as the curl test (deviceId test-1, code 123456) and prints
 *       the HTTP code + reply. Expected: 200 {"ok":true,"paired":false}. "user-exception" = the Base44 function crashed.
 *
 * LOW BATTERY: sends LOW_BATTERY to the caregiver app once when the battery reaches 20% or lower
 *   (re-arms when it goes back above 20%). Needs a battery reading, see hal_batteryPercent() below.
 *
 * LIVE CAMERA (caregiver switches it on and off in the app):
 *   Caregiver taps "Turn on live camera" -> Base44 answers the watch with cameraCommand START -> the K10 camera
 *   turns ON, the K10 screen shows the live picture (red "LIVE CAMERA" bar) and the watch streams frames to Base44
 *   as LIVE_FRAME events (small JPEGs, about 1-3 per second = "near-live" video) which the app shows.
 *   Caregiver taps "Turn off live camera" -> cameraCommand STOP arrives in the reply to the next frame (about
 *   1 s) -> camera OFF, streaming stops and the K10 returns to its MAIN SCREEN.
 *   The child pressing I'M OK / I'M LOST / HELP also starts the live camera (LIVE_ON_PRESS) and a saved photo is
 *   taken at that moment. cameraCommand CAPTURE_NOW (the app's "Photo" button) saves one photo.
 *   Press A or B during live view to bring the buttons back for 10 s (the child can always press HELP).
 *   Safety: the camera also stops after CAMERA_MAX_SESSION_MS (30 min) or if frames cannot be delivered for 60 s.
 *   A red "CAM" marker is shown whenever the camera is on.
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
#include "mbedtls/base64.h"
#if __has_include("esp_camera.h")
  #include "esp_camera.h"
  #include "img_converters.h"
  #define HAVE_ESP_CAMERA 1
#endif
// Battery of the K10 expansion board (18650): install DFRobot's "DFRobot_UnihikerExpansion" library to enable it.
#if __has_include("DFRobot_UnihikerExpansion.h")
  #include "DFRobot_UnihikerExpansion.h"
  #define HAVE_EXPANSION_BATTERY 1
#endif

// =====================================================================
// 1. CONFIG  (edit this section)
// =====================================================================
static const char *WIFI_SSID     = "YOUR_WIFI_NAME";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

static const char *API_ENDPOINT = "https://safesignal-caregiver-to-watch-acrn.base44.app/functions/watchStatus";
// CHILD_ID is only the default before pairing; after pairing the child id comes from the caregiver app.
static const char *CHILD_ID     = "child_001";
// ---- LINK DETAILS FOR THIS WATCH: type these two values into the caregiver app's "Connect Your SafeSignal Device" form ----
static const char *DEVICE_ID = "SAFESIGNAL-001";          // fixed Device ID of this K10 (unique per watch)
static const bool  USE_FIXED_PAIR_CODE = true;            // true = always the same code below; false = random code each time
static const char *FIXED_PAIR_CODE     = "482731";        // fixed 6-digit Pairing Code of this K10
// Extra header if Base44 needs one (leave EXTRA_HEADER_NAME empty for none), e.g. "api_key" / "xxxx"
static const char *EXTRA_HEADER_NAME  = "";
static const char *EXTRA_HEADER_VALUE = "";

// TLS: the prototype does NOT verify the server certificate (setInsecure). Traffic is encrypted but a
// man-in-the-middle is not detected. For a real product load the CA certificate instead.
static const bool TLS_INSECURE = true;

static const uint32_t COOLDOWN_MS         = 2500;   // ignore presses for this long after a press
static const uint32_t RETRY_INTERVAL_MS   = 5000;   // retry unsent events this often
static const uint32_t HEARTBEAT_MS        = 5000;   // "I'm alive" ping AND how often the watch checks for app commands (Start/Stop/Photo). 15000 = slower but lighter
static const int      LOW_BATTERY_PERCENT = 20;     // LOW_BATTERY event when the level reaches this or lower
static const uint32_t BATTERY_CHECK_MS    = 10000;
static const uint32_t GPS_CHECK_MS        = 5000;   // how often location is sampled
static const uint32_t GPS_LOSS_MS         = 30000;  // no valid location for this long => GPS_LOST
static const uint32_t REQUEST_TIMEOUT_MS  = 8000;
static const uint32_t WIFI_RETRY_MS       = 8000;
static const uint32_t PAIR_POLL_MS        = 3000;   // while pairing: tell Base44 our code / ask if linked
static const uint32_t PAIR_CODE_LIFE_MS   = 600000; // code expires after 10 minutes (a new one is made)
static const uint32_t UNPAIR_HOLD_MS      = 3000;   // hold A+B this long to pair again

// ---- CAMERA ----
static const bool     CAMERA_ENABLED          = true;      // false = never use the camera
static const uint32_t CAMERA_INTERVAL_MS      = 0;         // 0 = no periodic saved photos (live video instead). e.g. 60000 = also save one photo a minute
static const uint32_t CAMERA_MAX_SESSION_MS   = 1800000UL; // auto-stop after 30 min (the caregiver can stop it earlier)
static const int      CAMERA_JPEG_QUALITY     = 15;        // 1-63, LOWER = better quality / bigger file
static const bool     CAMERA_LIVE_PREVIEW     = true;      // show the live camera picture on the K10 screen while the camera is on
static const uint32_t BUTTONS_HOLD_MS         = 10000;     // pressing A/B during live view shows the buttons screen this long
static const bool     LIVE_ON_PRESS           = true;      // child presses OK / LOST / HELP -> live camera starts too
static const int      LIVE_JPEG_QUALITY       = 25;        // live frames: 1-63, higher = smaller & faster
static const uint32_t LIVE_FRAME_MIN_MS       = 250;       // never send frames faster than this (real speed = upload speed, ~1-3 fps)
static const uint32_t LIVE_NOLINK_STOP_MS     = 60000;     // camera stops if no frame could be delivered for this long
static const uint32_t IMAGE_TIMEOUT_MS        = 20000;     // photo upload timeout
static const int      IMAGE_MAX_TRIES         = 4;         // then the photo is dropped (a newer one replaces it anyway)

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
// Battery percent 0-100, or -1 if no reading is available (then no LOW_BATTERY alert can be sent).
// Uses the K10 expansion-board battery from DFRobot's "Battery capacity" example (eunihiker.getBattery()).
// If you read the battery another way, replace the body of this function.
static int hal_batteryPercent() {
#ifdef HAVE_EXPANSION_BATTERY
  static bool started = false;
  if (!started) { eunihiker.begin(); started = true; }
  int p = (int)eunihiker.getBattery();
  return (p >= 0 && p <= 100) ? p : -1;
#else
  return -1;
#endif
}

// ---- camera (uses the K10 camera through the standard esp32-camera driver) ----
// !! These calls were written without the board: if they do not compile / no photo appears, only this block needs fixing.
static void *bigAlloc(size_t n) { void *p = psramFound() ? ps_malloc(n) : nullptr; return p ? p : malloc(n); }

static bool hal_cameraStart() {
#ifdef HAVE_ESP_CAMERA
  k10.initBgCamerImage();            // brings up the K10 camera
  k10.setBgCamerImage(false);        // preview is switched on/off by hal_cameraPreview()
  return true;
#else
  return false;
#endif
}
// Live camera picture behind the SafeSignal screen (true) or plain black background (false).
static void hal_cameraPreview(bool on) { k10.setBgCamerImage(on); }
static void hal_cameraStop() { hal_cameraPreview(false); /* camera stays initialised; we simply stop capturing */ }
// Takes one photo. On success *jpg is a bigAlloc'd JPEG (caller frees with free()) and *len its size.
static bool hal_cameraCapture(uint8_t **jpg, size_t *len, int quality) {
#ifdef HAVE_ESP_CAMERA
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  bool ok = false;
  if (fb->format == PIXFORMAT_JPEG) {
    *jpg = (uint8_t *)bigAlloc(fb->len);
    if (*jpg) { memcpy(*jpg, fb->buf, fb->len); *len = fb->len; ok = true; }
  } else {
    ok = frame2jpg(fb, quality, jpg, len);       // RGB565 etc. -> JPEG
  }
  esp_camera_fb_return(fb);
  return ok;
#else
  return false;
#endif
}

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
static bool   gDirty = true;               // screen needs redrawing
static volatile bool gCamActive = false;   // camera session on (shared with the network task / heartbeat)

// ---- pairing ----
enum Mode { MODE_PAIRING, MODE_MAIN };
static Mode gMode = MODE_MAIN;
static bool gPaired = false;
static String gChildId;                    // set in setup() (CHILD_ID or the paired child)
static String gChildName;
static String gDeviceToken;                // sent as X-Device-Token once paired
static char gPairCode[12] = "000000";
static uint32_t gPairCodeAt = 0;
static volatile bool gPairBusy = false;
static volatile int gPairHttp = 0;         // last HTTP status from the pairing poll (0 = no connection)

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
  String s = "{\"childId\":\""; s += gChildId;
  s += "\",\"deviceId\":\""; s += DEVICE_ID;
  s += "\",\"status\":\""; s += status;
  s += "\",\"latitude\":"; s += jnum(f.ok, f.lat);
  s += ",\"longitude\":"; s += jnum(f.ok, f.lng);
  s += ",\"timestamp\":\""; s += timestampNow(); s += "\"}";
  return s;
}

// extra = already-formatted fields starting with a comma, e.g. ",\"batteryLevel\":18"
static String buildEventPayload(const char *eventType, const String &extra) {
  String s = "{\"childId\":\""; s += gChildId;
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
enum UiEvt { UE_PRESS_SENT, UE_PRESS_FAILED, UE_DELIVERED_LATER, UE_ACK, UE_PAIRED };
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
// Camera commands from the caregiver app, set by the network task and executed in loop():
//   {"cameraCommand":"START"|"STOP"|"CAPTURE_NOW"} in any response
enum CamCmd { CAMCMD_NONE = 0, CAMCMD_START = 1, CAMCMD_STOP = 2, CAMCMD_CAPTURE = 3 };
static volatile int gCamCmd = CAMCMD_NONE;

static void handleAck(const String &body) {
  String cc = jsonGetString(body, "cameraCommand");
  if (cc != "") Serial.printf("[SafeSignal] app command received: cameraCommand = %s\n", cc.c_str());
  if (cc == "START") gCamCmd = CAMCMD_START;
  else if (cc == "STOP") gCamCmd = CAMCMD_STOP;
  else if (cc == "CAPTURE_NOW") gCamCmd = CAMCMD_CAPTURE;

  String a = jsonGetString(body, "acknowledgement");
  if (a == "") a = jsonGetString(body, "ack");
  int code = ackCode(a);
  if (!code) return;
  String key = a + ":" + jsonGetString(body, "ackId");
  if (key == gLastAckKey) return;           // already shown
  gLastAckKey = key;
  pushUi(UE_ACK, code);
}

// Low-level POST of raw bytes. Returns false if the backend could not be reached (network failure).
// Any HTTP answer (even 4xx/5xx) counts as reachable.
static bool doPostBytes(const uint8_t *data, size_t len, int &code, String &resp, uint32_t timeoutMs) {
  code = 0;
  if (SIM.offline || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  if (TLS_INSECURE) client.setInsecure();
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(REQUEST_TIMEOUT_MS);
  if (!http.begin(client, API_ENDPOINT)) return false;
  http.addHeader("Content-Type", "application/json");
  if (EXTRA_HEADER_NAME[0]) http.addHeader(EXTRA_HEADER_NAME, EXTRA_HEADER_VALUE);
  if (gDeviceToken.length()) http.addHeader("X-Device-Token", gDeviceToken.c_str());   // proves this is the paired watch
  code = http.POST((uint8_t *)data, len);
  if (code <= 0) { http.end(); code = 0; return false; }
  resp = http.getString();
  http.end();
  return true;
}
static bool doPost(const String &body, int &code, String &resp) {
  return doPostBytes((const uint8_t *)body.c_str(), body.length(), code, resp, REQUEST_TIMEOUT_MS);
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

// ---- pairing: new code, save/clear, poll Base44 ----
static void newPairCode() {
  if (USE_FIXED_PAIR_CODE) {
    snprintf(gPairCode, sizeof(gPairCode), "%s", FIXED_PAIR_CODE);
  } else {
    uint32_t r = (uint32_t)random(1000000);
    snprintf(gPairCode, sizeof(gPairCode), "%06lu", (unsigned long)r);
  }
  gPairCodeAt = millis();
  Serial.printf("[SafeSignal] >>> LINK THIS WATCH IN THE APP:  Device ID = %s   Pairing Code = %s\n", DEVICE_ID, gPairCode);
}

static void savePairing() {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  prefs.putBool("paired", true);
  prefs.putString("childId", gChildId);
  prefs.putString("childName", gChildName);
  prefs.putString("token", gDeviceToken);
  xSemaphoreGive(gMutex);
}

static void clearPairing() {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  prefs.putBool("paired", false);
  prefs.remove("childId"); prefs.remove("childName"); prefs.remove("token");
  xSemaphoreGive(gMutex);
  gPaired = false; gChildId = CHILD_ID; gChildName = ""; gDeviceToken = "";
}

// Reply from Base44: {"paired":true,"childId":"...","childName":"Alex","deviceToken":"..."}
static bool jsonHasTrue(const String &body, const char *key) {
  int i = body.indexOf(String("\"") + key + "\"");
  if (i < 0) return false;
  i = body.indexOf(':', i);
  if (i < 0) return false;
  while (++i < (int)body.length() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n')) {}
  return body.substring(i, i + 4) == "true";
}

// Called every PAIR_POLL_MS while the pairing screen is up: registers our code and checks if the caregiver linked us.
static void pairPoll() {
  String body = "{\"deviceId\":\"" + String(DEVICE_ID) + "\",\"eventType\":\"PAIR_REQUEST\",\"pairingCode\":\"" +
                String(gPairCode) + "\",\"timestamp\":\"" + timestampNow() + "\"}";
  int code; String resp;
  bool reached = doPost(body, code, resp);
  gPairHttp = reached ? code : 0;
  if (!reached) { if (gOnline == 1) setOnline(false); return; }
  setOnline(true);
  static int lastLoggedCode = -1;
  if (code != lastLoggedCode) { lastLoggedCode = code; Serial.printf("[SafeSignal] pairing server replied %d: %s\n", code, resp.substring(0, 200).c_str()); }
  if (code == 409 && !USE_FIXED_PAIR_CODE) { newPairCode(); return; }   // someone else has this code: pick another
  if (code < 200 || code >= 300) { Serial.printf("[SafeSignal] pairing -> %d %s\n", code, resp.substring(0, 120).c_str()); return; }
  if (!jsonHasTrue(resp, "paired")) return;
  String cid = jsonGetString(resp, "childId"), tok = jsonGetString(resp, "deviceToken");
  if (cid == "" || tok == "") return;
  gChildId = cid; gDeviceToken = tok; gChildName = jsonGetString(resp, "childName");
  gPaired = true;
  savePairing();
  Serial.printf("[SafeSignal] PAIRED with child %s (%s)\n", gChildId.c_str(), gChildName.c_str());
  pushUi(UE_PAIRED);
}

// Same request as the curl test. Run it from the Serial Monitor with the letter t.
static void serverTest() {
  String body = "{\"eventType\":\"PAIR_REQUEST\",\"deviceId\":\"test-1\",\"pairingCode\":\"123456\"}";
  int code; String resp;
  Serial.printf("[SafeSignal] SERVER TEST -> POST %s\n", API_ENDPOINT);
  bool reached = doPost(body, code, resp);
  if (!reached) { Serial.println("[SafeSignal] SERVER TEST: no connection (check Wi-Fi / URL)"); return; }
  Serial.printf("[SafeSignal] SERVER TEST: HTTP %d  reply: %s\n", code, resp.c_str());
  if (code == 200 && resp.indexOf("\"ok\":true") >= 0) Serial.println("[SafeSignal] SERVER TEST: OK - Base44 accepted it");
  else if (resp.indexOf("user-exception") >= 0) Serial.println("[SafeSignal] SERVER TEST: Base44 function crashed (user-exception) - the app behind this URL has old/broken watchStatus code");
  else Serial.println("[SafeSignal] SERVER TEST: Base44 answered but not as expected - see the reply above");
}

static uint32_t gPressSeq = 0;                       // seq of the status event the child just sent
static volatile bool gPressReported = false;

// ---- camera photo hand-over: loop() captures, the network task uploads (only the newest photo is kept) ----
static uint8_t *gImgBuf = nullptr;
static size_t   gImgLen = 0;
static char     gImgTrigger[8] = "AUTO";
static String   gImgTs;
static bool     gImgHasLoc = false;
static double   gImgLat = 0, gImgLng = 0;
static int      gImgTries = 0;
static volatile bool gImgPending = false;

// ---- LIVE video: loop() captures one frame at a time, the network task uploads it over a kept-open connection ----
static uint8_t *gLiveBuf = nullptr;
static size_t   gLiveLen = 0;
static bool     gLiveIsSnap = false;                 // this frame is also saved as a photo (CAMERA_IMAGE)
static char     gLiveTrig[8] = "LIVE";
static uint32_t gLiveSeq = 0;
static volatile bool gLivePending = false;
static volatile uint32_t gLastLiveOk = 0;            // last time a frame was delivered
static volatile bool gLiveCloseReq = false;
static WiFiClientSecure *gLiveClient = nullptr;
static HTTPClient *gLiveHttp = nullptr;

static void liveClose() {
  if (gLiveHttp) { gLiveHttp->end(); delete gLiveHttp; gLiveHttp = nullptr; }
  if (gLiveClient) { delete gLiveClient; gLiveClient = nullptr; }
}
// POST over the kept-open live connection (much faster than a new TLS handshake per frame).
static bool doPostLive(const uint8_t *data, size_t len, int &code, String &resp) {
  code = 0;
  if (SIM.offline || WiFi.status() != WL_CONNECTED) { liveClose(); return false; }
  if (!gLiveHttp) {
    gLiveClient = new WiFiClientSecure();
    if (TLS_INSECURE) gLiveClient->setInsecure();
    gLiveHttp = new HTTPClient();
    gLiveHttp->setReuse(true);
    gLiveHttp->setTimeout(IMAGE_TIMEOUT_MS);
    gLiveHttp->setConnectTimeout(REQUEST_TIMEOUT_MS);
    if (!gLiveHttp->begin(*gLiveClient, API_ENDPOINT)) { liveClose(); return false; }
    gLiveHttp->addHeader("Content-Type", "application/json");
    if (EXTRA_HEADER_NAME[0]) gLiveHttp->addHeader(EXTRA_HEADER_NAME, EXTRA_HEADER_VALUE);
    if (gDeviceToken.length()) gLiveHttp->addHeader("X-Device-Token", gDeviceToken.c_str());
  }
  code = gLiveHttp->POST((uint8_t *)data, len);
  if (code <= 0) { code = 0; liveClose(); return false; }      // reconnect on the next frame
  resp = gLiveHttp->getString();
  return true;
}

static void setPendingImage(uint8_t *jpg, size_t len, const char *trigger) {
  Fix f = getFix();
  xSemaphoreTake(gMutex, portMAX_DELAY);
  if (gImgBuf) free(gImgBuf);                      // a newer photo replaces an unsent one
  gImgBuf = jpg; gImgLen = len; gImgTries = 0;
  snprintf(gImgTrigger, sizeof(gImgTrigger), "%s", trigger);
  gImgTs = timestampNow();
  gImgHasLoc = f.ok; gImgLat = f.lat; gImgLng = f.lng;
  gImgPending = true;
  xSemaphoreGive(gMutex);
  Serial.printf("[SafeSignal] photo ready (%u bytes, trigger %s)\n", (unsigned)len, trigger);
}

// Upload the pending photo as a CAMERA_IMAGE event (JPEG as base64 inside the JSON).
static void uploadImage() {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  uint8_t *jpg = gImgBuf; size_t jlen = gImgLen;
  gImgBuf = nullptr; gImgPending = false;           // take ownership while we upload
  String trig = gImgTrigger, ts = gImgTs;
  bool hasLoc = gImgHasLoc; double lat = gImgLat, lng = gImgLng;
  int tries = gImgTries;
  xSemaphoreGive(gMutex);
  if (!jpg) return;

  String head = "{\"childId\":\"" + gChildId + "\",\"deviceId\":\"" + String(DEVICE_ID) +
                "\",\"eventType\":\"CAMERA_IMAGE\",\"trigger\":\"" + trig +
                "\",\"latitude\":" + jnum(hasLoc, lat) + ",\"longitude\":" + jnum(hasLoc, lng) +
                ",\"timestamp\":\"" + ts + "\",\"imageFormat\":\"jpeg\",\"imageBase64\":\"";
  size_t b64max = ((jlen + 2) / 3) * 4 + 4;
  size_t total = head.length() + b64max + 4;
  char *buf = (char *)bigAlloc(total);
  if (!buf) { Serial.println("[SafeSignal] photo: out of memory, dropped"); free(jpg); return; }
  memcpy(buf, head.c_str(), head.length());
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char *)buf + head.length(), b64max, &olen, jpg, jlen);
  size_t n = head.length() + olen;
  buf[n++] = '"'; buf[n++] = '}';

  int code; String resp;
  bool reached = doPostBytes((const uint8_t *)buf, n, code, resp, IMAGE_TIMEOUT_MS);
  free(buf);
  bool sent = reached && code >= 200 && code < 300;
  if (reached) { setOnline(true); gLastHttpCode = code; handleAck(resp); }
  else { gLastHttpCode = 0; if (gOnline == 1) setOnline(false); }
  Serial.printf("[SafeSignal] photo upload -> %s (HTTP %d)\n", sent ? "OK" : "FAILED", code);

  bool retry = !sent && (!reached || code >= 500 || code == 408 || code == 429) && tries + 1 < IMAGE_MAX_TRIES;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  if (retry && !gImgBuf) { gImgBuf = jpg; gImgLen = jlen; gImgTries = tries + 1; gImgPending = true; jpg = nullptr; }   // keep for a retry unless a newer photo arrived
  xSemaphoreGive(gMutex);
  if (jpg) free(jpg);
}

// Upload the pending live frame (LIVE_FRAME, or CAMERA_IMAGE when it is also a saved photo).
// The reply carries the caregiver's commands, so "Turn off live camera" reaches the watch within one frame.
static void uploadLive() {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  uint8_t *jpg = gLiveBuf; size_t jlen = gLiveLen;
  bool snap = gLiveIsSnap; uint32_t seq = gLiveSeq;
  String trig = gLiveTrig;
  gLiveBuf = nullptr; gLivePending = false;
  xSemaphoreGive(gMutex);
  if (!jpg) return;
  Fix f = getFix();
  String head = "{\"childId\":\"" + gChildId + "\",\"deviceId\":\"" + String(DEVICE_ID) +
                "\",\"eventType\":\"" + (snap ? "CAMERA_IMAGE" : "LIVE_FRAME") + "\",\"trigger\":\"" + trig +
                "\",\"seq\":" + String((int)seq) +
                ",\"latitude\":" + jnum(f.ok, f.lat) + ",\"longitude\":" + jnum(f.ok, f.lng) +
                ",\"cameraOn\":true,\"timestamp\":\"" + timestampNow() + "\",\"imageFormat\":\"jpeg\",\"imageBase64\":\"";
  size_t b64max = ((jlen + 2) / 3) * 4 + 4;
  char *buf = (char *)bigAlloc(head.length() + b64max + 4);
  if (!buf) { free(jpg); return; }
  memcpy(buf, head.c_str(), head.length());
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char *)buf + head.length(), b64max, &olen, jpg, jlen);
  size_t n = head.length() + olen;
  buf[n++] = '"'; buf[n++] = '}';
  free(jpg);

  int code; String resp;
  bool reached = doPostLive((const uint8_t *)buf, n, code, resp);
  free(buf);
  if (reached) {
    setOnline(true);
    gLastHttpCode = code;
    if (code >= 200 && code < 300) {
      gLastLiveOk = millis();
      static uint32_t nOk = 0;                      // progress line so you can see the stream is running
      if (++nOk == 1 || nOk % 20 == 0) Serial.printf("[SafeSignal] live frames delivered: %u\n", (unsigned)nOk);
    } else Serial.printf("[SafeSignal] live frame -> HTTP %d %s\n", code, resp.substring(0, 120).c_str());
    handleAck(resp);                                // picks up cameraCommand STOP etc.
  } else {
    gLastHttpCode = 0;
    if (gOnline == 1) setOnline(false);
  }
}

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
  uint32_t lastHb = 0, lastTry = 0, lastWifi = 0, lastRestored = 0, lastPair = 0, lastImg = 0;
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

    // Pairing screen: register our code with Base44 and wait for the caregiver to link it
    if (gMode == MODE_PAIRING && !gPaired && now - lastPair >= PAIR_POLL_MS) {
      lastPair = now;
      pairPoll();
    }

    // Queued events: right away when new, otherwise every RETRY_INTERVAL_MS
    if (pendingCount() > 0 && (gKick || now - lastTry >= RETRY_INTERVAL_MS)) {
      gKick = false; lastTry = now;
      flushOutbox();
    }

    // Live video frame waiting? (sent straight away, one after another)
    if (gLivePending && gOnline != 0) uploadLive();
    if ((!gCamActive || gLiveCloseReq) && gLiveHttp) { liveClose(); gLiveCloseReq = false; }   // camera off -> drop the live connection

    // Single saved photo waiting? (the "Photo" button while the camera was off)
    if (gImgPending && gOnline != 0 && now - lastImg >= 2000) {
      lastImg = now;
      uploadImage();
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
    bool liveFlowing = gCamActive && (now - gLastLiveOk < 8000);   // live frames already prove we are alive
    if ((now - lastHb >= HEARTBEAT_MS || lastHb == 0) && !liveFlowing) {
      lastHb = now ? now : 1;
      String hb = String(",\"cameraOn\":") + (gCamActive ? "true" : "false");
      if (gBattery >= 0) hb += ",\"batteryLevel\":" + String((int)gBattery);
      sendPayload(buildEventPayload("HEARTBEAT", hb), -1, true);
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
  if (level <= LOW_BATTERY_PERCENT) {
    if (!gLowSent) {                                  // only once when it reaches 20% or lower
      gLowSent = true;
      submit(SLOT_BATT, buildEventPayload("LOW_BATTERY", ",\"batteryLevel\":" + String(level)));
    }
  } else {
    gLowSent = false;                                 // back above 20% -> arm for the next time
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
// 8b. CAMERA SESSION  (live video on/off; started by the app's Start or by a button press)
// =====================================================================
static uint32_t gCamEndAt = 0, gCamNextAt = 0, gLiveLastCap = 0, gCamStartedAt = 0;
static char     gCamTrigger[8] = "AUTO";            // trigger label of the next SAVED photo (OK / LOST / HELP / REMOTE)
static bool     gCamBroken = false;                 // camera failed to start: only an app request retries
static uint32_t gButtonsUntil = 0;                  // during live view: show the buttons screen until this time
static uint32_t gLastCapAt = 0;                     // when the last photo was taken (ignores command repeats)
static bool     gPreviewOn = false;                 // is the live picture currently on the screen?
static bool     gSnapshotNext = false;              // next live frame is also saved as a photo
static bool     gOneShot = false;                   // "Photo" while the camera was off: take one photo, then camera off again
static bool     gOneShotTaken = false;

static void camStart(const char *firstTrigger, bool oneShot = false) {
  if (!CAMERA_ENABLED || gCamBroken) return;
  if (!gCamActive) {
    if (!hal_cameraStart()) { gCamBroken = true; Serial.println("[SafeSignal] camera: could not start (no camera driver) - camera disabled"); return; }
    gCamActive = true;
    gCamStartedAt = millis();
    gLastLiveOk = millis();
    Serial.println(oneShot ? "[SafeSignal] camera: ON (one photo)" : "[SafeSignal] camera: ON (live)");
  }
  gOneShot = oneShot; gOneShotTaken = false;
  gButtonsUntil = 0;                                // live view straight away (unless an overlay is showing)
  gCamEndAt = millis() + CAMERA_MAX_SESSION_MS;
  gCamNextAt = millis() + CAMERA_INTERVAL_MS;
  snprintf(gCamTrigger, sizeof(gCamTrigger), "%s", firstTrigger);
  gSnapshotNext = true;                             // the first frame is also saved as a photo
  gDirty = true;
}

static void camStop() {
  if (!gCamActive) return;
  gCamActive = false;
  gPreviewOn = false;                               // loop() then redraws the MAIN SCREEN
  gButtonsUntil = 0; gOneShot = false; gSnapshotNext = false;
  hal_cameraStop();
  gLiveCloseReq = true;
  Serial.println("[SafeSignal] camera: OFF - back to the main screen");
  gDirty = true;
}

// Grab one frame: live frames are small; a saved photo uses the better quality setting.
static void camCapture() {
  gLastCapAt = millis();
  bool snap = gSnapshotNext;
  uint8_t *jpg = nullptr; size_t len = 0;
  if (!hal_cameraCapture(&jpg, &len, snap ? CAMERA_JPEG_QUALITY : LIVE_JPEG_QUALITY) || !jpg) { Serial.println("[SafeSignal] camera: capture failed"); return; }
  if (gOneShot) {                                   // single photo path (own upload)
    setPendingImage(jpg, len, gCamTrigger);
    gOneShotTaken = true; gSnapshotNext = false;
    return;
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  if (gLiveBuf) free(gLiveBuf);
  gLiveBuf = jpg; gLiveLen = len; gLiveIsSnap = snap; gLiveSeq++;
  snprintf(gLiveTrig, sizeof(gLiveTrig), "%s", snap ? gCamTrigger : "LIVE");
  gLivePending = true;
  xSemaphoreGive(gMutex);
  if (snap) { gSnapshotNext = false; snprintf(gCamTrigger, sizeof(gCamTrigger), "AUTO"); }
}

// Called from loop(): runs the app's commands and feeds the live stream.
static void camTick() {
  int cmd = gCamCmd; gCamCmd = CAMCMD_NONE;
  if (cmd == CAMCMD_START || cmd == CAMCMD_CAPTURE) gCamBroken = false;   // an app request retries a failed camera start
  if (cmd == CAMCMD_START) { if (!gCamActive || gOneShot) camStart("REMOTE"); else gCamEndAt = millis() + CAMERA_MAX_SESSION_MS; }
  else if (cmd == CAMCMD_STOP) camStop();
  else if (cmd == CAMCMD_CAPTURE) {
    if (!gCamActive) camStart("REMOTE", true);                           // camera was off: one photo, then off again
    else if (millis() - gLastCapAt > 4000) { snprintf(gCamTrigger, sizeof(gCamTrigger), "REMOTE"); gSnapshotNext = true; }
  }
  if (!gCamActive) return;
  uint32_t now = millis();
  if ((int32_t)(now - gCamEndAt) >= 0) { camStop(); return; }            // session limit reached
  if (gOneShot) {                                                         // photo only
    if (!gOneShotTaken) camCapture();
    else if (!gImgPending && now - gLastCapAt > 1500) camStop();
    return;
  }
  if (now - gCamStartedAt > LIVE_NOLINK_STOP_MS && now - gLastLiveOk > LIVE_NOLINK_STOP_MS) {   // nobody can receive the video
    Serial.println("[SafeSignal] camera: no frame delivered for a minute - stopping");
    camStop(); return;
  }
  if (CAMERA_INTERVAL_MS > 0 && (int32_t)(now - gCamNextAt) >= 0) { gCamNextAt = now + CAMERA_INTERVAL_MS; gSnapshotNext = true; }
  if (!gLivePending && now - gLiveLastCap >= LIVE_FRAME_MIN_MS) { gLiveLastCap = now; camCapture(); }
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

static uint32_t gCooldownUntil = 0;
static bool     gAwaitingPress = false;                // overlay "Sending..." until the network task reports
static uint32_t gAwaitingSince = 0;

static void showOverlay(uint32_t color, const String &t, const String &l1, const String &l2, uint32_t ms) {
  gOverlay = true; gOverlayColor = color; gOvTitle = t; gOvL1 = l1; gOvL2 = l2;
  gOverlayUntil = millis() + ms; gDirty = true;
}

// Live view is on while the camera session runs, unless a message / the buttons screen is showing.
static bool previewWanted() {
  return CAMERA_LIVE_PREVIEW && gCamActive && !gOneShot && gMode == MODE_MAIN && !gOverlay && (int32_t)(millis() - gButtonsUntil) >= 0;
}

// Live camera picture is drawn by the K10 behind the canvas; we only add two small text bars.
static void drawLive() {
  hal_clear();
  hal_rect(0, 0, SCREEN_W, 26, 0x000000);
  hal_circle(12, 13, 6, COL_RED);
  hal_text("LIVE CAMERA", 26, 5, COL_RED, 16);
  hal_rect(0, 294, SCREEN_W, 26, 0x000000);
  textCentered("Press A or B for buttons", 298, COL_WHITE, 16);
  hal_present();
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
  if (gCamActive) { hal_circle(166, 309, 5, COL_RED); hal_text("CAM", 174, 300, COL_RED, 16); }   // camera is on
  hal_circle(224, 309, 7, gOnline == 1 ? COL_CONN : gOnline == 0 ? COL_OFF : 0x777777);
  hal_present();
}

static void drawPairing() {
  hal_clear();
  textCentered("SafeSignal", 4, COL_WHITE, 24);
  textCentered("Connect your device", 34, COL_LGREY, 16);
  textCentered(gOnline == 1 ? "Waiting for caregiver..." : "Connecting...", 54, gOnline == 1 ? COL_CONN : COL_OFF, 16);

  textCentered("Device ID", 80, COL_LGREY, 16);
  textCentered(DEVICE_ID, 100, COL_WHITE, 24);

  textCentered("Pairing Code", 138, COL_LGREY, 16);
  hal_rect(8, 158, SCREEN_W - 16, 52, COL_BLUE);
  char spaced[16];                                       // "482 731" with extra spacing
  snprintf(spaced, sizeof(spaced), "%c %c %c   %c %c %c", gPairCode[0], gPairCode[1], gPairCode[2], gPairCode[3], gPairCode[4], gPairCode[5]);
  textCentered(spaced, 172, COL_WHITE, 24);

  textCentered("Type both in the app", 224, COL_WHITE, 16);
  if (!USE_FIXED_PAIR_CODE) {
    int left = (int)((PAIR_CODE_LIFE_MS - (millis() - gPairCodeAt)) / 1000);
    if (left < 0) left = 0;
    char t[24]; snprintf(t, sizeof(t), "Code valid %d:%02d", left / 60, left % 60);
    textCentered(t, 244, COL_LGREY, 16);
  }
  char sv[24];                                           // plain-language server status for the helper
  if (gPairHttp == 0) snprintf(sv, sizeof(sv), "Server: no link");
  else if (gPairHttp >= 200 && gPairHttp < 300) snprintf(sv, sizeof(sv), "Server: OK");
  else snprintf(sv, sizeof(sv), "Server error %d", gPairHttp);
  textCentered(sv, 268, (gPairHttp >= 200 && gPairHttp < 300) ? COL_CONN : COL_OFF, 16);
  hal_text(USE_FIXED_PAIR_CODE ? "B: skip for now" : "A:new code  B:skip", 14, 296, COL_LGREY, 16);
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
  if (LIVE_ON_PRESS) camStart(CHOICES[idx].status);   // live camera on (+ a saved photo of this moment)
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
      case UE_PAIRED:
        gMode = MODE_MAIN;
        showOverlay(COL_GREEN, "Paired", gChildName.length() ? "Linked to " + gChildName : String("Linked to caregiver"), "", 3500);
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
      case 't': serverTest(); break;
      case 'c': gCamBroken = false; camStart("TEST"); break;   // DEV: live camera on now
      case 'x': camStop(); break;                         // DEV: stop it
      case 'p': gChildId = "child_demo"; gChildName = "Demo"; gDeviceToken = "demo-token"; gPaired = true; savePairing(); pushUi(UE_PAIRED); break;
      case 'u': clearPairing(); newPairCode(); gMode = MODE_PAIRING; gDirty = true; Serial.println("DEV: unpaired"); break;
      case 'r': SIM.battery = -1; SIM.gpsLost = false; SIM.offline = false; Serial.println("DEV: reset simulations"); break;
      default: break;
    }
  }
}

// =====================================================================
// 11. setup() / loop()
// =====================================================================
static bool gPrevA = false, gPrevB = false;
static uint32_t gBothSince = 0, gLastPairDraw = 0;
static int8_t gDrawnOnline = -2, gDrawnGps = -2; static int gDrawnBatt = -2;

void setup() {
  Serial.begin(115200);
  gMutex = xSemaphoreCreateMutex();
  prefs.begin("safesignal", false);
  gHaveLastKnown = prefs.isKey("lkLat");
  if (gHaveLastKnown) { gLastLat = prefs.getDouble("lkLat", 0); gLastLng = prefs.getDouble("lkLng", 0); gSavedLat = gLastLat; gSavedLng = gLastLng; }
  loadQueueFromFlash();                                // unsent events survive a reboot
  gPaired = prefs.getBool("paired", false);
  gChildId = gPaired ? prefs.getString("childId", CHILD_ID) : String(CHILD_ID);
  gChildName = prefs.getString("childName", "");
  gDeviceToken = gPaired ? prefs.getString("token", "") : String("");
  if (!gPaired) { newPairCode(); gMode = MODE_PAIRING; }   // new watch: show the pairing code first

  hal_init();
  if (LOCATION_SOURCE == LOC_SERIAL_NMEA) Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  gLastValidFix = millis();
  xTaskCreatePinnedToCore(networkTask, "net", 16384, nullptr, 1, nullptr, 0);

  Serial.printf("[SafeSignal] K10 watch running. Endpoint: %s\n", API_ENDPOINT);
  Serial.println("DEV keys: b/B battery, g/G gps, o/O internet, 1/2/3 acks, r reset, t server test, c camera on, x camera off");
#ifdef HAVE_ESP_CAMERA
  Serial.println("[SafeSignal] camera driver: present");
#else
  Serial.println("[SafeSignal] camera driver: NOT FOUND in this board package - the camera cannot start");
#endif
  evalBattery();
}

void loop() {
  uint32_t now = millis();
  devCommands();

  if (now - gLastBattCheck >= BATTERY_CHECK_MS) { gLastBattCheck = now; evalBattery(); }
  if (now - gLastGpsCheck >= GPS_CHECK_MS)      { gLastGpsCheck = now;  gpsTick(); }

  // Buttons (act on the press edge)
  bool a = hal_buttonA(), b = hal_buttonB();
  if (gMode == MODE_PAIRING) {
    if (a && !gPrevA && !gOverlay && !USE_FIXED_PAIR_CODE) { newPairCode(); gDirty = true; }   // A = new code
    if (b && !gPrevB && !gOverlay) { gMode = MODE_MAIN; gDirty = true; }        // B = skip for now
    if (now - gLastPairDraw >= 1000) { gLastPairDraw = now; if (!gOverlay) gDirty = true; }   // countdown
    if (!USE_FIXED_PAIR_CODE && millis() - gPairCodeAt > PAIR_CODE_LIFE_MS) { newPairCode(); gDirty = true; }
  } else if (gPreviewOn && !gOverlay && ((a && !gPrevA) || (b && !gPrevB))) {
    gButtonsUntil = now + BUTTONS_HOLD_MS; gDirty = true;           // live view: first press just brings the buttons back
  } else {
    if (a && !gPrevA && !gOverlay) { gSel = (gSel + 1) % 3; gDirty = true; }
    if (b && !gPrevB) {
      if (gOverlay && !gAwaitingPress) gOverlayUntil = 0;            // B dismisses a message
      else pressChoice(gSel);
    }
    // hold A + B for 3 s = pair again
    if (a && b) { if (!gBothSince) gBothSince = now ? now : 1; else if (now - gBothSince >= UNPAIR_HOLD_MS) { gBothSince = 0; clearPairing(); newPairCode(); gMode = MODE_PAIRING; gDirty = true; } }
    else gBothSince = 0;
  }
  gPrevA = a; gPrevB = b;

  handleUiEvents();
  camTick();

  if (gAwaitingPress && now - gAwaitingSince > REQUEST_TIMEOUT_MS + 4000) {   // network task never answered
    gAwaitingPress = false;
    showOverlay(COL_GREY, "Unable to send", "retrying...", "", 2500);
  }
  if (gOverlay && (int32_t)(now - gOverlayUntil) >= 0) { gOverlay = false; gAwaitingPress = false; gDirty = true; }

  // Redraw only when something changed (updateCanvas is slow)
  if (gOnline != gDrawnOnline || gGpsOk != gDrawnGps || gBattery != gDrawnBatt) {
    gDrawnOnline = gOnline; gDrawnGps = gGpsOk; gDrawnBatt = gBattery; if (!gOverlay) gDirty = true;
  }
  bool wantPreview = previewWanted();
  if (wantPreview != gPreviewOn) { gPreviewOn = wantPreview; hal_cameraPreview(wantPreview); gDirty = true; }
  if (gCamActive && !gPreviewOn && gButtonsUntil && (int32_t)(now - gButtonsUntil) >= 0) gDirty = true;
  if (gDirty) {
    gDirty = false;
    if (gOverlay) drawOverlay(); else if (gMode == MODE_PAIRING) drawPairing(); else if (gPreviewOn) drawLive(); else drawMain();
  }
  delay(20);
}
