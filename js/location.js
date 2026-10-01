// GEOLOCATION – never throws; resolves to {latitude, longitude} (nulls if unavailable).
const Location = {
  testMode() {
    const el = document.getElementById("test-enabled");
    return CONFIG.TEST_MODE || (el && el.checked);
  },

  async get() {
    if (this.testMode()) {
      const lat = parseFloat(document.getElementById("test-lat").value);
      const lng = parseFloat(document.getElementById("test-lng").value);
      return {
        latitude: Number.isFinite(lat) ? lat : CONFIG.TEST_LAT,
        longitude: Number.isFinite(lng) ? lng : CONFIG.TEST_LNG,
      };
    }
    if (!("geolocation" in navigator)) return { latitude: null, longitude: null };
    return new Promise((resolve) => {
      navigator.geolocation.getCurrentPosition(
        (p) => resolve({ latitude: p.coords.latitude, longitude: p.coords.longitude }),
        () => resolve({ latitude: null, longitude: null }),
        { enableHighAccuracy: true, timeout: CONFIG.GEO_TIMEOUT_MS, maximumAge: 10000 }
      );
    });
  },
};
