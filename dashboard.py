#!/usr/bin/env python3
"""Bridge QMI8658 JSON samples from USB serial to a browser dashboard."""

from __future__ import annotations

import argparse
import glob
import json
import os
import queue
import select
import signal
import termios
import threading
import time
import webbrowser
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from doodle_demo.predictor import (
    DEFAULT_MODEL_ID,
    InvalidImage,
    LiteRTQuickDrawPredictor,
    ModelUnavailable,
    PredictionError,
)


SAMPLE_FIELDS = ("seq", "t", "ax", "ay", "az", "gx", "gy", "gz", "temp")
SENSITIVITY_MIN = 0.05
SENSITIVITY_MAX = 0.80
MAX_DOODLE_IMAGE_BYTES = 2 * 1024 * 1024
DOODLE_IMAGE_TYPES = {"image/png", "image/jpeg"}


@dataclass(frozen=True)
class ImuSample:
    seq: int
    t: int
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float
    temp: float

    @classmethod
    def from_line(cls, line: bytes) -> "ImuSample | None":
        try:
            text = line.decode("utf-8").strip()
            if not text.startswith("{"):
                return None
            payload = json.loads(text)
            if not all(field in payload for field in SAMPLE_FIELDS):
                return None
            return cls(
                seq=int(payload["seq"]),
                t=int(payload["t"]),
                ax=float(payload["ax"]),
                ay=float(payload["ay"]),
                az=float(payload["az"]),
                gx=float(payload["gx"]),
                gy=float(payload["gy"]),
                gz=float(payload["gz"]),
                temp=float(payload["temp"]),
            )
        except (UnicodeDecodeError, ValueError, TypeError, json.JSONDecodeError):
            return None

    def as_event(self) -> dict[str, Any]:
        return {"type": "sample", **self.__dict__}


def parse_device_event(line: bytes) -> dict[str, Any] | None:
    try:
        payload = json.loads(line.decode("utf-8").strip())
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict) or payload.get("type") not in {"airmouse", "cursor"}:
        return None
    if payload["type"] == "cursor":
        dx, dy = payload.get("dx"), payload.get("dy")
        if not isinstance(dx, (int, float)) or not isinstance(dy, (int, float)):
            return None
    return payload


class EventBroker:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._subscribers: set[queue.Queue[str]] = set()
        self.status = "Starting"
        self.last_sample: ImuSample | None = None
        self.airmouse: dict[str, Any] | None = None
        self.sample_count = 0

    def subscribe(self) -> queue.Queue[str]:
        subscriber: queue.Queue[str] = queue.Queue(maxsize=8)
        with self._lock:
            self._subscribers.add(subscriber)
            status = self.status
        subscriber.put_nowait(json.dumps({"type": "status", "status": status}))
        return subscriber

    def unsubscribe(self, subscriber: queue.Queue[str]) -> None:
        with self._lock:
            self._subscribers.discard(subscriber)

    def publish_sample(self, sample: ImuSample) -> None:
        self.last_sample = sample
        self.sample_count += 1
        self._publish(sample.as_event())

    def publish_status(self, status: str) -> None:
        if status == self.status:
            return
        self.status = status
        self._publish({"type": "status", "status": status})

    def publish_airmouse(self, state: dict[str, Any]) -> None:
        self.airmouse = state
        self._publish(state)

    def publish_device_event(self, event: dict[str, Any]) -> None:
        if event.get("type") == "airmouse":
            self.airmouse = event
        self._publish(event)

    def snapshot(self) -> dict[str, Any]:
        return {
            "status": self.status,
            "sample_count": self.sample_count,
            "last_sample": self.last_sample.as_event() if self.last_sample else None,
            "airmouse": self.airmouse,
        }

    def _publish(self, event: dict[str, Any]) -> None:
        encoded = json.dumps(event, separators=(",", ":"))
        with self._lock:
            subscribers = tuple(self._subscribers)
        for subscriber in subscribers:
            if subscriber.full():
                try:
                    subscriber.get_nowait()
                except queue.Empty:
                    pass
            try:
                subscriber.put_nowait(encoded)
            except queue.Full:
                pass


