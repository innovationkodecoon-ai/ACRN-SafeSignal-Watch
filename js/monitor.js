// FAILURE HANDLING – battery, GPS, connectivity (heartbeat) monitors + caregiver acknowledgements.
// Everything is sent through Api.send() to the same Base44 endpoint. Nothing here shows technical errors.
const Monitor = {
  state: { battery: null, gpsOk: null, online: null, offlineSince: null },
  battery: null,          // BatteryManager when supported
  lowSent: false,         // LOW_BATTERY already reported for this dip?
  gpsLostSent: false,
  lastValidFixAt: Date.now(),
  lastAckKey: null,
  heartbeatBusy: false,
  ui: null,               // { showFeedback } supplied by app.js

  start(ui) {
    this.ui = ui;
    Api.onReachable = (ok) => this.setOnline(ok);
    Api.onResponse = (data) => this.handleAck(data);
    this.initBattery();
    this.gpsLoop();
    this.heartbeat();
    setInterval(() => this.heartbeat(), CONFIG.HEARTBEAT_INTERVAL_MS);
    setInterval(() => this.evalBattery(), 10000);
    window.addEventListener("online", () => this.heartbeat());
    window.addEventListener("offline", () => this.setOnline(false));
    this.render();
  },

  // ---------- BATTERY ----------
  initBattery() {
    if (!navigator.getBattery) return this.evalBattery();     // unsupported -> shows "n/a"
    navigator.getBattery().then((b) => {
      this.battery = b;
      b.addEventListener("levelchange", () => this.evalBattery());
      this.evalBattery();
    }).catch(() => this.evalBattery());
  },

  level() {
    if (Sim.battery !== null) return Sim.battery;
    return this.battery ? Math.round(this.battery.level * 100) : null;
  },

  evalBattery() {
    const level = this.level();
    this.state.battery = level;
    this.render();
    if (level === null) return;
    if (level < CONFIG.LOW_BATTERY_PERCENT) {
      if (!this.lowSent) {                       // only once per dip below the threshold
        this.lowSent = true;
        Api.send(Api.buildEvent("LOW_BATTERY", { batteryLevel: level }));
      }
    } else {
      this.lowSent = false;                      // recovered -> arm for the next dip
    }
  },

  // ---------- GPS ----------
  async gpsLoop() {
    try { await this.gpsTick(); } catch (e) { console.warn("[SafeSignal] gps tick", e); }
    setTimeout(() => this.gpsLoop(), CONFIG.GPS_CHECK_INTERVAL_MS);
  },

  async gpsTick() {
    const fix = await Location.getFix();
    if (fix.ok) {
      this.lastValidFixAt = Date.now();
      this.state.gpsOk = true;
      if (this.gpsLostSent) {
        this.gpsLostSent = false;
        Api.send(Api.buildEvent("GPS_RESTORED", { latitude: fix.latitude, longitude: fix.longitude }));
      }
    } else if (Date.now() - this.lastValidFixAt >= CONFIG.GPS_LOSS_MS && !this.gpsLostSent) {
      this.gpsLostSent = true;
      this.state.gpsOk = false;
      const lk = Location.lastKnown;
      Api.send(Api.buildEvent("GPS_LOST", {
        lastKnownLatitude: lk ? lk.latitude : null,
        lastKnownLongitude: lk ? lk.longitude : null,
      }));
    }
    this.render();
  },

  // ---------- CONNECTIVITY + HEARTBEAT ----------
  // Every ~15s a HEARTBEAT is POSTed. Base44 should flag the watch as offline after several missed beats,
  // because an offline watch cannot report that it is offline. Any HTTP answer = reachable; network failure = offline.
  async heartbeat() {
    if (this.heartbeatBusy) return;
    this.heartbeatBusy = true;
    try { await Api.send(Api.buildEvent("HEARTBEAT")); } finally { this.heartbeatBusy = false; }
  },

  setOnline(next) {
    const prev = this.state.online;
    if (prev === next) return;
    this.state.online = next;
    this.render();
    if (next === false) {
      this.state.offlineSince = Api.timestamp();
    } else if (prev === false) {
      this.onRestored();                         // offline -> online
    } else if (Api.pending().length) {
      Api.flush();                               // first contact after start-up
    }
  },

  // Back online: deliver queued events (status first), then tell Base44 the connection returned.
  async onRestored() {
    const since = this.state.offlineSince;
    while (Api.flushing) await new Promise((r) => setTimeout(r, 100));   // let an in-flight flush finish first
    const sent = await Api.flush();
    if (sent.some((p) => p.status) && this.ui) this.ui.showFeedback("ok", "Caregiver updated ✓", "", "", 2500);
    await Api.send(Api.buildEvent("CONNECTIVITY_RESTORED", { offlineSince: since }));
  },

  // ---------- CAREGIVER ACKNOWLEDGEMENT ----------
  // Base44 can answer ANY watch POST (the 15s heartbeat is the natural carrier) with:
  //   { "acknowledgement": "CHILD_IS_SAFE" | "ON_MY_WAY" | "NEED_NEARBY_HELP", "ackId": "optional-unique-id" }
  ACKS: {
    CHILD_IS_SAFE:    { title: "You're safe ✓",     line: "Caregiver says OK" },
    ON_MY_WAY:        { title: "Help is coming",    line: "Caregiver is on the way" },
    NEED_NEARBY_HELP: { title: "Help is coming",    line: "Someone nearby is helping" },
  },

  handleAck(data) {
    let a = data && (data.acknowledgement || data.ack);
    let id = data && data.ackId;
    if (a && typeof a === "object") { id = a.id || id; a = a.type; }
    if (!a || !this.ACKS[a]) return;
    const key = a + ":" + (id || "");
    if (key === this.lastAckKey) return;         // already shown
    this.lastAckKey = key;
    this.showAck(a);
  },

  showAck(type) {
    const m = this.ACKS[type];
    if (m && this.ui) this.ui.showFeedback("ack", m.title, m.line, "", 6000);
  },

  // ---------- STATUS AREA ----------
  render() {
    const $ = (id) => document.getElementById(id);
    const b = this.state.battery;
    $("battery").textContent = "Battery: " + (b === null ? "n/a" : b + "%");
    $("gps").textContent = "GPS: " + (this.state.gpsOk === null ? "…" : this.state.gpsOk ? "✓" : "✗");
    const on = this.state.online;
    $("conn-dot").className = "dot " + (on === true ? "online" : on === false ? "offline" : "");
    const top = $("connection");
    top.textContent = on === true ? "🟢 Connected" : "🔴 Offline";
    top.className = "connection " + (on === true ? "online" : "offline");
  },
};
