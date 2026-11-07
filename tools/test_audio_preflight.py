#!/usr/bin/env python3
"""Regression tests for the offline audit, not a device-ready gate.

    python3 -I tools/test_audio_preflight.py
"""
import importlib.util
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('audio_preflight', HERE / 'audio_preflight.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)
SOURCE = HERE.parent.parent / 'chain-tree/bsd/dev/sweet_glink.c'


class ParsingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()

    def test_real_bodies(self):
        h, _ = audit.array(self.source, 'g_graph_vp_wr_dec_cnv')
        b, _ = audit.array(self.source, 'g_graph_aform_no_aw')
        hb, hm = audit.aform(h)
        bb, bm = audit.aform(b)
        self.assertEqual((len(h), len(b)), (856, 808))
        self.assertEqual(set(hm) - set(bm), {(0x10013d01, 0x42d3)})
        for pid in (0x08001000, 0x08001001):
            self.assertEqual(hb[pid], bb[pid])

    def test_truncated_body_fails(self):
        h, _ = audit.array(self.source, 'g_graph_vp_wr_dec_cnv')
        with self.assertRaises(ValueError):
            audit.aform(h[:-1])

    def test_acf_is_not_aform(self):
        b, _ = audit.array(self.source, 'g_graph_ukee_current')
        with self.assertRaises(ValueError):
            audit.aform(b)

    def test_missing_function_fails(self):
        with self.assertRaises(ValueError):
            audit.function(self.source, 'missing_probe')

    def test_comments_do_not_close_function(self):
        fragment = 'static int\nexample(void)\n{ /* } */ const char *s = "}"; return 0; }'
        self.assertEqual(audit.function(fragment, 'example'), fragment)


class HostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = audit.audit(SOURCE, HERE.parent / 'out/kernel.adsp', None)

    def test_timestamp_budget(self):
        self.assertEqual(self.result['cmdline_bytes_auto_timestamp'], 578)
        self.assertEqual(self.result['cmdline_bytes_duplicate_timestamp'], 599)

    def test_memory_positive_and_negative_controls(self):
        cases = [r for r in self.result['cases'] if r['kind'] == 'memory']
        self.assertEqual(len(cases), 3)
        for row in cases:
            self.assertEqual(row['total_buffers'], 121)
            if row['offset'] == 0:
                self.assertEqual(row['pcm_a_changed'], 1)
                self.assertEqual(row['descriptor_buffers'], 61)
            else:
                self.assertEqual(row['offset'], 0x4000)
                self.assertEqual(row['pcm_a_changed'], 0)
                self.assertEqual(row['descriptor_buffers'], 0)

    def test_converter_configs_are_gated(self):
        rows = [r for r in self.result['cases'] if r['kind'] == 'cfg']
        baseline, old_flag = rows
        self.assertEqual((baseline['dec'], baseline['cnv']), (0, 0))
        self.assertEqual((old_flag['dec'], old_flag['cnv'], old_flag['absent']), (1, 1, 2))

    def test_b1_missing_module_and_fallthrough(self):
        rows = [r for r in self.result['cases'] if r['kind'] == 'b1']
        accepted, refused = rows
        self.assertEqual((accepted['aw'], accepted['absent'], accepted['fallback']), (2, 2, 0))
        self.assertEqual((refused['aw'], refused['fallback']), (0, 1))

    def test_actual_eos_packet_bytes(self):
        eos, = [r for r in self.result['cases'] if r['kind'] == 'eos']
        self.assertEqual(eos['packet_bytes'], 44)
        self.assertEqual(eos['opcode'], 0x04001002)
        self.assertEqual(eos['payload_words'], [0, 0, 0, 4, 1])

    def test_descriptor_separation_and_guards(self):
        cases = [r for r in self.result['cases'] if r['kind'] == 'separation']
        self.assertEqual(len(cases), 4)
        for row in cases:
            self.assertEqual(row['pcm_a_changed'], 0)
            if row['nowait'] == 0 and row['dbb'] == 0 and row['size'] == 856:
                self.assertEqual((row['rc'], row['packet_offset']), (0, 0x4000))
            else:
                self.assertEqual((row['rc'], row['packet_offset']), (-1, 0))

    def test_audit_is_not_device_approval(self):
        self.assertIs(self.result['ready_to_boot'], False)


if __name__ == '__main__':
    unittest.main()
