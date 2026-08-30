#!/usr/bin/env python3
"""Serve the pigeon and gather the controllers that fly it.

Self-contained on purpose: this directory drops into any repository without
touching a shared file, which is the whole reason it is not part of the game
host next door. The controller half — discovery, the reliable control socket,
the lossy sample datagrams — is lifted from host/dogfight.py rather than
rewritten, because four separate reliability bugs were already found and fixed
in it and none of that is worth learning twice. See docs/connectivity.md.

Standard library only, so this runs on a laptop or a TV box with nothing to
install.
"""
import argparse
import json
import queue
import socket
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).parent

# Where the stick's measured axis frame lives. Every view reads it, so the
# calibration is done once for the board rather than once per page.
CALIBRATION = HERE / "calibration.json"

DISCOVERY_PORT = 41234
DISCOVERY_ASK = b"DOGFIGHT?"
DISCOVERY_REPLY = "DOGFIGHT {port}"

FLUSH_INTERVAL = 1 / 60          # one display frame's worth of samples per event
SILENCE_TIMEOUT = 3              # seconds without a word on any channel before a board is gone
POLL_TIMEOUT = 1                 # how often a quiet socket wakes up to check that
SAMPLE_COLUMNS = 10              # seq,t_us,ax,ay,az,gx,gy,gz,btn_l,btn_r

# Files this server will hand out. Everything else is 404, so a request can
# never reach outside this directory.
PAGES = {"/": "index.html", "/index.html": "index.html", "/googlekey": "google-key.txt"}


class Hub:
    """Fans controller samples out to any number of browser tabs."""

    def __init__(self):
        self.subscribers = []
        self.lock = threading.Lock()

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
                pass  # a stalled tab must never back up a controller's reader


class Controller:
    """One board. Owns its parsing and its share of the outgoing HUD state."""

    def __init__(self, name, write):
        self.name = name
        self.write = write
        self.header = {}
        self.lock = threading.Lock()
        self.batch = []
        self.dropped = 0
        self.events = []
        self.last_seq = None
        self.alive = True
        self.last_heard = time.monotonic()

    def feed(self, line):
        self.last_heard = time.monotonic()
        if line.startswith("#"):
            if line.startswith("#DOGFIGHT"):
                self.header = parse_banner(line)
                print(f"# {self.name}: {self.header}", file=sys.stderr)
            elif not line.startswith("#cols"):
                print(f"# {self.name} {line[1:].strip()}", file=sys.stderr)
            return

        if line.startswith("!"):
            with self.lock:
                self.events.append(line)
            return

        parts = line.split(",")
        if len(parts) < SAMPLE_COLUMNS:
            return
        try:
            values = [int(p) for p in parts[:SAMPLE_COLUMNS]]
        except ValueError:
            return

        seq = values[0]
        if self.last_seq is not None and seq != self.last_seq + 1:
            self.dropped += max(0, seq - self.last_seq - 1)
        self.last_seq = seq

        with self.lock:
            self.batch.append(values)

    def drain(self):
        with self.lock:
            batch, events = self.batch, self.events
            self.batch, self.events = [], []
        return batch, events

    def send_hud(self, fields):
        parts = " ".join(f"{k}={v}" for k, v in fields.items())
        try:
            self.write(f"!hud {parts}\n".encode())
        except OSError:
            self.alive = False


def parse_banner(line):
    """Pick the sample rate and scale factors out of the device's banner."""
    fields = {}
    for token in line.split()[1:]:
        key, _, value = token.partition("=")
        if not value:
            continue
        try:
            fields[key] = float(value)
        except ValueError:
            fields[key] = value
    return fields


class Fleet:
    """Every connected controller, in join order, which is player order."""

    def __init__(self):
        self.controllers = []
        self.lock = threading.Lock()

    def add(self, controller):
        with self.lock:
            self.controllers.append(controller)
        print(f"# {controller.name} joined as player {self.index(controller)}", file=sys.stderr)

    def remove(self, controller):
        with self.lock:
            if controller in self.controllers:
                self.controllers.remove(controller)
        print(f"# {controller.name} left", file=sys.stderr)

    def index(self, controller):
        return self.controllers.index(controller)

    def get(self, index):
        with self.lock:
            return self.controllers[index] if 0 <= index < len(self.controllers) else None

    def snapshot(self):
        with self.lock:
            return list(self.controllers)


def read_socket(sock, controller, fleet):
    """Lines arrive split across packets, so a partial tail has to survive."""
    fleet.add(controller)
    pending = b""
    try:
        while True:
            # Once samples moved to their own datagram this socket carries only
            # the occasional banner, so its own quiet says nothing about the
            # board. A timeout here is a chance to check, not a verdict.
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                if time.monotonic() - controller.last_heard > SILENCE_TIMEOUT:
                    break
                continue
            if not chunk:
                break
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                controller.feed(raw.decode("utf-8", "replace").strip())
    except OSError:
        pass
    finally:
        fleet.remove(controller)
        sock.close()


