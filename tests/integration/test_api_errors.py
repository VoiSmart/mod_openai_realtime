"""API failures explain the cause without requiring access to FreeSWITCH logs."""

import base64
import json

from test_module import (
    MOCK_URL,
    RECONNECT_GATE,
    ModuleIntegrationBase,
    api,
    assert_error,
    assert_ok,
    encode_json,
    mock_events,
    wait_for_event,
)


def json_api(command, arguments):
    # JSON escaping carries literal line breaks through the line-based event socket.
    request = {"command": "fsapi", "data": {"cmd": command, "arg": arguments}}
    return json.loads(api("json " + json.dumps(request)))["response"]["message"].strip()


class ApiErrorsTest(ModuleIntegrationBase):
    def test_start_rejects_line_breaks_in_extra_headers(self):
        marker = "private-header-marker"
        for api_key in ("", "integration-test-key"):
            assert_ok(self, f"uuid_setvar {self.uuid} STREAM_OPENAI_API_KEY {api_key}")
            for field in ("name", "value"):
                for line_break in ("\r", "\n", "\r\n"):
                    with self.subTest(api_key=bool(api_key), field=field, line_break=repr(line_break)):
                        name = f"X-{marker}{line_break}Injected" if field == "name" else "X-Test"
                        value = f"{marker}{line_break}Injected:yes" if field == "value" else marker
                        headers = json.dumps({name: value}, separators=(",", ":")).replace("\\", "\\\\")
                        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_EXTRA_HEADERS {headers}")
                        self.expect_module_errors("Error initializing mod_openai_audio_stream session")
                        response = assert_error(
                            self,
                            f"uuid_openai_audio_stream {self.uuid} start {MOCK_URL} mono",
                            "STREAM_EXTRA_HEADERS names and values must not contain CR or LF",
                        )
                        self.assertNotIn(marker, response)

        self.assertNotIn(marker, "\n".join(self.module_log_lines()))
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_EXTRA_HEADERS")
        self.start_stream()
        self.stop_stream()

    def test_start_rejects_line_breaks_in_api_key(self):
        marker = "private-api-key-marker"
        for line_break in ("\r", "\n", "\r\n"):
            with self.subTest(line_break=repr(line_break)):
                key = f"{marker}{line_break}Injected:yes"
                self.assertEqual(json_api("uuid_setvar", f"{self.uuid} STREAM_OPENAI_API_KEY {key}"), "+OK")
                self.assertEqual(json_api("uuid_getvar", f"{self.uuid} STREAM_OPENAI_API_KEY"), key)
                self.expect_module_errors("Error initializing mod_openai_audio_stream session")
                response = assert_error(
                    self,
                    f"uuid_raw_audio_stream {self.uuid} start {MOCK_URL} mono",
                    "STREAM_OPENAI_API_KEY must not contain CR or LF",
                )
                self.assertNotIn(marker, response)

        self.assertNotIn(marker, "\n".join(self.module_log_lines()))
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_OPENAI_API_KEY integration-test-key")
        self.start_stream(stream_api="uuid_raw_audio_stream")
        self.stop_stream(stream_api="uuid_raw_audio_stream")

    def test_syntax_errors_return_one_actionable_response(self):
        cases = (
            ("", "Expected a channel UUID and command", "invalid stream command argument count"),
            ("pause " + "argument " * 15, "Too many command arguments", "invalid stream command argument count"),
            ("unknown", "Unknown command", "unsupported mod_openai_audio_stream cmd"),
            ("pause extra", "Pause and resume", "pause does not accept arguments"),
            ("send_json", "send_json requires exactly one", "send_json requires exactly one argument"),
            ("stop e30= e30=", "Stop accepts at most one", "stop accepts at most one final json argument"),
            ("mute invalid", "Invalid mute target", "invalid mute target"),
            ("mute user openai", "Mute and unmute accept at most one", "mute accepts at most one target argument"),
            ("start", "Start requires a WebSocket URI and mix type", "start requires a websocket URI and mix type"),
            (f"start {MOCK_URL} mono mute_user extra", "Unexpected start argument", "unexpected argument"),
            (f"start {MOCK_URL} invalid", "Invalid mix type", "invalid mix type"),
            (f"start {MOCK_URL} mono 12000", "Invalid send sample rate", "invalid send sample rate"),
            (f"start {MOCK_URL} mono 24000 12000", "Invalid playback sample rate", "invalid playback sample rate"),
        )
        for stream_api in ("uuid_openai_audio_stream", "uuid_raw_audio_stream"):
            self.assertTrue(api(stream_api).startswith("USAGE"))
            for action, message, log_fragment in cases:
                with self.subTest(api=stream_api, action=action):
                    self.expect_module_errors(log_fragment)
                    assert_error(self, f"{stream_api} {self.uuid} {action}", message)
        self.expect_module_errors("Error locating session 00000000-0000-0000-0000-000000000000")
        assert_error(self, "uuid_openai_audio_stream 00000000-0000-0000-0000-000000000000 stop", "Channel not found")

    def test_duplicate_and_missing_stream_errors(self):
        self.start_stream()
        self.expect_module_errors("bug already attached")
        assert_error(self, f"uuid_raw_audio_stream {self.uuid} start {MOCK_URL} mono", "Stream already exists")
        self.assertEqual(assert_ok(self, f"uuid_openai_audio_stream {self.uuid} pause"), "+OK Success")
        self.stop_stream()
        for action, log_fragment in (
            ("pause", "no media bug found"),
            ("stop", "no media bug found"),
            ("send_json e30=", "no bug, failed sending json"),
        ):
            self.expect_module_errors(log_fragment)
            assert_error(self, f"uuid_openai_audio_stream {self.uuid} {action}", "Stream not found")

    def test_disconnected_socket_reports_send_failure_and_applied_mute(self):
        path = "/gated-api-errors"
        RECONNECT_GATE.unlink(missing_ok=True)
        self.addCleanup(RECONNECT_GATE.unlink, missing_ok=True)
        assert_ok(self, f"uuid_setvar {self.uuid} STREAM_NO_RECONNECT false")
        self.start_stream(f"{MOCK_URL}{path}", start_muted=False)
        before = len(mock_events())
        try:
            close = encode_json({"type": "integration.close"})
            assert_ok(self, f"uuid_openai_audio_stream {self.uuid} send_json {close}")
            self.assertIsNotNone(
                wait_for_event(
                    lambda event: event.get("event") == "reconnect-blocked" and event.get("path") == path, before
                )
            )
            assert_error(
                self,
                f"uuid_openai_audio_stream {self.uuid} send_json e30=",
                "WebSocket is not connected",
            )
            assert_error(
                self,
                f"uuid_openai_audio_stream {self.uuid} mute user",
                "User audio muted; WebSocket is not connected",
            )
            # The reported failure concerns mute silence, while the capture mute itself took effect.
            self.assertEqual(assert_ok(self, f"uuid_openai_audio_stream {self.uuid} mute user"), "+OK Success")
        finally:
            RECONNECT_GATE.touch()
        self.assertIsNotNone(
            wait_for_event(lambda event: event.get("event") == "connected" and event.get("path") == path, before)
        )
        self.stop_stream()

    def test_errors_hide_payloads_and_report_partial_stop_success(self):
        marker = "api-error-secret-marker"
        encoded = base64.b64encode(f'{{"secret":"{marker}"'.encode()).decode()
        for suppress_log in ("false", "true"):
            with self.subTest(suppress_log=suppress_log):
                assert_ok(self, f"uuid_setvar {self.uuid} STREAM_SUPPRESS_LOG {suppress_log}")
                self.start_stream()
                self.expect_module_errors("invalid JSON", "invalid websocket uri", "invalid JSON")
                json_error = assert_error(
                    self,
                    f"uuid_openai_audio_stream {self.uuid} send_json {encoded}",
                    "Payload is not a complete JSON value",
                )
                uri_error = assert_error(
                    self,
                    f"uuid_openai_audio_stream {self.uuid} start http://{marker}.example mono",
                    "Invalid WebSocket URI",
                )
                before = len(mock_events())
                stop_error = assert_error(
                    self,
                    f"uuid_openai_audio_stream {self.uuid} stop {encoded}",
                    "Stream stopped; final message failed: Payload is not a complete JSON value",
                )
                for response in (json_error, uri_error, stop_error):
                    self.assertNotIn(marker, response)
                    self.assertNotIn(encoded, response)
                self.assertIsNotNone(wait_for_event(lambda event: event.get("event") == "disconnected", before))
                self.start_stream()
                self.stop_stream()
