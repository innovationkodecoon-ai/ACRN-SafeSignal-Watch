# ACRN SafeSignal – Child Smartwatch Interface

Plain HTML/CSS/JS (no build step, no dependencies) so it runs in any browser, including a UNIHIKER.

## Which file does what
| Concern | File |
|---|---|
| 1. Interface (layout, colours, sizes) | `index.html`, `css/style.css` |
| 2. API endpoint & device/child IDs, timings | `js/config.js` (`API_ENDPOINT`) |
| 3. Button actions, cooldown, confirmation screens | `js/app.js` |
| 4. Geolocation + test-mode fake coordinates | `js/location.js` |
| 5. POST, retry queue, connection status | `js/api.js` |
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
4. Press a button: the server terminal prints the JSON; the watch shows "Sent ✓".
5. Or without the app:
   `curl -X POST http://localhost:3000/api/status -H "Content-Type: application/json" -d '{"childId":"child_001","deviceId":"safesignal_watch_001","status":"HELP","latitude":1.3521,"longitude":103.8198,"timestamp":"2026-10-01T10:30:00+08:00"}'`
6. Stop the server and press a button: "Unable to send / Trying again…" appears; restart the server and the unsent status is delivered automatically within ~5 s.
7. Alternative with no code: create a URL at https://webhook.site and paste it as `API_ENDPOINT`.

## Connect to Base44
Paste your Base44 endpoint URL into `API_ENDPOINT`. If it needs an API key header, add it to `EXTRA_HEADERS` in `js/config.js`. The endpoint must allow CORS from where the watch page is hosted.
Note: the connection indicator sends a `HEAD` request to the endpoint; a network/CORS failure on HEAD shows Offline even if POST works. If that happens, point the check at a dedicated health URL.
