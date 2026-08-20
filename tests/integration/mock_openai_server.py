#!/usr/bin/env python3
import argparse
import asyncio
import base64
import json
import math
import struct
from pathlib import Path

import websockets


def pcm16_tone(sample_rate, frequency, duration_seconds, amplitude=12000):
    sample_count = int(sample_rate * duration_seconds)
    samples = (
        int(amplitude * math.sin(2 * math.pi * frequency * index / sample_rate))
        for index in range(sample_count)
    )
    return struct.pack(f"<{sample_count}h", *samples)


class MockRealtimeServer:
    def __init__(self, event_log: Path, ready_file: Path):
        self.event_log = event_log
        self.ready_file = ready_file
        self._lock = asyncio.Lock()

    async def record(self, event, **fields):
        payload = {"event": event, **fields}
        async with self._lock:
            with self.event_log.open("a", encoding="utf-8") as log:
                log.write(json.dumps(payload, sort_keys=True) + "\n")

    async def handle(self, websocket, path=None):
        path = path or getattr(websocket, "path", "/")
        await self.record("connected", path=path)

        try:
            if path == "/close-immediately":
                await websocket.close(code=1011, reason="intentional integration-test close")
                await self.record("closed", path=path)
                return

            # Exercise the startup ordering: the peer is allowed to send a message as soon as
            # the WebSocket opens, before the first caller audio frame reaches the module.
            await websocket.send(json.dumps({"type": "session.updated", "session": {"id": "mock-session"}}))

            async for message in websocket:
                if isinstance(message, bytes):
                    await self.record("binary", size=len(message))
                    continue

                try:
                    payload = json.loads(message)
                except json.JSONDecodeError:
                    await self.record("invalid-json")
                    continue

                message_type = payload.get("type", "")
                if message_type == "input_audio_buffer.append":
                    try:
                        audio = base64.b64decode(payload.get("audio", ""), validate=True)
                    except (ValueError, TypeError):
                        await self.record("invalid-audio-message")
                    else:
                        await self.record(
                            "audio-received",
                            size=len(audio),
                            sample_aligned=len(audio) % 2 == 0,
                        )
                else:
                    await self.record("message", type=message_type, payload=payload)

                if message_type == "session.update":
                    await websocket.send(json.dumps({"type": "session.updated", "session": payload.get("session", {})}))
                elif message_type == "response.create":
                    await self.send_audio_response(websocket)
                elif message_type == "integration.reused_response_id":
                    await self.send_reused_response_id_audio(websocket)
        finally:
            await self.record("disconnected", path=path)

    async def send_audio_response(self, websocket):
        sample_rate = 24000
        frequency = 1000
        duration_seconds = 0.6
        audio = pcm16_tone(sample_rate, frequency, duration_seconds)
        chunk_sizes = (137, 521, 1003, 269, 1607)
        response_id = "integration-response"

        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_started"}))
        offset = 0
        chunk_index = 0
        while offset < len(audio):
            chunk_size = chunk_sizes[chunk_index % len(chunk_sizes)] * 2
            chunk = audio[offset : offset + chunk_size]
            await websocket.send(
                json.dumps(
                    {
                        "type": "response.output_audio.delta",
                        "response_id": response_id,
                        "delta": base64.b64encode(chunk).decode("ascii"),
                    }
                )
            )
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

    async def send_reused_response_id_audio(self, websocket):
        sample_rate = 24000
        response_id = "reused-response-id"

        # Complete one response, interrupt playback, and reuse its ID for a later response.
        # The peer-provided ID must not suppress otherwise valid playback audio.
        prime_audio = pcm16_tone(sample_rate, frequency=700, duration_seconds=0.04)
        await websocket.send(
            json.dumps(
                {
                    "type": "response.output_audio.delta",
                    "response_id": response_id,
                    "delta": base64.b64encode(prime_audio).decode("ascii"),
                }
            )
        )
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_started"}))
        await websocket.send(json.dumps({"type": "input_audio_buffer.speech_stopped"}))

        frequency = 1400
        duration_seconds = 0.6
        replacement_audio = pcm16_tone(sample_rate, frequency, duration_seconds)
        await websocket.send(
            json.dumps(
                {
                    "type": "response.output_audio.delta",
                    "response_id": response_id,
                    "delta": base64.b64encode(replacement_audio).decode("ascii"),
                }
            )
        )
        await websocket.send(json.dumps({"type": "response.output_audio.done", "response_id": response_id}))
        await self.record(
            "reused-response-id-audio-sent",
            response_id=response_id,
            frequency=frequency,
            duration_seconds=duration_seconds,
        )

    async def run(self, host, port):
        self.event_log.unlink(missing_ok=True)
        self.ready_file.unlink(missing_ok=True)
        async with websockets.serve(self.handle, host, port, max_size=16 * 1024 * 1024):
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
