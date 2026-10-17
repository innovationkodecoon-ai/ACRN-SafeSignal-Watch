# SafeSignal Watch – UNIHIKER K10 (Arduino C++)

Single sketch: `safesignal_k10/safesignal_k10.ino` (port of the Python UniHiker version / web watch).

## Install & run
1. Arduino IDE → add DFRobot's UNIHIKER board package, select **UNIHIKER K10**.
2. Open the sketch, set `WIFI_SSID` / `WIFI_PASSWORD` (section 1 CONFIG). The Base44 endpoint is `API_ENDPOINT` just below.
3. Upload, open Serial Monitor at 115200.

## Controls (K10 has buttons A / B)
- **A** = move highlight OK → LOST → HELP (shown as `> LABEL <` with a white frame). **B** = send the highlighted choice (also dismisses a message).

## Test without hardware conditions (Serial Monitor, type one letter)
`b` battery 18% · `B` battery 78% · `g` GPS lost · `G` GPS restored · `o` internet off · `O` internet on · `1/2/3` caregiver acks · `r` reset.

## Where things are
| Concern | Sketch section |
|---|---|
| Endpoint, IDs, timings, Wi-Fi, location source | 1. CONFIG |
| Everything that touches the K10 library (screen, buttons, battery) | 2. HARDWARE LAYER (`hal_*`) |
| Location, last known position | 4 |
| JSON payloads | 5 |
| Offline queue (saved in flash, survives reboot), retry | 6 |
| POST, connectivity, heartbeat, acknowledgements (network task) | 7 |
| LOW_BATTERY / GPS_LOST / GPS_RESTORED logic | 8 |
| Screen, button handling | 9, 11 |

## Things to verify on the real board
- The `hal_*` K10 calls were written from DFRobot's documented API without the board available; if one does not compile for your library version, only section 2 needs changing.
- The K10 has no GPS: set `LOCATION_SOURCE` to `LOC_FIXED` (demo coordinates) or `LOC_SERIAL_NMEA` (UART GPS module + pins).
- `hal_batteryPercent()` returns -1 (n/a) until you plug in the K10 battery-capacity call; LOW_BATTERY is never sent while it is n/a.
- TLS certificate checking is off (`TLS_INSECURE`) for the prototype.
- Payloads and events are identical to the web version; Base44 needs no changes.

## Pairing with the Base44 caregiver app
1. A new/unpaired watch shows a **6-digit code** (valid 10 min; **A** = new code, **B** = skip).
2. In the caregiver app: child profile → **Link a watch** → type the code → Link.
3. The watch shows "Paired", stores the child id + private token in flash, and from then on sends every event for that child with header `X-Device-Token`.
4. To pair again: hold **A + B for 3 seconds**. Serial test keys: `p` fake pairing, `u` unpair.
Wire format: watch → `{"eventType":"PAIR_REQUEST","deviceId":"safesignal_k10_001","pairingCode":"482913"}` every 3 s; Base44 answers `{"paired":true,"childId":"…","childName":"…","deviceToken":"…"}` once linked. `DEVICE_ID` is now `safesignal_k10_001` (must be unique per watch).

## Server test (same as the curl command)
Serial Monitor: type `t`. It POSTs `{"eventType":"PAIR_REQUEST","deviceId":"test-1","pairingCode":"123456"}` to `API_ENDPOINT` and prints the HTTP code and reply. Expected `200 {"ok":true,"paired":false}`. `user-exception` means the Base44 function behind the URL crashed (old/missing watchStatus code). The pairing screen also shows "Server: OK" / "Server error 500" / "Server: no link".

## Fixed link details for this K10 (matches the app's "Connect Your SafeSignal Device" form)
- **Device ID:** `SAFESIGNAL-001`  (`DEVICE_ID` in section 1 of the sketch)
- **Pairing Code:** `482731`  (`FIXED_PAIR_CODE`; set `USE_FIXED_PAIR_CODE=false` for a random code each time)
Type both into the app form. They are shown on the watch pairing screen and printed in the Serial Monitor. Use a different Device ID/code for each additional watch.

