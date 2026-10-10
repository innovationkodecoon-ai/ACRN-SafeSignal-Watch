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
