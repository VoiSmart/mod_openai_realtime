#!/usr/bin/env python3
import base64
import json
import os
import subprocess
import time
import unittest
import wave
from contextlib import contextmanager
from pathlib import Path

from audio import (
    AUDIBLE_SAMPLE_THRESHOLD,
    PCM16_BYTES_PER_SAMPLE,
    audible_duration,
    audio_activity_metrics,
    dominant_frequency,
    longest_silent_gap,
    read_mono_pcm16,
    tone_durations,
    tone_runs,
)
from esl import (
    CONNECT_EVENT,
    CONNECTION_ERROR_EVENT,
    JSON_EVENT,
    PLAY_EVENT,
    SPEECH_START_EVENT,
    SPEECH_STOP_EVENT,
    FreeSwitchEventSocket,
)

MOCK_URL = "ws://127.0.0.1:18080"
EVENT_LOG = Path("/tmp/mod-openai-mock-events.jsonl")
RECONNECT_GATE = EVENT_LOG.with_name("mod-openai-reconnect-gate")
PLAYBACK_DRAIN_MARGIN_SECONDS = 0.4
PLAYBACK_DURATION_TOLERANCE_SECONDS = 0.1
FREESWITCH_LOG = Path(os.environ.get("FREESWITCH_RUNTIME_LOG", "/tmp/mod-openai-freeswitch.log"))
ARTIFACT_DIR = Path(os.environ["TEST_ARTIFACT_DIR"]) if os.environ.get("TEST_ARTIFACT_DIR") else None
MODULE_LOG_SOURCES = ("mod_openai_audio_stream.c:", "openai_audio_streamer_glue.cpp:")
MEDIA_BUG_NAME = "audio_stream"  # Keep in sync with MY_BUG_NAME in mod_openai_audio_stream.h.


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


def assert_error(test, command, expected_message=None):
    result = api(command)
    lines = result.splitlines()
    test.assertEqual(len(lines), 1, f"command did not return one error line: {command}\n{result}")
    test.assertTrue(result.startswith("-ERR "), f"command unexpectedly succeeded: {command}\n{result}")
    if expected_message is not None:
        test.assertIn(expected_message, result)
    return result


def mock_events():
    if not EVENT_LOG.exists():
        return []
    events = []
    # The mock may still be appending the last record; consume only newline-terminated entries.
    for line in EVENT_LOG.read_text(encoding="utf-8").split("\n")[:-1]:
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


def wait_until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return False


def encode_json(payload):
    compact = json.dumps(payload, separators=(",", ":")).encode()
    return base64.b64encode(compact).decode()


def wait_for_realtime_playback(duration_seconds):
    # The mock queues audio immediately, but FreeSWITCH consumes it at media rate.
    time.sleep(duration_seconds + PLAYBACK_DRAIN_MARGIN_SECONDS)


class ModuleIntegrationBase(unittest.TestCase):
    def setUp(self):
        self.uuid = None
        self.recording = None
        self.cleanup_events = None
        self.event_start = len(mock_events())
        self.freeswitch_log_start = FREESWITCH_LOG.stat().st_size
        self.expected_module_error_fragments = []
        self.addCleanup(self.cleanup_resources)
        self.originate_call()
        self.cleanup_events = FreeSwitchEventSocket(self.uuid, events="CHANNEL_DESTROY")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_OPENAI_API_KEY integration-test-key")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_DISABLE_AUDIOFILES true")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT true")

    def tearDown(self):
        self.cleanup_resources()
        self.assertTrue(api("status").startswith("UP"), "FreeSWITCH stopped during the test")
        events = mock_events()[self.event_start :]
        invalid_events = [event for event in events if event.get("event") in {"invalid-json", "invalid-audio-message"}]
        self.assertEqual(invalid_events, [], "module sent malformed data to the mock WebSocket server")
        misaligned_audio = [
            event
            for event in events
            if event.get("event") in {"audio-received", "binary"} and event.get("sample_aligned") is not True
        ]
        self.assertEqual(misaligned_audio, [], "module sent a partial PCM16 sample")
        self.assert_expected_module_errors()

    def cleanup_resources(self):
        try:
            if self.uuid:
                uuid = self.uuid
                self.uuid = None
                api(f"uuid_kill {uuid}")
            if self.cleanup_events:
                self.assertIsNotNone(
                    self.cleanup_events.wait_for("CHANNEL_DESTROY"),
                    "channel teardown did not complete before checking module logs",
                )
        finally:
            if self.cleanup_events:
                self.cleanup_events.close()
                self.cleanup_events = None
            if self.recording and ARTIFACT_DIR is None:
                self.recording.unlink(missing_ok=True)

    def originate_call(self):
        result = api("originate null/+15555550100 &park()")
        self.assertTrue(result.startswith("+OK "), f"could not originate test channel: {result}")
        self.uuid = result.removeprefix("+OK ").strip()
        assert_ok(self, f"uuid_broadcast {self.uuid} silence_stream://-1 aleg")

    def start_read_tone(self, frequency=700):
        source = f"tone_stream://%(10000,0,{frequency});loops=-1"
        assert_ok(self, f"uuid_displace {self.uuid} start {source} 0 rm")
        return source

    def assert_gated_reconnect_discards_capture_backlog(self, path, start_index, tone_source):
        blocked = wait_for_event(
            lambda event: event.get("event") == "reconnect-blocked" and event.get("path") == path,
            start_index,
            timeout=10,
        )
        self.assertIsNotNone(blocked, "mock did not hold the reconnect while the capture source changed")

        try:
            backlog_accumulation_seconds = 0.25
            time.sleep(backlog_accumulation_seconds)
            assert_ok(self, f"uuid_displace {self.uuid} stop {tone_source}")
            quiet_capture_seconds = 0.25
            time.sleep(quiet_capture_seconds)
        finally:
            RECONNECT_GATE.touch()

        reconnected = wait_for_event(
            lambda event: event.get("event") == "connected" and event.get("path") == path,
            start_index,
            timeout=10,
        )
        self.assertIsNotNone(reconnected, "module did not reconnect after the capture stream was interrupted")
        first_capture = wait_for_event(
            lambda event: event.get("event") == "audio-received"
            and event.get("path") == path
            and event.get("connection_number") == reconnected["connection_number"],
            start_index,
        )
        self.assertIsNotNone(first_capture, "module did not resume capture after reconnect")
        self.assertLess(
            first_capture["peak_amplitude"],
            AUDIBLE_SAMPLE_THRESHOLD,
            "audible pre-reconnect capture reached the new connection",
        )

    def module_log_lines(self):
        with FREESWITCH_LOG.open("rb") as log:
            log.seek(self.freeswitch_log_start)
            lines = log.read().decode("utf-8", errors="replace").splitlines()
        return [line for line in lines if any(source in line for source in MODULE_LOG_SOURCES)]

    def assert_expected_module_errors(self):
        unmatched = [line for line in self.module_log_lines() if "[ERR]" in line]
        for fragment in self.expected_module_error_fragments:
            match = next((index for index, line in enumerate(unmatched) if fragment in line), None)
            if match is None:
                self.fail(f"module did not log expected error {fragment!r}; observed {unmatched}")
            unmatched.pop(match)
        self.assertEqual(unmatched, [], "module logged unexpected errors:\n" + "\n".join(unmatched))

    def expect_module_errors(self, *fragments):
        self.expected_module_error_fragments.extend(fragments)

    def start_stream(
        self,
        url=MOCK_URL,
        start_muted=True,
        stream_api="uuid_openai_audio_stream",
        send_rate="24k",
        playback_rate=None,
        mix_type="mono",
    ):
        before = len(mock_events())
        command = f"{stream_api} {self.uuid} start {url} {mix_type} {send_rate}"
        if playback_rate is not None:
            command += f" {playback_rate}"
        if start_muted:
            command += " mute_user"
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            assert_ok(self, command)
            self.assertIsNotNone(event_socket.wait_for(CONNECT_EVENT), "client did not finish connecting")
        connected = wait_for_event(lambda event: event.get("event") == "connected", before)
        self.assertIsNotNone(connected, "module did not connect to the mock WebSocket server")
        return connected

    def start_recording(self, scenario):
        recording_dir = ARTIFACT_DIR or Path("/tmp")
        recording_dir.mkdir(parents=True, exist_ok=True)
        self.recording = recording_dir / f"mod-openai-{scenario}-{self.uuid}.wav"
        self.recording.unlink(missing_ok=True)
        assert_ok(self, f"uuid_record {self.uuid} start {self.recording}")

    def stop_recording(self):
        assert_ok(self, f"uuid_record {self.uuid} stop {self.recording}")
        self.assertTrue(self.recording.exists(), "FreeSWITCH did not create the playback recording")
        return read_mono_pcm16(self.recording)

    def stop_stream(self, final_payload=None, stream_api="uuid_openai_audio_stream"):
        before = len(mock_events())
        command = f"{stream_api} {self.uuid} stop"
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
        last_bug_list = "no media-bug query completed"
        while time.monotonic() < deadline:
            last_bug_list = api(f"uuid_buglist {self.uuid}")
            self.assertFalse(last_bug_list.startswith("-ERR"), f"media-bug query failed: {last_bug_list}")
            if MEDIA_BUG_NAME not in last_bug_list:
                self.start_stream()
                return
            time.sleep(0.05)
        self.fail(f"stream context was not cleaned up after peer disconnect: {last_bug_list}")

    def trigger_response(
        self,
        expected_event,
        payload=None,
        stream_api="uuid_openai_audio_stream",
        timeout=5,
    ):
        before = len(mock_events())
        request = encode_json(payload if payload is not None else {"type": "response.create"})
        assert_ok(self, f"{stream_api} {self.uuid} send_json {request}")
        event = wait_for_event(lambda item: item.get("event") == expected_event, before, timeout=timeout)
        self.assertIsNotNone(event, f"mock did not emit {expected_event}")
        return event

    def trigger_connection_error(self, event_socket):
        assert_ok(
            self,
            f"uuid_openai_audio_stream {self.uuid} start ws://127.0.0.1:1 mono 24k mute_user",
        )
        error_event = event_socket.wait_for(CONNECTION_ERROR_EVENT)
        self.assertIsNotNone(error_event, "module did not emit connection-error details")

        payload = json.loads(error_event.get("_body", ""))
        self.assertEqual(payload.get("status"), "error")
        message = payload.get("message")
        self.assertIsInstance(message, dict)
        self.assertIsInstance(message["error"], str)
        self.assertTrue(message["error"], "connection-error reason is empty")
        self.assertIsInstance(message["http_status"], int)
        self.assertIsInstance(message["retries"], int)
        self.assertIsInstance(message["wait_time"], (int, float))
        return message

    def assert_recording_contains_tone(self, expected_frequency, expected_duration_seconds):
        self.assertTrue(
            self.recording.exists(),
            "FreeSWITCH did not create the playback recording",
        )
        sample_rate, samples = read_mono_pcm16(self.recording)
        duration = audible_duration(samples, sample_rate)
        self.assertGreater(
            duration,
            expected_duration_seconds - PLAYBACK_DURATION_TOLERANCE_SECONDS,
            "recording contains less audible audio than the mock sent",
        )
        self.assertLess(
            duration,
            expected_duration_seconds + PLAYBACK_DURATION_TOLERANCE_SECONDS,
            "recording contains more audible audio than the mock sent",
        )
        frequency = dominant_frequency(samples, sample_rate)
        self.assertAlmostEqual(frequency, expected_frequency, delta=75)


