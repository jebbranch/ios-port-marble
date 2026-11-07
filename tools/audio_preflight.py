#!/usr/bin/env python3
"""Offline audit of the frozen 0288 audio experiment; never accesses a device.

Extracts the actual C functions and executes them against host memory and mocked
transport. Hardware, timing, barriers and DSP acceptance are NOT modelled. Results
prove AP-side writes, dispatch and fallthrough only, not the cause of silence.

    python3 -I tools/audio_preflight.py --output-dir verify/audio-preflight-2026-11-07
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
ARGS = ("sweetbufs=120 sweetpcm=1 sweets1=1 sweetiova=1 "
        "sweets1va=0x1FFE8000 sweetshmhi=1 sweets1ap=3 sweetampfirst=1 "
        "sweetvol=16 sweettdmvendor=7 sweetvgraph8=1 sweeteos=1 sweetrxlog=1")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def masked_c(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: re.sub(r'[^\n]', ' ', m.group()), source, flags=re.S)


def function(source, name):
    masked = masked_c(source)
    match = re.search(r'^static\s+(?:void|int)\s*\n' + re.escape(name) +
                      r'\([^;]*?\)\s*\{', masked, re.M)
    require(match is not None, f"missing function {name}")
    opening = masked.index('{', match.start())
    depth = 0
    for end in range(opening, len(masked)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if not depth:
            return source[match.start():end + 1]
    raise ValueError(f"unclosed function {name}")


def array(source, name):
    match = re.search(r'static const uint8_t ' + re.escape(name) +
                      r'\[(\d+)\] = \{(.*?)\n\};', source, re.S)
    require(match is not None, f"missing array {name}")
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', match.group(2), flags=re.S)
    data = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{1,2})\b', body))
    require(len(data) == int(match.group(1)), f"array length mismatch: {name}")
    return data, match.group()


def aform(data):
    import struct
    blocks, offset = {}, 0
    while offset < len(data):
        require(offset + 16 <= len(data), "short APM record header")
        iid, pid, size, error = struct.unpack_from('<IIII', data, offset)
        require(iid == 1 and error == 0, "not a graph-level APM parameter header")
        end = offset + 16 + ((size + 7) & ~7)
        require(end <= len(data), "APM record exceeds body")
        require(pid not in blocks, "duplicate parameter block")
        blocks[pid] = data[offset + 16:offset + 16 + size]
        offset = end
    require(set(blocks) == set(range(0x08001000, 0x08001005)), "missing graph block")
    words = struct.unpack('<%dI' % (len(blocks[0x08001002]) // 4), blocks[0x08001002])
    require(words[0] == 1 and len(words) == 4 + 2 * words[3], "module count mismatch")
    modules = list(zip(words[4::2], words[5::2]))
    return blocks, modules


def make_harness(source):
    functions = {name: function(source, name) for name in (
        'g_pcm_bytes_init', 'g_fill_tone', 'g_pcm_prepare', 'g_apm_graph_open_shm',
        'g_audio_tone', 'g_audio_graph_cfg_vp', 'g_apm_cmd')}
    # This unused declaration is already present in the production function.
    # A sizeof use only prevents the existing unused-variable warning in the stub.
    cfg = functions['g_audio_graph_cfg_vp']
    require('static const uint8_t tdm_intf[28]' in cfg, "configuration fixture changed")
    functions['g_audio_graph_cfg_vp'] = cfg[:-1] + '\n\t(void)sizeof(tdm_intf);\n}'
    tone = functions['g_audio_tone']
    functions['g_audio_tone'] = tone[:-1] + '\n\t(void)src_port;\n}'
    define_names = ('G_PCM_OFF_A', 'G_PCM_BYTES', 'G_SHM_SIZE', 'G_VP_WR_IID',
                    'G_VP_TDM_IID', 'G_VP_AW_IID', 'G_VP_MFC_IID', 'SWEET_CC_MAX',
                    'GPR_DOMAIN_ADSP', 'GPR_DOMAIN_APPS', 'APM_CMD_GRAPH_OPEN')
    defines = []
    for name in define_names:
        match = re.search(r'^#define\s+' + name + r'\s+[^\n]+', source, re.M)
        require(match is not None, f"missing define {name}")
        defines.append(match.group())
    sine = re.search(r'static const int16_t g_sine_48\[48\] = \{.*?\n\};', source, re.S)
    require(sine is not None, "missing sine table")
    arrays = '\n'.join(array(source, name)[1] for name in (
        'g_graph_vp_wr_dec_cnv', 'g_graph_aform_no_aw'))
    bringup = function(source, 'g_apm_graph_bringup')
    begin = bringup.index('\n\t{\n\t\tint v8 = 0;')
    end = bringup.index('\n\n\t/*', begin)
    v8 = bringup[begin:end]
    require('g_audio_tone' in v8 and 'return;' in v8, "B1 branch extraction failed")
    return SUPPORT + '\n'.join(defines) + '\n' + arrays + '\n' + sine.group() + GLOBALS + (
        '\n'.join(functions[name] for name in ('g_pcm_bytes_init', 'g_fill_tone',
        'g_pcm_prepare', 'g_apm_graph_open_shm', 'g_audio_tone', 'g_audio_graph_cfg_vp', 'g_apm_cmd'))
    ) + '\nstatic void run_b1(void) { uint32_t port = 0x2010u; g_stream_port = port;\n' + v8 + (
        '\n fallback_count++;\n}\n') + MAIN


SUPPORT = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define __asm__(...)
'''
GLOBALS = r'''
typedef uintptr_t vm_offset_t;
static uint8_t region[0x5000], g_rsp[64];
static vm_offset_t g_shm_va;
static uint64_t g_shm_pa = 1;
static int g_shm_mapped = 1, g_tone_filled, g_vol_printed;
static uint32_t g_shm_handle = 0x12345678, g_rsp_len, g_stream_port;
static uint32_t g_pcm_bytes = G_PCM_BYTES;
static int g_rsp_ready, boot_v8, boot_vcfg2, reject_open;
static int boot_descsep, boot_nowait, boot_dbb;
static uint32_t last_graph_offset;
static int cfg_aw, cfg_bf, cfg_c0, cfg_absent, fallback_count;
static int sent_buffers, descriptor_buffers;
static const uint8_t *descriptor;
static size_t descriptor_size;
static int PE_parse_boot_argn(const char *n, void *p, unsigned long size) {
    int v;
    if (!strcmp(n, "sweetvol")) v = 16;
    else if (!strcmp(n, "sweetbufs")) v = 120;
    else if (!strcmp(n, "sweetpcm")) v = 1;
    else if (!strcmp(n, "sweettdmvendor")) v = 7;
    else if (!strcmp(n, "sweetvgraph8")) v = boot_v8;
    else if (!strcmp(n, "sweetvcfg2")) v = boot_vcfg2;
    else if (!strcmp(n, "sweetdescsep")) v = boot_descsep;
    else if (!strcmp(n, "sweetnowait")) v = boot_nowait;
    else if (!strcmp(n, "sweetdbb")) v = boot_dbb;
    else return 0;
    if (size != sizeof(v)) abort();
    memcpy(p, &v, size); return 1;
}
static void sweet_log_sync(void) {}
static void g_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t g_le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t g_shm_region_size(void) { return sizeof(region); }
static int g_apm_shm_map(uint32_t port) { (void)port; return 0; }
static void g_send_gpr(const uint8_t *p, uint32_t n) {
    (void)n; g_rsp_ready = 1; g_rsp_len = 32;
    last_graph_offset = g_le32(p + 24);
    g_put32(g_rsp + 24, APM_CMD_GRAPH_OPEN);
    g_put32(g_rsp + 28, reject_open ? 2 : 0);
}
static void g_drain(int ms) { (void)ms; }
static void g_wait(int ms) { (void)ms; }
static int g_apm_xfer(uint8_t *packet, uint32_t size, const char *what) {
    (void)what;
    if (size < 44) abort();
    printf("AUDIT {\"kind\":\"eos\",\"packet_bytes\":%u,\"opcode\":%u,"
        "\"payload_words\":[%u,%u,%u,%u,%u]}\n", size, g_le32(packet + 20),
        g_le32(packet + 24), g_le32(packet + 28), g_le32(packet + 32),
        g_le32(packet + 36), g_le32(packet + 40));
    return 0;
}
static int g_apm_set_cfg(uint32_t port, uint32_t iid, uint32_t pid,
    const void *data, uint32_t len, const char *what) {
    (void)port; (void)pid; (void)data; (void)len; (void)what;
    if (iid == G_VP_AW_IID) cfg_aw++;
    if (iid == 0x40bf) cfg_bf++;
    if (iid == 0x40c0) cfg_c0++;
    if ((boot_v8 && iid == G_VP_AW_IID) || iid == 0x40c8 || iid == 0x4165) {
        cfg_absent++; return -1;
    }
    return 0;
}
static int g_apm_set_cfg_batch(uint32_t port, const uint8_t *data, uint32_t len,
    uint32_t count, const char *what) {
    (void)port; (void)data; (void)len; (void)count; (void)what; return 0;
}
static int g_apm_set_cfg_oob(uint32_t port, const char *what) {
    (void)port; (void)what; return 0;
}
static void g_apm_get_cfg_dump(uint32_t port, uint32_t iid, uint32_t pid,
    uint32_t len, const char *what) {
    (void)port; (void)iid; (void)pid; (void)len; (void)what;
}
static int g_apm_graph_start(uint32_t port, uint32_t sg) { (void)port; (void)sg; return 0; }
static void g_apm_state_query_sync(const char *s) { (void)s; }
static void g_db_wait_report(const char *s) { (void)s; }
static void g_ibat_burst(const char *s) { (void)s; }
static void sweet_i2c_amp_readback(const char *s) { (void)s; }
static void delay(int ms) { (void)ms; }
static int g_apm_data_buffer(uint32_t port, uint32_t iid, uint32_t off,
    uint32_t len, uint32_t token) {
    (void)port; (void)iid; (void)token;
    if (off + len > sizeof(region)) abort();
    sent_buffers++;
    if (off == 0 && descriptor_size <= len && !memcmp(region, descriptor, descriptor_size))
        descriptor_buffers++;
    return 0;
}
'''
MAIN = r'''
static void reset(void) {
    memset(region, 0, sizeof(region)); g_shm_va = (vm_offset_t)region;
    g_tone_filled = 0; cfg_aw = cfg_bf = cfg_c0 = cfg_absent = 0;
    fallback_count = sent_buffers = descriptor_buffers = 0;
    last_graph_offset = 0;
}
static void separation_case(int nowait, int dbb, uint32_t size) {
    uint8_t before[G_PCM_BYTES];
    reset(); boot_descsep = 1; boot_nowait = nowait; boot_dbb = dbb;
    reject_open = 0;
    if (g_pcm_prepare()) abort();
    memcpy(before, region, sizeof(before));
    int rc = g_apm_graph_open_shm(0x2010, g_graph_vp_wr_dec_cnv, size, 0, "host/separated");
    printf("AUDIT {\"kind\":\"separation\",\"nowait\":%d,\"dbb\":%d,\"size\":%u,"
        "\"rc\":%d,\"packet_offset\":%u,\"pcm_a_changed\":%d}\n",
        nowait, dbb, size, rc, last_graph_offset, memcmp(before, region, sizeof(before)) != 0);
    boot_descsep = boot_nowait = boot_dbb = 0;
}
static void memory_case(const uint8_t *body, size_t size, uint32_t off) {
    uint8_t before[G_PCM_BYTES];
    reset(); boot_v8 = 0; reject_open = 0;
    descriptor = body; descriptor_size = size;
    if (g_pcm_prepare()) abort();
    memcpy(before, region, sizeof(before));
    if (g_apm_graph_open_shm(0x2010, body, (uint32_t)size, off, "host/open")) abort();
    if (g_pcm_prepare()) abort();
    int changed = memcmp(before, region, sizeof(before)) != 0;
    g_audio_tone(0x2010, G_VP_WR_IID);
    printf("AUDIT {\"kind\":\"memory\",\"body_bytes\":%zu,\"offset\":%u,"
        "\"pcm_a_changed\":%d,\"descriptor_buffers\":%d,\"total_buffers\":%d}\n",
        size, off, changed, descriptor_buffers, sent_buffers);
}
int main(void) {
    memory_case(g_graph_vp_wr_dec_cnv, sizeof(g_graph_vp_wr_dec_cnv), 0);
    memory_case(g_graph_aform_no_aw, sizeof(g_graph_aform_no_aw), 0);
    memory_case(g_graph_vp_wr_dec_cnv, sizeof(g_graph_vp_wr_dec_cnv), 0x4000);
    for (int vc = 0; vc <= 1; vc++) {
        reset(); boot_v8 = 0; boot_vcfg2 = vc;
        g_audio_graph_cfg_vp(0x2010);
        printf("AUDIT {\"kind\":\"cfg\",\"vcfg2\":%d,\"aw\":%d,\"dec\":%d,"
            "\"cnv\":%d,\"absent\":%d}\n", vc, cfg_aw, cfg_bf, cfg_c0, cfg_absent);
    }
    for (int refuse = 0; refuse <= 1; refuse++) {
        reset(); boot_v8 = 1; boot_vcfg2 = 0; reject_open = refuse;
        descriptor = g_graph_aform_no_aw; descriptor_size = sizeof(g_graph_aform_no_aw);
        run_b1();
        printf("AUDIT {\"kind\":\"b1\",\"refused\":%d,\"aw\":%d,"
            "\"absent\":%d,\"fallback\":%d,\"total_buffers\":%d}\n",
            refuse, cfg_aw, cfg_absent, fallback_count, sent_buffers);
    }
    uint8_t eos[4];
    g_put32(eos, 1u);
    if (g_apm_cmd(0x2010, G_VP_WR_IID, eos, sizeof(eos), 0x04001002u, "host/eos")) abort();
    separation_case(0, 0, 856);
    separation_case(1, 0, 856);
    separation_case(0, 4096, 856);
    separation_case(0, 0, 4097);
    return 0;
}
'''


def audit(source_path, kernel_path, output_dir):
    source = source_path.read_text()
    h1, _ = array(source, 'g_graph_vp_wr_dec_cnv')
    b1, _ = array(source, 'g_graph_aform_no_aw')
    hblocks, hmods = aform(h1)
    bblocks, bmods = aform(b1)
    require(all(hblocks[p] == bblocks[p] for p in (0x08001000, 0x08001001)),
            "B1 changed SG_CONFIG or CONTAINER_CONFIG")
    require(set(hmods) - set(bmods) == {(0x10013d01, 0x42d3)} and
            not set(bmods) - set(hmods), "B1 module delta differs from one AW node")
    script = (ROOT / 'scripts/mkimage.sh').read_text()
    base = re.search(r"^CMDLINE='([^']+)'", script, re.M).group(1)
    require('CMDLINE="$CMDLINE sweettime=$(date +%s)"' in script,
            "timestamp generation changed; re-audit cmdline")
    cmdline = base + ' ' + ARGS + ' sweettime=1794047000'
    result = {
        'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'scope': 'host mocks only; no DSP/hardware/audio verdict',
        'cmdline_bytes_auto_timestamp': len(cmdline.encode()),
        'cmdline_bytes_duplicate_timestamp': len((cmdline + ' sweettime=1794047000').encode()),
        'graph_bytes': {'h1': len(h1), 'b1': len(b1)},
    }
    if kernel_path.exists():
        kernel = kernel_path.read_bytes()
        result['kernel'] = {
            'bytes': len(kernel), 'sha256': hashlib.sha256(kernel).hexdigest(),
            'cntp_unpatched': kernel.count(bytes.fromhex('28e21bd5')),
            'markers': {m: kernel.count(m.encode()) for m in (
                'SWEET MARK: noaw v1 ', 'SWEET MARK: dcnv v2 ', 'SWEET MARK: rxall v1 ',
                'SWEET MARK: descsep v1 ')},
        }
    harness = make_harness(source)
    with tempfile.TemporaryDirectory(prefix='audio-preflight-') as temp:
        path = Path(temp)
        (path / 'host.c').write_text(harness)
        command = ['clang', '-std=gnu11', '-Wall', '-Wextra', '-Wformat=2', '-Werror',
                   str(path / 'host.c'), '-o', str(path / 'host')]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            if output_dir:
                output_dir.mkdir(parents=True, exist_ok=True)
                (output_dir / 'host.c').write_text(harness)
            raise RuntimeError(compiled.stderr)
        run = subprocess.run([str(path / 'host')], capture_output=True, text=True, check=True)
    rows = [json.loads(line[6:]) for line in run.stdout.splitlines() if line.startswith('AUDIT ')]
    require(len(rows) == 12, f"expected 12 exercised cases, got {len(rows)}")
    result['cases'] = rows
    result['ready_to_boot'] = False
    result['readiness_note'] = ('Frozen B1 has unresolved configuration targets/fallthrough; '
                                'the descriptor overlaps PCM. EOS framing differs between '
                                'the local GSL and Linux references. No device verdict.')
    result['compiler_flags'] = '-std=gnu11 -Wall -Wextra -Wformat=2 -Werror'
    result['host_adaptations'] = ['barriers elided; transport/timing/register access mocked',
                                'sizeof(tdm_intf) and (void)src_port silence existing unused declarations']
    if output_dir:
        output_dir.mkdir(parents=True, exist_ok=True)
        (output_dir / 'host.c').write_text(harness)
        (output_dir / 'host.log').write_text(run.stdout)
        (output_dir / 'audit.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT.parent / 'chain-tree/bsd/dev/sweet_glink.c')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'out/kernel.adsp')
    parser.add_argument('--output-dir', type=Path)
    options = parser.parse_args()
    print(json.dumps(audit(options.source, options.kernel, options.output_dir), indent=2))


if __name__ == '__main__':
    main()
