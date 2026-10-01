// API ENDPOINT + DEVICE SETTINGS – edit this file to connect to Base44.
const API_ENDPOINT = "https://3000-6abdd8a669e071e439539a84--b-a050b1a-8aa3351f348d1a7d.imported.base44-preview.app/api/status";

const CONFIG = {
  API_ENDPOINT,
  CHILD_ID: "child_001",
  DEVICE_ID: "safesignal_watch_001",
  COOLDOWN_MS: 2500,          // ignore presses for this long after a press
  RETRY_INTERVAL_MS: 5000,    // retry unsent status this often
  HEALTH_INTERVAL_MS: 10000,  // backend reachability check
  REQUEST_TIMEOUT_MS: 8000,
  GEO_TIMEOUT_MS: 5000,
  // Test mode: fake coordinates. Also enabled with ?test=1 in the URL or the Test panel.
  TEST_MODE: false,
  TEST_LAT: 1.3521,
  TEST_LNG: 103.8198,
  // Extra headers if Base44 needs them, e.g. { "api_key": "..." }
  EXTRA_HEADERS: {},
};
