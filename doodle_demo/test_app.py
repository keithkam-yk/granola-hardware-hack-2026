from __future__ import annotations

import http.client
import json
import threading
import unittest
from pathlib import Path

try:
    from .app import DoodleServer
    from .predictor import Prediction
except ImportError:
    from app import DoodleServer
    from predictor import Prediction


class FakePredictor:
    model_id = "fake/quickdraw"
    status = "ready"

    def __init__(self) -> None:
        self.received: bytes | None = None
        self.top_k: int | None = None

    def predict(self, image_bytes: bytes, *, top_k: int = 5) -> tuple[Prediction, ...]:
        self.received = image_bytes
        self.top_k = top_k
        return (
            Prediction("cat", 0.82, "🐱"),
            Prediction("tiger", 0.11, "🐯"),
        )[:top_k]


class DoodleServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.predictor = FakePredictor()
        index_path = Path(__file__).with_name("index.html")
        self.server = DoodleServer(("127.0.0.1", 0), index_path, self.predictor)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_address[1], timeout=2
        )

    def tearDown(self) -> None:
        self.connection.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_serves_the_drawing_page(self) -> None:
        self.connection.request("GET", "/")
        response = self.connection.getresponse()
        body = response.read()
        self.assertEqual(response.status, 200)
        self.assertIn(b"Doodle", body)
        self.assertIn(b"canvas", body)

    def test_predicts_a_raw_png_body(self) -> None:
        image_bytes = b"not-decoded-by-the-fake"
        self.connection.request(
            "POST",
            "/api/predict?top_k=2",
            body=image_bytes,
            headers={"Content-Type": "image/png", "Content-Length": str(len(image_bytes))},
        )
        response = self.connection.getresponse()
        payload = json.loads(response.read())
        self.assertEqual(response.status, 200)
        self.assertEqual(payload["model"], "fake/quickdraw")
        self.assertEqual(payload["predictions"][0]["label"], "cat")
        self.assertEqual(payload["predictions"][0]["emoji"], "🐱")
        self.assertEqual(self.predictor.received, image_bytes)
        self.assertEqual(self.predictor.top_k, 2)

    def test_rejects_unsupported_content_type(self) -> None:
        self.connection.request(
            "POST",
            "/api/predict",
            body=b"hello",
            headers={"Content-Type": "text/plain", "Content-Length": "5"},
        )
        response = self.connection.getresponse()
        payload = json.loads(response.read())
        self.assertEqual(response.status, 415)
        self.assertEqual(payload["error"]["code"], "unsupported_media_type")

    def test_health_does_not_load_the_model(self) -> None:
        self.connection.request("GET", "/health")
        response = self.connection.getresponse()
        payload = json.loads(response.read())
        self.assertEqual(response.status, 200)
        self.assertEqual(payload["model_status"], "ready")


if __name__ == "__main__":
    unittest.main()
