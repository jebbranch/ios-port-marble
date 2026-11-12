#!/usr/bin/env python3
"""Install/restore MarbleAudio in AppleAOPAudioPlugin's existing HFS slots.

The bundle directory and both target files already exist, so no HFS allocation,
extent or directory record is created.  The 85 KiB MarbleAudio executable is
zero-padded inside AppleAOPAudioPlugin's 65 existing blocks.  Info.plist uses
its existing block; only that catalog record's logical size is changed.

Offline planning reads iosroot16.img only:
  python3 install_marble_audio.py --offline

Device writes require the project recovery/TWRP state and are never implicit:
  SERIAL=2aa26095 python3 install_marble_audio.py --write
  SERIAL=2aa26095 python3 install_marble_audio.py --restore --write
"""

import argparse
import hashlib
import json
import os
import plistlib
import shlex
import struct
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, '..', '..', '..'))
HERE = os.path.join(ROOT, 'work', 'iphone-x-recon', 'hfs')
sys.path.insert(0, HERE)
import hfsread as H

BLOCK = 4096
SDA33 = '/dev/block/sda33'
HFS_ABS_4K_BLOCK = 0x3603600
IMG = os.path.join(HERE, 'iosroot16.img')
OUTDIR = os.path.normpath(os.path.join(
    HERE, '..', 'artifacts', '2026-11-10-halbridge'))
PLUGIN = os.path.join(
    ROOT, 'work', 'xnu-gh', 'userland', 'audio', 'out',
    'MarbleAudio.driver', 'MarbleAudio')
PLUGIN_PLIST = os.path.join(
    ROOT, 'work', 'xnu-gh', 'userland', 'audio', 'Info.plist')

BUNDLE = '/System/Library/Audio/Plug-Ins/HAL/AppleAOPAudioPlugin.driver'
EXECUTABLE = BUNDLE + '/AppleAOPAudioPlugin'
INFO = BUNDLE + '/Info.plist'
EXPECTED_EXECUTABLE_SIZE = 264256
EXPECTED_EXECUTABLE_BLOCKS = 65
EXPECTED_INFO_MAX_BLOCKS = 1
TARGET_SERIAL = '2aa26095'
READ_WITH_SU = False


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def adb(serial, *args, check=True):
    return subprocess.run(['adb', '-s', serial, *args],
                          capture_output=True, check=check).stdout


def adb_shell(serial, command, check=True):
    return adb(serial, 'shell', root_command(command), check=check)


def root_command(command):
    return 'su 0 sh -c ' + shlex.quote(command) if READ_WITH_SU else command


def partition_block(serial, volume_block):
    start = int(adb_shell(
        serial, 'cat /sys/class/block/sda33/start').strip())
    if start % 8 or start // 8 > HFS_ABS_4K_BLOCK:
        raise SystemExit('unexpected userdata partition geometry')
    return HFS_ABS_4K_BLOCK - start // 8 + volume_block


def read_blocks(serial, disk_block, count):
    data = adb(
        serial, 'exec-out',
        root_command('dd if=%s bs=%d skip=%d count=%d 2>/dev/null'
                     % (SDA33, BLOCK, disk_block, count)),
    )
    if len(data) != count * BLOCK:
        raise IOError('short read %d/%d at block %d'
                      % (len(data), count * BLOCK, disk_block))
    return data


def write_blocks(serial, disk_block, data, remote, write):
    if len(data) % BLOCK:
        raise ValueError('write is not block aligned')
    count = len(data) // BLOCK
    if not write:
        print('DRY RUN: would write %d block(s) at sda33 block %d'
              % (count, disk_block))
        return
    local = os.path.join(OUTDIR, os.path.basename(remote) + '.local')
    with open(local, 'wb') as handle:
        handle.write(data)
    adb(serial, 'push', local, remote)
    adb_shell(
        serial,
        'dd if=%s of=%s bs=%d seek=%d count=%d conv=notrunc'
        % (remote, SDA33, BLOCK, disk_block, count))
    adb_shell(serial, 'sync')
    if read_blocks(serial, disk_block, count) != data:
        raise SystemExit('readback mismatch at block %d' % disk_block)


