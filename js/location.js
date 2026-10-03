// GEOLOCATION – never throws. get() resolves to {latitude, longitude} (nulls if unavailable);
// getFix() also reports success and remembers the most recent valid location (last known).
const LASTKNOWN_KEY = "safesignal_lastknown";

const Location = {
  lastKnown: (() => {
    try { return JSON.parse(localStorage.getItem(LASTKNOWN_KEY)); } catch (e) { return null; }
  })(),

  testMode() {
    const el = document.getElementById("test-enabled");
    return CONFIG.TEST_MODE || (el && el.checked);
  },

  rememberValid(latitude, longitude) {
    this.lastKnown = { latitude, longitude, timestamp: Date.now() };
    try { localStorage.setItem(LASTKNOWN_KEY, JSON.stringify(this.lastKnown)); } catch (e) {}
  },

  // -> { ok, latitude, longitude }
  async getFix() {
    if (Sim.gpsLost) return { ok: false, latitude: null, longitude: null };
    let fix;
    if (this.testMode()) {
      const lat = parseFloat(document.getElementById("test-lat").value);
      const lng = parseFloat(document.getElementById("test-lng").value);
      fix = { ok: true,
        latitude: Number.isFinite(lat) ? lat : CONFIG.TEST_LAT,
        longitude: Number.isFinite(lng) ? lng : CONFIG.TEST_LNG };
    } else if (!("geolocation" in navigator)) {
      fix = { ok: false, latitude: null, longitude: null };
    } else {
      fix = await new Promise((resolve) => {
        navigator.geolocation.getCurrentPosition(
          (p) => resolve({ ok: true, latitude: p.coords.latitude, longitude: p.coords.longitude }),
          () => resolve({ ok: false, latitude: null, longitude: null }),
          { enableHighAccuracy: true, timeout: CONFIG.GEO_TIMEOUT_MS, maximumAge: 5000 }
        );
      });
    }
    if (fix.ok) this.rememberValid(fix.latitude, fix.longitude);
    return fix;
  },

  // Used by the I'M OK / LOST / HELP buttons.
  async get() {
    const f = await this.getFix();
    return { latitude: f.latitude, longitude: f.longitude };
  },
};
