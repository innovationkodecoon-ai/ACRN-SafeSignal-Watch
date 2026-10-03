// Local test receiver (no dependencies). Run: node tools/mock-server.js
// Serves the watch app at http://localhost:3000 and accepts POSTs at /api/status.
const http = require("http"), fs = require("fs"), path = require("path");
const PORT = process.env.PORT || 3000;
const ROOT = path.join(__dirname, "..");
const TYPES = { ".html": "text/html", ".css": "text/css", ".js": "text/javascript" };
const VALID = ["OK", "LOST", "HELP"];
const EVENTS = ["LOW_BATTERY", "GPS_LOST", "GPS_RESTORED", "HEARTBEAT", "CONNECTIVITY_RESTORED"];
let pendingAck = null; // set with: curl -X POST "localhost:3000/api/ack?type=ON_MY_WAY" ; returned once in the next response
const CORS = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "Content-Type, api_key", "Access-Control-Allow-Methods": "POST, GET, HEAD, OPTIONS" };

http.createServer((req, res) => {
  if (req.method === "OPTIONS") { res.writeHead(204, CORS); return res.end(); }
  if (req.url.startsWith("/api/ack")) {
    pendingAck = new URL(req.url, "http://x").searchParams.get("type");
    console.log("\n>>> Will acknowledge next response with:", pendingAck);
    res.writeHead(200, CORS); return res.end("ok");
  }
  if (req.url.startsWith("/api/status")) {
    if (req.method !== "POST") { res.writeHead(200, CORS); return res.end("SafeSignal mock endpoint (POST JSON here)"); }
    let body = "";
    req.on("data", (c) => (body += c));
    req.on("end", () => {
      try {
        const d = JSON.parse(body);
        if (d.eventType) { if (!EVENTS.includes(d.eventType)) throw new Error("unknown eventType"); }
        else if (!VALID.includes(d.status)) throw new Error("status must be OK, LOST or HELP");
        if (d.eventType === "HEARTBEAT") process.stdout.write("."); // quiet: one dot per heartbeat
        else console.log("\n=== RECEIVED", new Date().toISOString(), "===\n" + JSON.stringify(d, null, 2));
        const reply = { ok: true };
        if (pendingAck) { reply.acknowledgement = pendingAck; reply.ackId = String(Date.now()); pendingAck = null; }
        res.writeHead(200, { ...CORS, "Content-Type": "application/json" });
        res.end(JSON.stringify(reply));
      } catch (e) {
        console.log("REJECTED:", e.message, body);
        res.writeHead(400, { ...CORS, "Content-Type": "application/json" });
        res.end(JSON.stringify({ ok: false, error: e.message }));
      }
    });
    return;
  }
  const urlPath = req.url.split("?")[0];
  const file = path.join(ROOT, urlPath === "/" ? "index.html" : urlPath);
  if (!file.startsWith(ROOT) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); return res.end("Not found"); }
  res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "text/plain" });
  fs.createReadStream(file).pipe(res);
}).listen(PORT, () => console.log(`SafeSignal mock server: http://localhost:${PORT}  (POST /api/status)`));