class CurrentVolume(H.Volume):
    """Use the live header and live catalog, never the old prefix's extents."""

    def __init__(self, path, serial):
        self.serial = serial
        self.first_block = partition_block(serial, 0)
        self.partition_sectors = int(adb_shell(
            serial, 'cat /sys/class/block/sda33/size').strip())
        self.live_ranges = []
        # No local dump is needed. Parse the current on-device volume header.
        header = self._device_read(1024, 512)
        self.header = header
        if header[:2] not in (b'H+', b'HX'):
            raise SystemExit('target offset is not an HFS+/HFSX volume')
        (self.file_count, self.folder_count, self.block_size,
         self.total_blocks, self.free_blocks) = struct.unpack_from('>IIIII', header, 32)
        self.catalog = self._fork(header, 272)
        self.extents_file = self._fork(header, 192)
        if self.block_size != BLOCK:
            raise SystemExit('unsupported HFS allocation block size')
        if (self.first_block + self.total_blocks) * 8 > self.partition_sectors:
            raise SystemExit('HFS volume does not fit userdata partition')
        for start, count in self.catalog['extents']:
            if count:
                offset = start * BLOCK
                self.live_ranges.append(
                    (offset, self._device_read(offset, count * BLOCK)))

    def _device_read(self, offset, length):
        if offset < 0 or length < 0:
            raise SystemExit('negative volume read')
        first = offset // BLOCK
        last = (offset + length + BLOCK - 1) // BLOCK
        if (self.first_block + last) * 8 > self.partition_sectors:
            raise SystemExit('volume read outside userdata')
        data = read_blocks(self.serial, self.first_block + first, last - first)
        relative = offset - first * BLOCK
        return data[relative:relative + length]

    def _local(self, offset, length):
        return self._device_read(offset, length)

    def read(self, offset, length, fetch=True):
        for start, data in self.live_ranges:
            if start <= offset and offset + length <= start + len(data):
                return data[offset - start:offset - start + length]
        return self._device_read(offset, length)


def catalog_extent_offset(volume, logical):
    pos = 0
    for start, count in volume.catalog['extents']:
        if not start or not count:
            continue
        length = count * volume.block_size
        if pos <= logical < pos + length:
            return start * volume.block_size + logical - pos
        pos += length
    raise KeyError('catalog logical %#x not in extents' % logical)


def find_record(volume, parent_cnid, name):
    tree = H.BTree(volume, volume.catalog)
    for index, node, records in tree.leaves():
        offset = 14
        for _ in range(records):
            key_length = struct.unpack_from('>H', node, offset)[0]
            record = offset + 2 + key_length
            record += record & 1
            parent = struct.unpack_from('>I', node, offset + 2)[0]
            name_length = struct.unpack_from('>H', node, offset + 6)[0]
            record_name = node[offset + 8:offset + 8 + 2 * name_length].decode(
                'utf-16-be', 'replace')
            record_type = struct.unpack_from('>H', node, record)[0]
            if parent == parent_cnid and record_name == name and record_type == 2:
                return index, tree.node_size, node, record
            if record_type == 1:
                record_length = 88
            elif record_type == 2:
                record_length = 248
            elif record_type in (3, 4):
                thread_name = struct.unpack_from('>H', node, record + 8)[0]
                record_length = 10 + 2 * thread_name
            else:
                break
            offset = record + record_length
    raise KeyError('%s not found under CNID %d' % (name, parent_cnid))


def target(volume, entries, path):
    by_parent, by_cnid = H.build_index(entries)
    _, _, cnid, raw = H.resolve(by_parent, by_cnid, path)
    fork, inode = H.real_fork(by_cnid, raw)
    extents = [(start, count) for start, count in fork['extents'] if count]
    return by_parent, by_cnid, cnid, fork, inode, extents


