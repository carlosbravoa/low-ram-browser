"""Minimal Chrome DevTools Protocol client.

Stdlib only (including the WebSocket), so the harness also runs on the
target devices, which may not have pip or Node.
"""

import base64
import json
import os
import socket
import struct
import time
import urllib.parse


class CDPError(RuntimeError):
    pass


class WebSocket:
    def __init__(self, url, timeout=60):
        u = urllib.parse.urlparse(url)
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = u.path + ("?" + u.query if u.query else "")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {u.hostname}:{u.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("WebSocket handshake: connection closed")
            response += chunk
        head, self.buf = response.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status:
            raise ConnectionError(f"WebSocket handshake failed: {status!r}")

    def _send_frame(self, opcode, payload):
        n = len(payload)
        header = bytearray([0x80 | opcode])
        if n < 126:
            header.append(0x80 | n)
        elif n < 1 << 16:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def send(self, text):
        self._send_frame(0x1, text.encode())

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ConnectionError("WebSocket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        message = b""
        while True:
            b0, b1 = self._read(2)
            opcode = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                (n,) = struct.unpack(">H", self._read(2))
            elif n == 127:
                (n,) = struct.unpack(">Q", self._read(8))
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
            if opcode == 0x8:
                raise ConnectionError("WebSocket closed by peer")
            if opcode == 0x9:
                self._send_frame(0xA, data)
                continue
            if opcode == 0xA:
                continue
            message += data
            if b0 & 0x80:
                return message.decode()

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        self.sock.close()


class CDP:
    """Browser-level connection; page commands go through a flattened session."""

    def __init__(self, ws_url, timeout=60):
        self.ws = WebSocket(ws_url, timeout)
        self.next_id = 0
        # Events are dropped unless a caller asks to keep them (tracing).
        self.keep_events = False
        self.events = []

    def call(self, method, params=None, session_id=None):
        self.next_id += 1
        msg = {"id": self.next_id, "method": method, "params": params or {}}
        if session_id:
            msg["sessionId"] = session_id
        self.ws.send(json.dumps(msg))
        while True:
            reply = json.loads(self.ws.recv())
            if reply.get("id") != self.next_id:
                if self.keep_events and "method" in reply:
                    self.events.append(reply)
                continue
            if "error" in reply:
                raise CDPError(f"{method}: {reply['error']}")
            return reply.get("result", {})

    def attach_first_page(self, timeout=10):
        # lrb opens its first window after reading saved state, so DevTools
        # can answer a moment before there is a page.
        deadline = time.monotonic() + timeout
        while True:
            targets = self.call("Target.getTargets")["targetInfos"]
            pages = [t for t in targets if t["type"] == "page"]
            if pages:
                break
            if time.monotonic() > deadline:
                raise CDPError("no page target")
            time.sleep(0.2)
        result = self.call(
            "Target.attachToTarget", {"targetId": pages[0]["targetId"], "flatten": True}
        )
        return result["sessionId"]

    def wait_event(self, method):
        """Reads until event `method` arrives (keeping events); returns it."""
        for event in self.events:
            if event.get("method") == method:
                return event
        while True:
            message = json.loads(self.ws.recv())
            if "method" in message:
                self.events.append(message)
                if message["method"] == method:
                    return message

    def evaluate(self, session_id, expression, await_promise=False):
        result = self.call(
            "Runtime.evaluate",
            {"expression": expression, "awaitPromise": await_promise, "returnByValue": True},
            session_id,
        )
        if "exceptionDetails" in result:
            raise CDPError(f"evaluate: {result['exceptionDetails'].get('text')}")
        return result["result"].get("value")

    def close(self):
        self.ws.close()
