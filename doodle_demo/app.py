#!/usr/bin/env python3
"""Serve a local drawing canvas backed by a pretrained QuickDraw model."""

from __future__ import annotations

import argparse
import json
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Protocol
from urllib.parse import parse_qs, urlparse

try:
    from .predictor import (
        DEFAULT_MODEL_ID,
        HuggingFaceQuickDrawPredictor,
        InvalidImage,
        LiteRTQuickDrawPredictor,
        MOBILEVIT_MODEL_ID,
        ModelUnavailable,
        Prediction,
        PredictionError,
    )
except ImportError:  # Direct execution: python app.py
    from predictor import (  # type: ignore[no-redef]
        DEFAULT_MODEL_ID,
        HuggingFaceQuickDrawPredictor,
        InvalidImage,
        LiteRTQuickDrawPredictor,
        MOBILEVIT_MODEL_ID,
        ModelUnavailable,
        Prediction,
        PredictionError,
    )


MAX_IMAGE_BYTES = 2 * 1024 * 1024
SUPPORTED_IMAGE_TYPES = {"image/png", "image/jpeg"}


class Predictor(Protocol):
    model_id: str

    @property
    def status(self) -> str: ...

    def predict(self, image_bytes: bytes, *, top_k: int = 5) -> tuple[Prediction, ...]: ...


class DoodleServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        index_path: Path,
        predictor: Predictor,
    ) -> None:
        super().__init__(address, DoodleHandler)
        self.index_html = index_path.read_bytes()
        self.predictor = predictor


class DoodleHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def doodle_server(self) -> DoodleServer:
        return self.server  # type: ignore[return-value]

    def do_GET(self) -> None:
        route = urlparse(self.path).path
        if route == "/":
            self._send_bytes(HTTPStatus.OK, "text/html; charset=utf-8", self.doodle_server.index_html)
        elif route == "/health":
            self._send_json(
                HTTPStatus.OK,
                {
                    "status": "ok",
                    "model": self.doodle_server.predictor.model_id,
                    "model_status": self.doodle_server.predictor.status,
                },
            )
        else:
            self._send_error(HTTPStatus.NOT_FOUND, "not_found", "Route not found")

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/api/predict":
            self._send_error(HTTPStatus.NOT_FOUND, "not_found", "Route not found")
            return

        content_type = self.headers.get_content_type()
        if content_type not in SUPPORTED_IMAGE_TYPES:
            self._send_error(
                HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                "unsupported_media_type",
                "Send a PNG or JPEG image",
            )
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_error(HTTPStatus.BAD_REQUEST, "invalid_length", "Invalid Content-Length")
            return
        if content_length <= 0:
            self._send_error(HTTPStatus.BAD_REQUEST, "empty_image", "The image body is empty")
            return
        if content_length > MAX_IMAGE_BYTES:
            self._send_error(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "image_too_large",
                "Images must be smaller than 2 MiB",
            )
            return

        try:
            top_k = self._parse_top_k(parsed.query)
        except ValueError as error:
            self._send_error(HTTPStatus.BAD_REQUEST, "invalid_top_k", str(error))
            return

        image_bytes = self.rfile.read(content_length)
        try:
            predictions = self.doodle_server.predictor.predict(image_bytes, top_k=top_k)
        except InvalidImage as error:
            self._send_error(HTTPStatus.BAD_REQUEST, "invalid_image", str(error))
            return
        except ModelUnavailable as error:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "model_unavailable", str(error))
            return
        except PredictionError as error:
            self._send_error(HTTPStatus.INTERNAL_SERVER_ERROR, "prediction_failed", str(error))
            return
        except Exception as error:
            print(f"Unexpected prediction error: {error}")
            self._send_error(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                "prediction_failed",
                "Prediction failed unexpectedly",
            )
            return

        self._send_json(
            HTTPStatus.OK,
            {
                "model": self.doodle_server.predictor.model_id,
                "predictions": [prediction.as_dict() for prediction in predictions],
            },
        )

    @staticmethod
    def _parse_top_k(query: str) -> int:
        raw = parse_qs(query).get("top_k", ["5"])[0]
        try:
            top_k = int(raw)
        except ValueError as error:
            raise ValueError("top_k must be an integer between 1 and 10") from error
        if not 1 <= top_k <= 10:
            raise ValueError("top_k must be between 1 and 10")
        return top_k

    def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self._send_bytes(status, "application/json; charset=utf-8", encoded)

    def _send_error(self, status: HTTPStatus, code: str, message: str) -> None:
        # Some errors are detected before the request body is consumed. Closing
        # prevents those unread bytes from being parsed as another HTTP request.
        self.close_connection = True
        self._send_json(status, {"error": {"code": code, "message": message}})

    def _send_bytes(self, status: HTTPStatus, content_type: str, payload: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format_string: str, *args: Any) -> None:
        print(f"{self.address_string()} - {format_string % args}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="Bind address")
    parser.add_argument("--port", type=int, default=8766, help="HTTP port")
    parser.add_argument(
        "--backend", choices=("litert", "mobilevit"), default="litert", help="Model runtime"
    )
    parser.add_argument("--model", help="Override the selected backend's Hugging Face model ID")
    parser.add_argument("--no-open", action="store_true", help="Do not open a browser")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    index_path = Path(__file__).with_name("index.html")
    if args.backend == "mobilevit":
        predictor: Predictor = HuggingFaceQuickDrawPredictor(args.model or MOBILEVIT_MODEL_ID)
    else:
        predictor = LiteRTQuickDrawPredictor(args.model or DEFAULT_MODEL_ID)
    server = DoodleServer((args.host, args.port), index_path, predictor)
    browser_host = "127.0.0.1" if args.host == "0.0.0.0" else args.host
    url = f"http://{browser_host}:{args.port}"
    print(f"Doodle demo: {url}")
    print(f"Model: {predictor.model_id} ({args.backend}; downloads on first prediction)")
    if args.host == "0.0.0.0":
        print("ESP32 endpoint: http://<this-mac-ip>:%d/api/predict" % args.port)
    print("Press Ctrl+C to stop.")
    if not args.no_open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        threading.Thread(target=server.shutdown, daemon=True).start()
        server.server_close()


if __name__ == "__main__":
    main()
