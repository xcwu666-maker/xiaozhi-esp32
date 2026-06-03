# Local PC Control Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a local PC-side service that acts as the first self-hosted Xiaozhi cloud replacement: ESP32 connects to it by WebSocket, and the user can type text from a browser to make ESP32 display a dialogue response.

**Architecture:** The PC service uses Python standard library only. It serves an OTA endpoint, a WebSocket endpoint for ESP32, a simple browser UI, and an LLM adapter interface with a placeholder implementation that can later be wired to an API key.

**Tech Stack:** Python 3.11+, `http.server`, `socketserver`, `threading`, `json`, `unittest`.

---

### Task 1: Service Protocol Tests

**Files:**
- Create: `local_control_server/tests/test_protocol.py`
- Create: `local_control_server/__init__.py`
- Create: `local_control_server/server.py`

- [ ] **Step 1: Write tests for OTA response, LLM stub, and WebSocket frame helpers**

```python
import json
import unittest

from local_control_server.server import LocalXiaozhiServer, PlaceholderLLM, encode_ws_text_frame, decode_ws_text_frame


class ProtocolTests(unittest.TestCase):
    def test_ota_response_contains_websocket_config(self):
        server = LocalXiaozhiServer(host="0.0.0.0", port=8000, public_host="192.168.1.10")
        data = server.build_ota_response()

        self.assertIn("websocket", data)
        self.assertEqual(data["websocket"]["url"], "ws://192.168.1.10:8000/xiaozhi/ws")
        self.assertEqual(data["websocket"]["version"], 3)
        self.assertIn("server_time", data)
        self.assertIn("firmware", data)

    def test_placeholder_llm_echoes_input(self):
        llm = PlaceholderLLM()
        answer = llm.generate("我今天心情不好", [])

        self.assertIn("我今天心情不好", answer)
        self.assertIn("API", answer)

    def test_websocket_text_frame_roundtrip(self):
        payload = json.dumps({"type": "hello", "transport": "websocket"}, ensure_ascii=False)
        frame = encode_ws_text_frame(payload)

        decoded = decode_ws_text_frame(frame)

        self.assertEqual(decoded, payload)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails before implementation**

Run: `python -m unittest local_control_server.tests.test_protocol -v`

Expected: tests fail because `LocalXiaozhiServer`, `PlaceholderLLM`, and frame helpers are not implemented.

### Task 2: Local Server Core

**Files:**
- Modify: `local_control_server/server.py`

- [ ] **Step 1: Implement server state, OTA JSON, placeholder LLM, and WebSocket frame helpers**

The implementation must expose:

```python
class PlaceholderLLM:
    def generate(self, user_text: str, history: list[dict]) -> str: ...

class LocalXiaozhiServer:
    def __init__(self, host: str, port: int, public_host: str | None = None): ...
    def build_ota_response(self) -> dict: ...
    def build_server_hello(self) -> dict: ...
    def handle_typed_input(self, text: str) -> str: ...

def encode_ws_text_frame(text: str) -> bytes: ...
def decode_ws_text_frame(frame: bytes) -> str: ...
```

- [ ] **Step 2: Run protocol tests**

Run: `python -m unittest local_control_server.tests.test_protocol -v`

Expected: all tests pass.

### Task 3: HTTP, Browser UI, and ESP32 WebSocket

**Files:**
- Modify: `local_control_server/server.py`
- Create: `local_control_server/README.md`
- Create: `local_control_server/start_local_server.bat`

- [ ] **Step 1: Add HTTP routes**

Routes:

```text
GET  /                         Browser input page
GET  /xiaozhi/ota/             OTA config JSON
GET  /xiaozhi/ws               ESP32 WebSocket endpoint
POST /api/send                 Send typed user text to ESP32
GET  /api/status               Basic service status
```

- [ ] **Step 2: Add ESP32 message flow**

When ESP32 sends hello, reply:

```json
{"type":"hello","transport":"websocket","session_id":"local-session","audio_params":{"sample_rate":24000,"frame_duration":60}}
```

When browser sends text, send to ESP32:

```json
{"type":"stt","text":"..."}
{"type":"tts","state":"start"}
{"type":"tts","state":"sentence_start","text":"..."}
{"type":"tts","state":"stop"}
```

- [ ] **Step 3: Add startup scripts and docs**

The batch script runs:

```bat
python -m local_control_server.server --host 0.0.0.0 --port 8000
```

The README explains how to set ESP32 `CONFIG_OTA_URL` to the printed URL.

### Task 4: Verification

**Files:**
- Test: `local_control_server/tests/test_protocol.py`

- [ ] **Step 1: Run tests**

Run: `python -m unittest local_control_server.tests.test_protocol -v`

Expected: all tests pass.

- [ ] **Step 2: Start service smoke test**

Run: `python -m local_control_server.server --host 127.0.0.1 --port 8000 --once-status`

Expected: prints OTA URL and exits successfully.

