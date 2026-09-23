"""Inbound ESL client for observing module events in integration tests."""

import json
import socket
import time

SPEECH_START_EVENT = "mod_openai_audio_stream::openai_speech_start"
SPEECH_STOP_EVENT = "mod_openai_audio_stream::openai_speech_stop"
JSON_EVENT = "mod_openai_audio_stream::json"
PLAY_EVENT = "mod_openai_audio_stream::play"
CONNECTION_ERROR_EVENT = "mod_openai_audio_stream::error"
CONNECT_EVENT = "mod_openai_audio_stream::connect"


class FreeSwitchEventSocket:
    """Minimal inbound ESL client for module and channel lifecycle events."""

    def __init__(self, unique_id, events=None):
        self._unique_id = unique_id
        self.seen_events = []
        self._socket = socket.create_connection(("127.0.0.1", 8021), timeout=5)
        self._buffer = bytearray()
        try:
            headers, _ = self._receive_packet(5)
            if headers.get("content-type") != "auth/request":
                raise RuntimeError(f"unexpected FreeSWITCH event socket greeting: {headers}")
            self._command("auth ClueCon")
            if events is None:
                events = (
                    f"CUSTOM {SPEECH_START_EVENT} {SPEECH_STOP_EVENT} {JSON_EVENT} "
                    f"{CONNECTION_ERROR_EVENT} {CONNECT_EVENT} {PLAY_EVENT}"
                )
            self._command(f"event json {events}")
        except Exception:
            self.close()
            raise

    def close(self):
        self._socket.close()

    def __enter__(self):
        return self

    def __exit__(self, _exc_type, _exc_value, _traceback):
        self.close()

    def _header_boundary(self):
        boundaries = []
        for marker in (b"\r\n\r\n", b"\n\n"):
            index = self._buffer.find(marker)
            if index >= 0:
                boundaries.append((index, len(marker)))
        return min(boundaries) if boundaries else None

    def _receive_packet(self, timeout):
        deadline = time.monotonic() + timeout

        def receive_more():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout("FreeSWITCH event socket packet deadline exceeded")
            self._socket.settimeout(remaining)
            chunk = self._socket.recv(4096)
            if not chunk:
                raise RuntimeError("FreeSWITCH event socket closed unexpectedly")
            self._buffer.extend(chunk)

        boundary = self._header_boundary()
        while boundary is None:
            receive_more()
            boundary = self._header_boundary()

        header_end, marker_length = boundary
        raw_headers = bytes(self._buffer[:header_end]).decode("utf-8", errors="replace")
        headers = {}
        for line in raw_headers.replace("\r\n", "\n").split("\n"):
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()

        content_length = int(headers.get("content-length", "0"))
        if content_length < 0:
            raise ValueError("negative ESL Content-Length")
        body_start = header_end + marker_length
        packet_end = body_start + content_length
        while len(self._buffer) < packet_end:
            receive_more()
        # Keep the entire packet on timeout so the next wait can resume parsing it.
        body = bytes(self._buffer[body_start:packet_end])
        del self._buffer[:packet_end]
        return headers, body

    def _command(self, command):
        self._socket.sendall(f"{command}\n\n".encode())
        headers, _ = self._receive_packet(5)
        if headers.get("content-type") != "command/reply" or not headers.get("reply-text", "").startswith("+OK"):
            raise RuntimeError(f"FreeSWITCH event socket command failed: {command}: {headers}")

    def wait_for(self, event_name, timeout=5, predicate=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                headers, body = self._receive_packet(remaining)
            except socket.timeout:
                return None
            if headers.get("content-type") != "text/event-json":
                continue
            event = json.loads(body)
            name = event.get("Event-Subclass") or event.get("Event-Name")
            self.seen_events.append((event.get("Unique-ID"), name))
            if (
                event.get("Unique-ID") == self._unique_id
                and name == event_name
                and (predicate is None or predicate(event))
            ):
                return event
        return None
