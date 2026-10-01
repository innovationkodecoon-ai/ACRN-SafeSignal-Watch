// API + RETRY/ERROR HANDLING – POSTs payloads, queues the last unsent status, checks connectivity.
const VALID_STATUS = ["OK", "LOST", "HELP"];
const QUEUE_KEY = "safesignal_unsent";

const Api = {
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

  async post(payload) {
    if (!this.configured()) throw new Error("API endpoint not configured");
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), CONFIG.REQUEST_TIMEOUT_MS);
    try {
      console.log("[SafeSignal] REQUEST POST " + CONFIG.API_ENDPOINT, payload);
      const res = await fetch(CONFIG.API_ENDPOINT, {
        method: "POST",
        headers: { "Content-Type": "application/json", ...CONFIG.EXTRA_HEADERS },
        body: JSON.stringify(payload),
        signal: ctrl.signal,
      });
      const text = await res.text().catch(() => "");
      console.log("[SafeSignal] RESPONSE " + res.status + " " + res.statusText, text);
      if (!res.ok) throw new Error("HTTP " + res.status);
      return true;
    } finally { clearTimeout(t); }
  },

  // Local queue: only the LAST unsent status is kept.
  saveUnsent(payload) { try { localStorage.setItem(QUEUE_KEY, JSON.stringify(payload)); } catch (e) {} },
  getUnsent() { try { return JSON.parse(localStorage.getItem(QUEUE_KEY)); } catch (e) { return null; } },
  clearUnsent() { try { localStorage.removeItem(QUEUE_KEY); } catch (e) {} },

  // Try to send; on failure keep it queued. Returns true/false.
  async send(payload) {
    try {
      await this.post(payload);
      const q = this.getUnsent();
      if (q && q.timestamp === payload.timestamp) this.clearUnsent();
      return true;
    } catch (e) {
      console.warn("[SafeSignal] send failed:", e.message);
      this.saveUnsent(payload);
      return false;
    }
  },

  async retryUnsent() {
    const q = this.getUnsent();
    return q ? this.send(q) : true;
  },

  // Reachability: any HTTP response (even opaque/no-cors) means the server is up.
  async checkReachable() {
    if (!navigator.onLine || !this.configured()) return false;
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), CONFIG.REQUEST_TIMEOUT_MS);
    try {
      await fetch(CONFIG.API_ENDPOINT, { method: "HEAD", mode: "no-cors", cache: "no-store", signal: ctrl.signal });
      return true;
    } catch (e) { return false; }
    finally { clearTimeout(t); }
  },
};
