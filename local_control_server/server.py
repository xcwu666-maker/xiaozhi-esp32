"""Local PC-side control server for the Xiaozhi ESP32 firmware.

This service is intentionally dependency-free so it can run on a developer PC
without installing FastAPI, uvicorn, or a websocket package.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


HTML_PAGE = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>小蠖电脑端输入</title>
  <style>
    body { font-family: system-ui, "Microsoft YaHei", sans-serif; margin: 32px; background: #f6f7f9; color: #172033; }
    main { max-width: 860px; margin: 0 auto; }
    h1 { font-size: 24px; margin-bottom: 8px; }
    .hint { color: #667085; margin-bottom: 20px; }
    textarea { width: 100%; min-height: 96px; box-sizing: border-box; font: inherit; padding: 12px; border: 1px solid #cfd6e4; border-radius: 8px; }
    button { margin-top: 12px; padding: 10px 16px; border: 0; border-radius: 8px; background: #2563eb; color: white; font: inherit; cursor: pointer; }
    button:disabled { background: #98a2b3; cursor: wait; }
    pre { white-space: pre-wrap; background: #101828; color: #d1fadf; padding: 16px; border-radius: 8px; min-height: 160px; }
  </style>
</head>
<body>
  <main>
    <h1>小蠖电脑端输入</h1>
    <p class="hint">先让 ESP32 唤醒并连接本服务，然后在这里输入文字。服务会把文字和回复推送到设备屏幕与串口。</p>
    <textarea id="text" placeholder="例如：你好，我是从电脑打字输入的。"></textarea>
    <br>
    <button id="send">发送到小蠖</button>
    <h2>返回</h2>
    <pre id="log">等待输入...</pre>
  </main>
  <script>
    const btn = document.getElementById("send");
    const text = document.getElementById("text");
    const log = document.getElementById("log");
    btn.onclick = async () => {
      const value = text.value.trim();
      if (!value) return;
      btn.disabled = true;
      try {
        const resp = await fetch("/api/send", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({text: value})
        });
        const data = await resp.json();
        log.textContent = JSON.stringify(data, null, 2);
      } catch (err) {
        log.textContent = String(err);
      } finally {
        btn.disabled = false;
      }
    };
  </script>
</body>
</html>
"""


def guess_lan_ip() -> str:
    """Return a likely LAN IP address for URLs shown to ESP32."""
    ipconfig_ip = guess_lan_ip_from_ipconfig()
    if ipconfig_ip:
        return ipconfig_ip
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return socket.gethostbyname(socket.gethostname())
    finally:
        sock.close()


def guess_lan_ip_from_ipconfig() -> str | None:
    """Prefer a Windows LAN IPv4 address over virtual adapter addresses."""
    try:
        output = subprocess.check_output(["ipconfig"], text=True, encoding="gbk", errors="ignore")
    except (OSError, subprocess.CalledProcessError):
        return None
    candidates = re.findall(r"IPv4 [^:\r\n]*:\s*([0-9]+(?:\.[0-9]+){3})", output)
    usable = [
        ip for ip in candidates
        if not ip.startswith("127.")
        and not ip.startswith("169.254.")
        and not ip.endswith(".1")
    ]
    for prefix in ("192.168.", "10."):
        for ip in usable:
            if ip.startswith(prefix):
                return ip
    for ip in usable:
        if ip.startswith("172."):
            return ip
    return usable[0] if usable else None


def encode_ws_text_frame(text: str) -> bytes:
    payload = text.encode("utf-8")
    header = bytearray([0x81])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length <= 0xFFFF:
        header.append(126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(127)
        header.extend(struct.pack("!Q", length))
    return bytes(header) + payload


def decode_ws_text_frame(frame: bytes) -> str:
    if len(frame) < 2:
        raise ValueError("frame too short")
    opcode = frame[0] & 0x0F
    if opcode != 0x01:
        raise ValueError(f"not a text frame: opcode={opcode}")
    masked = bool(frame[1] & 0x80)
    length = frame[1] & 0x7F
    pos = 2
    if length == 126:
        length = struct.unpack("!H", frame[pos:pos + 2])[0]
        pos += 2
    elif length == 127:
        length = struct.unpack("!Q", frame[pos:pos + 8])[0]
        pos += 8
    mask = b""
    if masked:
        mask = frame[pos:pos + 4]
        pos += 4
    payload = bytearray(frame[pos:pos + length])
    if masked:
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
    return payload.decode("utf-8")


def read_exact(sock_file, size: int) -> bytes:
    data = sock_file.read(size)
    if data is None or len(data) != size:
        raise ConnectionError("websocket connection closed")
    return data


def read_ws_frame(sock_file) -> tuple[int, bytes]:
    first = read_exact(sock_file, 2)
    opcode = first[0] & 0x0F
    masked = bool(first[1] & 0x80)
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", read_exact(sock_file, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", read_exact(sock_file, 8))[0]
    mask = read_exact(sock_file, 4) if masked else b""
    payload = bytearray(read_exact(sock_file, length))
    if masked:
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
    return opcode, bytes(payload)