class ModuleIntegrationTest(ModuleIntegrationBase):
    @contextmanager
    def hold_stop_before_removal(self, command):
        markers = {
            name: Path(f"/tmp/mod-openai-lifecycle-{name}")
            for name in ("arm", "remove-entered", "close-entered", "continue")
        }
        for marker in markers.values():
            marker.unlink(missing_ok=True)
            self.addCleanup(marker.unlink, missing_ok=True)
        markers["arm"].touch()
        stop = subprocess.Popen(
            ["fs_cli", "-x", command],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertTrue(wait_until(markers["remove-entered"].exists), "stop did not reach media-bug removal")
            yield markers
            markers["continue"].touch()
            output, errors = stop.communicate(timeout=3)
            self.assertEqual(stop.returncode, 0, errors)
            self.assertTrue(output.startswith("+OK"), f"stop failed: {output}")
        finally:
            markers["continue"].touch()
            if stop.poll() is None:
                stop.kill()
            stop.communicate()

    def test_final_stop_payload(self):
        for buffer_ms in (20, 1000):
            with self.subTest(buffer_ms=buffer_ms):
                assert_ok(self, f"uuid_setvar {self.uuid} STREAM_BUFFER_SIZE {buffer_ms}")
                before = len(mock_events())
                self.start_stream(start_muted=False)
                self.assertIsNotNone(
                    wait_for_event(lambda event: event.get("event") == "audio-received", before),
                    "capture was not active before stop",
                )
                # Leave a partial packet for the stop flush in the buffered case.
                time.sleep(0.1)
                before = len(mock_events())
                payload = {"type": "integration.final", "marker": f"stop-payload-{buffer_ms}"}
                command = f"uuid_openai_audio_stream {self.uuid} stop {encode_json(payload)}"
                with self.hold_stop_before_removal(command):
                    self.assertIsNotNone(
                        wait_for_event(lambda event: event.get("payload") == payload, before),
                        "stop did not send the final payload before removal",
                    )
                    # Allow media callbacks while stop has released the context mutex.
                    time.sleep(0.25)
                self.assertIsNotNone(
                    wait_for_event(lambda event: event.get("event") == "disconnected", before),
                    "WebSocket did not disconnect after stop",
                )
                events = mock_events()[before:]
                final_index = next(i for i, event in enumerate(events) if event.get("payload") == payload)
                self.assertFalse(
                    any(event.get("event") == "audio-received" for event in events[final_index + 1 :]),
                    "capture audio followed the final stop payload",
                )
                if buffer_ms == 1000:
                    self.assertTrue(
                        any(event.get("event") == "audio-received" for event in events[:final_index]),
                        "stop did not flush the capture residue before the final payload",
                    )

    def test_suppress_log_snapshot_hides_payloads_without_suppressing_events(self):
        marker = f"suppressed-payload-{self.uuid}"
        final_payload = {"type": "integration.final", "marker": marker}
        encoded_final_payload = encode_json(final_payload)
        self.expect_module_errors(
            "WebSocket error response received (payload suppressed)",
            "invalid JSON (details suppressed)",
            "invalid websocket uri (details suppressed)",
            "start requires a websocket URI and mix type (arguments suppressed)",
        )
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_SUPPRESS_LOG true")

        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/suppress-log?credential={marker}")
            assert_ok(self, f"uuid_setvar {self.uuid} STREAM_SUPPRESS_LOG false")
            self.trigger_response(
                "error-response-sent",
                {"type": "response.create", "metadata": {"marker": marker}},
            )
            response_event = event_socket.wait_for(
                JSON_EVENT,
                predicate=lambda event: marker in event.get("_body", ""),
            )
            self.assertIsNotNone(response_event, "suppressed WebSocket response event was not emitted")

            invalid_json = base64.b64encode(f'{{"marker":"{marker}"'.encode()).decode()
            assert_error(self, f"uuid_openai_audio_stream {self.uuid} send_json {invalid_json}")
            assert_error(self, f"uuid_openai_audio_stream {self.uuid} start http://{marker}.example mono 24k")
            assert_error(self, f"uuid_openai_audio_stream {self.uuid} start wss://example.test?credential={marker}")

        self.stop_stream(final_payload)
        module_log = "\n".join(self.module_log_lines())
        self.assertNotIn(marker, module_log, "suppressed response or URI payload leaked into the module log")
        self.assertNotIn(encoded_final_payload, module_log, "suppressed final payload leaked into the module log")

    def record_playback_control(self, command, inverse_command, scenario):
        control_hold_seconds = 0.8
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/flow-control")
            self.start_recording(scenario)
            sent = self.trigger_response("flow-control-response-sent")
            started = event_socket.wait_for(SPEECH_START_EVENT)
            self.assertIsNotNone(
                started,
                f"module did not emit the playback-start event; observed {event_socket.seen_events}",
            )

            assert_ok(self, f"uuid_openai_audio_stream {self.uuid} {command}")
            held_since = time.monotonic()
            time.sleep(control_hold_seconds)
            assert_ok(self, f"uuid_openai_audio_stream {self.uuid} {inverse_command}")
            held_seconds = time.monotonic() - held_since
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(stopped, "playback did not drain after releasing the control")
            sample_rate, samples = self.stop_recording()

        runs, _, _ = audio_activity_metrics(samples, sample_rate, window_ms=20)
        self.assertGreaterEqual(runs, 2, f"{command} did not interrupt audible playback")
        self.assertGreater(
            longest_silent_gap(samples, sample_rate),
            control_hold_seconds / 2,
            f"{command} did not create the expected silent playback interval",
        )
        self.stop_stream()
        segments = tone_runs(samples, sample_rate, sent["frequencies"])
        self.assertEqual(
            [frequency for frequency, _ in segments],
            sent["frequencies"],
            "playback skipped or reordered the tone segments",
        )
        return dict(segments), sent["segment_duration"], held_seconds

    def test_pause_and_resume_control_playback(self):
        durations, segment_duration, _ = self.record_playback_control("pause", "resume", "pause-resume")
        for frequency, duration in durations.items():
            self.assertAlmostEqual(
                duration, segment_duration, delta=0.08, msg=f"pause lost or duplicated {frequency} Hz audio"
            )

    def assert_playback_mute_discards_audio(self, target):
        durations, segment_duration, held_seconds = self.record_playback_control(
            f"mute {target}", f"unmute {target}", f"mute-{target}"
        )
        self.assertAlmostEqual(
            sum(durations.values()),
            3 * segment_duration - held_seconds,
            delta=PLAYBACK_DURATION_TOLERANCE_SECONDS,
            msg="mute preserved audio for later replay or discarded too much audio",
        )
        self.assertLess(durations[1100], segment_duration / 2, "muted middle segment was replayed")
        self.assertAlmostEqual(durations[1500], segment_duration, delta=0.08)

    def test_openai_mute_controls_playback(self):
        self.assert_playback_mute_discards_audio("openai")

    def test_playback_emits_speech_lifecycle_events(self):
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream()
            self.trigger_response("audio-response-sent")
            started = event_socket.wait_for(SPEECH_START_EVENT)
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                started,
                f"module did not emit the playback-start event; observed {event_socket.seen_events}",
            )
            self.assertIsNotNone(
                stopped,
                f"module did not emit the playback-stop event after AudioDone; observed {event_socket.seen_events}",
            )
        self.stop_stream()

    def test_invalid_audio_delta_does_not_reopen_completed_playback(self):
        self.expect_module_errors("response.output_audio.delta no audio data")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/invalid-delta-after-done")
            sent = self.trigger_response("invalid-delta-after-done-sent")

            started = event_socket.wait_for(SPEECH_START_EVENT)
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT, timeout=sent["duration_seconds"] + 3)
            self.assertIsNotNone(
                started,
                f"module did not emit playback-start; observed {event_socket.seen_events}",
            )
            self.assertIsNotNone(
                stopped,
                f"invalid audio delta suppressed playback-stop; observed {event_socket.seen_events}",
            )

        self.stop_stream()

    def test_invalid_audio_base64_resets_pcm_boundary(self):
        self.expect_module_errors("base64 decode error")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/invalid-audio-base64-boundary")
            self.start_recording("invalid-audio-base64-boundary")
            sent = self.trigger_response("invalid-audio-base64-boundary-sent")
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                stopped,
                f"playback did not stop after malformed Base64; observed {event_socket.seen_events}",
            )
            sample_rate, samples = self.stop_recording()

        self.assertGreater(audible_duration(samples, sample_rate), 0.35)
        self.assertAlmostEqual(
            dominant_frequency(samples, sample_rate),
            sent["replacement_frequency"],
            delta=75,
            msg="PCM carry crossed a rejected Base64 audio delta",
        )
        self.stop_stream()

    def test_inbound_json_must_be_complete_and_nul_free(self):
        self.expect_module_errors("Dropping text WebSocket message containing a NUL byte")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/invalid-inbound-json")
            self.trigger_response("invalid-inbound-json-sent")
            rejected = event_socket.wait_for(
                JSON_EVENT,
                predicate=lambda event: "trailing-inbound-json" in event.get("_body", ""),
            )
            self.assertIsNotNone(rejected, "JSON with trailing data was accepted as an audio delta")

        self.stop_stream()

    def test_caller_audio_reaches_websocket(self):
        before = len(mock_events())
        self.start_stream(start_muted=False)
        audio = wait_for_event(lambda event: event.get("event") == "audio-received", before)
        self.assertIsNotNone(audio, "module did not send caller audio to the WebSocket")
        self.assertGreater(audio["size"], 0, "module sent an empty audio payload")
        self.assertTrue(audio["sample_aligned"], "module sent a partial PCM16 sample")
        self.stop_stream()

    def test_capture_buffering_and_user_mute(self):
        capture_rate = 24000
        buffer_ms = 100
        aggregated_size = capture_rate * buffer_ms // 1000 * PCM16_BYTES_PER_SAMPLE
        mute_silence_size = capture_rate * PCM16_BYTES_PER_SAMPLE
        self.start_read_tone()
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_BUFFER_SIZE {buffer_ms}")

        before = len(mock_events())
        self.start_stream(start_muted=False)
        aggregated = wait_for_event(
            lambda event: event.get("event") == "audio-received" and event.get("size") == aggregated_size,
            before,
        )
        self.assertIsNotNone(aggregated, "capture audio was not aggregated to the configured duration")
        self.assertGreaterEqual(
            aggregated["peak_amplitude"],
            AUDIBLE_SAMPLE_THRESHOLD,
            "audible caller audio was not captured",
        )

        before = len(mock_events())
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} mute user")
        mute_silence = wait_for_event(
            lambda event: event.get("event") == "audio-received" and event.get("size") == mute_silence_size,
            before,
        )
        self.assertIsNotNone(mute_silence, "muting did not send one second of silence")
        self.assertTrue(mute_silence["all_zero"], "muting sent non-silent PCM data")

        mute_observation_seconds = 0.25
        muted_since = len(mock_events())
        time.sleep(mute_observation_seconds)
        audio_while_muted = [event for event in mock_events()[muted_since:] if event.get("event") == "audio-received"]
        self.assertEqual(audio_while_muted, [], "caller audio continued while user mute was active")

        before = len(mock_events())
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} unmute user")
        resumed = wait_for_event(
            lambda event: event.get("event") == "audio-received" and event.get("size") == aggregated_size,
            before,
        )
        self.assertIsNotNone(resumed, "capture audio did not resume after unmute")
        self.assertGreaterEqual(
            resumed["peak_amplitude"],
            AUDIBLE_SAMPLE_THRESHOLD,
            "audible caller audio did not resume after unmute",
        )
        self.stop_stream()

    def test_stereo_capture_preserves_channel_separation(self):
        self.start_read_tone()
        path = "/stereo-capture"
        for sample_rate in (8000, 24000):
            with self.subTest(sample_rate=sample_rate):
                before = len(mock_events())
                self.start_stream(
                    f"{MOCK_URL}{path}", start_muted=False, mix_type="stereo", send_rate=f"{sample_rate // 1000}k"
                )
                captured = wait_for_event(
                    lambda event: event.get("event") == "audio-received"
                    and event.get("path") == path
                    and len(event.get("channel_peak_amplitudes", [])) == 2,
                    before,
                )
                self.stop_stream()
                self.assertIsNotNone(captured, "module did not send interleaved stereo capture")
                expected_frame_bytes = sample_rate * 20 // 1000 * 2 * PCM16_BYTES_PER_SAMPLE
                self.assertEqual(captured["size"], expected_frame_bytes, "incomplete 20 ms stereo capture frame")
                caller_peak, callee_peak = captured["channel_peak_amplitudes"]
                self.assertGreaterEqual(
                    caller_peak,
                    AUDIBLE_SAMPLE_THRESHOLD,
                    "caller channel did not contain the test tone",
                )
                self.assertLess(callee_peak, AUDIBLE_SAMPLE_THRESHOLD, "caller tone leaked into the callee channel")

    def test_invalid_capture_buffer_size_uses_default(self):
        default_frame_bytes = 24000 * 20 // 1000 * PCM16_BYTES_PER_SAMPLE
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_BUFFER_SIZE 50")

        before = len(mock_events())
        self.start_stream(start_muted=False)
        audio = wait_for_event(
            lambda event: event.get("event") == "audio-received" and event.get("size") == default_frame_bytes,
            before,
        )
        self.assertIsNotNone(audio, "invalid capture buffer size did not fall back to 20 ms")
        self.stop_stream()

    def test_extra_headers_merge_with_authorization(self):
        value = 'preserved "quotes"; /?=+ %0D%0A literal\\r\\n \tend'
        extra_headers = json.dumps(
            {
                "authorization": "Bearer must-be-replaced",
                "X-Integration-Test": value,
            },
            separators=(",", ":"),
        )
        # uuid_setvar consumes one level of backslash escaping.
        escaped_headers = extra_headers.replace("\\", "\\\\")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_EXTRA_HEADERS {escaped_headers}")
        self.assertEqual(api(f"uuid_getvar {self.uuid} STREAM_EXTRA_HEADERS"), extra_headers)

        for api_key in ("", "integration-test-key"):
            with self.subTest(api_key=bool(api_key)):
                assert_ok(self, f"uuid_setvar {self.uuid} STREAM_OPENAI_API_KEY {api_key}")
                connected = self.start_stream()
                self.stop_stream()
                expected_authorization = f"Bearer {api_key}" if api_key else "Bearer must-be-replaced"
                self.assertEqual(connected["authorization_headers"], [expected_authorization])
                self.assertEqual(connected["integration_headers"], [value])

        self.assertFalse(any("STREAM_OPENAI_API_KEY is not set" in line for line in self.module_log_lines()))

    def test_playback_audio_reaches_channel(self):
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream()
            self.start_recording("playback")

            sent = self.trigger_response("audio-response-sent")
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                stopped,
                f"playback did not stop after response.output_audio.done; observed {event_socket.seen_events}",
            )
            self.stop_recording()

        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.stop_stream()

    def test_audio_delta_without_response_id_reaches_channel(self):
        self.start_stream(f"{MOCK_URL}/no-response-id")
        self.start_recording("no-response-id")
        sent = self.trigger_response("audio-response-sent")
        wait_for_realtime_playback(sent["duration_seconds"])
        self.stop_recording()
        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.stop_stream()

    def test_reused_response_id_does_not_suppress_new_playback(self):
        self.start_stream()
        self.start_recording("response-id")

        sent = self.trigger_response(
            "reused-response-id-audio-sent",
            {"type": "integration.reused_response_id"},
        )
        wait_for_realtime_playback(sent["duration_seconds"])
        self.stop_recording()

        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.stop_stream()

    def test_playback_recovers_from_repeated_underruns(self):
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/underrun")
            self.start_recording("underrun")

            sent = self.trigger_response("underrun-response-sent", timeout=10)
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                stopped,
                f"playback did not stop after the underrun response; observed {event_socket.seen_events}",
            )
            sample_rate, samples = self.stop_recording()

        # Use analysis windows shorter than each 5 ms burst: otherwise a burst crossing a
        # window boundary can be counted as 10 ms and make the upper bound timing-dependent.
        runs, active_time, active_span = audio_activity_metrics(samples, sample_rate, window_ms=1)
        expected_active_time = sent["burst_count"] * sent["burst_duration"]
        expected_active_span = (sent["burst_count"] - 1) * sent["burst_interval"] + sent["burst_duration"]
        self.assertGreaterEqual(runs, sent["burst_count"] - 4, "playback did not recover after repeated underruns")
        self.assertGreater(
            active_span,
            expected_active_span - PLAYBACK_DURATION_TOLERANCE_SECONDS,
            "short replacement frames compressed the playback timeline",
        )
        self.assertGreater(active_time, expected_active_time * 0.6)
        self.assertLess(active_time, expected_active_time * 1.5, "playback repeated or stretched underrun audio")
        self.assertAlmostEqual(dominant_frequency(samples, sample_rate), sent["frequency"], delta=75)
        self.stop_stream()

    def test_barge_in_discards_buffered_audio(self):
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/barge-in")
            self.start_recording("barge-in")

            sent = self.trigger_response("barge-in-response-sent")
            interrupted_stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                interrupted_stopped,
                f"barge-in did not stop buffered playback; observed {event_socket.seen_events}",
            )
            replacement_started = event_socket.wait_for(SPEECH_START_EVENT)
            self.assertIsNotNone(
                replacement_started,
                f"replacement playback did not start; observed {event_socket.seen_events}",
            )
            replacement_stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                replacement_stopped,
                f"replacement playback did not stop; observed {event_socket.seen_events}",
            )
            sample_rate, samples = self.stop_recording()

        durations = tone_durations(
            samples,
            sample_rate,
            (sent["interrupted_frequency"], sent["replacement_frequency"]),
        )
        self.assertLess(
            durations[sent["interrupted_frequency"]],
            0.5,
            "audio queued before barge-in was still played",
        )
        self.assertGreater(
            durations[sent["replacement_frequency"]],
            0.4,
            "replacement audio following barge-in did not reach the channel",
        )
        self.stop_stream()

    def test_debug_audio_file_is_private_and_removed_on_stop(self):
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_DISABLE_AUDIOFILES false")
        self.start_stream(f"{MOCK_URL}/debug-audio")

        temp_dir = Path(api("global_getvar temp_dir"))
        debug_file = temp_dir / f"{self.uuid}_0.tmp.wav"
        debug_file.unlink(missing_ok=True)
        sent = self.trigger_response("debug-audio-response-sent")

        def debug_wav_is_complete():
            try:
                with wave.open(str(debug_file), "rb") as recording:
                    return len(recording.readframes(recording.getnframes())) == sent["byte_count"]
            except (EOFError, FileNotFoundError, wave.Error):
                return False

        self.assertTrue(wait_until(debug_wav_is_complete), "module did not finish writing the debug WAV file")

        self.assertEqual(debug_file.stat().st_mode & 0o777, 0o600, "debug audio is not owner-only")
        with wave.open(str(debug_file), "rb") as recording:
            self.assertEqual(recording.getnchannels(), 1)
            self.assertEqual(recording.getsampwidth(), 2)
            self.assertEqual(recording.getframerate(), sent["sample_rate"])
            self.assertEqual(recording.getnframes() * recording.getsampwidth(), sent["byte_count"])
        self.assertEqual(
            list(temp_dir.glob(f"{self.uuid}_*.tmp.wav")),
            [debug_file],
            "a partial PCM16 fragment produced its own debug WAV",
        )

        self.stop_stream()
        self.assertTrue(wait_until(lambda: not debug_file.exists()), "debug WAV survived stream teardown")

    def test_duplicate_audio_delta_fields_are_stripped_only_after_decoding(self):
        self.expect_module_errors("base64 decode error")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_DISABLE_AUDIOFILES false")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_SUPPRESS_LOG false")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/duplicate-audio-delta")
            sent = self.trigger_response("duplicate-audio-delta-sent")
            for event_type, response_id in (
                (JSON_EVENT, "duplicate-fragment"),
                (PLAY_EVENT, "duplicate-audio"),
            ):
                event = event_socket.wait_for(event_type, predicate=lambda event: response_id in event.get("_body", ""))
                self.assertIsNotNone(event, f"missing {event_type} for {response_id}")
                payload = json.loads(event["_body"])
                self.assertEqual(payload["type"], "response.output_audio.delta")
                self.assertEqual(payload["response_id"], response_id)
                self.assertFalse(any(key.lower() == "delta" for key in payload), payload)
                if event_type == PLAY_EVENT:
                    with wave.open(payload["file"], "rb") as recording:
                        self.assertEqual(recording.getnframes() * recording.getsampwidth(), sent["byte_count"])

            rejected = event_socket.wait_for(
                JSON_EVENT, predicate=lambda event: "duplicate-invalid" in event.get("_body", "")
            )
            self.assertIsNotNone(rejected, "invalid Base64 was not forwarded")
            self.assertEqual(rejected["_body"], sent["rejected_message"])
        self.stop_stream()

        for response_id in ("duplicate-fragment", "duplicate-audio"):
            logged = [line for line in self.module_log_lines() if response_id in line and "Received message:" in line]
            self.assertTrue(logged, f"missing debug log for {response_id}")
            for line in logged:
                payload = json.loads(line.split("Received message: ", 1)[1])
                self.assertFalse(any(key.lower() == "delta" for key in payload), payload)

    def test_raw_audio_streams_binary_pcm_in_both_directions(self):
        stream_api = "uuid_raw_audio_stream"
        before = len(mock_events())
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            # setUp keeps debug WAVs disabled because the mock deliberately sends one frame per byte.
            # Requesting the mock's 8 kHz rate isolates PCM16 stitching; JSON playback covers resampling.
            self.start_stream(
                f"{MOCK_URL}/raw-audio",
                start_muted=False,
                stream_api=stream_api,
                playback_rate="8k",
            )
            caller_audio = wait_for_event(lambda event: event.get("event") == "binary", before)
            self.assertIsNotNone(caller_audio, "raw mode did not send caller audio as a binary WebSocket frame")
            self.assertGreater(caller_audio["size"], 0)
            self.assertTrue(caller_audio["sample_aligned"], "raw caller audio ended with a partial PCM16 sample")

            self.start_recording("raw-audio")
            sent = self.trigger_response("raw-audio-response-sent", stream_api=stream_api)
            self.assertGreater(sent["split_frame_count"], 1, "mock did not split the binary PCM stream")
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                stopped,
                f"raw playback did not stop after response.output_audio.done; observed {event_socket.seen_events}",
            )
            sample_rate, samples = self.stop_recording()

        durations = tone_durations(
            samples,
            sample_rate,
            (sent["split_frequency"], sent["regular_frequency"]),
        )
        self.assertGreater(
            durations[sent["split_frequency"]],
            sent["split_duration"] / 2,
            "split PCM16 samples were not reconstructed",
        )
        self.assertGreater(
            durations[sent["regular_frequency"]],
            sent["regular_duration"] * 0.8,
            "regular binary playback was truncated",
        )
        self.stop_stream(stream_api=stream_api)

    def test_raw_audio_resets_decoder_state_after_interruption(self):
        stream_api = "uuid_raw_audio_stream"
        self.start_stream(
            f"{MOCK_URL}/raw-audio-boundary",
            stream_api=stream_api,
            playback_rate="24k",
        )
        self.start_recording("raw-audio-boundary")
        sent = self.trigger_response("raw-audio-boundary-response-sent", stream_api=stream_api)
        wait_for_realtime_playback(sent["replacement_duration"])
        sample_rate, samples = self.stop_recording()
        self.assertGreater(audible_duration(samples, sample_rate), 0.35)
        self.assertAlmostEqual(
            dominant_frequency(samples, sample_rate),
            sent["replacement_frequency"],
            delta=75,
            msg="PCM carry or resampler state leaked across the interrupted stream",
        )
        self.stop_stream(stream_api=stream_api)

    def test_raw_audio_resets_decoder_state_after_oversized_frame(self):
        self.expect_module_errors("Dropping oversized binary WebSocket message")
        stream_api = "uuid_raw_audio_stream"
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(
                f"{MOCK_URL}/raw-audio-oversized-boundary",
                stream_api=stream_api,
                playback_rate="24k",
            )
            self.start_recording("raw-audio-oversized-boundary")
            sent = self.trigger_response(
                "raw-audio-oversized-boundary-response-sent",
                stream_api=stream_api,
            )
            stopped = event_socket.wait_for(SPEECH_STOP_EVENT)
            self.assertIsNotNone(
                stopped,
                f"raw playback did not stop after the oversized frame; observed {event_socket.seen_events}",
            )
            sample_rate, samples = self.stop_recording()

        self.assertGreater(audible_duration(samples, sample_rate), 0.35)
        self.assertAlmostEqual(
            dominant_frequency(samples, sample_rate),
            sent["replacement_frequency"],
            delta=75,
            msg="PCM carry crossed an oversized raw frame",
        )
        self.stop_stream(stream_api=stream_api)

    def test_raw_debug_wav_contains_only_complete_pcm16_samples(self):
        stream_api = "uuid_raw_audio_stream"
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_DISABLE_AUDIOFILES false")
        temp_dir = Path(api("global_getvar temp_dir"))
        debug_pattern = f"{self.uuid}_*.tmp.wav"

        self.start_stream(
            f"{MOCK_URL}/raw-audio-debug",
            stream_api=stream_api,
            playback_rate="8k",
        )
        sent = self.trigger_response("raw-audio-debug-response-sent", stream_api=stream_api)

        def debug_payload_bytes():
            try:
                return sum(
                    len(read_mono_pcm16(path)[1]) * PCM16_BYTES_PER_SAMPLE for path in temp_dir.glob(debug_pattern)
                )
            except (EOFError, wave.Error):
                return -1

        self.assertTrue(
            wait_until(lambda: debug_payload_bytes() == sent["byte_count"]),
            "raw debug WAVs did not preserve the complete PCM16 stream",
        )
        debug_files = list(temp_dir.glob(debug_pattern))
        self.assertEqual(len(debug_files), 1, "a partial PCM16 fragment produced its own debug WAV")
        sample_rate, samples = read_mono_pcm16(debug_files[0])
        self.assertEqual(sample_rate, sent["sample_rate"])
        self.assertEqual(len(samples) * PCM16_BYTES_PER_SAMPLE, sent["byte_count"])

        self.stop_stream(stream_api=stream_api)
        self.assertTrue(
            wait_until(lambda: not list(temp_dir.glob(debug_pattern))),
            "raw debug WAV survived stream teardown",
        )

    def test_double_start_is_rejected(self):
        self.expect_module_errors("bug already attached")
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

    def test_unload_refuses_active_stream_and_succeeds_after_stop(self):
        self.start_stream(start_muted=False)
        self.assertIsNotNone(
            wait_for_event(lambda event: event.get("event") == "audio-received", self.event_start),
            "capture did not reach the backend before unload",
        )
        assert_error(self, "unload mod_openai_audio_stream", "Module in use")
        self.assertTrue(api("status").startswith("UP"))
        self.trigger_response("audio-response-sent")
        self.stop_stream()

        assert_ok(self, "unload mod_openai_audio_stream")
        try:
            self.assertTrue(api("status").startswith("UP"))
        finally:
            assert_ok(self, "load mod_openai_audio_stream")
        self.start_stream(stream_api="uuid_raw_audio_stream")
        self.stop_stream(stream_api="uuid_raw_audio_stream")

    def test_stop_overlaps_hangup(self):
        self.start_stream()
        before = len(mock_events())
        with self.hold_stop_before_removal(f"uuid_openai_audio_stream {self.uuid} stop") as markers:
            assert_ok(self, f"uuid_kill {self.uuid}")
            self.assertTrue(wait_until(markers["close-entered"].exists), "hangup did not enter CLOSE")
        self.assertEqual(api(f"uuid_exists {self.uuid}"), "false")
        self.uuid = None
        self.assertIsNotNone(
            wait_for_event(lambda event: event.get("event") == "disconnected", before),
            "WebSocket survived the overlapping stop and hangup",
        )

    def test_query_without_path_reaches_backend(self):
        query = "?token=integration&encoded=%0D%0A"
        connected = self.start_stream(f"{MOCK_URL}{query}")
        self.assertEqual(connected["path"], f"/{query}")
        self.stop_stream()

    def test_reconnects_after_transient_peer_failure(self):
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")
        before = len(mock_events())
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            first_connection = self.start_stream(f"{MOCK_URL}/reconnect")
            self.assertEqual(first_connection["connection_number"], 1)
            self.assertIsNotNone(event_socket.wait_for(CONNECT_EVENT))

            reconnected = wait_for_event(
                lambda event: event.get("event") == "connected"
                and event.get("path") == "/reconnect"
                and event.get("connection_number") == 2,
                before,
                timeout=10,
            )
            self.assertIsNotNone(reconnected, "module did not reconnect after a transient peer failure")
            self.assertIsNotNone(event_socket.wait_for(CONNECT_EVENT), "client did not finish reconnecting")

        self.trigger_response("audio-response-sent")
        self.stop_stream()

    def test_reconnect_discards_capture_residue_before_control_flush(self):
        self.start_read_tone()
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_BUFFER_SIZE 1000")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")

        capture_residue_accumulation_seconds = 0.2
        self.start_stream(f"{MOCK_URL}/close-on-command", start_muted=False)
        time.sleep(capture_residue_accumulation_seconds)
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} pause")

        before_close = len(mock_events())
        close_message = encode_json({"type": "integration.close"})
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close_message}")
            closed = wait_for_event(
                lambda event: event.get("event") == "closed" and event.get("path") == "/close-on-command",
                before_close,
            )
            self.assertIsNotNone(closed, "mock peer did not close the capture stream")
            reconnected = wait_for_event(
                lambda event: event.get("event") == "connected",
                before_close,
                timeout=10,
            )
            self.assertIsNotNone(reconnected, "module did not reconnect after the capture stream was interrupted")
            self.assertIsNotNone(event_socket.wait_for(CONNECT_EVENT), "client did not finish reconnecting")
        reconnected_path = reconnected["path"]
        reconnected_number = reconnected["connection_number"]

        before = len(mock_events())
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} mute user")
        silence = wait_for_event(
            lambda event: event.get("event") == "audio-received"
            and event.get("path") == reconnected_path
            and event.get("connection_number") == reconnected_number
            and event.get("all_zero"),
            before,
        )
        self.assertIsNotNone(silence, "mute user did not send silence after reconnect")
        stale_audio = [
            event
            for event in mock_events()[before:]
            if event.get("event") == "audio-received"
            and event.get("path") == reconnected_path
            and event.get("connection_number") == reconnected_number
            and event.get("peak_amplitude", 0) >= AUDIBLE_SAMPLE_THRESHOLD
        ]
        self.assertEqual(stale_audio, [], "capture residue from the dropped connection reached the new connection")
        self.stop_stream()

    def test_reconnect_discards_media_bug_capture_backlog(self):
        RECONNECT_GATE.unlink(missing_ok=True)
        self.addCleanup(RECONNECT_GATE.unlink, missing_ok=True)
        tone_source = self.start_read_tone()
        capture_buffer_ms = 200
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_BUFFER_SIZE {capture_buffer_ms}")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")

        path = "/gated-reconnect"
        before_start = len(mock_events())
        self.start_stream(f"{MOCK_URL}{path}", start_muted=False)
        audible_capture = wait_for_event(
            lambda event: event.get("event") == "audio-received"
            and event.get("path") == path
            and event.get("peak_amplitude", 0) >= AUDIBLE_SAMPLE_THRESHOLD,
            before_start,
        )
        self.assertIsNotNone(audible_capture, "read-side test tone did not reach the WebSocket")

        before_close = len(mock_events())
        close_message = encode_json({"type": "integration.close"})
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close_message}")
        closed = wait_for_event(
            lambda event: event.get("event") == "closed" and event.get("path") == path,
            before_close,
        )
        self.assertIsNotNone(closed, "mock peer did not close the active capture stream")

        self.assert_gated_reconnect_discards_capture_backlog(path, before_close, tone_source)
        self.stop_stream()

    def test_initial_connection_retry_discards_capture_backlog(self):
        self.expect_module_errors("WebSocket connection error:")
        RECONNECT_GATE.unlink(missing_ok=True)
        self.addCleanup(RECONNECT_GATE.unlink, missing_ok=True)
        tone_source = self.start_read_tone()
        capture_buffer_ms = 200
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_BUFFER_SIZE {capture_buffer_ms}")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")

        path = "/gated-initial-connect"
        before_start = len(mock_events())
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL}{path} mono 24k")
        rejected = wait_for_event(
            lambda event: event.get("event") == "handshake-rejected" and event.get("path") == path,
            before_start,
        )
        self.assertIsNotNone(rejected, "mock did not reject the initial WebSocket handshake")

        self.assert_gated_reconnect_discards_capture_backlog(path, before_start, tone_source)
        self.stop_stream()

    def test_reconnect_preserves_completed_playback_lifecycle(self):
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")
            before = len(mock_events())
            first_connection = self.start_stream(f"{MOCK_URL}/reconnect-during-playback")
            self.assertEqual(first_connection["connection_number"], 1)
            self.start_recording("reconnect-playback")

            sent = self.trigger_response("queued-playback-response-sent")

            started = event_socket.wait_for(SPEECH_START_EVENT)
            self.assertIsNotNone(
                started,
                f"module did not emit playback-start before reconnect; observed {event_socket.seen_events}",
            )
            close_message = encode_json({"type": "integration.close"})
            assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close_message}")
            reconnected = wait_for_event(
                lambda event: event.get("event") == "connected"
                and event.get("path") == "/reconnect-during-playback"
                and event.get("connection_number") == 2,
                before,
                timeout=10,
            )
            self.assertIsNotNone(reconnected, "module did not reconnect while completed playback was draining")

            stopped = event_socket.wait_for(SPEECH_STOP_EVENT, timeout=sent["duration_seconds"] + 3)
            self.assertIsNotNone(
                stopped,
                f"module lost playback-stop state across reconnect; observed {event_socket.seen_events}",
            )
            self.stop_recording()

        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.stop_stream()

    def test_terminal_close_drains_queued_playback(self):
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/terminal-close-during-playback")
            self.start_recording("terminal-close-playback")
            sent = self.trigger_response("queued-playback-response-sent")
            self.assertIsNotNone(event_socket.wait_for(SPEECH_START_EVENT), "playback did not start before close")

            before_close = len(mock_events())
            close_message = encode_json({"type": "integration.close"})
            assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close_message}")
            closed = wait_for_event(lambda event: event.get("event") == "closed", before_close)
            self.assertIsNotNone(closed, "mock did not close the connection while audio was queued")
            self.assertIsNotNone(
                event_socket.wait_for(SPEECH_STOP_EVENT, timeout=sent["duration_seconds"] + 3),
                "terminal close did not complete playback without response.output_audio.done",
            )
            self.stop_recording()

        self.assert_recording_contains_tone(sent["frequency"], sent["duration_seconds"])
        self.restart_after_automatic_cleanup()
        self.stop_stream()

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

    def test_connection_error_logs_diagnostics(self):
        self.expect_module_errors("WebSocket connection error:")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            message = self.trigger_connection_error(event_socket)

        matching_logs = [line for line in self.module_log_lines() if "WebSocket connection error:" in line]
        self.assertEqual(len(matching_logs), 1, f"unexpected connection-error logs: {matching_logs}")
        diagnostic = matching_logs[0]
        self.assertIn(message["error"], diagnostic)
        self.assertIn(f"HTTP status {message['http_status']}", diagnostic)
        self.assertIn(f"retries {message['retries']}", diagnostic)
        self.assertIn(f"retry wait {message['wait_time']:g} ms", diagnostic)

    def test_connection_error_respects_log_suppression(self):
        self.expect_module_errors("WebSocket connection error (details suppressed)")
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_SUPPRESS_LOG true")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            message = self.trigger_connection_error(event_socket)

        matching_logs = [line for line in self.module_log_lines() if "WebSocket connection error" in line]
        self.assertEqual(len(matching_logs), 1, f"unexpected connection-error logs: {matching_logs}")
        diagnostic = matching_logs[0]
        self.assertIn("details suppressed", diagnostic)
        self.assertNotIn(message["error"], diagnostic)
        self.assertNotIn("HTTP status", diagnostic)

    def test_rejected_message_logs_are_bounded_per_connection(self):
        self.expect_module_errors(*("Dropping oversized text WebSocket message" for _ in range(2)))
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")
        with FreeSwitchEventSocket(self.uuid) as event_socket:
            self.start_stream(f"{MOCK_URL}/rejected-messages")
            for connection in range(2):
                for _ in range(2):
                    self.trigger_response("rejected-messages-sent")
                    processed = event_socket.wait_for(
                        JSON_EVENT,
                        predicate=lambda event: "integration.rejections_complete" in event.get("_body", ""),
                    )
                    self.assertIsNotNone(processed, "valid messages stopped arriving after rejected frames")
                if connection == 0:
                    before = len(mock_events())
                    close_message = encode_json({"type": "integration.close"})
                    assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close_message}")
                    self.assertIsNotNone(event_socket.wait_for(CONNECT_EVENT), "client did not finish reconnecting")
                    self.assertIsNotNone(wait_for_event(lambda event: event.get("event") == "connected", before))
            self.stop_stream()

        binary_warnings = [line for line in self.module_log_lines() if "Received binary WebSocket frame" in line]
        self.assertEqual(len(binary_warnings), 2, "binary frame warnings were not limited to one per connection")

    def test_peer_close_while_paused_cleans_up_without_resume(self):
        before = len(mock_events())
        self.start_stream(f"{MOCK_URL}/close-on-command")
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} pause")
        close_message = encode_json({"type": "integration.close"})
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close_message}")
        closed = wait_for_event(lambda event: event.get("event") == "closed", before)
        self.assertIsNotNone(closed, "mock peer did not close the paused WebSocket")
        disconnected = wait_for_event(lambda event: event.get("event") == "disconnected", before)
        self.assertIsNotNone(disconnected, "mock WebSocket handler did not finish after paused close")

        self.restart_after_automatic_cleanup()
        self.stop_stream()

    def test_send_json_validates_and_preserves_payloads(self):
        self.expect_module_errors(
            "base64 decode error",
            "base64 decode error",
            "invalid JSON",
            "invalid JSON",
            "decoded JSON contains a NUL byte",
            "decoded JSON contains invalid UTF-8",
        )
        self.start_stream()

        assert_error(self, f"uuid_openai_audio_stream {self.uuid} send_json %%%=", "Invalid Base64 JSON payload")
        assert_error(
            self, f"uuid_openai_audio_stream {self.uuid} send_json eyJhIjoxfQ=!", "Invalid Base64 JSON payload"
        )
        invalid_json = base64.b64encode(b'{"type":').decode("ascii")
        assert_error(
            self,
            f"uuid_openai_audio_stream {self.uuid} send_json {invalid_json}",
            "Payload is not a complete JSON value",
        )
        trailing_data = base64.b64encode(b'{"type":"integration.trailing"}garbage').decode("ascii")
        assert_error(
            self,
            f"uuid_openai_audio_stream {self.uuid} send_json {trailing_data}",
            "Payload is not a complete JSON value",
        )
        embedded_nul = base64.b64encode(b'{"type":"integration.nul"}\0ignored').decode("ascii")
        assert_error(
            self, f"uuid_openai_audio_stream {self.uuid} send_json {embedded_nul}", "JSON payload contains a NUL byte"
        )
        invalid_utf8 = base64.b64encode(b'{"type":"\xff"}').decode("ascii")
        assert_error(
            self, f"uuid_openai_audio_stream {self.uuid} send_json {invalid_utf8}", "JSON payload is not valid UTF-8"
        )

        before = len(mock_events())
        trailing_whitespace = base64.b64encode(b'{"type":"integration.whitespace"} \r\n\t').decode("ascii")
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {trailing_whitespace}")
        delivered = wait_for_event(
            lambda event: event.get("event") == "message" and event.get("type") == "integration.whitespace",
            before,
        )
        self.assertIsNotNone(delivered, "valid JSON with trailing whitespace was rejected")

        before = len(mock_events())
        exact_payload = b'{"type":"integration.preserved","value":9007199254740993,"text":"a\\u0000b"}'
        assert_ok(
            self,
            f"uuid_openai_audio_stream {self.uuid} send_json {base64.b64encode(exact_payload).decode('ascii')}",
        )
        preserved = wait_for_event(
            lambda event: event.get("event") == "message" and event.get("type") == "integration.preserved",
            before,
        )
        self.assertIsNotNone(preserved, "valid JSON payload was not delivered")
        self.assertEqual(preserved["payload"]["value"], 9007199254740993)
        self.assertEqual(preserved["payload"]["text"], "a\0b")

        self.stop_stream()

    def test_cjson_accepted_payload_is_forwarded_unchanged(self):
        self.start_stream(url=f"{MOCK_URL}/verbatim-text")
        # cJSON accepts leading zeros; the module leaves syntax validation to that parser.
        payload = '{"value":01}'
        encoded = base64.b64encode(payload.encode()).decode("ascii")
        for command in ("send_json", "stop"):
            with self.subTest(command=command):
                before = len(mock_events())
                assert_ok(self, f"uuid_openai_audio_stream {self.uuid} {command} {encoded}")
                delivered = wait_for_event(lambda event: event.get("event") == "text-received", before)
                self.assertIsNotNone(delivered, "cJSON-accepted payload was not delivered")
                self.assertEqual(delivered["text"], payload)
        self.assertIsNotNone(
            wait_for_event(lambda event: event.get("event") == "disconnected", before),
            "WebSocket did not disconnect after the final payload",
        )

    def test_commands_reject_excessive_json_depth(self):
        self.expect_module_errors(*["JSON nested deeper than 128 levels"] * 2)
        self.start_stream(url=f"{MOCK_URL}/verbatim-text")
        before = len(mock_events())
        payload = '{"value":' + "[" * 128 + "0" + "]" * 128 + "}"
        encoded = base64.b64encode(payload.encode()).decode("ascii")
        reason = "JSON payload exceeds maximum nesting depth"
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} send_json {encoded}", reason)

        valid_payload = '{"type":"integration.after_depth_rejection"}'
        valid_encoded = base64.b64encode(valid_payload.encode()).decode("ascii")
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {valid_encoded}")
        self.assertIsNotNone(wait_for_event(lambda event: event.get("text") == valid_payload, before))

        assert_error(
            self,
            f"uuid_openai_audio_stream {self.uuid} stop {encoded}",
            f"Stream stopped; final message failed: {reason}",
        )
        self.assertIsNotNone(wait_for_event(lambda event: event.get("event") == "disconnected", before))
        delivered = [event["text"] for event in mock_events()[before:] if event.get("event") == "text-received"]
        self.assertEqual(delivered, [valid_payload], "an over-depth payload reached the backend")

    def test_stop_rejects_invalid_utf8_final_payload(self):
        self.expect_module_errors("decoded JSON contains invalid UTF-8")
        self.start_stream()

        before = len(mock_events())
        invalid_utf8 = base64.b64encode(b'{"type":"\xff"}').decode("ascii")
        assert_error(
            self,
            f"uuid_openai_audio_stream {self.uuid} stop {invalid_utf8}",
            "Stream stopped; final message failed: JSON payload is not valid UTF-8",
        )
        disconnected = wait_for_event(lambda event: event.get("event") == "disconnected", before)
        self.assertIsNotNone(disconnected, "WebSocket did not disconnect after rejecting the final payload")

    def test_all_mute_controls_both_audio_directions(self):
        capture_rate = 24000
        mute_silence_size = capture_rate * PCM16_BYTES_PER_SAMPLE
        tone_source = self.start_read_tone()

        before = len(mock_events())
        self.start_stream(start_muted=False)
        captured = wait_for_event(
            lambda event: event.get("event") == "audio-received"
            and event.get("peak_amplitude", 0) >= AUDIBLE_SAMPLE_THRESHOLD,
            before,
        )
        self.assertIsNotNone(captured, "caller audio did not reach the backend before mute all")

        before = len(mock_events())
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} mute all")
        silence = wait_for_event(
            lambda event: event.get("event") == "audio-received"
            and event.get("size") == mute_silence_size
            and event.get("all_zero"),
            before,
        )
        self.assertIsNotNone(silence, "mute all did not mute caller audio")

        before = len(mock_events())
        assert_ok(self, f"uuid_openai_audio_stream {self.uuid} unmute all")
        resumed = wait_for_event(
            lambda event: event.get("event") == "audio-received"
            and event.get("peak_amplitude", 0) >= AUDIBLE_SAMPLE_THRESHOLD,
            before,
        )
        self.assertIsNotNone(resumed, "unmute all did not resume caller audio")
        self.stop_stream()
        assert_ok(self, f"uuid_displace {self.uuid} stop {tone_source}")

        assert_ok(self, f"uuid_break {self.uuid} all")
        assert_ok(self, f"uuid_broadcast {self.uuid} silence_stream://-1 aleg")
        self.assert_playback_mute_discards_audio("all")

    def test_invalid_mute_target_is_rejected(self):
        self.expect_module_errors("invalid mute target")
        self.start_stream()
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} mute invalid")
        self.stop_stream()

    def test_invalid_start_arguments_are_rejected(self):
        self.expect_module_errors(
            "invalid websocket uri",
            "invalid mix type",
            "invalid send sample rate",
            "invalid playback sample rate",
            "unexpected argument",
        )
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start http://example.test mono 24k")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} invalid 24k")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 12000")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 24k 12000")
        assert_error(self, f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono 24k 24k unexpected")

    def test_decimal_sample_rate_upper_bound_is_accepted(self):
        self.start_stream(send_rate="48000", playback_rate="48000")
        self.stop_stream()


if __name__ == "__main__":
    unittest.main(verbosity=2)
