#!/usr/bin/env python3
import base64
import json
import math
import subprocess
import sys
import time
import unittest
import wave
from array import array
from pathlib import Path


MOCK_URL = "ws://127.0.0.1:18080"
EVENT_LOG = Path("/tmp/mod-openai-mock-events.jsonl")
PLAYBACK_DRAIN_MARGIN_SECONDS = 0.4
PLAYBACK_DURATION_TOLERANCE_SECONDS = 0.1


def api(command, timeout=10):
    result = subprocess.run(
        ["fs_cli", "-x", command],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(f"fs_cli failed for {command!r}: {result.stderr.strip()}")
    return result.stdout.strip()


def assert_ok(test, command):
    result = api(command)
    test.assertTrue(result.startswith("+OK"), f"command failed: {command}\n{result}")
    return result


def assert_error(test, command):
    result = api(command)
    test.assertTrue(result.startswith("-ERR"), f"command unexpectedly succeeded: {command}\n{result}")
    return result


def mock_events():
    if not EVENT_LOG.exists():
        return []
    events = []
    for line in EVENT_LOG.read_text(encoding="utf-8").splitlines():
        if line:
            events.append(json.loads(line))
    return events


def wait_for_event(predicate, start_index=0, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        events = mock_events()
        for event in events[start_index:]:
            if predicate(event):
                return event
        time.sleep(0.05)
    return None


def encode_json(payload):
    compact = json.dumps(payload, separators=(",", ":")).encode()
    return base64.b64encode(compact).decode()


def read_mono_pcm16(path):
    with wave.open(str(path), "rb") as recording:
        channels = recording.getnchannels()
        sample_width = recording.getsampwidth()
        sample_rate = recording.getframerate()
        frames = recording.readframes(recording.getnframes())

    if sample_width != 2:
        raise AssertionError(f"expected PCM16 recording, got {sample_width * 8}-bit samples")

    samples = array("h")
    samples.frombytes(frames)
    if sys.byteorder == "big":
        samples.byteswap()
    if channels > 1:
        samples = array(
            "h",
            (
                sum(samples[index : index + channels]) // channels
                for index in range(0, len(samples), channels)
            ),
        )
    return sample_rate, samples


def goertzel_power(samples, sample_rate, frequency):
    coefficient = 2 * math.cos(2 * math.pi * frequency / sample_rate)
    previous = 0.0
    previous_previous = 0.0
    for sample in samples:
        current = sample + coefficient * previous - previous_previous
        previous_previous = previous
        previous = current
    return previous_previous**2 + previous**2 - coefficient * previous * previous_previous


def dominant_frequency(samples, sample_rate):
    frequencies = range(300, min(3000, sample_rate // 2), 25)
    return max(frequencies, key=lambda frequency: goertzel_power(samples, sample_rate, frequency))


def audible_duration(samples, sample_rate):
    window_size = max(sample_rate // 50, 1)
    active_samples = 0
    for offset in range(0, len(samples) - window_size + 1, window_size):
        window = samples[offset : offset + window_size]
        mean_square = sum(sample * sample for sample in window) / len(window)
        if mean_square >= 500**2:
            active_samples += len(window)
    return active_samples / sample_rate


def wait_for_realtime_playback(duration_seconds):
    # The mock queues audio immediately, but FreeSWITCH consumes it at media rate.
    time.sleep(duration_seconds + PLAYBACK_DRAIN_MARGIN_SECONDS)


class ModuleIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.uuid = self.originate_call()
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_OPENAI_API_KEY integration-test-key")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_DISABLE_AUDIOFILES true")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT true")

    def tearDown(self):
        if getattr(self, "uuid", None):
            api(f"uuid_kill {self.uuid}")
        recording = getattr(self, "recording", None)
        if recording:
            recording.unlink(missing_ok=True)
        self.assertTrue(api("status").startswith("UP"), "FreeSWITCH stopped during the test")

    def originate_call(self):
        result = api("originate null/+15555550100 &park()")
        self.assertTrue(result.startswith("+OK "), f"could not originate test channel: {result}")
        uuid = result.removeprefix("+OK ").strip()
        assert_ok(self, f"uuid_broadcast {uuid} silence_stream://-1 aleg")
        return uuid

    def start_stream(self, url=MOCK_URL, start_muted=True):
        before = len(mock_events())
        command = f"uuid_openai_audio_stream {self.uuid} start {url} mono 24k"
        if start_muted:
            command += " mute_user"
        assert_ok(self, command)
        connected = wait_for_event(lambda event: event.get("event") == "connected", before)
        self.assertIsNotNone(connected, "module did not connect to the mock WebSocket server")

    def stop_stream(self, final_payload=None):
        before = len(mock_events())
        command = f"uuid_openai_audio_stream {self.uuid} stop"
        if final_payload is not None:
            command += f" {encode_json(final_payload)}"
        assert_ok(self, command)

        if final_payload is not None:
            delivered = wait_for_event(
                lambda event: event.get("event") == "message" and event.get("payload") == final_payload,
                before,
            )
            self.assertIsNotNone(delivered, "final stop payload was not delivered before disconnect")

        disconnected = wait_for_event(lambda event: event.get("event") == "disconnected", before)
        self.assertIsNotNone(disconnected, "WebSocket did not disconnect after stop")

    def restart_after_automatic_cleanup(self, timeout=5):
        deadline = time.monotonic() + timeout
        last_result = "no start attempt completed"
        while time.monotonic() < deadline:
            before = len(mock_events())
            command = f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 24k mute_user"
            last_result = api(command)
            if last_result.startswith("+OK"):
                connected = wait_for_event(lambda event: event.get("event") == "connected", before)
                self.assertIsNotNone(connected, "restart succeeded but WebSocket did not reconnect")
                return
            time.sleep(0.05)
        self.fail(f"stream context was not cleaned up after peer disconnect: {last_result}")

    def assert_recording_contains_tone(self, expected_frequency, expected_duration_seconds):
        self.assertTrue(
            self.recording.exists(),
            "FreeSWITCH did not create the playback recording",
        )
        sample_rate, samples = read_mono_pcm16(self.recording)
        self.assertGreater(
            audible_duration(samples, sample_rate),
            expected_duration_seconds - PLAYBACK_DURATION_TOLERANCE_SECONDS,
            "recording contains less audible audio than the mock sent",
        )
        frequency = dominant_frequency(samples, sample_rate)
        self.assertAlmostEqual(frequency, expected_frequency, delta=75)

    def test_flow_control_and_final_stop_payload(self):
        self.start_stream()
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} pause")
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} resume")
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} mute openai")
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} unmute openai")

        self.stop_stream({"type": "integration.final", "marker": "stop-payload"})

    def test_caller_audio_reaches_websocket(self):
        before = len(mock_events())
        self.start_stream(start_muted=False)
        audio = wait_for_event(lambda event: event.get("event") == "audio-received", before)
        self.assertIsNotNone(audio, "module did not send caller audio to the WebSocket")
        self.assertGreater(audio["size"], 0, "module sent an empty audio payload")
        self.assertTrue(audio["sample_aligned"], "module sent a partial PCM16 sample")
        self.stop_stream()

    def test_playback_audio_reaches_channel(self):
        self.start_stream()
        self.recording = Path(f"/tmp/mod-openai-playback-{self.uuid}.wav")
        self.recording.unlink(missing_ok=True)
        assert_ok(self, f"uuid_record {self.uuid} start {self.recording}")

        before = len(mock_events())
        request = encode_json({"type": "response.create"})
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {request}")
        sent = wait_for_event(lambda event: event.get("event") == "audio-response-sent", before)
        self.assertIsNotNone(sent, "mock playback tone was not delivered")
        wait_for_realtime_playback(sent["duration_seconds"])
        assert_ok(self, f"uuid_record {self.uuid} stop {self.recording}")

        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.stop_stream()

    def test_reused_response_id_does_not_suppress_new_playback(self):
        self.start_stream()
        self.recording = Path(f"/tmp/mod-openai-response-id-{self.uuid}.wav")
        self.recording.unlink(missing_ok=True)
        assert_ok(self, f"uuid_record {self.uuid} start {self.recording}")

        before = len(mock_events())
        request = encode_json({"type": "integration.reused_response_id"})
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {request}")
        sent = wait_for_event(
            lambda event: event.get("event") == "reused-response-id-audio-sent",
            before,
        )
        self.assertIsNotNone(
            sent,
            "mock did not send audio with the reused response_id",
        )
        wait_for_realtime_playback(sent["duration_seconds"])
        assert_ok(self, f"uuid_record {self.uuid} stop {self.recording}")

        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.stop_stream()

    def test_double_start_is_rejected(self):
        self.start_stream()
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 24k mute_user")
        self.stop_stream()

    def test_rapid_restart(self):
        for _ in range(10):
            self.start_stream()
            self.stop_stream()

    def test_hangup_while_streaming(self):
        self.start_stream()
        before = len(mock_events())
        assert_ok(self, f"uuid_kill {self.uuid}")
        self.uuid = None
        disconnected = wait_for_event(lambda event: event.get("event") == "disconnected", before)
        self.assertIsNotNone(disconnected, "WebSocket did not disconnect after channel hangup")

    def test_immediate_peer_close_without_reconnect(self):
        before = len(mock_events())
        assert_ok(
            self,
            f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL}/close-immediately mono 24k mute_user",
        )
        closed = wait_for_event(lambda event: event.get("event") == "closed", before)
        self.assertIsNotNone(closed, "mock peer did not close the WebSocket")
        disconnected = wait_for_event(lambda event: event.get("event") == "disconnected", before)
        self.assertIsNotNone(disconnected, "mock WebSocket handler did not finish after peer close")

        self.restart_after_automatic_cleanup()
        self.stop_stream()

    def test_invalid_start_arguments_are_rejected(self):
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start http://example.test mono 24k")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} invalid 24k")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 12000")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 24k unexpected")


if __name__ == "__main__":
    unittest.main(verbosity=2)
