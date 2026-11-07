#!/usr/bin/env python3
"""Synthetic validity/failure controls for the descsep scorer; no device use."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('score_descsep', HERE / 'score_descsep.py')
score = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(score)


def command(value):
    flags = dict(score.FLAGS, sweetdescsep=str(value), sweettime='1794047000')
    return 'debug=0xc serial=3 ' + ' '.join(f'{k}={v}' for k, v in flags.items())


def image(cmd):
    data = bytearray(4096)
    struct.pack_into('<8sII', data, 0, b'SWEETXNU', 1, 1)
    data[0x48:0x48 + len(cmd)] = cmd.encode()
    return bytes(data) + b'\0'.join(score.MARKERS)


def ring_text(value):
    parts = ['boot-args: sweettime=1794047000',
        'SWEET MARK: dcnv v2 on=1 bytes=856 start=0xb0000002',
        'SWEET GLINK: APM graph_open/dec-cnv src=0x2010 (shm handle=0x1 len=856)',
        'SWEET GLINK: graph_open/dec-cnv rsp op=0x1001000 status=0',
        'SWEET GLINK: graph_start rsp op=0x1001002 status=0',
        'SWEET GLINK: dec-cnv started',
        'SWEET MARK: rxall v1 op=0x2001005 src=0x1',
        'SWEET MARK: rxall v1 op=0x2001001 src=0x1',
        'SWEET MARK: tone gain g=16 peak=16384',
        'pcm tone done (120 buffers)',
        'dblat v2 tone n=120 min=5 avg=5 max=5 ms timeout=0 step=5',
        'SWEET MARK: eos v2 on=0 dst=0x40bd policy=1',
        '===== done =====', 'PSCI SYSTEM_RESET']
    parts += ['SWEET MARK: rxall v1 op=0x5001004 src=0x40bd'] * 121
    if value:
        parts += ['SWEET MARK: descsep v1 on=1 off=0x4000 len=856']
    return ('\n'.join(parts) + '\n').encode()


class Gates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='descsep-score-test-')
        self.root = Path(self.temp.name)
        self.old_root = score.ROOT
        score.ROOT = self.root
        (self.root / 'logs/raw').mkdir(parents=True)

    def tearDown(self):
        score.ROOT = self.old_root
        self.temp.cleanup()

    def image_result(self, data, value):
        path = self.root / 'fixture.img'
        path.write_bytes(data)
        return score.image_gate(path, value, None)

    def ring_result(self, text, value, timestamp='1794047000'):
        size = 16384
        self.assertLess(len(text), size)
        raw = struct.pack('<8sIIQQ', b'SWEETLOG', 1, size, len(text), 1)
        raw = raw.ljust(4096, b'\0') + text.ljust(size, b'\0')
        (self.root / 'logs/raw/ring-fixture.bin').write_bytes(raw)
        (self.root / 'logs/ring-fixture.txt').write_bytes(text)
        return score.ring_gate('fixture', timestamp, value)

    def test_image_positive(self):
        self.assertTrue(self.image_result(image(command(1)), 1)['image_valid'])

    def test_wrong_value(self):
        self.assertFalse(self.image_result(image(command(0)), 1)['image_valid'])

    def test_duplicate_timestamp(self):
        self.assertFalse(self.image_result(image(command(1) + ' sweettime=1794047001'), 1)['image_valid'])

    def test_missing_marker(self):
        data = image(command(1)).replace(score.MARKERS[0], b'not-the-required-marker')
        self.assertFalse(self.image_result(data, 1)['image_valid'])

    def test_no_payload(self):
        with self.assertRaises(ValueError):
            score.payload_cmdline(b'not a payload')

    def test_both_ring_positive_controls(self):
        for value in (0, 1):
            self.assertTrue(self.ring_result(ring_text(value), value)['ring_valid'])

    def test_stale_ring(self):
        self.assertFalse(self.ring_result(ring_text(1), 1, '1794047001')['ring_valid'])

    def test_fallback_invalid(self):
        text = ring_text(1) + b'SWEET GLINK: APM graph_open/vp-wr src=0x2010\n'
        self.assertFalse(self.ring_result(text, 1)['ring_valid'])

    def test_missing_witness_invalid(self):
        self.assertFalse(self.ring_result(ring_text(0), 1)['ring_valid'])

    def test_eos_not_a_control(self):
        text = ring_text(0) + b'SWEET GLINK: APM wr_eos/last src=0x2010\n'
        self.assertFalse(self.ring_result(text, 0)['ring_valid'])


if __name__ == '__main__':
    unittest.main()
