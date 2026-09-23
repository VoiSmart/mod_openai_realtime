import json
import unittest
from unittest.mock import patch

from test_module import mock_events


class MockEventsTest(unittest.TestCase):
    def test_incomplete_record_is_read_only_after_its_newline(self):
        first = '{"event":"first"}\n'
        second = '{"event":"second"}'
        with patch("test_module.EVENT_LOG") as log:
            log.exists.return_value = True
            for suffix in (second[:12], second):
                log.read_text.return_value = first + suffix
                self.assertEqual(mock_events(), [{"event": "first"}])
            log.read_text.return_value = first + second + "\n"
            self.assertEqual(mock_events(), [{"event": "first"}, {"event": "second"}])

    def test_malformed_complete_record_is_not_ignored(self):
        with patch("test_module.EVENT_LOG") as log:
            log.exists.return_value = True
            log.read_text.return_value = '{"event":invalid}\n'
            with self.assertRaises(json.JSONDecodeError):
                mock_events()


if __name__ == "__main__":
    unittest.main()
