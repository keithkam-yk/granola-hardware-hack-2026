#!/usr/bin/env python3
"""Bridge the wand's USB CSV stream to a browser viewer.

Reads samples off the serial port in one thread and fans them out to browser
clients over Server-Sent Events. SSE (rather than a websocket) keeps this to the
Python standard library plus pyserial, so it runs anywhere with no install step.
"""
import argparse
import glob
import json
import queue
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial

HERE = Path(__file__).parent
FLUSH_INTERVAL = 1 / 60  # batch a display frame's worth of samples per SSE event


class Hub:
    """Fans serial samples out to any number of SSE subscribers."""

    def __init__(self):
        self.subscribers = []
        self.lock = threading.Lock()
        self.header = {}

    def subscribe(self):
        q = queue.Queue(maxsize=200)
        with self.lock:
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)

    def publish(self, payload):
        with self.lock:
            targets = list(self.subscribers)
        for q in targets:
            try:
                q.put_nowait(payload)
            except queue.Full:
                pass  # a stalled browser tab must not back up the serial reader


def parse_header(line, hub):
    """Pick scale factors out of the firmware's `#WAND ...` banner."""
    fields = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, _, value = token.partition("=")
            try:
                fields[key] = float(value)
            except ValueError:
                fields[key] = value
    hub.header = fields
    print(f"# device: {fields}", file=sys.stderr)


def reader(port, hub, csv_path):
    ser = serial.Serial(port, 115200, timeout=0.05)
    csv_file = open(csv_path, "w") if csv_path else None
    batch = []
    last_flush = time.time()
    host_t0 = None
    dev_t0 = None
    last_seq = None
    dropped = 0
    best_skew = float("inf")
    buf = b""
    synced = False   # the first read lands mid-line; that fragment is not a sample

    while True:
        chunk = ser.read(8192)
        now = time.time()
        if chunk:
            buf += chunk
            if not synced:
                head, sep, rest = buf.partition(b"\n")
                if not sep:
                    continue
                buf, synced = rest, True
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                if line.startswith("#"):
                    if line.startswith("#WAND"):
                        parse_header(line, hub)
                    else:
                        pass  # column banner repeats; not worth echoing
                    continue
                parts = line.split(",")
                if len(parts) < 8:
                    continue
                try:
                    values = [int(p) for p in parts]
                except ValueError:
                    continue
                # Boards running the pre-touch firmware send 8 columns; pad so
                # one viewer works against either.
                values += [0] * (11 - len(values))
                seq, t_us = values[0], values[1]

                if last_seq is not None and seq != last_seq + 1:
                    dropped += max(0, seq - last_seq - 1)
                last_seq = seq

                if host_t0 is None:
                    host_t0, dev_t0 = now, t_us
                # Skew between the two clocks. Its running minimum is the
                # best case the link has managed, so reporting the excess over
                # that minimum gives latency above baseline rather than an
                # arbitrary offset fixed by whenever the host happened to attach.
                skew = (now - host_t0) - (t_us - dev_t0) / 1e6
                if skew < best_skew:
                    best_skew = skew
                lag_ms = (skew - best_skew) * 1000

                batch.append(values[:11] + [round(lag_ms, 2)])
                if csv_file:
                    csv_file.write(line + "\n")

        if batch and now - last_flush >= FLUSH_INTERVAL:
            hub.publish({"h": hub.header, "dropped": dropped, "s": batch})
            batch = []
            last_flush = now


class Handler(BaseHTTPRequestHandler):
    hub = None

    def log_message(self, *args):
        pass  # the sample stream is the only output worth seeing

    def do_POST(self):
        """Accept a recorded session from the viewer so drift can be diagnosed
        against real hand-held data rather than a board sitting on a desk."""
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        captures = HERE / "captures"
        captures.mkdir(exist_ok=True)
        existing = sorted(captures.glob("session-*.json"))
        path = captures / f"session-{len(existing):03d}.json"
        path.write_bytes(body)
        print(f"# captured {len(body)} bytes -> {path}", file=sys.stderr)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(str(path).encode())

    def do_GET(self):
        if self.path.startswith("/stream"):
            return self.serve_stream()
        body = (HERE / "viewer.html").read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        q = self.hub.subscribe()
        try:
            while True:
                payload = q.get()
                self.wfile.write(b"data: " + json.dumps(payload).encode() + b"\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.hub.unsubscribe(q)


def autodetect_port():
    candidates = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not candidates:
        sys.exit("No USB serial device found. Pass --port explicitly.")
    return candidates[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial device (default: autodetect)")
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--csv", default=None, help="also record raw samples to this file")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    hub = Hub()
    Handler.hub = hub

    threading.Thread(target=reader, args=(port, hub, args.csv), daemon=True).start()

    url = f"http://localhost:{args.http_port}/"
    print(f"# reading {port}\n# viewer at {url}", file=sys.stderr)
    if not args.no_browser:
        threading.Timer(0.7, lambda: webbrowser.open(url)).start()

    ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler).serve_forever()


if __name__ == "__main__":
    main()
