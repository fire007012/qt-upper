from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict
from urllib.parse import parse_qs, urlparse
import json
import threading

from bridge_core import BridgeCore
from bridge_protocol import now_ms
from debug_events import RingBufferEventSink


HTML = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>host_bridge_node debug</title>
  <style>
    body { margin: 0; font-family: system-ui, sans-serif; background: #f5f7f8; color: #1f2933; }
    header { padding: 14px 18px; background: #263238; color: white; }
    main { padding: 16px; display: grid; gap: 14px; grid-template-columns: 320px 1fr; }
    section { background: white; border: 1px solid #d9e2e7; border-radius: 6px; padding: 12px; }
    h2 { font-size: 16px; margin: 0 0 10px; }
    pre { white-space: pre-wrap; overflow-wrap: anywhere; font-size: 12px; background: #f0f3f5; padding: 10px; border-radius: 4px; }
    table { width: 100%; border-collapse: collapse; font-size: 12px; }
    th, td { border-bottom: 1px solid #e5eaee; padding: 6px; text-align: left; vertical-align: top; }
    .ok { color: #137333; font-weight: 600; }
    .warn { color: #b06000; font-weight: 600; }
    .bad { color: #b3261e; font-weight: 600; }
    @media (max-width: 900px) { main { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <header><strong>host_bridge_node debug</strong> <span id="ts"></span></header>
  <main>
    <section>
      <h2>State</h2>
      <pre id="state">loading...</pre>
      <h2>Cameras</h2>
      <pre id="cameras">loading...</pre>
    </section>
    <section>
      <h2>Recent Events</h2>
      <table>
        <thead><tr><th>ID</th><th>Level</th><th>Category</th><th>Message</th><th>Data</th></tr></thead>
        <tbody id="events"></tbody>
      </table>
    </section>
  </main>
<script>
async function refresh() {
  const [state, cameras, events] = await Promise.all([
    fetch('/api/state').then(r => r.json()),
    fetch('/api/cameras').then(r => r.json()),
    fetch('/api/events?limit=80').then(r => r.json()),
  ]);
  document.getElementById('ts').textContent = new Date().toLocaleTimeString();
  document.getElementById('state').textContent = JSON.stringify(state, null, 2);
  document.getElementById('cameras').textContent = JSON.stringify(cameras, null, 2);
  document.getElementById('events').innerHTML = events.events.map(e => {
    const cls = e.level === 'error' ? 'bad' : (e.level === 'warning' ? 'warn' : 'ok');
    return `<tr><td>${e.id}</td><td class="${cls}">${e.level}</td><td>${e.category}</td><td>${e.message}</td><td><pre>${JSON.stringify(e.data, null, 2)}</pre></td></tr>`;
  }).join('');
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
"""


class DebugHttpServer:
    def __init__(self, host: str, port: int, core: BridgeCore, events: RingBufferEventSink) -> None:
        self.host = host
        self.port = port
        self.core = core
        self.events = events
        self.httpd: ThreadingHTTPServer | None = None

    def start(self) -> None:
        server = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                parsed = urlparse(self.path)
                if parsed.path == "/":
                    self._send_html(HTML)
                elif parsed.path == "/api/state":
                    self._send_json({
                        "timestamp_ms": now_ms(),
                        "state": server.core.state_snapshot(),
                    })
                elif parsed.path == "/api/events":
                    query = parse_qs(parsed.query)
                    limit = int(query.get("limit", ["100"])[0])
                    self._send_json({
                        "timestamp_ms": now_ms(),
                        "events": server.events.recent(limit),
                    })
                elif parsed.path == "/api/cameras":
                    self._send_json({
                        "timestamp_ms": now_ms(),
                        "cameras": server.core.current_cameras(),
                    })
                else:
                    self.send_error(404)

            def log_message(self, fmt: str, *args: Any) -> None:
                return

            def _send_html(self, body: str) -> None:
                data = body.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def _send_json(self, payload: Dict[str, Any]) -> None:
                data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer((self.host, self.port), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.events.emit("debug_ui", "debug HTTP UI started", data={
            "host": self.host,
            "port": self.port,
        })
        print(f"[bridge] debug UI http://{self.host}:{self.port}", flush=True)