class PlaceholderLLM:
    """LLM placeholder used until the user provides API credentials."""

    def generate(self, user_text: str, history: list[dict[str, Any]]) -> str:
        return (
            "我已经收到你的电脑端输入："
            f"{user_text}。当前还是本地占位回复，后续把 API Key 填入后，"
            "这里会改为调用真正的 LLM API。"
        )


@dataclass
class OpenAICompatibleLLM:
    """Minimal OpenAI-compatible chat completions client.

    Configure with environment variables:
    - LOCAL_XIAOZHI_LLM_API_KEY
    - LOCAL_XIAOZHI_LLM_BASE_URL, default https://api.openai.com/v1/chat/completions
    - LOCAL_XIAOZHI_LLM_MODEL, default gpt-4o-mini
    """

    api_key: str
    base_url: str
    model: str

    @classmethod
    def from_env(cls) -> "OpenAICompatibleLLM | None":
        api_key = os.environ.get("LOCAL_XIAOZHI_LLM_API_KEY", "").strip()
        if not api_key:
            return None
        return cls(
            api_key=api_key,
            base_url=os.environ.get("LOCAL_XIAOZHI_LLM_BASE_URL", "https://api.openai.com/v1/chat/completions"),
            model=os.environ.get("LOCAL_XIAOZHI_LLM_MODEL", "gpt-4o-mini"),
        )

    def generate(self, user_text: str, history: list[dict[str, Any]]) -> str:
        messages = [
            {"role": "system", "content": "你是小蠖机器人电脑端服务，回答要简洁、自然、中文。"},
        ]
        messages.extend(history[-8:])
        messages.append({"role": "user", "content": user_text})
        payload = json.dumps({"model": self.model, "messages": messages}, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            self.base_url,
            data=payload,
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                data = json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
            return f"LLM API 调用失败：{exc}"
        return data["choices"][0]["message"]["content"]


class LocalXiaozhiServer:
    def __init__(self, host: str, port: int, public_host: str | None = None):
        self.host = host
        self.port = port
        self.public_host = public_host or guess_lan_ip()
        self.llm = OpenAICompatibleLLM.from_env() or PlaceholderLLM()
        self.history: list[dict[str, Any]] = []
        self._ws_handlers: list["LocalRequestHandler"] = []
        self._lock = threading.RLock()

    def build_ota_response(self) -> dict[str, Any]:
        now_ms = int(time.time() * 1000)
        return {
            "server_time": {
                "timestamp": now_ms,
                "timezone_offset": 480,
            },
            "websocket": {
                "url": f"ws://{self.public_host}:{self.port}/xiaozhi/ws",
                "token": "local-dev-token",
                "version": 3,
            },
            "firmware": {
                "version": "2.2.6",
                "url": "",
            },
        }

    def build_server_hello(self) -> dict[str, Any]:
        return {
            "type": "hello",
            "transport": "websocket",
            "session_id": "local-session",
            "audio_params": {
                "sample_rate": 24000,
                "frame_duration": 60,
            },
        }

    def register_ws(self, handler: "LocalRequestHandler") -> None:
        with self._lock:
            if handler not in self._ws_handlers:
                self._ws_handlers.append(handler)

    def unregister_ws(self, handler: "LocalRequestHandler") -> None:
        with self._lock:
            self._ws_handlers = [item for item in self._ws_handlers if item is not handler]

    def connected_devices(self) -> int:
        with self._lock:
            return len(self._ws_handlers)

    def broadcast_json(self, payload: dict[str, Any]) -> int:
        text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        sent = 0
        with self._lock:
            handlers = list(self._ws_handlers)
        for handler in handlers:
            if handler.send_ws_text(text):
                sent += 1
        return sent

    def handle_typed_input(self, text: str) -> str:
        cleaned = text.strip()
        if not cleaned:
            raise ValueError("empty text")
        answer = self.llm.generate(cleaned, self.history)
        self.history.append({"role": "user", "content": cleaned})
        self.history.append({"role": "assistant", "content": answer})
        self.broadcast_json({"type": "stt", "text": cleaned})
        self.broadcast_json({"type": "tts", "state": "start"})
        self.broadcast_json({"type": "tts", "state": "sentence_start", "text": answer})
        self.broadcast_json({"type": "tts", "state": "stop"})
        return answer


class LocalRequestHandler(BaseHTTPRequestHandler):
    server: "LocalHTTPServer"

    def log_message(self, fmt: str, *args) -> None:
        print(f"[HTTP] {self.address_string()} {fmt % args}")

    @property
    def app(self) -> LocalXiaozhiServer:
        return self.server.app

    def do_GET(self) -> None:
        if self.path == "/" or self.path.startswith("/?"):
            self.send_bytes(HTTPStatus.OK, HTML_PAGE.encode("utf-8"), "text/html; charset=utf-8")
            return
        if self.path.startswith("/xiaozhi/ota"):
            self.send_json(self.app.build_ota_response())
            return
        if self.path == "/api/status":
            self.send_json({
                "connected_devices": self.app.connected_devices(),
                "ota_url": f"http://{self.app.public_host}:{self.app.port}/xiaozhi/ota/",
                "websocket_url": f"ws://{self.app.public_host}:{self.app.port}/xiaozhi/ws",
                "history_items": len(self.app.history),
            })
            return
        if self.path == "/xiaozhi/ws":
            self.handle_websocket()
            return
        self.send_error(HTTPStatus.NOT_FOUND, "not found")

    def do_POST(self) -> None:
        if self.path.startswith("/xiaozhi/ota"):
            length = int(self.headers.get("Content-Length", "0"))
            if length:
                self.rfile.read(length)
            self.send_json(self.app.build_ota_response())
            return
        if self.path != "/api/send":
            self.send_error(HTTPStatus.NOT_FOUND, "not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            data = json.loads(raw.decode("utf-8"))
            text = str(data.get("text", ""))
            answer = self.app.handle_typed_input(text)
        except (json.JSONDecodeError, ValueError) as exc:
            self.send_json({"ok": False, "error": str(exc)}, status=HTTPStatus.BAD_REQUEST)
            return
        self.send_json({
            "ok": True,
            "input": text,
            "answer": answer,
            "connected_devices": self.app.connected_devices(),
        })

    def send_bytes(self, status: HTTPStatus, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, data: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(data, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_bytes(status, body, "application/json; charset=utf-8")

    def handle_websocket(self) -> None:
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing Sec-WebSocket-Key")
            return
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.app.register_ws(self)
        print("[WS] ESP32 connected")
        try:
            while True:
                opcode, payload = read_ws_frame(self.rfile)
                if opcode == 0x08:
                    break
                if opcode == 0x01:
                    self.handle_ws_text(payload.decode("utf-8"))
                elif opcode == 0x02:
                    print(f"[WS] ignored binary audio frame: {len(payload)} bytes")
        except (ConnectionError, OSError) as exc:
            print(f"[WS] disconnected: {exc}")
        finally:
            self.app.unregister_ws(self)

    def handle_ws_text(self, text: str) -> None:
        print(f"[WS] recv: {text}")
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return
        if data.get("type") == "hello":
            self.send_ws_text(json.dumps(self.app.build_server_hello(), ensure_ascii=False, separators=(",", ":")))
        elif data.get("type") == "listen":
            state = data.get("state")
            if state == "detect":
                wake_text = data.get("text", "")
                self.app.broadcast_json({"type": "stt", "text": wake_text})

    def send_ws_text(self, text: str) -> bool:
        try:
            self.connection.sendall(encode_ws_text_frame(text))
            return True
        except OSError as exc:
            print(f"[WS] send failed: {exc}")
            self.app.unregister_ws(self)
            return False


class LocalHTTPServer(ThreadingHTTPServer):
    def __init__(self, server_address, request_handler, app: LocalXiaozhiServer):
        super().__init__(server_address, request_handler)
        self.app = app


def run_server(host: str, port: int, public_host: str | None = None) -> None:
    app = LocalXiaozhiServer(host=host, port=port, public_host=public_host)
    httpd = LocalHTTPServer((host, port), LocalRequestHandler, app)
    print("Local Xiaozhi control server started")
    print(f"Browser UI: http://{app.public_host}:{port}/")
    print(f"ESP32 OTA URL: http://{app.public_host}:{port}/xiaozhi/ota/")
    print(f"ESP32 WebSocket: ws://{app.public_host}:{port}/xiaozhi/ws")
    print("Press Ctrl+C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
    finally:
        httpd.server_close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Local Xiaozhi PC-side control server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--public-host", default=None, help="LAN IP visible to ESP32")
    parser.add_argument("--once-status", action="store_true", help="Print URLs and exit")
    args = parser.parse_args()

    app = LocalXiaozhiServer(host=args.host, port=args.port, public_host=args.public_host)
    print(f"Browser UI: http://{app.public_host}:{args.port}/")
    print(f"ESP32 OTA URL: http://{app.public_host}:{args.port}/xiaozhi/ota/")
    print(f"ESP32 WebSocket: ws://{app.public_host}:{args.port}/xiaozhi/ws")
    if args.once_status:
        return
    run_server(args.host, args.port, args.public_host)


if __name__ == "__main__":
    main()
