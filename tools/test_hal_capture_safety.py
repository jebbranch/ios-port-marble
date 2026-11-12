#!/usr/bin/env python3
"""Offline failure controls for capture flags and HFS install/restore."""
import argparse
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
os.environ["ADB"] = "0"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load("hal_capture_builder", ROOT / "tools/build_hal_capture.py")
installer = load("marble_audio_installer",
                 ROOT / "tools/install_marble_audio.py")


class SafetyTests(unittest.TestCase):
    def test_installer_snapshot_matches_compatibility_path(self):
        self.assertEqual(
            (ROOT / "tools/install_marble_audio.py").read_bytes(),
            (ROOT.parent / "iphone-x-recon/hfs/install_marble_audio.py").read_bytes())

    def test_capture_flags_positive(self):
        builder.check_flags(builder.ARGS + " sweettime=1790000000")

    def test_duplicate_flags_rejected(self):
        with self.assertRaises(AssertionError):
            builder.check_flags(builder.ARGS + " sweetadsp=1")

    def test_hardware_flags_rejected(self):
        for token in ("sweetpcm=1", "sweettdmclk=1", "sweetampfirst=1",
                      "sweethap=1", "sweetgmuboot=1", "sweetreboot=1"):
            with self.subTest(token=token), self.assertRaises(AssertionError):
                builder.check_flags(builder.ARGS + " " + token)

    def test_adsp_enabled_rejected(self):
        with self.assertRaises(AssertionError):
            builder.check_flags(builder.ARGS.replace("sweetadsp=0", "sweetadsp=1"))

    def test_long_cmdline_rejected(self):
        with self.assertRaises(AssertionError):
            builder.check_flags(builder.ARGS + " unknown=" + "x" * 300)

    def test_catalog_restore_preserves_neighbors(self):
        block = bytearray(b"x" * installer.BLOCK)
        struct.pack_into(">Q", block, 180, 704)
        installed = installer.update_catalog_size(block, 180, (704,), 596)
        changed = bytearray(installed)
        changed[20:28] = b"IOS-EDIT"
        restored = installer.update_catalog_size(changed, 180, (704, 596), 704)
        self.assertEqual(restored[:180], changed[:180])
        self.assertEqual(restored[188:], changed[188:])
        self.assertEqual(restored[20:28], b"IOS-EDIT")
        self.assertEqual(struct.unpack_from(">Q", restored, 180)[0], 704)

    def test_unknown_catalog_size_rejected(self):
        with self.assertRaises(SystemExit):
            installer.update_catalog_size(bytes(installer.BLOCK), 8, (704,), 596)

    def test_catalog_straddling_rejected(self):
        with self.assertRaises(SystemExit):
            installer.update_catalog_size(bytes(installer.BLOCK), 4092, (0,), 596)

    def test_wrong_serial_before_any_adb(self):
        with patch.object(installer, "adb", side_effect=AssertionError("adb called")):
            with self.assertRaises(SystemExit):
                installer.live(argparse.Namespace(
                    serial="other-device", write=True, restore=False))

    def test_normal_android_write_refused(self):
        with patch.object(installer, "adb", return_value=b"device\n"), \
                patch.object(installer, "adb_shell", return_value=b"normal\n"):
            with self.assertRaises(SystemExit):
                installer.live(argparse.Namespace(
                    serial="2aa26095", write=True, restore=False))

    def test_mounted_userdata_write_refused(self):
        def shell(_serial, command, **_kw):
            return (b"/dev/block/dm-0 /data ext4 rw 0 0\n"
                    if command == "cat /proc/mounts" else b"recovery\n")
        with patch.object(installer, "adb", return_value=b"recovery\n"), \
                patch.object(installer, "adb_shell", side_effect=shell):
            with self.assertRaises(SystemExit):
                installer.live(argparse.Namespace(
                    serial="2aa26095", write=True, restore=False))

    def test_short_read_rejected(self):
        with patch.object(installer, "adb", return_value=b"x" * 4095):
            with self.assertRaises(IOError):
                installer.read_blocks("2aa26095", 200, 1)

    def test_misaligned_write_refused(self):
        with self.assertRaises(ValueError):
            installer.write_blocks("2aa26095", 200, b"x", "/tmp/test", False)

    def test_write_readback_mismatch_refused(self):
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(installer, "OUTDIR", folder), \
                patch.object(installer, "adb", return_value=b""), \
                patch.object(installer, "adb_shell", return_value=b""), \
                patch.object(installer, "read_blocks", return_value=b"y" * 4096):
            with self.assertRaises(SystemExit):
                installer.write_blocks(
                    "2aa26095", 200, b"x" * 4096, "/tmp/test", True)

    def test_partition_geometry_refused(self):
        with patch.object(installer, "adb_shell", return_value=b"9\n"):
            with self.assertRaises(SystemExit):
                installer.partition_block("2aa26095", 0)

    def test_catalog_second_block_address(self):
        class Volume:
            block_size = 4096
            catalog = {"extents": [(100, 4)]}
        # A file record in the second half of an 8192-byte B-tree node.
        offset = installer.catalog_extent_offset(Volume(), 8192 + 5000 + 88)
        self.assertEqual(offset // 4096, 103)
        self.assertEqual(offset % 4096, 992)

    def test_live_volume_reads_header_without_local_dump(self):
        header = bytearray(512)
        header[:2] = b"HX"
        struct.pack_into(">IIIII", header, 32, 5, 3, 4096, 100, 40)
        struct.pack_into(">QII", header, 272, 8192, 0, 2)
        struct.pack_into(">II", header, 288, 5, 2)
        first = installer.HFS_ABS_4K_BLOCK

        def shell(_serial, command, **_kwargs):
            return b"0\n" if command.endswith("/start") else str((first + 100) * 8).encode()

        def blocks(_serial, at, count):
            if at == first and count == 1:
                return bytes(1024) + bytes(header) + bytes(2560)
            self.assertEqual((at, count), (first + 5, 2))
            return b"L" * 8192

        with patch.object(installer, "adb_shell", side_effect=shell), \
                patch.object(installer, "read_blocks", side_effect=blocks):
            volume = installer.CurrentVolume("/missing/local/dump", "2aa26095")
            self.assertEqual(volume.catalog["extents"][0], (5, 2))
            self.assertEqual(volume.read(5 * 4096, 8192), b"L" * 8192)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SafetyTests)
    with redirect_stdout(io.StringIO()):
        result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(json.dumps({"passed": result.wasSuccessful(),
                      "tests": result.testsRun,
                      "scope": "host-only mocks; no device reads or writes"},
                     indent=2))
    raise SystemExit(0 if result.wasSuccessful() else 1)
