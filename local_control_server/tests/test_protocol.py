import json
import threading
import unittest
import urllib.request

from local_control_server.server import (
    LocalHTTPServer,
    LocalRequestHandler,
    LocalXiaozhiServer,
    PlaceholderLLM,
    decode_ws_text_frame,
    encode_ws_text_frame,
)


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

    def test_ota_endpoint_accepts_post_from_firmware(self):
        app = LocalXiaozhiServer(host="127.0.0.1", port=0, public_host="127.0.0.1")
        httpd = LocalHTTPServer(("127.0.0.1", 0), LocalRequestHandler, app)
        _, port = httpd.server_address
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            body = json.dumps({"board": {"type": "test"}}).encode("utf-8")
            request = urllib.request.Request(
                f"http://127.0.0.1:{port}/xiaozhi/ota/",
                data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )

            with urllib.request.urlopen(request, timeout=3) as response:
                data = json.loads(response.read().decode("utf-8"))

            self.assertEqual(response.status, 200)
            self.assertIn("websocket", data)
        finally:
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    unittest.main()
