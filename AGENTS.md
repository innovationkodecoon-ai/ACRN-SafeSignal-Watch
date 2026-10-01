# Agent notes
- No build step. `tools/mock-server.js` serves static files from disk (edits show on browser reload) and accepts POSTs at `/api/status`; it runs under `node --watch`.
- `js/config.js` ships with `API_ENDPOINT = "PASTE_BASE44_ENDPOINT_HERE"`, so sends fail/queue until set. For local testing use `"/api/status"` (mock server). `?test=1` uses fake GPS coords.
- Verify: `curl -X POST localhost:3000/api/status -H 'Content-Type: application/json' -d '{"status":"OK"}'`; received payloads appear in `docker compose -f docker-compose.base44.yml logs web`.
