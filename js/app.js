// BUTTON ACTIONS – wires buttons to location + API, shows feedback, handles cooldown.
// Failure handling (battery / GPS / heartbeat / acknowledgements) lives in js/monitor.js.
// The hidden dev panel (simulate failures) is wired at the bottom of this file.
(function () {
  const $ = (id) => document.getElementById(id);
  const buttons = document.querySelectorAll(".big");
  let cooling = false;

  const MESSAGES = {
    OK:   { cls: "ok",   title: "You're safe ✓", l1: "Caregiver updated", l2: "" },
    LOST: { cls: "lost", title: "Caregiver notified", l1: "", l2: "" },
    HELP: { cls: "help", title: "HELP SENT", l1: "Caregiver notified", l2: "" },
  };

  function showFeedback(cls, title, l1, l2, ms) {
    const f = $("feedback");
    f.className = "feedback " + cls;
    $("fb-title").textContent = title;
    $("fb-line1").textContent = l1 || "";
    $("fb-line2").textContent = l2 || "";
    clearTimeout(showFeedback.t);
    showFeedback.t = setTimeout(() => f.classList.add("hidden"), ms);
  }

  function updateQueueInfo() {
    const q = Api.pending();
    $("queue-info").textContent = q.length
      ? "Unsent: " + q.map((p) => p.status || p.eventType).join(", ")
      : "Nothing unsent";
  }

  async function press(status) {
    if (cooling) return;                       // duplicate-press protection
    cooling = true;
    buttons.forEach((b) => (b.disabled = true));
    setTimeout(() => { cooling = false; buttons.forEach((b) => (b.disabled = false)); }, CONFIG.COOLDOWN_MS);

    const loc = await Location.get();          // never fails; nulls if no GPS
    const payload = Api.buildPayload(status, loc);
    console.log("Sending SafeSignal event...");
    console.log(JSON.stringify(payload, null, 2));
    const m = MESSAGES[status];
    showFeedback(m.cls, "Sending…", "", "", 10000);

    const result = await Api.send(payload);
    if (result === "sent") {
      showFeedback(m.cls, m.title, m.l1, "Caregiver updated ✓", 2500);
    } else {
      showFeedback("fail", "Unable to send — retrying", "", "", 2500);
    }
    updateQueueInfo();
  }

  buttons.forEach((b) => b.addEventListener("click", () => press(b.dataset.status)));

  // Retry loop: keep trying queued events (status first) until they go through.
  async function retryLoop() {
    if (Api.pending().length) {
      const sent = await Api.flush();
      if (sent.some((p) => p.status)) showFeedback("ok", "Caregiver updated ✓", "", "", 2500);
      updateQueueInfo();
    }
  }
  setInterval(retryLoop, CONFIG.RETRY_INTERVAL_MS);

  // ---------- Hidden dev panel (not part of the child interface) ----------
  const panel = $("test-panel");
  const params = new URLSearchParams(location.search);
  function openPanel() { panel.classList.remove("hidden"); updateQueueInfo(); }
  if (CONFIG.TEST_MODE || params.has("test") || params.has("dev")) openPanel();
  if (CONFIG.TEST_MODE || params.has("test")) $("test-enabled").checked = true;
  // 5 quick taps on the "SafeSignal" title also reveals it
  let taps = [];
  $("title").addEventListener("click", () => {
    const now = Date.now();
    taps = taps.filter((t) => now - t < 2000).concat(now);
    if (taps.length >= 5) { taps = []; openPanel(); }
  });
  $("dev-close").addEventListener("click", () => panel.classList.add("hidden"));

  const log = (msg) => {
    const el = $("dev-log");
    el.textContent = new Date().toLocaleTimeString() + " " + msg + "\n" + el.textContent.split("\n").slice(0, 5).join("\n");
  };
  Api.onEvent = (p, result) => {
    if (p.eventType === "HEARTBEAT" && result === "sent") return;   // keep the log readable
    log((p.status || p.eventType) + " → " + result);
    updateQueueInfo();
  };

  const sims = {
    "sim-battery-low":  () => { Sim.battery = 18; Monitor.evalBattery(); },
    "sim-battery-ok":   () => { Sim.battery = 78; Monitor.evalBattery(); },
    "sim-gps-lost":     () => { Sim.gpsLost = true; Monitor.lastValidFixAt = Date.now() - CONFIG.GPS_LOSS_MS; Monitor.gpsTick(); },
    "sim-gps-ok":       () => { Sim.gpsLost = false; Monitor.gpsTick(); },
    "sim-offline":      () => { Sim.offline = true; Monitor.setOnline(false); Monitor.heartbeat(); },
    "sim-online":       () => { Sim.offline = false; Monitor.heartbeat(); },
    "sim-ack-safe":     () => Monitor.showAck("CHILD_IS_SAFE"),
    "sim-ack-way":      () => Monitor.showAck("ON_MY_WAY"),
    "sim-ack-nearby":   () => Monitor.showAck("NEED_NEARBY_HELP"),
    "sim-reset":        () => { Sim.battery = null; Sim.gpsLost = false; Sim.offline = false; Monitor.evalBattery(); Monitor.heartbeat(); },
  };
  Object.keys(sims).forEach((id) => $(id).addEventListener("click", () => { sims[id](); updateQueueInfo(); }));

  Monitor.start({ showFeedback });
  updateQueueInfo();
  retryLoop();
})();
