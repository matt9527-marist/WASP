#!/usr/bin/env python3
"""
Serve ./src over HTTP and proxy /api/* to the WASP backend (default http://127.0.0.1:8080).
This avoids browser CORS when the dashboard uses fetch() against /api/... on the same origin.

Usage (from this directory):
  python serve_dashboard.py

Then open:
  http://127.0.0.1:8765/index.html

Environment:
  WASP_BACKEND  Backend base URL (default http://127.0.0.1:8080)
  PORT          Listen port (default 8765)
"""
from __future__ import annotations

import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from http.server import HTTPServer, SimpleHTTPRequestHandler

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src")
BACKEND = os.environ.get("WASP_BACKEND", "http://127.0.0.1:8080").rstrip("/")
PORT = int(os.environ.get("PORT", "8765"))


class DashboardHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path.startswith("/api/"):
            self._proxy_get(parsed)
            return
        super().do_GET()

    def _proxy_get(self, parsed: urllib.parse.ParseResult) -> None:
        target = f"{BACKEND}{parsed.path}"
        if parsed.query:
            target += f"?{parsed.query}"
        try:
            req = urllib.request.Request(target, method="GET")
            with urllib.request.urlopen(req, timeout=120) as resp:
                body = resp.read()
                self.send_response(resp.status)
                for key in ("Content-Type", "Content-Disposition", "Content-Length"):
                    if key in resp.headers:
                        self.send_header(key, resp.headers[key])
                self.end_headers()
                self.wfile.write(body)
        except urllib.error.HTTPError as e:
            payload = e.read() if e.fp else b""
            self.send_response(e.code)
            for key in ("Content-Type", "Content-Disposition"):
                if key in e.headers:
                    self.send_header(key, e.headers[key])
            self.end_headers()
            self.wfile.write(payload)
        except Exception as ex:
            msg = f"Proxy error: {ex}".encode()
            self.send_response(502)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(msg)))
            self.end_headers()
            self.wfile.write(msg)

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(), fmt % args))


def main() -> None:
    if not os.path.isdir(ROOT):
        print(f"Missing static folder: {ROOT}", file=sys.stderr)
        sys.exit(1)
    httpd = HTTPServer(("127.0.0.1", PORT), DashboardHandler)
    print(f"Serving {ROOT} at http://127.0.0.1:{PORT}/")
    print(f"Proxying /api -> {BACKEND}")
    print("Open http://127.0.0.1:{}/index.html".format(PORT))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