def serve_controllers(fleet, tcp_port):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", tcp_port))
    listener.listen(8)
    while True:
        sock, addr = listener.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # A board that reboots leaves its old socket half-open, and the reader
        # would sit in recv for ever holding a phantom player. Controllers
        # stream continuously, so silence this long means gone.
        sock.settimeout(POLL_TIMEOUT)
        controller = Controller(f"{addr[0]}", sock.sendall)
        threading.Thread(target=read_socket, args=(sock, controller, fleet), daemon=True).start()


def read_datagrams(fleet, udp_port):
    """Samples arrive by datagram so one lost packet cannot hold up the rest.

    A datagram carries whole lines and is matched to a controller by source
    address, which is also the name it joined under."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", udp_port))
    while True:
        data, addr = listener.recvfrom(8192)
        controller = next((c for c in fleet.snapshot() if c.name == addr[0]), None)
        if not controller:
            continue
        for raw in data.split(b"\n"):
            line = raw.decode("utf-8", "replace").strip()
            if line:
                controller.feed(line)


def answer_discovery(tcp_port):
    """Boards broadcast for a host rather than storing an address that goes
    stale the next time DHCP moves things around."""
    responder = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    responder.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    responder.bind(("0.0.0.0", DISCOVERY_PORT))
    while True:
        data, addr = responder.recvfrom(64)
        if data.strip().startswith(DISCOVERY_ASK):
            responder.sendto(DISCOVERY_REPLY.format(port=tcp_port).encode(), addr)


def read_serial(port, fleet):
    import serial

    link = serial.Serial(port, 115200, timeout=0.05)
    controller = Controller(port, link.write)
    fleet.add(controller)
    pending = b""
    try:
        while True:
            chunk = link.read(8192)
            if not chunk:
                continue
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                controller.feed(raw.decode("utf-8", "replace").strip())
    finally:
        fleet.remove(controller)


def pump(fleet, hub):
    """One event per display frame carrying every controller's samples."""
    while True:
        time.sleep(FLUSH_INTERVAL)
        players = []
        for controller in fleet.snapshot():
            batch, events = controller.drain()
            if not batch and not events:
                continue
            players.append({
                "name": controller.name,
                "header": controller.header,
                "dropped": controller.dropped,
                "samples": batch,
                "events": events,
            })
        if players:
            hub.publish({"players": players})


class Handler(BaseHTTPRequestHandler):
    hub = None
    fleet = None

    def log_message(self, *args):
        pass  # the controllers are the only traffic worth reporting

    def do_GET(self):
        if self.path.startswith("/stream"):
            return self.serve_stream()

        name = PAGES.get(self.path.split("?")[0])
        if not name:
            return self.send_error(404, "no such page")
        page = HERE / name
        if not page.is_file():
            # The key is the one file that is deliberately not in the repo, so
            # say which file is missing rather than serving a blank city.
            return self.send_error(404, f"{name} is missing — see pigeon/README.md")

        body = page.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        """Panel state going back to a board, or a freshly measured stick frame."""
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) or b"{}"

        if self.path.startswith("/calibration"):
            CALIBRATION.write_bytes(raw)
            print(f"# calibration saved to {CALIBRATION.name}", file=sys.stderr)
            return self.send_json(b"{}")

        payload = json.loads(raw)
        controller = self.fleet.get(int(payload.pop("player", 0)))
        if controller:
            controller.send_hud(payload)
        self.send_response(200 if controller else 404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def send_json(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
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


def lan_address():
    """The address a phone on the same wifi should be pointed at. No packet is
    actually sent; connecting a UDP socket just picks the outbound interface."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 1))
        return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        probe.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--http-port", type=int, default=8770)
    parser.add_argument("--tcp-port", type=int, default=41235,
                        help="where controllers connect after finding this host")
    parser.add_argument("--serial", default=None,
                        help="also take a controller over USB, e.g. /dev/cu.usbmodem101")
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    hub, fleet = Hub(), Fleet()
    Handler.hub, Handler.fleet = hub, fleet

    threading.Thread(target=answer_discovery, args=(args.tcp_port,), daemon=True).start()
    threading.Thread(target=serve_controllers, args=(fleet, args.tcp_port), daemon=True).start()
    threading.Thread(target=read_datagrams, args=(fleet, args.tcp_port + 1), daemon=True).start()
    threading.Thread(target=pump, args=(fleet, hub), daemon=True).start()
    if args.serial:
        threading.Thread(target=read_serial, args=(args.serial, fleet), daemon=True).start()

    url = f"http://{lan_address()}:{args.http_port}/"
    print(f"# game at {url}\n"
          f"# controllers welcome on tcp {args.tcp_port}, samples on udp {args.tcp_port + 1}",
          file=sys.stderr)
    if not args.no_browser:
        threading.Timer(0.7, lambda: webbrowser.open(url)).start()

    ThreadingHTTPServer(("0.0.0.0", args.http_port), Handler).serve_forever()


if __name__ == "__main__":
    main()
