"""Plain-HTTP reverse proxy to the controller for the integrated browser.

The integrated browser's port-forward downgrades https→http and rejects the
controller's self-signed cert, so the UI won't load directly on
https://127.0.0.1:8043. This proxy TLS-terminates to the controller and
exposes it over plain http on localhost so the integrated browser can load
it. It handles three things the controller needs:

1. **Cookie `Secure` stripping.** The session cookie (`CONTROLLER_SESSIONID`) is
   marked `Secure`, so a dumb socat/tcp-tunnel tunnel breaks login over http.
   The proxy rewrites `Set-Cookie` to drop the `Secure` (and `SameSite=None`)
   attributes so the browser keeps the session over plain http.

2. **gzip/deflate decompression.** Browsers send `Accept-Encoding: gzip` and
   the controller responds with `Content-Encoding: gzip` over chunked
   transfer-encoding. The proxy de-chunks and decompresses the body and
   strips the `Content-Encoding` header, otherwise the browser shows
   `ERR_CONTENT_DECODING_FAILED` on the SPA shell.

3. **WebSocket tunneling.** The controller's Tools → Network Check and live
   status feed use STOMP over WebSocket at
   `/{omadacId}/ws/status/{id}/{rand}/websocket`. A plain HTTP proxy returns
   400 for the `Upgrade: websocket` request, so the proxy detects the upgrade
   header and raw-tunnels a bidirectional byte pump to the upstream TLS
   socket (forwarding the handshake with `Host` fixed, `Upgrade`/`Connection`
   headers preserved, and the session cookie forwarded). The controller
   returns `101 Switching Protocols` and frames flow through.

Usage:
    python3 tools/controller_proxy.py 8090
then open http://localhost:8090 in the integrated browser and log in with the
controller admin account.

Stderr logs `proxy WS >> ...` / `proxy WS << upstream status=...` lines for
each WebSocket upgrade so you can confirm the STOMP/WebSocket channel is live
(Watching these is the easiest way to tell whether the Tools → Network Check
live-probe flow will work — a 101 means the device-monitor channel has a
working transport; a 400 means the proxy isn't tunneling the upgrade).

Only depends on the Python 3 standard library (no third-party packages).
"""
from __future__ import annotations

import http.server
import os
import ssl
import socket
import sys
import threading
from urllib.parse import urlsplit

# Adapted for the repository's integrated test environment. The controller
# runs in another container, so the upstream and bind addresses come from
# the environment instead of being fixed to loopback.
UPSTREAM_HOST = os.environ.get("CONTROLLER_HOST", "127.0.0.1")
UPSTREAM_PORT = int(os.environ.get("CONTROLLER_PORT", "8043"))
BIND_ADDR = os.environ.get("PROXY_BIND", "127.0.0.1")


def _strip_secure(cookie_header: str) -> str:
    """Remove `Secure` (and `SameSite=None`) attributes so the cookie works
    over plain http. Keep everything else."""
    if not cookie_header:
        return cookie_header
    parts = [p.strip() for p in cookie_header.split(";")]
    keep = [p for p in parts if p.lower() not in ("secure", "samesite=none")]
    return "; ".join(keep)


def _dechunk(body: bytes) -> bytes:
    """Decode an HTTP/1.1 chunked-transfer body into its plain bytes."""
    out = bytearray()
    i = 0
    while i < len(body):
        crlf = body.find(b"\r\n", i)
        if crlf < 0:
            break
        size_field = body[i:crlf].split(b";")[0].strip()
        try:
            size = int(size_field, 16)
        except ValueError:
            break
        if size == 0:
            break
        start = crlf + 2
        out += body[start:start + size]
        i = start + size + 2  # skip the trailing \r\n
    return bytes(out)


class Proxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _upstream(self, method: str, body: bytes = b"") -> None:
        # Build the upstream path from the original URL.
        parsed = urlsplit(self.path)
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query

        # Open a TLS connection to the controller (self-signed cert ignored).
        ctx = ssl._create_unverified_context()
        raw = socket.create_connection((UPSTREAM_HOST, UPSTREAM_PORT), timeout=30)
        upstream = ctx.wrap_socket(raw, server_hostname=UPSTREAM_HOST)

        # Forward headers, fixing Host and Connection.
        headers = []
        has_host = False
        for key, val in self.headers.items():
            lk = key.lower()
            if lk == "host":
                has_host = True
                headers.append(f"Host: {UPSTREAM_HOST}:{UPSTREAM_PORT}")
            elif lk in ("connection", "proxy-connection", "keep-alive", "te", "trailers"):
                continue
            else:
                headers.append(f"{key}: {val}")
        if not has_host:
            headers.append(f"Host: {UPSTREAM_HOST}:{UPSTREAM_PORT}")
        headers.append("Connection: close")

        req_line = f"{method} {path} HTTP/1.1\r\n"
        req = (req_line + "\r\n".join(headers) + "\r\n\r\n").encode("latin-1")
        if body:
            req += body
        upstream.sendall(req)

        # Read the full upstream response.
        resp = b""
        upstream.settimeout(30)
        try:
            while True:
                chunk = upstream.recv(65536)
                if not chunk:
                    break
                resp += chunk
        except socket.timeout:
            pass
        finally:
            upstream.close()

        # Split status line / headers / body.
        header_end = resp.find(b"\r\n\r\n")
        if header_end < 0:
            self.send_error(502, "bad upstream response")
            return
        head = resp[:header_end].decode("latin-1", "replace")
        body_bytes = resp[header_end + 4:]
        lines = head.split("\r\n")
        status_line = lines[0]
        try:
            _, code, reason = status_line.split(" ", 2)
        except ValueError:
            _, code = status_line.split(" ", 1)
            reason = ""
        code = int(code)

        # If the upstream used chunked transfer-encoding, de-chunk the body
        # before any content-encoding decompression (the chunk framing bytes
        # would otherwise corrupt the gzip stream).
        chunked = False
        for line in lines[1:]:
            if ":" in line and line.split(":", 1)[0].strip().lower() == "transfer-encoding":
                if "chunked" in line.lower():
                    chunked = True
        if chunked:
            body_bytes = _dechunk(body_bytes)

        # Detect Content-Encoding so we can decompress and present a plain
        # body to the browser (avoids ERR_CONTENT_DECODING_FAILED when the
        # upstream's compression is inconsistent across the chunked stream).
        encoding = ""
        for line in lines[1:]:
            if ":" in line and line.split(":", 1)[0].strip().lower() == "content-encoding":
                encoding = line.split(":", 1)[1].strip().lower()
        if encoding in ("gzip", "x-gzip"):
            import gzip
            try:
                body_bytes = gzip.decompress(body_bytes)
            except OSError as exc:
                sys.stderr.write(f"proxy: gzip decompress failed ({exc}); len={len(body_bytes)}\n")
        elif encoding == "deflate":
            import zlib
            try:
                body_bytes = zlib.decompress(body_bytes)
            except zlib.error:
                try:
                    body_bytes = zlib.decompress(body_bytes, -zlib.MAX_WBITS)
                except zlib.error as exc:
                    sys.stderr.write(f"proxy: deflate decompress failed ({exc})\n")
        elif encoding == "br":
            try:
                import brotli
                body_bytes = brotli.decompress(body_bytes)
            except Exception as exc:
                sys.stderr.write(f"proxy: br decompress failed ({exc})\n")

        self.send_response(code, reason)
        # Re-emit headers, stripping Secure from Set-Cookie and dropping
        # Transfer-Encoding (we send a fixed Content-Length).
        for line in lines[1:]:
            if not line:
                continue
            if ":" not in line:
                continue
            key, _, val = line.partition(":")
            key = key.strip()
            val = val.strip()
            lk = key.lower()
            if lk in ("transfer-encoding", "connection", "content-length",
                      "content-encoding", "vary"):
                continue
            if lk == "set-cookie":
                val = _strip_secure(val)
            self.send_header(key, val)
        self.send_header("Content-Length", str(len(body_bytes)))
        self.end_headers()
        if method != "HEAD":
            self.wfile.write(body_bytes)

    def _tunnel_websocket(self, method: str, body: bytes = b"") -> bool:
        """Handle a WebSocket upgrade by raw-tunneling to the upstream
        TLS socket. The controller's Tools → Network Check uses STOMP over
        WebSocket; this bidirectional byte pump lets the browser's WS
        client talk directly to the controller's WS endpoint.

        Returns True if the upgrade was handled (caller must not run the
        normal HTTP path). Returns False if the request wasn't a WS upgrade.
        """
        upgrade = self.headers.get("Upgrade", "").lower()
        if upgrade != "websocket":
            return False

        parsed = urlsplit(self.path)
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query

        # Open a TLS connection to the controller.
        ctx = ssl._create_unverified_context()
        raw = socket.create_connection((UPSTREAM_HOST, UPSTREAM_PORT), timeout=30)
        upstream = ctx.wrap_socket(raw, server_hostname=UPSTREAM_HOST)

        # Forward the original request line + headers verbatim, only fixing
        # Host and keeping the Upgrade/Connection headers intact so the
        # upstream sees a real WS handshake. The WebSocket client may also
        # send a body (e.g. POST-style open frame) — forward it too.
        req_lines = [f"{method} {path} HTTP/1.1"]
        has_host = False
        for key, val in self.headers.items():
            lk = key.lower()
            if lk == "host":
                has_host = True
                req_lines.append(f"Host: {UPSTREAM_HOST}:{UPSTREAM_PORT}")
            elif lk in ("proxy-connection", "keep-alive", "te", "trailers"):
                continue
            else:
                req_lines.append(f"{key}: {val}")
        if not has_host:
            req_lines.append(f"Host: {UPSTREAM_HOST}:{UPSTREAM_PORT}")
        req = ("\r\n".join(req_lines) + "\r\n\r\n").encode("latin-1")
        if body:
            req += body
        upstream.sendall(req)
        sys.stderr.write(f"proxy WS >> {method} {path} origin={self.headers.get('Origin','')!r} cookie={self.headers.get('Cookie','')[:40]!r}\n")

        # Read the upstream's HTTP handshake response and forward its status
        # line + headers to the browser verbatim, then switch to a raw byte
        # pump for the remainder of the connection.
        upstream.settimeout(30)
        handshake = bytearray()
        while b"\r\n\r\n" not in handshake:
            try:
                chunk = upstream.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            handshake += chunk
        if b"\r\n\r\n" not in handshake:
            upstream.close()
            self.send_error(502, "websocket upstream handshake failed")
            return True

        head_end = handshake.find(b"\r\n\r\n")
        head = handshake[:head_end].decode("latin-1", "replace")
        rest = bytes(handshake[head_end + 4:])
        lines = head.split("\r\n")
        status_line = lines[0]
        try:
            _, code, reason = status_line.split(" ", 2)
        except ValueError:
            _, code = status_line.split(" ", 1)
            reason = ""
        code = int(code)
        sys.stderr.write(f"proxy WS << upstream status={code} path={path}\n")

        # Re-emit the upstream's handshake status + headers to the browser.
        self.send_response(code, reason)
        for line in lines[1:]:
            if not line or ":" not in line:
                continue
            key, _, val = line.partition(":")
            key = key.strip()
            val = val.strip()
            lk = key.lower()
            if lk == "set-cookie":
                val = _strip_secure(val)
            if lk in ("transfer-encoding", "content-length"):
                continue  # WS frames have no Content-Length
            self.send_header(key, val)
        self.send_header("Connection", "Upgrade")
        self.end_headers()

        # Forward any leftover bytes (already read past the handshake) first.
        if rest:
            try:
                self.wfile.write(rest)
            except OSError:
                upstream.close()
                return True

        # Bidirectional byte pump: browser <-> upstream TLS socket. WebSocket
        # frames are opaque to the proxy; just relay bytes until either side
        # closes.
        self.wfile.flush()
        client = self.connection  # the raw socket under wfile
        client.settimeout(None)
        upstream.settimeout(None)

        def pump(src, dst, on_close):
            try:
                while True:
                    chunk = src.recv(65536)
                    if not chunk:
                        break
                    dst.sendall(chunk)
            except OSError:
                pass
            finally:
                on_close()

        done = threading.Event()

        def on_close():
            done.set()
            try:
                upstream.close()
            except OSError:
                pass
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

        t_up = threading.Thread(target=pump, args=(upstream, client, on_close), daemon=True)
        t_up.start()
        # Pump browser -> upstream on this thread.
        pump(client, upstream, on_close)
        done.wait(5.0)
        return True

    def do_GET(self):
        if urlsplit(self.path).path == "/__ora/reset-browser-state":
            body = b"""<!doctype html><meta charset=\"utf-8\"><script>
(async () => {
    localStorage.clear();
    sessionStorage.clear();
    if ('caches' in globalThis) {
        await Promise.all((await caches.keys()).map((name) => caches.delete(name)));
    }
    await new Promise((resolve) => {
        const request = indexedDB.deleteDatabase('asset-cache');
        request.onsuccess = request.onerror = request.onblocked = resolve;
    });
    location.replace('/');
})();
</script>"""
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        # WebSocket upgrade requests are raw-tunneled, not proxied as
        # normal HTTP (the controller's Tools → Network Check probes flow over
        # STOMP/WebSocket; tunneling them makes the live-probe UI work).
        if self.headers.get("Upgrade", "").lower() == "websocket":
            self._tunnel_websocket("GET")
            return
        self._upstream("GET")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        self._upstream("POST", body)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        self._upstream("PUT", body)

    def do_PATCH(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        self._upstream("PATCH", body)

    def do_DELETE(self):
        self._upstream("DELETE")

    def do_OPTIONS(self):
        self._upstream("OPTIONS")

    def log_message(self, fmt, *args):  # quiet
        pass


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("PROXY_PORT", "8090"))
    httpd = http.server.ThreadingHTTPServer((BIND_ADDR, port), Proxy)
    print(f"controller proxy: http://{BIND_ADDR}:{port} -> https://{UPSTREAM_HOST}:{UPSTREAM_PORT}",
          flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())