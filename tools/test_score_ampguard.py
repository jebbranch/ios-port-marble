#!/usr/bin/env python3
"""Synthetic PA-guard controls; never accesses a device."""
import importlib.util
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('score_ampguard', HERE / 'score_ampguard.py')
score = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(score)


def text(powerdown=False, nack=False):
    lines = ['SWEET MARK: ampguard v1 pass=1 skip=1 init=0 profiles=0 rstA=0 rstB=0',
             'SWEET MARK: bitbang skipped',
             'SWEET MARK: ampguard v1 pass=2 skip=1 init=1 profiles=2 rstA=1 rstB=1',
             'SWEET MARK: bitbang skipped']
    for addr in (0x34, 0x35):
        lines += [f'SWEET I2C: geni amp {addr:#x} r=0 id=0x2032',
                  f'SWEET I2C: geni amp {addr:#x} profile 44/44']
        values = {0: 0x2032, 3: 0xffff, 4: 0x4003 if powerdown else 0xb440,
                  5: 0x38 if powerdown else 0x05a9}
        for reg, value in values.items():
            status = -2 if nack and addr == 0x35 and reg == 4 else 0
            if status:
                value = 0xa5a5
            lines.append(f'SWEET MARK: ampread post-init addr={addr:#x} reg={reg:#x} r={status} val={value:#x}')
    return '\n'.join(lines)


class GuardTests(unittest.TestCase):
    def test_positive(self):
        result = score.amp_gate(text())
        self.assertTrue(result['amp_guard_valid'])
        self.assertTrue(result['power_mute_bits_clear'])

    def test_powerdown_is_readable_but_not_working(self):
        result = score.amp_gate(text(powerdown=True))
        self.assertTrue(result['amp_guard_valid'])
        self.assertFalse(result['power_mute_bits_clear'])
        self.assertEqual(result['post_init_states'][0]['pwdn'], 1)
        self.assertEqual(result['post_init_states'][0]['hmute'], 1)

    def test_nack_is_unknown_not_a_measurement(self):
        result = score.amp_gate(text(nack=True))
        self.assertFalse(result['amp_guard_valid'])
        self.assertFalse(result['post_init_states'][1]['known'])
        self.assertNotIn('pwdn', result['post_init_states'][1])

    def test_old_probe_does_not_pass(self):
        result = score.amp_gate('SWEET I2C: geni amp 0x34 profile 44/44')
        self.assertFalse(result['amp_guard_valid'])

    def test_scan_invalidates_default_path(self):
        result = score.amp_gate(text() + '\nSWEET I2C: released scan found 0 device(s)')
        self.assertFalse(result['amp_guard_valid'])

    def test_low_reset_invalidates_late_guard(self):
        result = score.amp_gate(text().replace('profiles=2 rstA=1 rstB=1', 'profiles=2 rstA=0 rstB=0'))
        self.assertFalse(result['amp_guard_valid'])

    def test_incomplete_readback(self):
        result = score.amp_gate(text().replace('reg=0x5', 'reg=0x6'))
        self.assertFalse(result['amp_guard_valid'])


if __name__ == '__main__':
    unittest.main()
