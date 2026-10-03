// API ENDPOINT + DEVICE SETTINGS – edit this file to connect to Base44.
const API_ENDPOINT = "https://safesignal-caregiver-to-watch-acrn.base44.app/functions/watchStatus";

const CONFIG = {
  API_ENDPOINT,
  CHILD_ID: "child_001",
  DEVICE_ID: "safesignal_watch_001",
  COOLDOWN_MS: 2500,          // ignore presses for this long after a press
  RETRY_INTERVAL_MS: 5000,    // retry unsent status this often
  HEARTBEAT_INTERVAL_MS: 15000,  // "I'm alive" ping; Base44 flags the watch if several are missed
  LOW_BATTERY_PERCENT: 20,       // LOW_BATTERY event when level drops below this
  GPS_CHECK_INTERVAL_MS: 5000,   // how often location is sampled
  GPS_LOSS_MS: 30000,            // no valid location for this long => GPS_LOST
  REQUEST_TIMEOUT_MS: 8000,
  GEO_TIMEOUT_MS: 5000,
  // Test mode: fake coordinates. Also enabled with ?test=1 in the URL (this also reveals the dev panel).
  TEST_MODE: false,
  TEST_LAT: 1.3521,
  TEST_LNG: 103.8198,
  // Extra headers if Base44 needs them, e.g. { "api_key": "..." }
  EXTRA_HEADERS: {},
};

// DEV / TEST SIMULATION – flipped by the hidden dev panel (see js/app.js). null/false = use real conditions.
const Sim = { battery: null, gpsLost: false, offline: false };