def install_plist():
    with open(PLUGIN_PLIST, 'rb') as handle:
        data = plistlib.load(handle)
    data['CFBundleExecutable'] = 'AppleAOPAudioPlugin'
    data['CFBundleName'] = 'MarbleAudio'
    data['CFBundleIdentifier'] = 'com.marble.audio'
    result = plistlib.dumps(data, fmt=plistlib.FMT_BINARY, sort_keys=True)
    if plistlib.loads(result)['CFBundleExecutable'] != 'AppleAOPAudioPlugin':
        raise AssertionError('generated plist did not round-trip')
    return result


def build_plan(volume, entries, restore=False):
    by_parent, by_cnid, _, executable, _, executable_extents = target(
        volume, entries, EXECUTABLE)
    _, _, _, info, _, info_extents = target(volume, entries, INFO)
    _, _, bundle_cnid, _ = H.resolve(by_parent, by_cnid, BUNDLE)

    if executable['logical'] != EXPECTED_EXECUTABLE_SIZE:
        raise SystemExit('unexpected executable size %d'
                         % executable['logical'])
    if not executable_extents or executable_extents != [(executable_extents[0][0],
                               EXPECTED_EXECUTABLE_BLOCKS)]:
        raise SystemExit('unexpected executable extents %r'
                         % (executable_extents,))
    if len(info_extents) != 1 or info_extents[0][1] != EXPECTED_INFO_MAX_BLOCKS:
        raise SystemExit('unexpected Info.plist extents %r' % (info_extents,))

    index, node_size, node, record = find_record(
        volume, bundle_cnid, 'Info.plist')
    field_volume = catalog_extent_offset(
        volume, index * node_size + record + 88)
    size_in_block = field_volume % BLOCK
    if size_in_block + 8 > BLOCK:
        raise SystemExit('Info.plist size field straddles block')

    plugin = b'' if restore else open(PLUGIN, 'rb').read()
    if not restore and plugin[:4] != b'\xcf\xfa\xed\xfe':
        raise SystemExit('MarbleAudio is not an arm64 Mach-O')
    if len(plugin) > executable['logical']:
        raise SystemExit('plugin %d > target logical size %d'
                         % (len(plugin), executable['logical']))
    plist = b'' if restore else install_plist()
    if len(plist) > info_extents[0][1] * BLOCK:
        raise SystemExit('install plist does not fit its existing extent')

    executable_blocks = (
        plugin + b'\0' *
        (EXPECTED_EXECUTABLE_BLOCKS * BLOCK - len(plugin)))
    info_blocks = (
        plist + b'\0' * (info_extents[0][1] * BLOCK - len(plist)))
    return {
        'plugin': plugin,
        'plist': plist,
        'executable_blocks': executable_blocks,
        'info_blocks': info_blocks,
        'executable_volume_block': executable_extents[0][0],
        'info_volume_block': info_extents[0][0],
        'catalog_volume_block': field_volume // BLOCK,
        'catalog_size_offset': size_in_block,
        'old_info_size': info['logical'],
        'new_info_size': len(plist),
    }