## Low battery + camera (new)
- **LOW_BATTERY** is sent once when the battery reaches 20% or lower (re-arms above 20%). It needs a battery reading: install DFRobot's `DFRobot_UnihikerExpansion` library (uses `eunihiker.getBattery()`); otherwise the screen shows `Batt:n/a`. Test with Serial key `b`.
- **Camera:** any button press (OK / LOST / HELP) starts a camera session: photo now, then one every 60 s, uploaded as `CAMERA_IMAGE` (JPEG base64). Stops after 60 min or when Base44 replies `cameraCommand: "STOP"`; `START` / `CAPTURE_NOW` also supported. A red `CAM` marker shows on screen while on. Serial keys: `c` start, `x` stop.
- The camera calls are in the `hal_camera*` functions (section 2) and were written without the board.

## Camera started from the app (Start / Photo / Stop)
The watch checks for app commands on every heartbeat (now every 5 s). `cameraCommand` START turns the K10 camera on and shows the live picture on the K10 screen (press A/B to bring the buttons back for 10 s), photos are taken now and every 60 s; STOP turns it off; CAPTURE_NOW takes one photo. The heartbeat reports `cameraOn` true/false so the app can show the real camera state. Serial shows `app command received: cameraCommand = START` when the command arrives.

## LIVE camera (replaces the 60-second photos)
Caregiver app **Turn on live camera** -> watch gets `cameraCommand: START` -> K10 camera ON, live picture on the K10 screen, frames streamed to Base44 as `LIVE_FRAME` events (~1-3 per second, "near-live"). **Turn off** -> `cameraCommand: STOP` arrives in the reply to the next frame (~1 s) -> camera OFF and the K10 goes back to the main screen. `CAPTURE_NOW` saves one photo (`CAMERA_IMAGE`). Child presses OK/LOST/HELP also start the live camera (`LIVE_ON_PRESS`). Safety: auto-stop after 30 min or if no frame is delivered for 60 s. Serial keys: `c` live on, `x` live off.

## Camera only on the caregiver's request
`LIVE_ON_PRESS = false`: the child's buttons never start the camera. START/Photo commands are ignored for 20 s after power-on (stale commands). The K10 live bar shows `sent N` / `HTTP code` / `no link` so you can see whether the app receives the frames. Frames are `LIVE_FRAME` events; `"saveAsPhoto":true` marks a frame to also keep as a photo.

## Geofencing – danger zones (beach, pond, road …)

The `ZONES` table near the top of `safesignal_k10.ino` (section 1b) lists the dangerous places: name, type, centre
latitude/longitude, radius in metres and a **risk 1–4**. Replace the example coordinates with real ones.
The watch checks its GPS every 5 s **on the watch itself**, so it still works when the internet is down (the alert is
queued and sent first when the connection returns).

| Risk | Early warning | Entering the zone | Reminder while inside |
|------|---------------|-------------------|------------------------|
| 4 CRITICAL | 100 m before | caregiver **+ ACRN** (`urgent`), 1 GPS fix is enough | every 30 s |
| 3 HIGH | 50 m before | caregiver **+ ACRN** | every 60 s |
| 2 MEDIUM | – | caregiver | every 5 min |
| 1 LOW | – | caregiver (once) | – |

Events sent (all to the same `watchStatus` endpoint): `GEOFENCE_APPROACHING`, `GEOFENCE_ENTER`,
`GEOFENCE_STILL_INSIDE`, `GEOFENCE_EXIT`, each with `zoneName, zoneType, riskLevel, riskScore, distanceMeters,
radiusMeters, zoneLatitude, zoneLongitude, latitude, longitude, notifyCaregiver, notifyACRN, urgent, secondsInside,
repeatCount`. The heartbeat now also carries `latitude`, `longitude` and `dangerRisk`.
The child sees a coloured full-screen warning; the main screen shows `! Zone name` while inside. A geofence alert does
**not** turn the camera on – only the caregiver can do that.
The K10 board itself has no GPS: set `LOCATION_SOURCE = LOC_SERIAL_NMEA` and wire a GPS module (pins in section 1).

Test without leaving the house (Serial Monitor): `w` = walk into the next zone, `a` = stand just outside it,
`W` = leave, `r` = reset.
