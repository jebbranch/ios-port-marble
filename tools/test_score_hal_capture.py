#!/usr/bin/env python3
import struct
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from score_hal_capture import MARKER, score

GOOD = (
    b"boot-args sweettime=1794493381\n" + MARKER +
    b"\nSWEET ADSP: disabled\nSWEET HAL: open=1\n"
    b"SWEET HAL: pcm write=1 frames=1024 sound=0 peak=0 queued=1024 overwritten=0\n"
    b"SWEET HAL: pcm write=4 frames=4096 sound=1000 peak=32768 queued=4096 overwritten=0\n"
)


def raw(text, tail=b""):
    data = bytearray(4096 + 4096)
    data[:8] = b"SWEETLOG"
    struct.pack_into("<IIQQ", data, 8, 1, 4096, len(text), 1)
    data[4096:4096 + len(text)] = text
    data[4096 + len(text):4096 + len(text) + len(tail)] = tail
    return bytes(data)


class ScoreTests(unittest.TestCase):
    def test_positive(self):
        self.assertTrue(score(raw(GOOD), 1794493381)["capture_gates_passed"])

    def test_old_identity(self):
        self.assertFalse(score(raw(GOOD), 1)["capture_gates_passed"])

    def test_stale_tail_cannot_supply_marker(self):
        text = GOOD.replace(MARKER, b"")
        result = score(raw(text, MARKER), 1794493381)
        self.assertFalse(result["gates"]["marker"])
        self.assertEqual(result["past_window_marker_count"], 1)

    def test_hardware(self):
        self.assertFalse(score(raw(GOOD + b"SWEET MARK: ampwrite"), 1794493381)["capture_gates_passed"])

    def test_silent(self):
        text = GOOD.replace(b"sound=1000 peak=32768", b"sound=0 peak=0")
        self.assertFalse(score(raw(text), 1794493381)["capture_gates_passed"])

    def test_only_open(self):
        self.assertFalse(score(raw(GOOD.split(b"SWEET HAL: pcm")[0]), 1794493381)["capture_gates_passed"])

    def test_count_overflow(self):
        text = GOOD.replace(b"queued=4096", b"queued=99999")
        self.assertFalse(score(raw(text), 1794493381)["capture_gates_passed"])

    def test_wrapped_identity(self):
        data = bytearray(raw(GOOD))
        struct.pack_into("<Q", data, 16, 4097)
        with self.assertRaises(ValueError):
            score(data, 1794493381)

    def test_truncated(self):
        with self.assertRaises(ValueError):
            score(raw(GOOD)[:-1], 1794493381)


if __name__ == "__main__":
    unittest.main()
