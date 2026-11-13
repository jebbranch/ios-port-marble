#!/usr/bin/env python3
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from score_halstream import score

RAW = Path(__file__).resolve().parent.parent / "logs/raw/ring-halstream-boot70.bin"
TEXT = Path(__file__).resolve().parent.parent / "logs/ring-halstream-boot70.txt"


class ScoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = RAW.read_bytes()
        cls.text = TEXT.read_bytes()

    def test_positive(self):
        self.assertTrue(score(self.raw, self.text, 1794581323)["passed"])

    def test_old_identity(self):
        self.assertFalse(score(self.raw, self.text, 1)["passed"])

    def test_text_mismatch(self):
        with self.assertRaises(ValueError):
            score(self.raw, self.text + b"x", 1794581323)

    def test_missing_done(self):
        changed = self.text.replace(
            b"attempted=30 sent=30 done=30 reject=0",
            b"attempted=30 sent=30 done=29 reject=0")
        raw = bytearray(self.raw)
        raw[4096:4096 + len(changed)] = changed
        self.assertFalse(score(bytes(raw), changed, 1794581323)["passed"])

    def test_amp_write_rejected(self):
        needle = b"SWEET ADSP: early amp probe"
        replacement = b"SWEET MARK: ampwrite" + b" " * (len(needle) - 20)
        changed = self.text.replace(needle, replacement, 1)
        raw = bytearray(self.raw)
        raw[4096:4096 + len(changed)] = changed
        self.assertFalse(score(bytes(raw), changed, 1794581323)["passed"])


if __name__ == "__main__":
    unittest.main()
