#!/usr/bin/env python3
"""Positive and negative evidence tests for the PCIe identity scorer."""
import struct
import unittest
from score_wifi_pcie import score


TEXT = """sweettime=1795000000
SWEET MARBLE TOUCH: disabled (marble_touch=0)
SWEET ADSP: disabled (sweetadsp=1)
SWEET MARK: wifi handoff v1 readonly=1 stage=2 gdsc=0x22f001 halted=0x17f wlan=0 perst=0
SWEET WIFI: phase2 milestone=1
SWEET MARK: wifi pcie v2 stage=2 id=0x110317cb subsystem=0x10817cb bar=0x60400004 bytes=0x200000 soc=0x400c1211 sub=0x1016d0e1
"""


def ring(text):
    payload = text.encode()
    raw = bytearray(4096 + 8192)
    raw[:8] = b"SWEETLOG"
    struct.pack_into("<IIQQ", raw, 8, 1, 8192, len(payload), 10)
    raw[4096:4096 + len(payload)] = payload
    return bytes(raw)


class ScorerTest(unittest.TestCase):
    def test_success(self):
        self.assertTrue(score(ring(TEXT), 1795000000)["passed"])

    def test_stale(self):
        self.assertFalse(score(ring(TEXT), 1795000001)["passed"])

    def test_wrong_id(self):
        self.assertFalse(score(ring(TEXT.replace("0x110317cb", "0x11031234")),
                               1795000000)["passed"])

    def test_duplicate(self):
        line = TEXT.splitlines()[-1] + "\n"
        self.assertFalse(score(ring(TEXT + line), 1795000000)["passed"])

    def test_failure(self):
        text = "\n".join(TEXT.splitlines()[:-1]) + (
            "\nSWEET WIFI: phase 2 failed errno=60 poisoned=1 milestone=0\n")
        result = score(ring(text), 1795000000)
        self.assertFalse(result["passed"])
        self.assertEqual(result["classification"], "bounded phase2 failure")

    def test_panicked(self):
        result = score(ring(TEXT + "panic(cpu 0)\n"), 1795000000)
        self.assertFalse(result["passed"])

    def test_bad_bar(self):
        self.assertFalse(score(ring(TEXT.replace("bytes=0x200000", "bytes=0x180000")),
                               1795000000)["passed"])

    def test_wrapped(self):
        raw = bytearray(ring(TEXT))
        struct.pack_into("<Q", raw, 16, 8193)
        with self.assertRaises(ValueError):
            score(bytes(raw), 1795000000)


if __name__ == "__main__":
    unittest.main()