def save_offline(plan, volume):
    os.makedirs(OUTDIR, exist_ok=True)
    executable_original = volume.read(
        plan['executable_volume_block'] * BLOCK,
        len(plan['executable_blocks']), fetch=False)
    info_original = volume.read(
        plan['info_volume_block'] * BLOCK,
        len(plan['info_blocks']), fetch=False)
    catalog_original = volume.read(
        plan['catalog_volume_block'] * BLOCK, BLOCK, fetch=False)
    catalog_pending = bytearray(catalog_original)
    old_size = struct.unpack_from(
        '>Q', catalog_original, plan['catalog_size_offset'])[0]
    if old_size != plan['old_info_size']:
        raise SystemExit('offline catalog size %d != %d'
                         % (old_size, plan['old_info_size']))
    struct.pack_into(
        '>Q', catalog_pending, plan['catalog_size_offset'],
        plan['new_info_size'])
    files = {
        'AppleAOPAudioPlugin.orig.blocks.bin': executable_original,
        'AppleAOPAudioPlugin.pending.blocks.bin':
            plan['executable_blocks'],
        'Info.plist.orig.block.bin': info_original,
        'Info.plist.pending.block.bin': plan['info_blocks'],
        'catalog.orig.block.bin': catalog_original,
        'catalog.pending.block.bin': bytes(catalog_pending),
    }
    for name, data in files.items():
        with open(os.path.join(OUTDIR, name), 'wb') as handle:
            handle.write(data)
    manifest = {
        'mode': 'offline-local-prefix',
        'target_bundle': BUNDLE,
        'plugin_bytes': len(plan['plugin']),
        'plugin_sha256': sha256(plan['plugin']),
        'install_plist_bytes': len(plan['plist']),
        'install_plist_sha256': sha256(plan['plist']),
        'old_info_size': plan['old_info_size'],
        'new_info_size': plan['new_info_size'],
        'volume_blocks': {
            'executable': plan['executable_volume_block'],
            'info': plan['info_volume_block'],
            'catalog': plan['catalog_volume_block'],
        },
        'files': {name: sha256(data) for name, data in files.items()},
        'device_written': False,
    }
    with open(os.path.join(OUTDIR, 'offline-plan.json'), 'w') as handle:
        json.dump(manifest, handle, indent=2)
        handle.write('\n')
    print(json.dumps(manifest, indent=2))


