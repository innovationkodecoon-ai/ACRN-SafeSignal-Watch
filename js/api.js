// API + RETRY/ERROR HANDLING – POSTs payloads to Base44, queues unsent events locally, retries.
// Events: status buttons ({status}) and system events ({eventType}). All go to CONFIG.API_ENDPOINT.
const VALID_STATUS = ["OK", "LOST", "HELP"];
const QUEUE_KEY = "safesignal_queue_v2";

// Queue slots (only the LATEST event of each kind is kept). HEARTBEAT and CONNECTIVITY_RESTORED are never queued.
const SLOT_ORDER = ["status", "LOW_BATTERY", "GPS"];

const Api = {
  onReachable: null,   // fn(bool)  – backend answered (true) or network failed (false)
  onResponse: null,    // fn(data)  – parsed JSON body of a successful response (acknowledgements)
  onEvent: null,       // fn(payload, result) – for the dev panel log
  flushing: false,

  configured() {
    return CONFIG.API_ENDPOINT && !CONFIG.API_ENDPOINT.startsWith("PASTE_");
  },

  // ISO 8601 with local UTC offset, e.g. 2026-10-01T10:30:00+08:00
  timestamp() {
    const d = new Date(), p = (n) => String(n).padStart(2, "0");
    const off = -d.getTimezoneOffset(), sign = off >= 0 ? "+" : "-";
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}` +
      `${sign}${p(Math.floor(Math.abs(off) / 60))}:${p(Math.abs(off) % 60)}`;
  },

  buildPayload(status, loc) {
    if (!VALID_STATUS.includes(status)) throw new Error("Invalid status: " + status);
    return {
      childId: CONFIG.CHILD_ID,
      deviceId: CONFIG.DEVICE_ID,
      status,
      latitude: loc.latitude,
      longitude: loc.longitude,
      timestamp: this.timestamp(),
    };
  },

  // System event: { childId, deviceId, eventType, ...extra, timestamp }
  buildEvent(eventType, extra) {
    return { childId: CONFIG.CHILD_ID, deviceId: CONFIG.DEVICE_ID, eventType, ...(extra || {}), timestamp: this.timestamp() };
  },

  // Low-level POST. Resolves {ok, status, data} for ANY HTTP response; throws {network:true} if unreachable.
  async post(payload) {
    if (!this.configured()) { const e = new Error("API endpoint not configured"); e.network = true; throw e; }
    if (Sim.offline || !navigator.onLine) { const e = new Error("offline"); e.network = true; throw e; }
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), CONFIG.REQUEST_TIMEOUT_MS);
    try {
      console.log("[SafeSignal] REQUEST POST " + CONFIG.API_ENDPOINT, payload);
      let res;
      try {
        res = await fetch(CONFIG.API_ENDPOINT, {
          method: "POST",
          headers: { "Content-Type": "application/json", ...CONFIG.EXTRA_HEADERS },
          body: JSON.stringify(payload),
          signal: ctrl.signal,
        });
      } catch (err) { err.network = true; throw err; }
      const text = await res.text().catch(() => "");
      console.log("[SafeSignal] RESPONSE " + res.status + " " + res.statusText, text);
      let data = null;
      try { data = JSON.parse(text); } catch (e) {}
      if (this.onReachable) this.onReachable(true);
      if (res.ok && data && this.onResponse) this.onResponse(data);
      return { ok: res.ok, status: res.status, data };
    } finally { clearTimeout(t); }
  },

  // ---- local queue ----
  slotFor(p) {
    if (p.status) return "status";
    if (p.eventType === "LOW_BATTERY") return "LOW_BATTERY";
    if (p.eventType === "GPS_LOST" || p.eventType === "GPS_RESTORED") return "GPS";
    return null;
  },
  readQueue() { try { return JSON.parse(localStorage.getItem(QUEUE_KEY)) || {}; } catch (e) { return {}; } },
  writeQueue(q) { try { localStorage.setItem(QUEUE_KEY, JSON.stringify(q)); } catch (e) {} },
  saveSlot(slot, p) { const q = this.readQueue(); q[slot] = p; this.writeQueue(q); },
  clearSlot(slot, p) {
    const q = this.readQueue();
    if (q[slot] && q[slot].timestamp === p.timestamp) { delete q[slot]; this.writeQueue(q); }
  },
  pending() { const q = this.readQueue(); return SLOT_ORDER.filter((s) => q[s]).map((s) => q[s]); },

  // Send one payload. Returns "sent" | "queued" | "dropped".
  // Status buttons are kept and retried on any failure. System events are kept on network/5xx failure,
  // dropped on a 4xx (the backend rejected the payload, retrying will not help).
  async send(payload) {
    const slot = this.slotFor(payload);
    let result;
    try {
      const r = await this.post(payload);
      if (r.ok) { if (slot) this.clearSlot(slot, payload); result = "sent"; }
      else {
        console.warn("[SafeSignal] backend rejected event:", r.status);
        if (slot && (payload.status || r.status >= 500 || r.status === 408 || r.status === 429)) { this.saveSlot(slot, payload); result = "queued"; }
        else { if (slot) this.clearSlot(slot, payload); result = "dropped"; }
      }
    } catch (e) {
      console.warn("[SafeSignal] send failed:", e.message);
      if (e.network && this.onReachable) this.onReachable(false);
      if (slot) { this.saveSlot(slot, payload); result = "queued"; } else result = "dropped";
    }
    if (this.onEvent) this.onEvent(payload, result);
    return result;
  },

  // Try to deliver everything queued, highest priority first. Returns the payloads that were sent.
  async flush() {
    if (this.flushing) return [];
    this.flushing = true;
    const sent = [];
    try {
      for (const p of this.pending()) {
        const r = await this.send(p);
        if (r === "queued") break;          // still failing – try again on the next tick
        if (r === "sent") sent.push(p);
      }
    } finally { this.flushing = false; }
    return sent;
  },
};
