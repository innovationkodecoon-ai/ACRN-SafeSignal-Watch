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
 *   w = walk INTO the next danger zone   a = stand just outside it   W = leave (real location again)
 *   f = CAMERA LINK TEST (sends one tiny LIVE_FRAME and explains the reply)
 *   c = live camera ON now     x = live camera OFF (back to main screen)
 *   t = SERVER TEST: sends the same PAIR_REQUEST as the curl test (deviceId test-1, code 123456) and prints
 *       the HTTP code + reply. Expected: 200 {"ok":true,"paired":false}. "user-exception" = the Base44 function crashed.
 *
 * GEOFENCING (danger zones): the ZONES table in section 1b lists dangerous places (beach, pond, road...) with a
 *   radius and a risk 1-4. The watch checks its GPS every 5 s. Entering a zone sends GEOFENCE_ENTER to the caregiver
 *   app; HIGH / CRITICAL zones also set notifyACRN = true so ACRN is alerted and can offer help. CRITICAL / HIGH zones
 *   warn 100 m / 50 m BEFORE the zone (GEOFENCE_APPROACHING), repeat GEOFENCE_STILL_INSIDE every 30 s / 60 s while the
 *   child stays inside, and send GEOFENCE_EXIT when the child leaves. The child sees a coloured full-screen warning.
 *   Location comes from LOCATION_SOURCE (use LOC_SERIAL_NMEA with a GPS module). Test with the keys w / a / W.
 *   The camera is NOT turned on by a geofence alert: only the caregiver can switch it on.
 *
 * ARE YOU OK?: when the child enters a danger zone the watch shows a blinking "ARE YOU OK?" screen and keeps showing it
 *   until the child presses B (I'M OK) or A (HELP). If there is no answer for 60 s, GEOFENCE_NO_REPLY (urgent) is sent.
 *   The zones come from the caregiver app (ZONES_REQUEST, synced every minute and saved); the table in section 1b is the fallback.
 *   On power-up the PAIRING screen is always shown first; an already paired watch continues to the main screen once online.
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
 *   THE CAMERA NEVER TURNS ON BY ITSELF: only the caregiver's Start / Photo in the app switches it on (the child's
 *   button presses do not, unless you set LIVE_ON_PRESS = true). Camera commands are ignored for the first 20 s
 *   after the watch boots, so an old pending command cannot switch the camera on at start-up.
 *   cameraCommand CAPTURE_NOW (the app's "Photo" button) saves one photo.
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
static const char *WIFI_SSID     = "KODECOON-OFFICE";
static const char *WIFI_PASSWORD = "startworking";

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
static const bool     LIVE_ON_PRESS           = false;     // false = camera ONLY turns on when the caregiver asks (true = child presses also start it)
static const uint32_t CMD_IGNORE_AFTER_BOOT_MS = 20000;    // ignore START / Photo commands this long after power-on (stale commands)
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

// ---------------------------------------------------------------------
// 1b. DANGER ZONES (geofencing).  EDIT THIS TABLE: one line per dangerous place.
//   name    : shown on the watch and in the caregiver / ACRN app (no double quotes please)
//   type    : BEACH, POND, RIVER, ROAD, CONSTRUCTION, ... (free text, the app shows it)
//   lat,lng : centre of the place (right-click the spot in Google Maps -> copy the numbers)
//   radiusM : size of the danger area in metres
//   risk    : 1 LOW | 2 MEDIUM | 3 HIGH | 4 CRITICAL   (risk-based behaviour below)
// The coordinates below are EXAMPLES in Singapore - replace them with the real places near the child.
//
// RISK-BASED BEHAVIOUR
//   risk 4 CRITICAL : early warning 100 m BEFORE the zone, ACRN + caregiver alerted the moment the child enters
//                     (1 GPS fix is enough), reminder every 30 s while inside, red full-screen warning
//   risk 3 HIGH     : early warning 50 m before, ACRN + caregiver alerted on entering, reminder every 60 s
//   risk 2 MEDIUM   : caregiver alerted on entering (not ACRN), reminder every 5 min
//   risk 1 LOW      : caregiver told once on entering, no reminders
// ---------------------------------------------------------------------
// BUILT-IN zones (used until the caregiver app sends its own list, see ZONE SYNC below).
struct Zone { const char *name; const char *type; double lat; double lng; uint16_t radiusM; uint8_t risk; };
static const Zone DEFAULT_ZONES[] = {
  // name                  type            lat        lng         radius  risk
  { "East Coast Beach",    "BEACH",        1.30080,   103.91220,  250,    4 },
  { "MacRitchie Reservoir","WATER",        1.34400,   103.83400,  200,    4 },
  { "Park Pond",           "POND",         1.36400,   103.84600,  80,     3 },
  { "Orchard Road",        "BUSY ROAD",    1.30480,   103.83180,  60,     3 },
  { "Building Site",       "CONSTRUCTION", 1.33000,   103.80000,  70,     2 },
};
static const int DEFAULT_ZONE_COUNT = sizeof(DEFAULT_ZONES) / sizeof(DEFAULT_ZONES[0]);

// ZONE SYNC: the danger zones the caregiver sets in the app are downloaded by the watch (ZONES_REQUEST, every minute)
// and saved in flash, so they also work offline. The built-in list above is only used while the app has sent none.
struct ZoneRT { char name[32]; char type[16]; double lat; double lng; uint16_t radiusM; uint8_t risk; };
static const int      MAX_ZONES          = 24;
static const bool     USE_BUILTIN_ZONES  = true;      // false = no built-in zones, only the caregiver's
static const uint32_t ZONE_SYNC_MS       = 60000;

// "ARE YOU OK?" screen: shown when the child enters a danger zone (risk >= this) until the child presses I'M OK (B) or HELP (A)
static const uint8_t  ASK_OK_MIN_RISK    = 1;
static const uint32_t ASK_ESCALATE_MS    = 60000;     // no answer for this long => GEOFENCE_NO_REPLY (urgent) to caregiver / ACRN

// On power-up the PAIRING screen is shown first. If the watch is already paired it moves on to the main screen
// after the connection is up (at least MIN, at most MAX milliseconds).
static const uint32_t BOOT_SPLASH_MIN_MS = 3000;
static const uint32_t BOOT_SPLASH_MAX_MS = 12000;

static const uint32_t GEO_CONFIRM_FIXES   = 2;      // consecutive GPS fixes needed to confirm enter / leave (stops GPS jitter false alarms; CRITICAL needs only 1)
static const double   GEO_LEAVE_MARGIN    = 1.15;   // must be 15% + 10 m outside the zone to count as "left"
static const bool     GEO_ALERT_ON_BOOT   = true;   // true = if the watch starts inside a danger zone, alert straight away

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
static int textW(const char *s, int size) { return size >= 24 ? (int)strlen(s) * 25 / 2 : (int)strlen(s) * 6; }   // measured on the K10 screen
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
struct Sim { volatile int battery = -1; volatile bool gpsLost = false; volatile bool offline = false;
             volatile bool fake = false; volatile double fLat = 0; volatile double fLng = 0; };   // fake = pretend the watch is at fLat/fLng
static Sim SIM;

static SemaphoreHandle_t gMutex;
static Preferences prefs;

static volatile int8_t gOnline = -1;       // -1 unknown, 0 offline, 1 online
static volatile int8_t gGpsOk  = -1;       // -1 unknown, 0 lost, 1 ok
static volatile int    gBattery = -1;      // percent or -1 (n/a)
static volatile uint8_t gGeoRisk = 0;      // highest risk (1-4) of the danger zone the child is inside now, 0 = none
static const char *gGeoName = nullptr;     // name of that zone
static bool    gAwaitingPress = false;     // overlay "Sending..." until the network task reports
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
  if (SIM.fake) {
    f = {true, SIM.fLat, SIM.fLng};
  } else if (LOCATION_SOURCE == LOC_FIXED) {
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
enum SlotId { SLOT_STATUS = 0, SLOT_BATT = 1, SLOT_GPS = 2, SLOT_GEO = 3, SLOT_COUNT = 4 };
static const char *SLOT_KEYS[SLOT_COUNT] = {"q0", "q1", "q2", "q3"};
static const int FLUSH_ORDER[SLOT_COUNT] = {SLOT_GEO, SLOT_STATUS, SLOT_BATT, SLOT_GPS};   // danger-zone alerts go out first

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
static void zonesSync();                       // defined in section 8a

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
static uint32_t gCmdAcceptAt = 0;                    // set in setup(): commands that switch the camera ON are ignored before this time

static void handleAck(const String &body) {
  String cc = jsonGetString(body, "cameraCommand");
  if (cc != "") Serial.printf("[SafeSignal] app command received: cameraCommand = %s\n", cc.c_str());
  bool accept = (int32_t)(millis() - gCmdAcceptAt) >= 0;          // not right after power-on
  if (cc == "STOP") gCamCmd = CAMCMD_STOP;                        // STOP is always obeyed
  else if (cc == "START" || cc == "CAPTURE_NOW") {
    if (accept) gCamCmd = (cc == "START") ? CAMCMD_START : CAMCMD_CAPTURE;
    else Serial.println("[SafeSignal] camera command ignored (watch just started)");
  }

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

// DEV key 'f': CAMERA LINK TEST. Sends one tiny LIVE_FRAME (a 1-pixel picture, no camera needed) so you can see
// whether Base44 accepts live frames and whether it answers with a cameraCommand.
static void cameraLinkTest() {
  String body = "{\"childId\":\"" + gChildId + "\",\"deviceId\":\"" + String(DEVICE_ID) +
    "\",\"eventType\":\"LIVE_FRAME\",\"saveAsPhoto\":false,\"trigger\":\"TEST\",\"seq\":1,\"latitude\":null,\"longitude\":null,"
    "\"cameraOn\":true,\"timestamp\":\"" + timestampNow() + "\",\"imageFormat\":\"jpeg\",\"imageBase64\":"
    "\"/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAABAAEBAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxA=\"}";
  Serial.printf("[SafeSignal] CAMERA LINK TEST -> POST %s\n", API_ENDPOINT);
  Serial.printf("  paired: %s, device token: %s, camera driver compiled in: %s\n", gPaired ? "yes" : "NO", gDeviceToken.length() ? "set" : "MISSING",
#ifdef HAVE_ESP_CAMERA
    "yes");
#else
    "NO");
#endif
  int code; String resp;
  if (!doPost(body, code, resp)) { Serial.println("  RESULT: no connection to the server (check Wi-Fi / URL)"); return; }
  Serial.printf("  HTTP %d  reply: %s\n", code, resp.substring(0, 300).c_str());
  if (code == 401 || code == 403) Serial.println("  RESULT: Base44 rejected the watch. Pair the watch with the app first (hold A+B for 3 s, enter the code in the app).");
  else if (code == 400 || code == 404 || code == 422) Serial.println("  RESULT: Base44 does not understand LIVE_FRAME yet - paste the camera prompt into your Base44 app.");
  else if (code >= 500 || resp.indexOf("user-exception") >= 0) Serial.println("  RESULT: the Base44 function crashed on LIVE_FRAME - the camera part of watchStatus is broken/missing.");
  else if (code >= 200 && code < 300) Serial.println("  RESULT: Base44 accepted the frame. Now press 'Turn on live camera' in the app; the Serial Monitor must print 'app command received: cameraCommand = START'.");
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
static volatile uint32_t gLiveOkCount = 0;           // frames the app/Base44 accepted (shown on the K10 screen)
static volatile int gLiveLastCode = 0;               // last HTTP code of a frame upload (0 = no link)
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

// Upload the pending live frame as a LIVE_FRAME event ("saveAsPhoto":true when it should also be kept as a photo).
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
                "\",\"eventType\":\"LIVE_FRAME\",\"saveAsPhoto\":" + (snap ? "true" : "false") + ",\"trigger\":\"" + trig +
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
    gLiveLastCode = code;
    if (code >= 200 && code < 300) {
      gLastLiveOk = millis(); gLiveOkCount++;
      static uint32_t nOk = 0;                      // progress line so you can see the stream is running
      if (++nOk == 1 || nOk % 20 == 0) Serial.printf("[SafeSignal] live frames delivered: %u\n", (unsigned)nOk);
    } else Serial.printf("[SafeSignal] live frame -> HTTP %d %s\n", code, resp.substring(0, 120).c_str());
    handleAck(resp);                                // picks up cameraCommand STOP etc.
  } else {
    gLastHttpCode = 0; gLiveLastCode = 0;
    if (gOnline == 1) setOnline(false);
  }
}

// Deliver everything queued, highest priority first.
static void flushOutbox() {
  for (int k = 0; k < SLOT_COUNT; k++) {
    int i = FLUSH_ORDER[k];
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
  uint32_t lastHb = 0, lastTry = 0, lastWifi = 0, lastRestored = 0, lastPair = 0, lastImg = 0, lastZones = 0;
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

    // Danger zones set by the caregiver in the app
    if (gPaired && gOnline == 1 && (lastZones == 0 || now - lastZones >= ZONE_SYNC_MS)) { lastZones = now ? now : 1; zonesSync(); }

    // Heartbeat. An offline watch cannot report that it is offline, so Base44 treats
    // several missed beats (about 45 s) as "connectivity lost".
    bool liveFlowing = gCamActive && (now - gLastLiveOk < 8000);   // live frames already prove we are alive
    if ((now - lastHb >= HEARTBEAT_MS || lastHb == 0) && !liveFlowing) {
      lastHb = now ? now : 1;
      String hb = String(",\"cameraOn\":") + (gCamActive ? "true" : "false");
      if (gBattery >= 0) hb += ",\"batteryLevel\":" + String((int)gBattery);
      if (gGpsOk == 1 && gHaveLastKnown) hb += ",\"latitude\":" + jnum(true, gLastLat) + ",\"longitude\":" + jnum(true, gLastLng);   // live location for the map
      hb += ",\"dangerRisk\":" + String((int)gGeoRisk);        // 0 = not in a danger zone
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

// =====================================================================
// 8a. GEOFENCING  (danger zones, checked on the watch itself - works even when the internet is down;
//     the alert is queued and sent as soon as the connection returns)
// =====================================================================
static void showOverlay(uint32_t color, const String &t, const String &l1, const String &l2, uint32_t ms);   // defined in section 9

// ---- the zone list in use: the caregiver's zones from the app (synced), or the built-in list ----
static ZoneRT gZones[MAX_ZONES];
static int    gZoneCount = 0;
static ZoneRT gZonesNew[MAX_ZONES];              // staging: filled by the network task, applied by loop()
static int    gZonesNewCount = 0;
static volatile bool gZonesNewReady = false;
static String gZonesRaw;                         // last zone list received from the app (network task only)

static void copyStr(char *dst, size_t n, const String &s) { strncpy(dst, s.c_str(), n - 1); dst[n - 1] = 0; }

static bool jsonGetNum(const String &obj, const char *key, double &v) {
  String k = String("\"") + key + "\"";
  int i = obj.indexOf(k);
  if (i < 0) return false;
  i = obj.indexOf(':', i + k.length());
  if (i < 0) return false;
  i++;
  while (i < (int)obj.length() && (obj[i] == ' ' || obj[i] == '\t' || obj[i] == '\n' || obj[i] == '\r')) i++;
  if (i >= (int)obj.length()) return false;
  char c = obj[i];
  if (!(c == '-' || c == '.' || (c >= '0' && c <= '9'))) return false;
  v = obj.substring(i).toDouble();
  return true;
}
static bool jsonIsFalse(const String &obj, const char *key) {
  int i = obj.indexOf(String("\"") + key + "\"");
  if (i < 0) return false;
  i = obj.indexOf(':', i);
  if (i < 0) return false;
  while (++i < (int)obj.length() && (obj[i] == ' ' || obj[i] == '\t')) {}
  return obj.substring(i, i + 5) == "false";
}
static int riskFromText(String t) {
  t.toUpperCase();
  if (t.startsWith("CRIT")) return 4;
  if (t.startsWith("HIGH")) return 3;
  if (t.startsWith("MED")) return 2;
  if (t.startsWith("LOW")) return 1;
  return 3;
}

// {"zones":[{"name":"East Coast Beach","type":"BEACH","latitude":1.3,"longitude":103.9,"radiusMeters":250,"riskScore":4}, ...]}
// Accepted keys: name|zoneName, type|zoneType, latitude|lat, longitude|lng|lon, radiusMeters|radiusM|radius,
// riskScore|risk (1-4) or riskLevel ("LOW".."CRITICAL"), active:false = skipped.
static bool parseZones(const String &body, ZoneRT *out, int &n) {
  n = 0;
  int k = body.indexOf("\"zones\"");
  if (k < 0) return false;
  int a = body.indexOf('[', k);
  if (a < 0) return false;
  int e = body.indexOf(']', a);
  if (e < 0) return false;
  int pos = a;
  while (n < MAX_ZONES) {
    int o = body.indexOf('{', pos);
    if (o < 0 || o > e) break;
    int c = body.indexOf('}', o);
    if (c < 0) break;
    String obj = body.substring(o, c + 1);
    pos = c + 1;
    if (jsonIsFalse(obj, "active")) continue;
    String name = jsonGetString(obj, "name"); if (name == "") name = jsonGetString(obj, "zoneName");
    String type = jsonGetString(obj, "type"); if (type == "") type = jsonGetString(obj, "zoneType");
    double la = 0, lo = 0, r = 0, rk = 0;
    bool okLa = jsonGetNum(obj, "latitude", la) || jsonGetNum(obj, "lat", la);
    bool okLo = jsonGetNum(obj, "longitude", lo) || jsonGetNum(obj, "lng", lo) || jsonGetNum(obj, "lon", lo);
    bool okR  = jsonGetNum(obj, "radiusMeters", r) || jsonGetNum(obj, "radiusM", r) || jsonGetNum(obj, "radius", r);
    if (name == "" || !okLa || !okLo || !okR || r <= 0) continue;
    int risk = 3;
    if (jsonGetNum(obj, "riskScore", rk) || jsonGetNum(obj, "risk", rk) || jsonGetNum(obj, "riskLevel", rk)) risk = (int)rk;
    else { String t = jsonGetString(obj, "riskLevel"); if (t == "") t = jsonGetString(obj, "risk"); if (t != "") risk = riskFromText(t); }
    if (risk < 1) risk = 1;
    if (risk > 4) risk = 4;
    ZoneRT &Z = out[n++];
    copyStr(Z.name, sizeof(Z.name), name);
    copyStr(Z.type, sizeof(Z.type), type == "" ? String("ZONE") : type);
    Z.lat = la; Z.lng = lo; Z.radiusM = (uint16_t)(r > 65000 ? 65000 : r); Z.risk = (uint8_t)risk;
  }
  return true;
}

// Boot: the zones the app sent last time (saved in flash), otherwise the built-in list.
static void loadZones() {
  String saved = prefs.getString("zonesJson", "");
  int n = 0;
  if (saved.length() >= 2 && parseZones(String("{\"zones\":") + saved + "}", gZones, n)) {
    gZoneCount = n; gZonesRaw = saved;
    Serial.printf("[SafeSignal] %d danger zone(s) loaded from the caregiver app (saved)\n", n);
    return;
  }
  gZoneCount = 0;
  if (USE_BUILTIN_ZONES) {
    for (int i = 0; i < DEFAULT_ZONE_COUNT && i < MAX_ZONES; i++) {
      const Zone &D = DEFAULT_ZONES[i]; ZoneRT &Z = gZones[gZoneCount++];
      copyStr(Z.name, sizeof(Z.name), D.name); copyStr(Z.type, sizeof(Z.type), D.type);
      Z.lat = D.lat; Z.lng = D.lng; Z.radiusM = D.radiusM; Z.risk = D.risk;
    }
  }
  Serial.printf("[SafeSignal] %d built-in danger zone(s) (the app has not sent any yet)\n", gZoneCount);
}

// Network task: ask the app for the caregiver's zones. Reply must contain "zones":[...]
static void zonesSync() {
  int code; String resp;
  if (!doPost(buildEventPayload("ZONES_REQUEST", ""), code, resp)) return;
  static int lastCode = -1;
  if (code != lastCode) { lastCode = code; Serial.printf("[SafeSignal] zone sync -> HTTP %d %s\n", code, resp.substring(0, 160).c_str()); }
  if (code < 200 || code >= 300) return;
  static ZoneRT tmp[MAX_ZONES];
  int n = 0;
  if (!parseZones(resp, tmp, n)) return;               // the app sent no zone list: keep what we have
  int a = resp.indexOf('[', resp.indexOf("\"zones\"")), e = resp.indexOf(']', a);
  String raw = resp.substring(a, e + 1);
  if (raw == gZonesRaw) return;                        // unchanged
  if (n == 0 && raw.length() > 2) { Serial.println("[SafeSignal] zone sync: could not read the zones (check name / latitude / longitude / radiusMeters / riskScore)"); return; }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  memcpy(gZonesNew, tmp, sizeof(ZoneRT) * n);
  gZonesNewCount = n; gZonesNewReady = true;
  if (raw.length() < 3500) prefs.putString("zonesJson", raw);
  xSemaphoreGive(gMutex);
  gZonesRaw = raw;
  Serial.printf("[SafeSignal] zone sync: %d danger zone(s) received from the caregiver app\n", n);
}

enum GeoState : uint8_t { GS_OUTSIDE = 0, GS_APPROACH = 1, GS_INSIDE = 2 };
static GeoState gGeoState[MAX_ZONES];
static uint8_t  gGeoPend[MAX_ZONES];             // consecutive fixes disagreeing with the current state
static uint32_t gGeoEnteredAt[MAX_ZONES];        // millis() when the child entered
static uint32_t gGeoLastAlert[MAX_ZONES];        // millis() of the last ENTER / reminder
static uint16_t gGeoRepeats[MAX_ZONES];
static bool     gGeoFirstFix = true;
static char     gGeoNameBuf[32];

// "ARE YOU OK?" state
static bool     gAsk = false;
static uint8_t  gAskRisk = 0;
static char     gAskName[32];
static uint32_t gAskSince = 0;
static bool     gAskEscalated = false;

static const char *riskName(int r) { return r >= 4 ? "CRITICAL" : r == 3 ? "HIGH" : r == 2 ? "MEDIUM" : "LOW"; }
static bool     riskNotifiesAcrn(int r) { return r >= 3; }                                   // HIGH and CRITICAL go to ACRN
static uint32_t riskRepeatMs(int r) { return r >= 4 ? 30000UL : r == 3 ? 60000UL : r == 2 ? 300000UL : 0; }   // 0 = no reminders
static int      riskApproachM(int r) { return r >= 4 ? 100 : r == 3 ? 50 : 0; }              // early warning distance
static int      riskConfirm(int r) { return r >= 4 ? 1 : (int)GEO_CONFIRM_FIXES; }          // CRITICAL reacts on the first fix

// Distance in metres between two points (haversine)
static double distanceM(double la1, double lo1, double la2, double lo2) {
  const double R = 6371000.0, D = 0.017453292519943295;
  double dLa = (la2 - la1) * D, dLo = (lo2 - lo1) * D;
  double a = sin(dLa / 2) * sin(dLa / 2) + cos(la1 * D) * cos(la2 * D) * sin(dLo / 2) * sin(dLo / 2);
  return 2 * R * asin(sqrt(a));
}

// Sends GEOFENCE_APPROACHING / ENTER / STILL_INSIDE / NO_REPLY / CHILD_OK / CHILD_HELP / EXIT to Base44.
static void geoSend(const char *eventType, int z, const Fix &f, double dist) {
  const ZoneRT &Z = gZones[z];
  bool enterLike = strcmp(eventType, "GEOFENCE_ENTER") == 0 || strcmp(eventType, "GEOFENCE_STILL_INSIDE") == 0;
  bool noReply = strcmp(eventType, "GEOFENCE_NO_REPLY") == 0;
  bool acrn = enterLike ? riskNotifiesAcrn(Z.risk) : noReply ? (Z.risk >= 2) : false;
  String x = String(",\"zoneName\":\"") + Z.name + "\",\"zoneType\":\"" + Z.type + "\"";
  x += String(",\"riskLevel\":\"") + riskName(Z.risk) + "\",\"riskScore\":" + String((int)Z.risk);
  x += ",\"distanceMeters\":" + String((int)(dist + 0.5)) + ",\"radiusMeters\":" + String((int)Z.radiusM);
  x += ",\"zoneLatitude\":" + jnum(true, Z.lat) + ",\"zoneLongitude\":" + jnum(true, Z.lng);
  x += ",\"latitude\":" + jnum(f.ok, f.lat) + ",\"longitude\":" + jnum(f.ok, f.lng);
  x += String(",\"notifyCaregiver\":true,\"notifyACRN\":") + (acrn ? "true" : "false");
  x += String(",\"urgent\":") + (((acrn && Z.risk >= 4) || noReply) ? "true" : "false");
  x += String(",\"awaitingChildReply\":") + (gAsk ? "true" : "false");
  if (gGeoEnteredAt[z]) x += ",\"secondsInside\":" + String((int)((millis() - gGeoEnteredAt[z]) / 1000));
  x += ",\"repeatCount\":" + String((int)gGeoRepeats[z]);
  if (gBattery >= 0) x += ",\"batteryLevel\":" + String((int)gBattery);
  submit(SLOT_GEO, buildEventPayload(eventType, x));
}

// Full-screen warning for the child (used when the "ARE YOU OK?" screen is not showing). Colour follows the risk.
static void geoWarnScreen(int z, bool reminder) {
  if (gAwaitingPress) return;                              // do not hide the "Sending..." of a button press
  const ZoneRT &Z = gZones[z];
  uint32_t col = Z.risk >= 4 ? COL_RED : Z.risk == 3 ? 0xE04A00 : Z.risk == 2 ? COL_ORANGE : COL_BLUE;
  const char *title = Z.risk >= 4 ? "DANGER!" : Z.risk == 3 ? "Be careful!" : "Careful";
  const char *l2 = Z.risk >= 3 ? "Caregiver and helpers were told. Stay here." : "Your caregiver was told.";
  showOverlay(col, title, String("You are at ") + Z.name, l2, reminder ? 4000 : 9000);
}

// New zone list from the app: swap it in (loop thread only), keeping the state of zones that did not change.
static void applyNewZones() {
  if (!gZonesNewReady) return;
  static ZoneRT nz[MAX_ZONES], oz[MAX_ZONES];
  int nn;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  nn = gZonesNewCount;
  memcpy(nz, gZonesNew, sizeof(ZoneRT) * nn);
  gZonesNewReady = false;
  xSemaphoreGive(gMutex);

  int on = gZoneCount;
  memcpy(oz, gZones, sizeof(ZoneRT) * on);
  GeoState st[MAX_ZONES]; uint8_t pe[MAX_ZONES]; uint32_t en[MAX_ZONES], la[MAX_ZONES]; uint16_t rp[MAX_ZONES];
  memcpy(st, gGeoState, sizeof(st)); memcpy(pe, gGeoPend, sizeof(pe));
  memcpy(en, gGeoEnteredAt, sizeof(en)); memcpy(la, gGeoLastAlert, sizeof(la)); memcpy(rp, gGeoRepeats, sizeof(rp));
  memset(gGeoState, 0, sizeof(gGeoState)); memset(gGeoPend, 0, sizeof(gGeoPend));
  memset(gGeoEnteredAt, 0, sizeof(gGeoEnteredAt)); memset(gGeoLastAlert, 0, sizeof(gGeoLastAlert)); memset(gGeoRepeats, 0, sizeof(gGeoRepeats));
  for (int i = 0; i < nn; i++) {
    gZones[i] = nz[i];
    for (int j = 0; j < on; j++) {
      if (strcmp(nz[i].name, oz[j].name) == 0) { gGeoState[i] = st[j]; gGeoPend[i] = pe[j]; gGeoEnteredAt[i] = en[j]; gGeoLastAlert[i] = la[j]; gGeoRepeats[i] = rp[j]; break; }
    }
  }
  gZoneCount = nn;
  Serial.printf("[SafeSignal] now using %d danger zone(s) from the caregiver app\n", nn);
}

static void geoTick(const Fix &f) {
  applyNewZones();
  if (!f.ok) return;
  uint32_t now = millis();
  for (int z = 0; z < gZoneCount; z++) {
    const ZoneRT &Z = gZones[z];
    double d = distanceM(f.lat, f.lng, Z.lat, Z.lng);
    double rIn = Z.radiusM, rApp = Z.radiusM + riskApproachM(Z.risk);
    GeoState cur = gGeoState[z], want;
    // hysteresis: once inside / approaching, the child must be clearly outside to leave
    if (d <= rIn) want = GS_INSIDE;
    else if (cur == GS_INSIDE && d <= rIn * GEO_LEAVE_MARGIN + 10) want = GS_INSIDE;
    else if (d <= rApp) want = riskApproachM(Z.risk) ? GS_APPROACH : GS_OUTSIDE;
    else if (cur == GS_APPROACH && d <= rApp + 20) want = GS_APPROACH;
    else want = GS_OUTSIDE;

    if (want == cur) {
      gGeoPend[z] = 0;
      if (cur == GS_INSIDE) {                               // reminders while the child stays in the zone
        uint32_t rep = riskRepeatMs(Z.risk);
        if (rep && now - gGeoLastAlert[z] >= rep) {
          gGeoLastAlert[z] = now; gGeoRepeats[z]++;
          Serial.printf("[SafeSignal] GEOFENCE reminder: still inside %s (risk %s)\n", Z.name, riskName(Z.risk));
          geoSend("GEOFENCE_STILL_INSIDE", z, f, d);
          if (!gAsk) geoWarnScreen(z, true);
        }
      }
      continue;
    }
    // the first fix after power-on is trusted immediately; otherwise confirm with a 2nd fix (CRITICAL: 1)
    int need = (gGeoFirstFix && GEO_ALERT_ON_BOOT) ? 1 : riskConfirm(Z.risk);
    if (++gGeoPend[z] < need) continue;
    gGeoPend[z] = 0;
    gGeoState[z] = want;
    if (want == GS_INSIDE) {
      gGeoEnteredAt[z] = now ? now : 1; gGeoLastAlert[z] = now; gGeoRepeats[z] = 0;
      Serial.printf("[SafeSignal] GEOFENCE ENTER: %s (%s) risk %s, %d m from centre\n", Z.name, Z.type, riskName(Z.risk), (int)d);
      if (Z.risk >= ASK_OK_MIN_RISK) {                      // "ARE YOU OK?" until the child answers
        if (!gAsk) { gAsk = true; gAskSince = now ? now : 1; gAskEscalated = false; gAskRisk = 0; }
        if (Z.risk > gAskRisk) { gAskRisk = Z.risk; copyStr(gAskName, sizeof(gAskName), Z.name); }
        gDirty = true;
      }
      geoSend("GEOFENCE_ENTER", z, f, d);
      if (!gAsk) geoWarnScreen(z, false);
    } else if (cur == GS_INSIDE) {
      Serial.printf("[SafeSignal] GEOFENCE EXIT: %s\n", Z.name);
      geoSend("GEOFENCE_EXIT", z, f, d);
      gGeoEnteredAt[z] = 0;
      if (!gAsk) showOverlay(COL_GREEN, "Safe again", String("You left ") + Z.name, "", 3000);
    } else if (want == GS_APPROACH) {
      Serial.printf("[SafeSignal] GEOFENCE APPROACHING: %s, %d m away\n", Z.name, (int)d);
      geoSend("GEOFENCE_APPROACHING", z, f, d);
      if (!gAwaitingPress && !gAsk) showOverlay(COL_ORANGE, "Careful", String("Danger area ahead: ") + Z.name, "Please turn back", 5000);
    }
  }
  gGeoFirstFix = false;
  // highest-risk zone the child is inside right now (drives the banner on the main screen)
  uint8_t top = 0; const char *nm = nullptr;
  for (int z = 0; z < gZoneCount; z++) if (gGeoState[z] == GS_INSIDE && gZones[z].risk > top) { top = gZones[z].risk; nm = gZones[z].name; }
  if (top != gGeoRisk || (top && strcmp(nm, gGeoNameBuf) != 0)) {
    if (top) { strncpy(gGeoNameBuf, nm, sizeof(gGeoNameBuf) - 1); gGeoNameBuf[sizeof(gGeoNameBuf) - 1] = 0; }
    gGeoName = top ? gGeoNameBuf : nullptr; gGeoRisk = top; gDirty = true;
  }
}

// If the child does not answer "ARE YOU OK?" for ASK_ESCALATE_MS, tell the caregiver / ACRN urgently (once).
static void askTick() {
  if (!gAsk || gAskEscalated) return;
  if (millis() - gAskSince < ASK_ESCALATE_MS) return;
  gAskEscalated = true;
  for (int z = 0; z < gZoneCount; z++) if (strcmp(gZones[z].name, gAskName) == 0) {
    Fix f = getFix();
    Serial.printf("[SafeSignal] GEOFENCE NO REPLY: the child did not answer ARE YOU OK? in %s\n", gAskName);
    geoSend("GEOFENCE_NO_REPLY", z, f, f.ok ? distanceM(f.lat, f.lng, gZones[z].lat, gZones[z].lng) : 0);
    break;
  }
}

static void gpsTick() {
  Fix f = getFix();
  if (f.ok) {
    gLastValidFix = millis();
    gGpsOk = 1;
    geoTick(f);
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
static uint32_t gAwaitingSince = 0;

static void showOverlay(uint32_t color, const String &t, const String &l1, const String &l2, uint32_t ms) {
  gOverlay = true; gOverlayColor = color; gOvTitle = t; gOvL1 = l1; gOvL2 = l2;
  gOverlayUntil = millis() + ms; gDirty = true;
}

// Live view is on while the camera session runs, unless a message / the buttons screen is showing.
static bool previewWanted() {
  return CAMERA_LIVE_PREVIEW && gCamActive && !gOneShot && gMode == MODE_MAIN && !gOverlay && !gAsk && (int32_t)(millis() - gButtonsUntil) >= 0;
}

// Live camera picture is drawn by the K10 behind the canvas; we only add two small text bars.
static void drawLive() {
  hal_clear();
  hal_rect(0, 0, SCREEN_W, 26, 0x000000);
  hal_circle(12, 13, 6, COL_RED);
  hal_text("LIVE CAMERA", 26, 5, COL_RED, 16);
  char st[24];                                           // delivery status, so you can see whether the app receives frames
  if (gLiveLastCode >= 200 && gLiveLastCode < 300) snprintf(st, sizeof(st), "sent %u", (unsigned)gLiveOkCount);
  else if (gLiveLastCode == 0) snprintf(st, sizeof(st), "no link");
  else snprintf(st, sizeof(st), "HTTP %d", gLiveLastCode);
  hal_text(st, 150, 5, (gLiveLastCode >= 200 && gLiveLastCode < 300) ? COL_CONN : COL_OFF, 16);
  hal_rect(0, 294, SCREEN_W, 26, 0x000000);
  textCentered("Press A or B for buttons", 298, COL_WHITE, 16);
  hal_present();
}

// Connection state shown on the watch:  1 online | 2 Wi-Fi ok but no internet | 3 no Wi-Fi | 0 still connecting
static int connState() {
  if (gOnline == 1) return 1;
  if (WiFi.status() != WL_CONNECTED) return gOnline == 0 ? 3 : 0;
  return gOnline == 0 ? 2 : 0;
}

static void drawMain() {
  hal_clear();
  // Title: two-tone "Safe" + "Signal", centred as one word
  int tw = textW("SafeSignal", 24), tx = (SCREEN_W - tw) / 2;
  hal_text("Safe", tx, 4, COL_WHITE, 24);
  hal_text("Signal", tx + textW("Safe", 24), 4, 0x4DB8FF, 24);
  hal_rect(SCREEN_W / 2 - 30, 31, 60, 2, 0x4DB8FF);                       // thin accent line under the title
  if (gGeoRisk && gGeoName) {                            // inside a danger zone: warning replaces the subtitle
    String b = String("! ") + gGeoName;
    textCentered(b.c_str(), 35, gGeoRisk >= 4 ? COL_OFF : COL_ORANGE, 16);
  } else textCentered("Tap how you feel", 35, COL_LGREY, 16);
  // Internet status as a centred pill:  (dot) Online / No internet / No Wi-Fi / Connecting...
  int cs = connState();
  const char *ct = cs == 1 ? "Online" : cs == 2 ? "No internet" : cs == 3 ? "No Wi-Fi" : "Connecting...";
  uint32_t cc = cs == 1 ? COL_CONN : cs == 0 ? 0xAAAAAA : COL_OFF;
  uint32_t bg = cs == 1 ? 0x0F3A1B : cs == 0 ? 0x2A2A2A : 0x4A1512;
  int ctw = textW(ct, 16), pw = ctw + 42, px = (SCREEN_W - pw) / 2;
  hal_circle(px + 11, 60, 11, bg);
  hal_circle(px + pw - 11, 60, 11, bg);
  hal_rect(px + 11, 49, pw - 22, 22, bg);
  hal_circle(px + 17, 60, 4, cc);
  hal_text(ct, px + 28, 52, cc, 16);

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
  hal_circle(224, 309, 7, cs == 1 ? COL_CONN : cs == 0 ? 0x777777 : COL_OFF);
  hal_present();
}

static void drawPairing() {
  hal_clear();
  textCentered("SafeSignal", 4, COL_WHITE, 24);
  textCentered("Connect your device", 34, COL_LGREY, 16);
  if (gPaired) textCentered(gOnline == 1 ? "Paired - Online" : "Paired - Connecting...", 54, gOnline == 1 ? COL_CONN : 0xAAAAAA, 16);
  else textCentered(gOnline == 1 ? "Waiting for caregiver..." : "Connecting...", 54, gOnline == 1 ? COL_CONN : COL_OFF, 16);

  textCentered("Device ID", 80, COL_LGREY, 16);
  textCentered(DEVICE_ID, 100, COL_WHITE, 24);

  textCentered("Pairing Code", 138, COL_LGREY, 16);
  hal_rect(8, 158, SCREEN_W - 16, 52, gPaired ? COL_GREEN : COL_BLUE);
  char spaced[16];                                       // "482 731" with extra spacing
  snprintf(spaced, sizeof(spaced), "%c %c %c   %c %c %c", gPairCode[0], gPairCode[1], gPairCode[2], gPairCode[3], gPairCode[4], gPairCode[5]);
  textCentered(spaced, 172, COL_WHITE, 24);

  if (gPaired) {
    textCentered(gChildName.length() ? (String("Linked to ") + gChildName).c_str() : "Linked to caregiver", 224, COL_WHITE, 16);
    textCentered("Starting...", 244, COL_LGREY, 16);
    hal_text("B: continue", 14, 296, COL_LGREY, 16);
    hal_present();
    return;
  }
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

// "ARE YOU OK?" - shown after the child enters a danger zone, until the child answers (B = I'M OK, A = HELP)
static void drawAsk() {
  bool flash = ((millis() / 1000) & 1) == 0;             // top block blinks to get attention
  hal_clear();
  hal_rect(0, 0, SCREEN_W, 130, flash ? COL_RED : 0x8A1010);
  textCentered("ARE YOU OK?", 14, COL_WHITE, 24);
  textWrapped(String("You are at ") + gAskName, 52, COL_WHITE, 16);
  textCentered("Danger area", 106, 0xFFD0D0, 16);
  hal_rect(8, 140, SCREEN_W - 16, 64, COL_GREEN);
  textCentered("B: I'M OK", 140 + 20, COL_WHITE, 24);
  hal_rect(8, 214, SCREEN_W - 16, 64, 0xA01818);
  textCentered("A: HELP", 214 + 20, COL_WHITE, 24);
  textCentered("Your caregiver was told", 290, COL_LGREY, 16);
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

// The child answers "ARE YOU OK?":  idx 0 = I'M OK, 2 = HELP. Sends the normal status AND a geofence reply.
static void answerAsk(int idx) {
  for (int z = 0; z < gZoneCount; z++) if (strcmp(gZones[z].name, gAskName) == 0) {
    Fix f = getFix();
    gAsk = false;                                        // so the reply is not marked "awaiting"
    geoSend(idx == 0 ? "GEOFENCE_CHILD_OK" : "GEOFENCE_CHILD_HELP", z, f, f.ok ? distanceM(f.lat, f.lng, gZones[z].lat, gZones[z].lng) : 0);
    break;
  }
  gAsk = false; gAskEscalated = false; gAskRisk = 0;
  gOverlay = false; gCooldownUntil = 0; gSel = idx; gDirty = true;
  pressChoice(idx);
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
static int gSimZone = -1;
// mode 0 = at the centre of the next zone, 1 = just outside its edge (inside the early-warning ring)
static void simZone(int mode) {
  if (gZoneCount == 0) { Serial.println("DEV: no danger zones loaded"); return; }
  if (mode == 0 || gSimZone < 0) gSimZone = (gSimZone + 1) % gZoneCount;
  if (gSimZone >= gZoneCount) gSimZone = 0;
  const ZoneRT &Z = gZones[gSimZone];
  double offM = mode == 0 ? 0 : Z.radiusM + (riskApproachM(Z.risk) ? riskApproachM(Z.risk) / 2 : 15);
  SIM.fLat = Z.lat + offM / 111320.0; SIM.fLng = Z.lng; SIM.fake = true;
  Serial.printf("DEV: pretending to be %s %s (%s, risk %s)\n", mode == 0 ? "INSIDE" : "just outside", Z.name, Z.type, riskName(Z.risk));
  gpsTick(); gpsTick();                                  // two fixes = confirmed, like real life
}

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
      case 'f': cameraLinkTest(); break;                  // DEV: does Base44 accept live frames?
      case 'c': gCamBroken = false; camStart("TEST"); break;   // DEV: live camera on now
      case 'x': camStop(); break;                         // DEV: stop it
      case 'p': gChildId = "child_demo"; gChildName = "Demo"; gDeviceToken = "demo-token"; gPaired = true; savePairing(); pushUi(UE_PAIRED); break;
      case 'u': clearPairing(); newPairCode(); gMode = MODE_PAIRING; gDirty = true; Serial.println("DEV: unpaired"); break;
      case 'w': simZone(0); break;                        // DEV: walk INTO the next danger zone
      case 'a': simZone(1); break;                        // DEV: stand just OUTSIDE it (early-warning distance)
      case 'W': SIM.fake = false; gpsTick(); gpsTick(); Serial.println("DEV: back to the real location (left the zone)"); break;
      case 'r': SIM.battery = -1; SIM.gpsLost = false; SIM.offline = false; SIM.fake = false; Serial.println("DEV: reset simulations"); break;
      default: break;
    }
  }
}

// =====================================================================
// 11. setup() / loop()
// =====================================================================
static bool gPrevA = false, gPrevB = false;
static uint32_t gBothSince = 0, gLastPairDraw = 0, gLastLiveDraw = 0, gLastAskDraw = 0, gBootAt = 0;
static int8_t gDrawnOnline = -2, gDrawnGps = -2; static int gDrawnConn = -2; static int gDrawnBatt = -2;

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
  newPairCode(); gMode = MODE_PAIRING; gBootAt = millis();   // ALWAYS start on the pairing screen (paired watches move on by themselves)
  loadZones();                                         // danger zones: from the app (saved) or built-in

  hal_init();
  if (LOCATION_SOURCE == LOC_SERIAL_NMEA) Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  gLastValidFix = millis();
  gCmdAcceptAt = millis() + CMD_IGNORE_AFTER_BOOT_MS;
  xTaskCreatePinnedToCore(networkTask, "net", 16384, nullptr, 1, nullptr, 0);

  Serial.printf("[SafeSignal] K10 watch running. Endpoint: %s\n", API_ENDPOINT);
  Serial.println("DEV keys: b/B battery, g/G gps, o/O internet, 1/2/3 acks, r reset, t server test, c camera on, x camera off, f camera link test, w walk into danger zone, a approach it, W leave");
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
  if (gAsk) {                                                            // "ARE YOU OK?" has priority over everything
    if (b && !gPrevB) answerAsk(0);                                      // B = I'M OK
    else if (a && !gPrevA) answerAsk(2);                                 // A = HELP
  } else if (gMode == MODE_PAIRING) {
    if (gPaired && ((gOnline == 1 && now - gBootAt >= BOOT_SPLASH_MIN_MS) || now - gBootAt >= BOOT_SPLASH_MAX_MS)) { gMode = MODE_MAIN; gDirty = true; }   // paired: on to the main screen
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
  askTick();
  if (gAsk && now - gLastAskDraw >= 1000) { gLastAskDraw = now; gDirty = true; }   // blinking

  if (gAwaitingPress && now - gAwaitingSince > REQUEST_TIMEOUT_MS + 4000) {   // network task never answered
    gAwaitingPress = false;
    showOverlay(COL_GREY, "Unable to send", "retrying...", "", 2500);
  }
  if (gOverlay && (int32_t)(now - gOverlayUntil) >= 0) { gOverlay = false; gAwaitingPress = false; gDirty = true; }

  // Redraw only when something changed (updateCanvas is slow)
  int cn = connState();
  if (cn != gDrawnConn) { gDrawnConn = cn; if (!gOverlay) gDirty = true; }
  if (gOnline != gDrawnOnline || gGpsOk != gDrawnGps || gBattery != gDrawnBatt) {
    gDrawnOnline = gOnline; gDrawnGps = gGpsOk; gDrawnBatt = gBattery; if (!gOverlay) gDirty = true;
  }
  bool wantPreview = previewWanted();
  if (wantPreview != gPreviewOn) { gPreviewOn = wantPreview; hal_cameraPreview(wantPreview); gDirty = true; }
  if (gCamActive && !gPreviewOn && gButtonsUntil && (int32_t)(now - gButtonsUntil) >= 0) gDirty = true;
  if (gPreviewOn && now - gLastLiveDraw >= 1500) { gLastLiveDraw = now; gDirty = true; }   // refresh the delivery counter
  if (gDirty) {
    gDirty = false;
    if (gAsk) drawAsk(); else if (gOverlay) drawOverlay(); else if (gMode == MODE_PAIRING) drawPairing(); else if (gPreviewOn) drawLive(); else drawMain();
  }
  delay(20);
}
