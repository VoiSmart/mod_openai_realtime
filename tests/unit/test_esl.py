"""Exercise wire framing without requiring a running FreeSWITCH process."""

import json
import socket
import unittest
from unittest.mock import Mock, patch

from esl import SPEECH_START_EVENT, FreeSwitchEventSocket


class EventSocketTest(unittest.TestCase):
    def setUp(self):
        client_socket, self.peer = socket.socketpair()
        self.addCleanup(self.peer.close)
        self.addCleanup(client_socket.close)
        self.peer.sendall(b"Content-Type: auth/request\n\n" + b"Content-Type: command/reply\nReply-Text: +OK\n\n" * 2)
        with patch("esl.socket.create_connection", return_value=client_socket):
            self.client = FreeSwitchEventSocket("test-call")

    @staticmethod
    def packet(uuid="test-call", newline=b"\n"):
        body = json.dumps({"Unique-ID": uuid, "Event-Subclass": SPEECH_START_EVENT}).encode()
        headers = newline.join([b"Content-Type: text/event-json", f"Content-Length: {len(body)}".encode(), b"", b""])
        return headers + body

    def test_timeout_preserves_partial_headers_and_body(self):
        packet = self.packet()
        for split in (12, len(packet) - 5):
            with self.subTest(split=split):
                self.peer.sendall(packet[:split])
                self.assertIsNone(self.client.wait_for(SPEECH_START_EVENT, timeout=0.01))
                self.peer.sendall(packet[split:])
                event = self.client.wait_for(SPEECH_START_EVENT, timeout=1)
                self.assertIsNotNone(event)
                self.assertEqual(event["Unique-ID"], "test-call")

    def test_coalesced_packets_and_both_header_delimiters(self):
        self.peer.sendall(self.packet(newline=b"\r\n") + self.packet())
        for _ in range(2):
            self.assertIsNotNone(self.client.wait_for(SPEECH_START_EVENT, timeout=1))

    def test_filters_events_for_other_calls(self):
        self.peer.sendall(self.packet("other-call") + self.packet())
        event = self.client.wait_for(SPEECH_START_EVENT, timeout=1)
        self.assertEqual(event["Unique-ID"], "test-call")
        self.assertEqual(len(self.client.seen_events), 2)

    def test_channel_destroy_is_matched_by_event_name(self):
        body = json.dumps({"Unique-ID": "test-call", "Event-Name": "CHANNEL_DESTROY"}).encode()
        self.peer.sendall(
            self.packet() + f"Content-Type: text/event-json\nContent-Length: {len(body)}\n\n".encode() + body
        )
        event = self.client.wait_for("CHANNEL_DESTROY", timeout=1)
        self.assertIsNotNone(event)
        self.assertEqual(event["Event-Name"], "CHANNEL_DESTROY")
        self.assertEqual(len(self.client.seen_events), 2)

    def test_packet_deadline_is_not_restarted_for_each_fragment(self):
        fragmented_socket = Mock()
        fragmented_socket.recv.side_effect = [b"Content-Type: text/", b"event-json\n"]
        with (
            patch.object(self.client, "_socket", fragmented_socket),
            patch("esl.time.monotonic", side_effect=[0, 0, 0.6, 1.2]),
        ):
            with self.assertRaises(socket.timeout):
                self.client._receive_packet(1)
        self.assertEqual(fragmented_socket.recv.call_count, 2)
        self.assertAlmostEqual(fragmented_socket.settimeout.call_args.args[0], 0.4)

    def test_closed_socket_is_reported(self):
        self.peer.recv(4096)  # Consume the authentication/subscription commands before closing.
        self.peer.close()
        with self.assertRaisesRegex(RuntimeError, "closed unexpectedly"):
            self.client.wait_for(SPEECH_START_EVENT, timeout=1)


if __name__ == "__main__":
    unittest.main()
