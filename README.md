# ACRN SafeSignal – Child Smartwatch Interface

Plain HTML/CSS/JS (no build step, no dependencies) so it runs in any browser, including a UNIHIKER.

## Which file does what
| Concern | File |
|---|---|
| 1. Interface (layout, colours, sizes) | `index.html`, `css/style.css` |
| 2. API endpoint & device/child IDs, timings | `js/config.js` (`API_ENDPOINT`) |
| 3. Button actions, cooldown, confirmation screens | `js/app.js` |
| 4. Geolocation + test-mode fake coordinates | `js/location.js` |
| 5. POST, retry queue (offline queueing) | `js/api.js` |
| 6. Failure handling: battery, GPS, heartbeat, connectivity, caregiver acknowledgements | `js/monitor.js` |
| Hidden dev panel (simulate failures) | `index.html` (`#test-panel`) + bottom of `js/app.js`; simulation switches `Sim` in `js/config.js` |
| Local test receiver | `tools/mock-server.js` |

## Payload (POST, `Content-Type: application/json`)
```json
{ "childId":"child_001","deviceId":"safesignal_watch_001","status":"LOST",
  "latitude":1.3521,"longitude":103.8198,"timestamp":"2026-10-01T10:30:00+08:00" }
```
`status` is only `OK`, `LOST` or `HELP`. Latitude/longitude are `null` if GPS is unavailable.
All three buttons send the full payload (OK included).

## Test the POST before Base44
1. `node tools/mock-server.js` (Node 18+; serves the app and receives POSTs).
2. In `js/config.js` set `const API_ENDPOINT = "http://localhost:3000/api/status";`
3. Open `http://localhost:3000/?test=1` (test mode uses fake coordinates 1.3521, 103.8198).
4. Press a button: the server terminal prints the JSON; the watch shows "Caregiver updated ✓".
5. Or without the app:
   `curl -X POST http://localhost:3000/api/status -H "Content-Type: application/json" -d '{"childId":"child_001","deviceId":"safesignal_watch_001","status":"HELP","latitude":1.3521,"longitude":103.8198,"timestamp":"2026-10-01T10:30:00+08:00"}'`
6. Stop the server and press a button: "Unable to send / Trying again…" appears; restart the server and the unsent status is delivered automatically within ~5 s.
7. Alternative with no code: create a URL at https://webhook.site and paste it as `API_ENDPOINT`.

## Connect to Base44
The Base44 endpoint is already set in `API_ENDPOINT` (`js/config.js`, line 2). If it needs an API key header, add it to `EXTRA_HEADERS` in `js/config.js`. The endpoint must allow CORS from where the watch page is hosted.
Note: the connection indicator sends a `HEAD` request to the endpoint; a network/CORS failure on HEAD shows Offline even if POST works. If that happens, point the check at a dedicated health URL.

## Failure handling (events sent to the same endpoint)
All payloads include `childId`, `deviceId`, `timestamp`. System events carry `eventType` instead of `status`.

| Event | Extra fields | When |
|---|---|---|
| `LOW_BATTERY` | `batteryLevel` | once when battery first drops below 20%; re-arms after it returns to 20%+ |
| `GPS_LOST` | `lastKnownLatitude/Longitude` | no valid location for 30 s (sampled every 5 s) |
| `GPS_RESTORED` | `latitude`, `longitude` | first valid location after GPS_LOST |
| `HEARTBEAT` | – | every 15 s. Base44 should flag the watch offline after ~3 missed beats (45 s) |
| `CONNECTIVITY_RESTORED` | `offlineSince` | after reconnecting, once queued events are delivered |

Offline: the top indicator turns red/"Offline", the latest unsent OK/LOST/HELP (plus LOW_BATTERY and GPS event) is saved in `localStorage` and retried every 5 s; status events are delivered first. HEARTBEAT is never queued.
Caregiver acknowledgement: reply to any watch POST (the 15 s heartbeat is the natural carrier) with `{"acknowledgement":"CHILD_IS_SAFE"|"ON_MY_WAY"|"NEED_NEARBY_HELP","ackId":"unique"}` and the watch shows a short message once per `ackId`.
**Base44 must accept `eventType` payloads** (no `status` field) and return 200; otherwise they are logged and dropped.

## Simulating failures
Open the page with `?dev=1` (panel only) or `?test=1` (panel + fake coordinates), or tap the "SafeSignal" title 5 times. Buttons: Battery 18% / 78%, GPS lost (fires immediately instead of waiting 30 s) / restored, Internet off / on, caregiver acks, Reset all. With `node tools/mock-server.js`, `curl -X POST "localhost:3000/api/ack?type=ON_MY_WAY"` makes the next response carry an acknowledgement.