class SerialReader(threading.Thread):
    def __init__(self, device: str, broker: EventBroker, stop_event: threading.Event) -> None:
        super().__init__(name="serial-reader", daemon=True)
        self.device = device
        self.broker = broker
        self.stop_event = stop_event
        self._fd_lock = threading.Lock()
        self._fd: int | None = None

    def send_command(self, command: str) -> bool:
        payload = (command.rstrip("\n") + "\n").encode("ascii")
        with self._fd_lock:
            if self._fd is None:
                return False
            try:
                os.write(self._fd, payload)
            except OSError:
                return False
        return True

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                self._read_until_disconnected()
            except OSError as error:
                self.broker.publish_status(f"Waiting for {self.device}: {error.strerror or error}")
                self.stop_event.wait(1.0)

    def _read_until_disconnected(self) -> None:
        fd = os.open(self.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self._configure(fd)
            with self._fd_lock:
                self._fd = fd
            self.broker.publish_status(f"Connected to {self.device}")
            pending = bytearray()
            while not self.stop_event.is_set():
                readable, _, _ = select.select([fd], [], [], 0.5)
                if not readable:
                    continue
                chunk = os.read(fd, 4096)
                if not chunk:
                    raise OSError("serial device disconnected")
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    sample = ImuSample.from_line(line)
                    if sample is not None:
                        self.broker.publish_sample(sample)
                        continue
                    device_event = parse_device_event(line)
                    if device_event is not None:
                        self.broker.publish_device_event(device_event)
        finally:
            with self._fd_lock:
                self._fd = None
            os.close(fd)

    @staticmethod
    def _configure(fd: int) -> None:
        attributes = termios.tcgetattr(fd)
        attributes[0] = termios.IGNPAR
        attributes[1] = 0
        attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attributes[3] = 0
        attributes[4] = termios.B115200
        attributes[5] = termios.B115200
        attributes[6][termios.VMIN] = 0
        attributes[6][termios.VTIME] = 5
        termios.tcsetattr(fd, termios.TCSANOW, attributes)
        termios.tcflush(fd, termios.TCIFLUSH)


class DashboardServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], index_path: Path,
                 broker: EventBroker, reader: SerialReader,
                 doodle_predictor: LiteRTQuickDrawPredictor) -> None:
        super().__init__(address, DashboardHandler)
        self.index_html = index_path.read_bytes()
        self.flight_html = index_path.with_name("flight.html").read_bytes()
        self.cat_html = index_path.with_name("cat.html").read_bytes()
        self.hockey_html = index_path.with_name("hockey.html").read_bytes()
        self.doodle_html = index_path.with_name("doodle.html").read_bytes()
        self.broker = broker
        self.reader = reader
        self.doodle_predictor = doodle_predictor


class DashboardHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def dashboard(self) -> DashboardServer:
        return self.server  # type: ignore[return-value]

    def do_GET(self) -> None:
        route = urlparse(self.path).path
        if route == "/":
            self._send_bytes("text/html; charset=utf-8", self.dashboard.index_html)
        elif route in {"/flight", "/flight/", "/flight.html"}:
            self._send_bytes("text/html; charset=utf-8", self.dashboard.flight_html)
        elif route in {"/cat", "/cat/", "/cat.html"}:
            self._send_bytes("text/html; charset=utf-8", self.dashboard.cat_html)
        elif route in {"/hockey", "/hockey/", "/hockey.html"}:
            self._send_bytes("text/html; charset=utf-8", self.dashboard.hockey_html)
        elif route in {"/doodle", "/doodle/", "/doodle.html"}:
            self._send_bytes("text/html; charset=utf-8", self.dashboard.doodle_html)
        elif route == "/events":
            self._stream_events()
        elif route == "/health":
            payload = json.dumps(self.dashboard.broker.snapshot()).encode()
            self._send_bytes("application/json", payload)
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:
        route = urlparse(self.path).path
        if route == "/api/doodle/predict":
            self._predict_doodle()
            return
        if route != "/settings":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            sensitivity = float(payload["sensitivity"])
            if not SENSITIVITY_MIN <= sensitivity <= SENSITIVITY_MAX:
                raise ValueError("sensitivity out of range")
        except (ValueError, TypeError, KeyError, json.JSONDecodeError):
            self.send_error(HTTPStatus.BAD_REQUEST, "Sensitivity must be between 0.05 and 0.80")
            return
        if not self.dashboard.reader.send_command(f"SENS {sensitivity:.3f}"):
            self.send_error(HTTPStatus.CONFLICT, "Serial device is not connected")
            return
        self._send_bytes("application/json", b'{"ok":true}')

    def _predict_doodle(self) -> None:
        content_type = self.headers.get_content_type()
        if content_type not in DOODLE_IMAGE_TYPES:
            self.close_connection = True
            self._send_json_error(
                HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                "Send a PNG or JPEG image",
            )
            return
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            top_k = int(parse_qs(urlparse(self.path).query).get("top_k", ["5"])[0])
            if not 1 <= top_k <= 10:
                raise ValueError
        except ValueError:
            self.close_connection = True
            self._send_json_error(HTTPStatus.BAD_REQUEST, "Invalid request")
            return
        if not 0 < content_length <= MAX_DOODLE_IMAGE_BYTES:
            self.close_connection = True
            self._send_json_error(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "Image must be between 1 byte and 2 MiB",
            )
            return

        image_bytes = self.rfile.read(content_length)
        try:
            predictions = self.dashboard.doodle_predictor.predict(image_bytes, top_k=top_k)
        except InvalidImage as error:
            self._send_json_error(HTTPStatus.BAD_REQUEST, str(error))
            return
        except ModelUnavailable as error:
            self._send_json_error(HTTPStatus.SERVICE_UNAVAILABLE, str(error))
            return
        except PredictionError as error:
            self._send_json_error(HTTPStatus.INTERNAL_SERVER_ERROR, str(error))
            return
        except Exception as error:
            print(f"Unexpected doodle prediction error: {error}")
            self._send_json_error(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                "Prediction failed unexpectedly",
            )
            return
        payload = json.dumps(
            {
                "model": self.dashboard.doodle_predictor.model_id,
                "predictions": [prediction.as_dict() for prediction in predictions],
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode()
        self._send_bytes("application/json; charset=utf-8", payload)

    def _send_json_error(self, status: HTTPStatus, message: str) -> None:
        payload = json.dumps({"error": {"message": message}}).encode()
        self._send_bytes("application/json; charset=utf-8", payload, status=status)

    def _send_bytes(
        self, content_type: str, payload: bytes, *, status: HTTPStatus = HTTPStatus.OK
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _stream_events(self) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        subscriber = self.dashboard.broker.subscribe()
        try:
            while True:
                try:
                    payload = subscriber.get(timeout=5.0)
                    message = f"data: {payload}\n\n".encode()
                except queue.Empty:
                    message = b": heartbeat\n\n"
                self.wfile.write(message)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.dashboard.broker.unsubscribe(subscriber)

    def log_message(self, _format: str, *_args: Any) -> None:
        return


def find_serial_device(requested: str | None) -> str:
    if requested:
        return requested
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not candidates:
        raise SystemExit("No ESP32 serial device found. Connect it or pass --device PORT.")
    return candidates[0]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="Serial device (default: first /dev/cu.usbmodem*)")
    parser.add_argument("--host", default="127.0.0.1", help="Dashboard bind address")
    parser.add_argument("--port", type=int, default=8765, help="Dashboard HTTP port")
    parser.add_argument("--no-open", action="store_true", help="Do not open a browser automatically")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    device = find_serial_device(args.device)
    stop_event = threading.Event()
    broker = EventBroker()
    index_path = Path(__file__).with_name("web") / "index.html"
    reader = SerialReader(device, broker, stop_event)
    predictor = LiteRTQuickDrawPredictor(DEFAULT_MODEL_ID)
    server = DashboardServer((args.host, args.port), index_path, broker, reader, predictor)
    reader.start()

    def stop(_signum: int, _frame: Any) -> None:
        stop_event.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    url = f"http://{args.host}:{args.port}"
    print(f"IMU dashboard: {url}")
    print(f"Serial source: {device}")
    print("Press Ctrl+C to stop.")
    if not args.no_open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    finally:
        stop_event.set()
        server.server_close()
        reader.join(timeout=2.0)


if __name__ == "__main__":
    main()
