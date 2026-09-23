#!/usr/bin/env python3
import argparse
import asyncio
import base64
import json
import math
import struct
from http import HTTPStatus
from pathlib import Path

from websockets.legacy.server import serve

MAX_WS_MESSAGE_BYTES = 8 * 1024 * 1024  # Keep in sync with openai_audio_streamer_glue.cpp.


def pcm16_tone(sample_rate, frequency, duration_seconds, amplitude=12000, start_sample=0):
    sample_count = int(sample_rate * duration_seconds)
    samples = (
        int(amplitude * math.sin(2 * math.pi * frequency * (start_sample + index) / sample_rate))
        for index in range(sample_count)
    )
    return struct.pack(f"<{sample_count}h", *samples)


class MockRealtimeServer:
    def __init__(self, event_log: Path, ready_file: Path):
        self.event_log = event_log
        self.ready_file = ready_file
        self.reconnect_gate = event_log.with_name("mod-openai-reconnect-gate")
        self._lock = asyncio.Lock()
        self._connection_counts = {}
        self._handshake_counts = {}

    async def record(self, event, **fields):
        payload = {"event": event, **fields}
        async with self._lock:
            with self.event_log.open("a", encoding="utf-8") as log:
                log.write(json.dumps(payload, sort_keys=True) + "\n")

    async def process_request(self, path, _request_headers):
        handshake_number = self._handshake_counts.get(path, 0) + 1
        self._handshake_counts[path] = handshake_number
        if path == "/gated-initial-connect" and handshake_number == 1:
            await self.record("handshake-rejected", path=path, handshake_number=handshake_number)
            body = b"retry connection"
            return (
                HTTPStatus.SERVICE_UNAVAILABLE,
                [("Content-Type", "text/plain"), ("Content-Length", str(len(body)))],
                body,
            )
        if (
            path in {"/gated-reconnect", "/gated-initial-connect", "/gated-api-errors"}
            and handshake_number > 1
            and not self.reconnect_gate.exists()
        ):
            await self.record("reconnect-blocked", path=path, handshake_number=handshake_number)
            while not self.reconnect_gate.exists():
                await asyncio.sleep(0.01)
        return None

    async def handle(self, websocket, path):
        headers = websocket.request_headers
        connection_number = self._connection_counts.get(path, 0) + 1
        self._connection_counts[path] = connection_number
        await self.record(
            "connected",
            path=path,
            connection_number=connection_number,
            authorization_headers=headers.get_all("Authorization"),
            integration_headers=headers.get_all("X-Integration-Test"),
        )

        try:
            if path == "/close-immediately" or (path == "/reconnect" and connection_number == 1):
                await websocket.close(code=1011, reason="intentional integration-test close")
                await self.record("closed", path=path, connection_number=connection_number)
                return

            # Exercise the startup ordering: the peer is allowed to send a message as soon as
            # the WebSocket opens, before the first caller audio frame reaches the module.
            await websocket.send(json.dumps({"type": "session.updated", "session": {"id": "mock-session"}}))

            async for message in websocket:
                if isinstance(message, bytes):
                    await self.record(
                        "binary",
                        path=path,
                        connection_number=connection_number,
                        size=len(message),
                        sample_aligned=len(message) % 2 == 0,
                    )
                    continue

                if path == "/verbatim-text":
                    await self.record("text-received", text=message)
                    continue

                try:
                    payload = json.loads(message)
                except json.JSONDecodeError:
                    await self.record("invalid-json")
                    continue

                message_type = payload.get("type", "")
                if (
                    path
                    in {
                        "/close-on-command",
                        "/gated-reconnect",
                        "/gated-api-errors",
                        "/rejected-messages",
                        "/reconnect-during-playback",
                        "/terminal-close-during-playback",
                    }
                    and message_type == "integration.close"
                ):
                    await websocket.close(code=1011, reason="intentional paused-close integration test")
                    await self.record("closed", path=path, connection_number=connection_number)
                    return

                if message_type == "input_audio_buffer.append":
                    try:
                        audio = base64.b64decode(payload.get("audio", ""), validate=True)
                    except (ValueError, TypeError):
                        await self.record("invalid-audio-message")
                    else:
                        samples = struct.unpack(f"<{len(audio) // 2}h", audio) if len(audio) % 2 == 0 else ()
                        audio_event = {
                            "path": path,
                            "connection_number": connection_number,
                            "size": len(audio),
                            "sample_aligned": len(audio) % 2 == 0,
                            "all_zero": not any(audio),
                            "peak_amplitude": max((abs(sample) for sample in samples), default=0),
                        }
                        if path == "/stereo-capture":
                            audio_event["channel_peak_amplitudes"] = [
                                max((abs(sample) for sample in samples[channel::2]), default=0) for channel in range(2)
                            ]
                        await self.record("audio-received", **audio_event)
                else:
                    await self.record("message", type=message_type, payload=payload)

                if message_type == "session.update":
                    await websocket.send(json.dumps({"type": "session.updated", "session": payload.get("session", {})}))
                elif message_type == "response.create":
                    if path == "/underrun":
                        await self.send_underrun_response(websocket)
                    elif path.startswith("/suppress-log"):
                        await self.send_error_response(websocket, payload)
                    elif path == "/invalid-delta-after-done":
                        await self.send_invalid_delta_after_done_response(websocket)
                    elif path == "/invalid-audio-base64-boundary":
                        await self.send_invalid_audio_base64_boundary_response(websocket)
                    elif path == "/invalid-inbound-json":
                        await self.send_invalid_inbound_json(websocket)
                    elif path == "/rejected-messages":
                        await self.send_rejected_messages(websocket)
                    elif path == "/barge-in":
                        await self.send_barge_in_response(websocket)
                    elif path in {"/reconnect-during-playback", "/terminal-close-during-playback"}:
                        await self.send_queued_playback_response(websocket, done=path == "/reconnect-during-playback")
                    elif path == "/flow-control":
                        await self.send_flow_control_response(websocket)
                    elif path == "/debug-audio":
                        await self.send_debug_audio_response(websocket)
                    elif path == "/duplicate-audio-delta":
                        await self.send_duplicate_audio_delta_response(websocket)
                    elif path == "/raw-audio":
                        await self.send_raw_audio_response(websocket)
                    elif path == "/raw-audio-debug":
                        await self.send_raw_audio_debug_response(websocket)
                    elif path == "/raw-audio-boundary":
                        await self.send_raw_audio_boundary_response(websocket)
                    elif path == "/raw-audio-oversized-boundary":
                        await self.send_raw_audio_oversized_boundary_response(websocket)
                    elif path == "/no-response-id":
                        await self.send_audio_response(websocket, response_id=None)
                    else:
                        await self.send_audio_response(websocket)
                elif message_type == "integration.reused_response_id":
                    await self.send_reused_response_id_audio(websocket)
        finally:
            await self.record("disconnected", path=path)

    async def send_rejected_messages(self, websocket):
        oversized = "x" * (MAX_WS_MESSAGE_BYTES + 1)
        for _ in range(3):
            await websocket.send(b"\x00\x00")
            await websocket.send(oversized)
        await websocket.send(json.dumps({"type": "integration.rejections_complete"}))
        await self.record("rejected-messages-sent")

    async def send_audio_delta(self, websocket, response_id, audio):
        payload = {"type": "response.output_audio.delta", "delta": base64.b64encode(audio).decode("ascii")}
        if response_id is not None:
            payload["response_id"] = response_id
        await websocket.send(json.dumps(payload))

    async def send_audio_response(self, websocket, response_id="integration-response"):
        sample_rate = 24000
        frequency = 1000
        duration_seconds = 0.6
        audio = pcm16_tone(sample_rate, frequency, duration_seconds)
        chunk_sizes = (137, 521, 1003, 269, 1607)
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_started"}))
        offset = 0
        chunk_index = 0
        while offset < len(audio):
            chunk_size = chunk_sizes[chunk_index % len(chunk_sizes)] * 2
            chunk = audio[offset : offset + chunk_size]
            await self.send_audio_delta(websocket, response_id, chunk)
            offset += len(chunk)
            chunk_index += 1

        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_stopped"}))
        await self.record(
            "audio-response-sent",
            response_id=response_id,
            frequency=frequency,
            duration_seconds=duration_seconds,
        )

    async def send_error_response(self, websocket, request):
        marker = request.get("metadata", {}).get("marker", "missing-marker")
        await websocket.send(json.dumps({"type": "error", "error": {"message": marker}}))
        await self.record("error-response-sent", marker=marker)

    async def send_invalid_delta_after_done_response(self, websocket):
        sample_rate = 24000
        duration_seconds = 1.0
        response_id = "integration-invalid-delta-after-done"
        await self.send_audio_delta(
            websocket,
            response_id,
            pcm16_tone(sample_rate, frequency=1000, duration_seconds=duration_seconds),
        )
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await websocket.send(json.dumps({"type": "response.output_audio.delta", "response_id": response_id}))
        await self.record(
            "invalid-delta-after-done-sent",
            response_id=response_id,
            duration_seconds=duration_seconds,
        )

    async def send_invalid_audio_base64_boundary_response(self, websocket):
        sample_rate = 24000
        replacement_frequency = 700
        replacement_duration = 0.5
        response_id = "integration-invalid-audio-base64"

        await self.send_audio_delta(websocket, response_id, b"\x7f")
        # Mid-string padding was partially decoded by the legacy path. Combined with the
        # orphan byte above, it leaves a carry byte that shifts the replacement PCM stream.
        await websocket.send(
            json.dumps(
                {
                    "type": "response.output_audio.delta",
                    "response_id": response_id,
                    "delta": "AAAAAA=A",
                }
            )
        )
        await self.send_audio_delta(
            websocket,
            response_id,
            pcm16_tone(sample_rate, replacement_frequency, replacement_duration),
        )
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record(
            "invalid-audio-base64-boundary-sent",
            replacement_frequency=replacement_frequency,
            replacement_duration=replacement_duration,
        )

    async def send_invalid_inbound_json(self, websocket):
        audio = base64.b64encode(pcm16_tone(24000, frequency=1000, duration_seconds=0.04)).decode("ascii")
        delta = json.dumps(
            {
                "type": "response.output_audio.delta",
                "delta": audio,
                "marker": "trailing-inbound-json",
            }
        )
        await websocket.send(delta + "garbage")
        await websocket.send(delta + "\0garbage")
        await self.record("invalid-inbound-json-sent")

    async def send_queued_playback_response(self, websocket, done):
        sample_rate = 24000
        frequency = 1100
        duration_seconds = 3.0
        response_id = "integration-queued-playback-response"

        # The test closes the connection after playback starts, while audio is still queued.
        await self.send_audio_delta(
            websocket,
            response_id,
            pcm16_tone(sample_rate, frequency, duration_seconds),
        )
        if done:
            await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record(
            "queued-playback-response-sent",
            response_id=response_id,
            frequency=frequency,
            duration_seconds=duration_seconds,
        )

    async def send_reused_response_id_audio(self, websocket):
        sample_rate = 24000
        response_id = "reused-response-id"

        # Complete one response, interrupt playback, and reuse its ID for a later response.
        # The peer-provided ID must not suppress otherwise valid playback audio.
        prime_audio = pcm16_tone(sample_rate, frequency=700, duration_seconds=0.04)
        await self.send_audio_delta(websocket, response_id, prime_audio)
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_started"}))
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_stopped"}))

        frequency = 1400
        duration_seconds = 0.6
        replacement_audio = pcm16_tone(sample_rate, frequency, duration_seconds)
        await self.send_audio_delta(websocket, response_id, replacement_audio)
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record(
            "reused-response-id-audio-sent",
            response_id=response_id,
            frequency=frequency,
            duration_seconds=duration_seconds,
        )

    async def send_underrun_response(self, websocket):
        sample_rate = 24000
        frequency = 1000
        burst_duration = 0.005
        burst_interval = 0.05
        burst_count = 24
        response_id = "integration-underrun-response"
        samples_per_burst = int(sample_rate * burst_duration)

        for burst_index in range(burst_count):
            audio = pcm16_tone(
                sample_rate,
                frequency,
                burst_duration,
                start_sample=burst_index * samples_per_burst,
            )
            await self.send_audio_delta(websocket, response_id, audio)
            if burst_index + 1 < burst_count:
                await asyncio.sleep(burst_interval)

        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record(
            "underrun-response-sent",
            response_id=response_id,
            frequency=frequency,
            burst_count=burst_count,
            burst_duration=burst_duration,
            burst_interval=burst_interval,
        )

    async def send_flow_control_response(self, websocket):
        sample_rate = 24000
        frequencies = [700, 1100, 1500]
        segment_duration = 0.5
        response_id = "integration-flow-control-response"
        await self.send_audio_delta(
            websocket,
            response_id,
            b"".join(pcm16_tone(sample_rate, frequency, segment_duration) for frequency in frequencies),
        )
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record(
            "flow-control-response-sent",
            response_id=response_id,
            frequencies=frequencies,
            segment_duration=segment_duration,
        )

    async def send_barge_in_response(self, websocket):
        sample_rate = 24000
        interrupted_frequency = 700
        replacement_frequency = 1400
        interrupted_response_id = "integration-interrupted-response"
        replacement_response_id = "integration-replacement-response"

        # At 8 kHz, three seconds exceed the 16384-sample queue chunk: barge-in must
        # clear both the C++ queue and the FreeSWITCH playback buffer.
        await self.send_audio_delta(
            websocket,
            interrupted_response_id,
            pcm16_tone(sample_rate, interrupted_frequency, 3.0),
        )
        await asyncio.sleep(0.25)
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_started"}))
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": interrupted_response_id}))

        # The clear must discard the buffered audio without suppressing subsequent playback.
        await self.send_audio_delta(
            websocket,
            replacement_response_id,
            pcm16_tone(sample_rate, replacement_frequency, 0.5),
        )
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": replacement_response_id}))
        await self.record(
            "barge-in-response-sent",
            interrupted_frequency=interrupted_frequency,
            replacement_frequency=replacement_frequency,
        )

    async def send_debug_audio_response(self, websocket):
        sample_rate = 24000
        response_id = "integration-debug-audio-response"
        audio = pcm16_tone(sample_rate, 1000, 0.1)
        # Split one PCM16 sample across deltas so debug output must follow decoder alignment.
        await self.send_audio_delta(websocket, response_id, audio[:1])
        await self.send_audio_delta(websocket, response_id, audio[1:])
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record("debug-audio-response-sent", sample_rate=sample_rate, byte_count=len(audio))

    async def send_duplicate_audio_delta_response(self, websocket):
        audio = pcm16_tone(24000, 1000, 0.1)
        rejected_message = None
        for response_id, encoded in (
            ("duplicate-fragment", base64.b64encode(audio[:1]).decode("ascii")),
            ("duplicate-audio", base64.b64encode(audio[1:]).decode("ascii")),
            ("duplicate-invalid", "A"),
        ):
            payload = json.dumps({"type": "response.output_audio.delta", "response_id": response_id, "delta": encoded})
            # A dict cannot represent repeated keys; exercise both exact and case-insensitive duplicates.
            message = payload[:-1] + ',"delta":"duplicate-payload-marker","Delta":"case-duplicate-marker"}'
            await websocket.send(message)
            if response_id == "duplicate-invalid":
                rejected_message = message
        await websocket.send(json.dumps({"type": "response.output_audio.done"}))
        await self.record("duplicate-audio-delta-sent", rejected_message=rejected_message, byte_count=len(audio))

    async def send_raw_audio_response(self, websocket):
        sample_rate = 8000
        split_frequency = 700
        regular_frequency = 1400
        split_duration = 0.12
        regular_duration = 0.5
        split_audio = pcm16_tone(sample_rate, split_frequency, split_duration)
        regular_audio = pcm16_tone(sample_rate, regular_frequency, regular_duration)

        # Splitting every PCM16 sample across two one-byte WebSocket frames makes the
        # carry-byte contract observable: dropping odd trailing bytes removes this tone.
        for byte in split_audio:
            await websocket.send(bytes((byte,)))
        await websocket.send(regular_audio)

        await websocket.send(json.dumps({"type": "response.output_audio.done"}))
        await self.record(
            "raw-audio-response-sent",
            split_frequency=split_frequency,
            regular_frequency=regular_frequency,
            split_duration=split_duration,
            regular_duration=regular_duration,
            split_frame_count=len(split_audio),
        )

    async def send_raw_audio_boundary_response(self, websocket):
        sample_rate = 24000
        stale_frequency = 1800
        replacement_frequency = 700
        replacement_duration = 0.5

        # The orphan byte belongs to the PCM stream interrupted by speech_started. Carrying it
        # into the next stream shifts every following PCM16 sample by one byte.
        stale_audio = pcm16_tone(sample_rate, stale_frequency, 0.08) + b"\x7f"
        await websocket.send(stale_audio)
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_started"}))
        await websocket.send(pcm16_tone(sample_rate, replacement_frequency, replacement_duration))
        # Deliberately omit response_id: stream-boundary handling must not depend on metadata.
        await websocket.send(json.dumps({"type": "response.output_audio.done"}))
        await self.record(
            "raw-audio-boundary-response-sent",
            stale_frequency=stale_frequency,
            replacement_frequency=replacement_frequency,
            replacement_duration=replacement_duration,
        )

    async def send_raw_audio_oversized_boundary_response(self, websocket):
        sample_rate = 24000
        replacement_frequency = 700
        replacement_duration = 0.5

        await websocket.send(b"\x7f")
        await websocket.send(bytes(MAX_WS_MESSAGE_BYTES + 1))
        await websocket.send(pcm16_tone(sample_rate, replacement_frequency, replacement_duration))
        await websocket.send(json.dumps({"type": "response.output_audio.done"}))
        await self.record(
            "raw-audio-oversized-boundary-response-sent",
            replacement_frequency=replacement_frequency,
            replacement_duration=replacement_duration,
        )

    async def send_raw_audio_debug_response(self, websocket):
        sample_rate = 8000
        audio = pcm16_tone(sample_rate, frequency=600, duration_seconds=0.04)
        await websocket.send(audio[:1])
        await websocket.send(audio[1:])
        await websocket.send(json.dumps({"type": "response.output_audio.done"}))
        await self.record(
            "raw-audio-debug-response-sent",
            sample_rate=sample_rate,
            byte_count=len(audio),
        )

    async def run(self, host, port):
        self.event_log.unlink(missing_ok=True)
        self.ready_file.unlink(missing_ok=True)
        self.reconnect_gate.unlink(missing_ok=True)
        async with serve(
            self.handle,
            host,
            port,
            max_size=16 * 1024 * 1024,
            process_request=self.process_request,
        ):
            self.ready_file.touch()
            await asyncio.Future()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--event-log", type=Path, required=True)
    parser.add_argument("--ready-file", type=Path, required=True)
    args = parser.parse_args()

    server = MockRealtimeServer(args.event_log, args.ready_file)
    asyncio.run(server.run(args.host, args.port))


if __name__ == "__main__":
    main()
