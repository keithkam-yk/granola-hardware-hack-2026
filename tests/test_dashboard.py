import http.client
import json
import threading
import unittest
from pathlib import Path

from dashboard import DashboardServer, EventBroker, ImuSample, parse_device_event
from doodle_demo.predictor import Prediction


class ImuSampleTests(unittest.TestCase):
    def test_parses_sensor_json(self) -> None:
        payload = {
            "seq": 7, "t": 1200,
            "ax": 1.1, "ay": 2.2, "az": 9.7,
            "gx": -0.4, "gy": 0.5, "gz": 1.6,
            "temp": 26.3,
        }
        sample = ImuSample.from_line(json.dumps(payload).encode())
        self.assertIsNotNone(sample)
        assert sample is not None
        self.assertEqual(sample.seq, 7)
        self.assertAlmostEqual(sample.az, 9.7)

    def test_ignores_firmware_log_lines_and_incomplete_json(self) -> None:
        self.assertIsNone(ImuSample.from_line(b"I (100) imu_streamer: booting"))
        self.assertIsNone(ImuSample.from_line(b'{"seq":1}'))
        self.assertIsNone(ImuSample.from_line(b"{not json}"))


class EventBrokerTests(unittest.TestCase):
    def test_publishes_sample_to_subscriber(self) -> None:
        broker = EventBroker()
        subscriber = broker.subscribe()
        subscriber.get_nowait()  # Initial status event.
        sample = ImuSample(1, 20, 0.1, 0.2, 9.8, 1.0, 2.0, 3.0, 25.0)
        broker.publish_sample(sample)
        event = json.loads(subscriber.get_nowait())
        self.assertEqual(event["type"], "sample")
        self.assertEqual(event["seq"], 1)

    def test_publishes_airmouse_state_and_keeps_it_in_snapshot(self) -> None:
        broker = EventBroker()
        state = {
            "type": "airmouse", "ble": "connected", "tracking": True,
            "calibrated": True, "pwr": False, "boot": True, "sensitivity": 0.18,
        }
        broker.publish_airmouse(state)
        self.assertEqual(broker.snapshot()["airmouse"], state)


class DeviceEventTests(unittest.TestCase):
    def test_parses_airmouse_state(self) -> None:
        state = parse_device_event(
            b'{"type":"airmouse","ble":"advertising","tracking":true,'
            b'"pwr":false,"boot":true}'
        )
        self.assertIsNotNone(state)
        assert state is not None
        self.assertEqual(state["ble"], "advertising")
        self.assertTrue(state["tracking"])
        self.assertTrue(state["boot"])

    def test_parses_relative_cursor_movement(self) -> None:
        event = parse_device_event(
            b'{"type":"cursor","dx":-3,"dy":7,"mode":"boot",'
            b'"pwr":false,"boot":true}'
        )
        self.assertEqual(
            event, {
                "type": "cursor", "dx": -3, "dy": 7, "mode": "boot",
                "pwr": False, "boot": True,
            }
        )

    def test_rejects_cursor_event_without_numeric_deltas(self) -> None:
        self.assertIsNone(parse_device_event(b'{"type":"cursor","dx":"left"}'))

    def test_parses_calibrated_orientation(self) -> None:
        event = parse_device_event(
            b'{"type":"orientation","step":"done","calibrated":true,'
            b'"roll":12.5,"pitch":-8.0,"yaw":31.0,'
            b'"rollNorm":0.3,"pitchNorm":-0.2,"yawNorm":0.6}'
        )
        self.assertIsNotNone(event)
        assert event is not None
        self.assertTrue(event["calibrated"])
        self.assertAlmostEqual(event["yawNorm"], 0.6)

    def test_rejects_orientation_without_numeric_angles(self) -> None:
        self.assertIsNone(
            parse_device_event(
                b'{"type":"orientation","roll":"right","pitch":0,"yaw":0}'
            )
        )

    def test_ignores_samples_logs_and_invalid_json(self) -> None:
        self.assertIsNone(parse_device_event(b'{"seq":1}'))
        self.assertIsNone(parse_device_event(b"I (42) boot"))
        self.assertIsNone(parse_device_event(b"{"))


class FakeReader:
    def __init__(self) -> None:
        self.commands: list[str] = []

    def send_command(self, command: str) -> bool:
        self.commands.append(command)
        return True


class FakeDoodlePredictor:
    model_id = "fake/quickdraw"

    def predict(self, image_bytes: bytes, *, top_k: int = 5) -> tuple[Prediction, ...]:
        if not image_bytes:
            raise AssertionError("image should not be empty")
        return (Prediction("cat", 0.82, "🐱"),)[:top_k]


class DashboardServerTests(unittest.TestCase):
    def setUp(self) -> None:
        index_path = Path(__file__).parents[1] / "web" / "index.html"
        self.reader = FakeReader()
        self.server = DashboardServer(
            ("127.0.0.1", 0),
            index_path,
            EventBroker(),
            self.reader,  # type: ignore[arg-type]
            FakeDoodlePredictor(),  # type: ignore[arg-type]
        )
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

    def test_serves_motion_doodle_page(self) -> None:
        self.connection.request("GET", "/doodle")
        response = self.connection.getresponse()
        body = response.read()
        self.assertEqual(response.status, 200)
        self.assertIn(b"Air Doodle", body)
        self.assertIn(b"EventSource('/events')", body)

    def test_predicts_doodle_on_dashboard_origin(self) -> None:
        image_bytes = b"fake png bytes"
        self.connection.request(
            "POST",
            "/api/doodle/predict?top_k=1",
            body=image_bytes,
            headers={"Content-Type": "image/png", "Content-Length": len(image_bytes)},
        )
        response = self.connection.getresponse()
        payload = json.loads(response.read())
        self.assertEqual(response.status, 200)
        self.assertEqual(payload["model"], "fake/quickdraw")
        self.assertEqual(payload["predictions"][0]["label"], "cat")

    def test_sends_flight_altitude_to_board(self) -> None:
        body = json.dumps({"altitude": 937})
        self.connection.request(
            "POST",
            "/api/flight/altitude",
            body=body,
            headers={"Content-Type": "application/json"},
        )
        response = self.connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(json.loads(response.read()), {"ok": True})
        self.assertEqual(self.reader.commands, ["ALT 937"])

    def test_rejects_out_of_range_flight_altitude(self) -> None:
        body = json.dumps({"altitude": 9999})
        self.connection.request(
            "POST",
            "/api/flight/altitude",
            body=body,
            headers={"Content-Type": "application/json"},
        )
        response = self.connection.getresponse()
        response.read()
        self.assertEqual(response.status, 400)
        self.assertEqual(self.reader.commands, [])


if __name__ == "__main__":
    unittest.main()