def live(args):
    global READ_WITH_SU
    serial = args.serial
    if serial != TARGET_SERIAL:
        raise SystemExit('only research device %s is allowed' % TARGET_SERIAL)
    os.environ['ANDROID_SERIAL'] = serial
    state = adb(serial, 'get-state').decode().strip()
    READ_WITH_SU = state == 'device' and not args.write
    mode = adb_shell(serial, 'getprop ro.boot.mode 2>/dev/null; true').decode().strip()
    print('target', serial, 'adb state', state, 'boot mode', mode)
    if args.write and state != 'recovery':
        raise SystemExit('--write requires temporary recovery/TWRP; current state is ' + state)
    mounts = adb_shell(serial, 'cat /proc/mounts').decode()
    if args.write and any(
            len(line.split()) >= 2 and (
                line.split()[0].startswith(SDA33) or
                line.split()[1] in ('/data', '/userdata'))
            for line in mounts.splitlines()):
        raise SystemExit('userdata must be unmounted before raw HFS writes')

    volume = CurrentVolume(IMG, serial)
    entries = H.parse_catalog(volume)
    plan = build_plan(volume, entries, restore=args.restore)
    disk = {
        key: partition_block(serial, plan[key + '_volume_block'])
        for key in ('executable', 'info', 'catalog')
    }
    current = {
        'executable': read_blocks(
            serial, disk['executable'],
            len(plan['executable_blocks']) // BLOCK),
        'info': read_blocks(serial, disk['info'],
                            len(plan['info_blocks']) // BLOCK),
        'catalog': read_blocks(serial, disk['catalog'], 1),
    }
    os.makedirs(OUTDIR, exist_ok=True)
    backups = {
        'executable': 'AppleAOPAudioPlugin.live.orig.blocks.bin',
        'info': 'Info.plist.live.orig.block.bin',
        'catalog': 'catalog.live.orig.block.bin',
    }
    live_manifest = os.path.join(OUTDIR, 'live-install-manifest.json')

    if args.restore:
        if not os.path.exists(live_manifest):
            raise SystemExit('restore manifest missing: ' + live_manifest)
        saved = json.load(open(live_manifest))
        if saved['serial'] != serial or saved['disk_blocks'] != disk:
            raise SystemExit('restore target or file extents changed')
        if saved['catalog_size_offset'] != plan['catalog_size_offset']:
            raise SystemExit('Info.plist catalog record moved; do not overwrite')
        restored = {}
        for key, name in backups.items():
            path = os.path.join(OUTDIR, name)
            if not os.path.exists(path):
                raise SystemExit('restore backup missing: ' + path)
            restored[key] = open(path, 'rb').read()
            if sha256(restored[key]) != saved['backup_sha256'][key]:
                raise SystemExit('restore backup digest mismatch: ' + key)
        for key in ('executable', 'info'):
            if sha256(current[key]) not in (
                    saved['backup_sha256'][key], saved['pending_sha256'][key]):
                raise SystemExit('unknown current %s content; refusing restore' % key)
        catalog_restored = update_catalog_size(
            current['catalog'], plan['catalog_size_offset'],
            (saved['old_info_size'], saved['new_info_size']),
            saved['old_info_size'])
        write_blocks(serial, disk['executable'], restored['executable'],
                     '/tmp/marble_audio_exec.restore.bin', args.write)
        write_blocks(serial, disk['info'], restored['info'],
                     '/tmp/marble_audio_info.restore.bin', args.write)
        # iOS may have changed other records in this same block. Restore ONLY
        # the target logical-size field in the freshly read catalog block.
        write_blocks(serial, disk['catalog'], catalog_restored,
                     '/tmp/marble_audio_catalog.restore.bin', args.write)
        print('restored AppleAOPAudioPlugin executable, Info.plist and catalog size')
        return

    old_size = struct.unpack_from(
        '>Q', current['catalog'], plan['catalog_size_offset'])[0]
    if old_size != plan['old_info_size']:
        raise SystemExit('live Info.plist size %d != expected %d'
                         % (old_size, plan['old_info_size']))
    catalog_pending = update_catalog_size(
        current['catalog'], plan['catalog_size_offset'],
        (plan['old_info_size'],), plan['new_info_size'])
    if not args.write:
        print('DRY RUN: live geometry validated; no backup or device writes')
        evidence = {'live-hfs-header.bin': volume.header}
        for index, (_, data) in enumerate(volume.live_ranges):
            evidence['live-catalog-extent-%d.bin' % index] = data
        for key, data in current.items():
            evidence['live-readonly-%s.blocks.bin' % key] = data
        for name, data in evidence.items():
            with open(os.path.join(OUTDIR, name), 'wb') as handle:
                handle.write(data)
        result = {
            'mode': 'live-readonly-not-install-backup',
            'serial': serial, 'adb_state': state, 'disk_blocks': disk,
            'volume_blocks': {
                key: plan[key + '_volume_block']
                for key in ('executable', 'info', 'catalog')
            },
            'catalog_size_offset': plan['catalog_size_offset'],
            'old_info_size': plan['old_info_size'],
            'new_info_size': plan['new_info_size'],
            'plugin_sha256': sha256(plan['plugin']),
            'files': {name: sha256(data) for name, data in evidence.items()},
            'device_written': False,
        }
        with open(os.path.join(OUTDIR, 'live-readonly-plan.json'), 'w') as handle:
            json.dump(result, handle, indent=2)
            handle.write('\n')
        print(json.dumps(result, indent=2))
        return
    for key, name in backups.items():
        path = os.path.join(OUTDIR, name)
        if os.path.exists(path):
            if open(path, 'rb').read() != current[key]:
                raise SystemExit(
                    'existing live backup does not match current %s; '
                    'restore before installing another version' % key)
        else:
            with open(path, 'wb') as handle:
                handle.write(current[key])
            print('backup', path)

    saved = {
        'serial': serial, 'disk_blocks': disk,
        'catalog_size_offset': plan['catalog_size_offset'],
        'old_info_size': plan['old_info_size'],
        'new_info_size': plan['new_info_size'],
        'backup_sha256': {key: sha256(data) for key, data in current.items()},
        'pending_sha256': {
            'executable': sha256(plan['executable_blocks']),
            'info': sha256(plan['info_blocks']),
        },
    }
    if os.path.exists(live_manifest):
        if json.load(open(live_manifest)) != saved:
            raise SystemExit('existing install manifest differs; refusing overwrite')
    else:
        with open(live_manifest, 'w') as handle:
            json.dump(saved, handle, indent=2)
            handle.write('\n')

    write_blocks(serial, disk['executable'], plan['executable_blocks'],
                 '/tmp/marble_audio_exec.pending.bin', args.write)
    write_blocks(serial, disk['info'], plan['info_blocks'],
                 '/tmp/marble_audio_info.pending.bin', args.write)
    write_blocks(serial, disk['catalog'], bytes(catalog_pending),
                 '/tmp/marble_audio_catalog.pending.bin', args.write)
    print('installed MarbleAudio into the existing AppleAOPAudioPlugin slots')


def update_catalog_size(block, offset, allowed, size):
    """Patch only the target 8-byte size, retaining all neighboring records."""
    if len(block) != BLOCK or offset < 0 or offset + 8 > BLOCK:
        raise SystemExit('invalid catalog block or size-field offset')
    old = struct.unpack_from('>Q', block, offset)[0]
    if old not in allowed:
        raise SystemExit('unexpected live catalog size %d' % old)
    pending = bytearray(block)
    struct.pack_into('>Q', pending, offset, size)
    return bytes(pending)


def check_saved_plan(source):
    """Check retained block evidence only; never treat it as current geometry."""
    saved = json.load(open(os.path.join(source, 'offline-plan.json')))
    blocks = {}
    for name, expected in saved['files'].items():
        blocks[name] = open(os.path.join(source, name), 'rb').read()
        if sha256(blocks[name]) != expected:
            raise SystemExit('saved block digest mismatch: ' + name)
    original = blocks['catalog.orig.block.bin']
    previous = blocks['catalog.pending.block.bin']
    offsets = [
        offset for offset in range(BLOCK - 7)
        if struct.unpack_from('>Q', original, offset)[0] == saved['old_info_size']
        and update_catalog_size(original, offset, (saved['old_info_size'],),
                                saved['new_info_size']) == previous
    ]
    if len(offsets) != 1:
        raise SystemExit('saved catalog before/after is not one size-field edit')
    plugin = open(PLUGIN, 'rb').read()
    plist = install_plist()
    capacity = len(blocks['AppleAOPAudioPlugin.orig.blocks.bin'])
    if (plugin[:4] != b'\xcf\xfa\xed\xfe' or len(plugin) > EXPECTED_EXECUTABLE_SIZE
            or capacity != EXPECTED_EXECUTABLE_BLOCKS * BLOCK or len(plist) > BLOCK):
        raise SystemExit('new plugin does not fit retained allocation')
    pending_catalog = update_catalog_size(
        original, offsets[0], (saved['old_info_size'],), len(plist))
    result = {
        'mode': 'retained-block-evidence-only-not-current-device-geometry',
        'source': source, 'source_plugin_sha256': saved['plugin_sha256'],
        'plugin_sha256': sha256(plugin), 'plugin_bytes': len(plugin),
        'install_plist_bytes': len(plist), 'catalog_size_offset': offsets[0],
        'pending_catalog_sha256': sha256(pending_catalog),
        'device_read': False, 'device_written': False,
        'live_header_and_extents_required_before_install': True,
    }
    os.makedirs(OUTDIR, exist_ok=True)
    with open(os.path.join(OUTDIR, 'saved-plan-check.json'), 'w') as handle:
        json.dump(result, handle, indent=2)
        handle.write('\n')
    print(json.dumps(result, indent=2))


def main():
    global OUTDIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--restore', action='store_true')
    parser.add_argument('--offline-saved-plan',
                        help='verify previously retained block evidence without a full dump')
    parser.add_argument('--outdir', help='separate absolute directory for this run and its backups')
    parser.add_argument('--serial', default=os.environ.get('SERIAL', '2aa26095'))
    args = parser.parse_args()
    if args.outdir:
        if not os.path.isabs(args.outdir):
            parser.error('--outdir must be absolute')
        OUTDIR = args.outdir
    if args.offline_saved_plan:
        if args.offline or args.write or args.restore:
            parser.error('--offline-saved-plan cannot be combined with live or other offline modes')
        check_saved_plan(args.offline_saved_plan)
        return
    if args.offline:
        if args.write or args.restore:
            parser.error('--offline cannot be combined with --write or --restore')
        volume = H.Volume(IMG)
        entries = H.parse_catalog(volume)
        plan = build_plan(volume, entries)
        save_offline(plan, volume)
        return
    live(args)


if __name__ == '__main__':
    main()
