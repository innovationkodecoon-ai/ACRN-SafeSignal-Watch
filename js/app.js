// BUTTON ACTIONS – wires buttons to location + API, shows feedback, handles cooldown.
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
    const q = Api.getUnsent();
    $("queue-info").textContent = q ? `Unsent: ${q.status} @ ${q.timestamp}` : "No unsent status";
  }

  async function updateConnection() {
    const ok = await Api.checkReachable();
    const el = $("connection");
    el.textContent = ok ? "🟢 Connected" : "🔴 Offline";
    el.className = "connection " + (ok ? "online" : "offline");
    return ok;
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

    const sent = await Api.send(payload);
    if (sent) {
      showFeedback(m.cls, m.title, m.l1, "Caregiver updated ✓", 2500);
    } else {
      showFeedback("fail", "Unable to send — retrying", "", "", 2500);
    }
    updateQueueInfo();
    updateConnection();
  }

  buttons.forEach((b) => b.addEventListener("click", () => press(b.dataset.status)));

  // Retry loop + reconnect handling
  async function retryLoop() {
    if (Api.getUnsent()) {
      if (await Api.retryUnsent()) showFeedback("ok", "Sent ✓", "Earlier status delivered", "", 2500);
      updateQueueInfo();
    }
  }
  setInterval(retryLoop, CONFIG.RETRY_INTERVAL_MS);
  setInterval(updateConnection, CONFIG.HEALTH_INTERVAL_MS);
  window.addEventListener("online", () => { updateConnection(); retryLoop(); });
  window.addEventListener("offline", updateConnection);

  // Test panel
  $("test-toggle").addEventListener("click", () => { $("test-panel").classList.toggle("hidden"); updateQueueInfo(); });
  if (CONFIG.TEST_MODE || new URLSearchParams(location.search).has("test")) {
    $("test-enabled").checked = true;
    $("test-panel").classList.remove("hidden");
  }

  // Battery (if the browser supports it)
  if (navigator.getBattery) {
    navigator.getBattery().then((b) => {
      const show = () => ($("battery").textContent = "Battery: " + Math.round(b.level * 100) + "%");
      show(); b.addEventListener("levelchange", show);
    }).catch(() => {});
  }

  updateConnection();
  updateQueueInfo();
  retryLoop();
})();
