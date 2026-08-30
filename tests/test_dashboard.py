import json
import unittest

from dashboard import EventBroker, ImuSample, parse_device_event


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
            "type": "airmouse", "ble": "connected", "clutch": "active",
            "calibrated": True, "touch": True, "sensitivity": 0.18,
        }
        broker.publish_airmouse(state)
        self.assertEqual(broker.snapshot()["airmouse"], state)


class DeviceEventTests(unittest.TestCase):
    def test_parses_airmouse_state(self) -> None:
        state = parse_device_event(
            b'{"type":"airmouse","ble":"advertising","clutch":"idle"}'
        )
        self.assertIsNotNone(state)
        assert state is not None
        self.assertEqual(state["ble"], "advertising")

    def test_parses_relative_cursor_movement(self) -> None:
        event = parse_device_event(
            b'{"type":"cursor","dx":-3,"dy":7,"mode":"laser"}'
        )
        self.assertEqual(
            event, {"type": "cursor", "dx": -3, "dy": 7, "mode": "laser"}
        )

    def test_rejects_cursor_event_without_numeric_deltas(self) -> None:
        self.assertIsNone(parse_device_event(b'{"type":"cursor","dx":"left"}'))

    def test_ignores_samples_logs_and_invalid_json(self) -> None:
        self.assertIsNone(parse_device_event(b'{"seq":1}'))
        self.assertIsNone(parse_device_event(b"I (42) boot"))
        self.assertIsNone(parse_device_event(b"{"))


if __name__ == "__main__":
    unittest.main()
