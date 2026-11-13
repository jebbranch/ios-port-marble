#!/usr/bin/env python3
"""Compile and run the actual HAL bridge ring/ioctl functions on the host.

No device access. The production functions are extracted verbatim from the
chain tree and linked against lock/log mocks.
"""
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT.parent / 'chain-tree-halbridge/bsd/dev/sweet_audio_bridge.c'
USER_HEADER = ROOT / 'userland/audio/marble_audio.h'


def masked(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                  lambda m: re.sub(r'[^\n]', ' ', m.group()),
                  source, flags=re.S)


def function(source, name):
    clean = masked(source)
    match = re.search(r'^(?:static\s+)?(?:uint32_t|int)\s*\n' +
                      re.escape(name) + r'\([^;]+?\)\s*\{', clean, re.M)
    if match is None:
        raise ValueError('missing actual function ' + name)
    begin = clean.index('{', match.start())
    depth = 0
    for end in range(begin, len(clean)):
        depth += (clean[end] == '{') - (clean[end] == '}')
        if depth == 0:
            return source[match.start():end + 1]
    raise ValueError('unclosed function ' + name)


SUPPORT = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "marble_audio.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MARBLE_AUDIO_RING 65536u
#define MARBLE_AUDIO_BELOW_MAX (90u*8u)
#define MARBLE_AUDIO_GET_STATS _IOR('m',2,struct marble_audio_stats)
typedef int lck_mtx_t;
struct proc { int unused; };
struct marble_audio_stats {
 uint64_t opens, closes, writes, frames, sound_frames, overwritten_frames, consumed_frames;
 uint32_t peak, queued_frames, volume_below, volume_muted, volume_generation;
};
static lck_mtx_t bridge_lock;
static int16_t bridge_ring[MARBLE_AUDIO_RING*MARBLE_AUDIO_CHANNELS];
static uint64_t bridge_head, bridge_tail;
static struct marble_audio_stats bridge_stats;
static void lck_mtx_lock(lck_mtx_t *l){(void)l;}
static void lck_mtx_unlock(lck_mtx_t *l){(void)l;}
static void bridge_log(const char *f,...){(void)f;}
'''

MAIN = r'''
static void reset_all(void) {
 memset(bridge_ring,0,sizeof(bridge_ring)); memset(&bridge_stats,0,sizeof(bridge_stats));
 bridge_head=bridge_tail=0;
}
int main(void) {
 reset_all();
 int16_t in[200*2], out[240*2];
 for(unsigned i=0;i<200;i++){in[2*i]=(int16_t)(100+i);in[2*i+1]=(int16_t)(-100-(int)i);}
 assert(bridge_accept_locked(in,200,299,200)==200);
 assert(bridge_stats.writes==1 && bridge_stats.frames==200 && bridge_stats.queued_frames==200);
 memset(out,0x5a,sizeof(out));
 assert(sweet_audio_bridge_consume(out,240)==200);
 assert(!memcmp(out,in,sizeof(in)));
 for(unsigned i=400;i<480;i++) assert(out[i]==0);
 assert(bridge_stats.consumed_frames==200 && bridge_stats.queued_frames==0);

 reset_all();
 int16_t pair[2]={7,-7};
 for(unsigned i=0;i<MARBLE_AUDIO_RING+10;i++) bridge_accept_locked(pair,1,7,0);
 assert(bridge_stats.overwritten_frames==10 && bridge_stats.queued_frames==MARBLE_AUDIO_RING);
 assert(bridge_tail==10 && bridge_head==MARBLE_AUDIO_RING+10);

 struct marble_audio_volume v={.below=0xffffffffu,.muted=3};
 assert(bridge_ioctl(0,MARBLE_AUDIO_SET_VOLUME,(caddr_t)&v,0,NULL)==0);
 assert(bridge_stats.volume_below==MARBLE_AUDIO_BELOW_MAX);
 assert(bridge_stats.volume_muted==1 && bridge_stats.volume_generation==1);
 assert(sweet_audio_bridge_muted()==1);
 struct marble_audio_stats s; memset(&s,0,sizeof(s));
 assert(bridge_ioctl(0,MARBLE_AUDIO_GET_STATS,(caddr_t)&s,0,NULL)==0);
 assert(s.volume_below==MARBLE_AUDIO_BELOW_MAX && s.overwritten_frames==10);
 assert(bridge_ioctl(0,0,(caddr_t)&s,0,NULL)==ENOTTY);
 return 0;
}
'''


def main():
    source = SOURCE.read_text()
    code = (SUPPORT + '\n' +
            function(source, 'bridge_accept_locked') + '\n' +
            function(source, 'sweet_audio_bridge_consume') + '\n' +
            function(source, 'sweet_audio_bridge_muted') + '\n' +
            function(source, 'bridge_ioctl') + '\n' + MAIN)
    with tempfile.TemporaryDirectory(prefix='halbridge-host-') as name:
        folder = Path(name)
        cfile = folder / 'halbridge.c'
        exe = folder / 'halbridge'
        cfile.write_text(code)
        flags = ['-std=gnu11', '-Wall', '-Wextra', '-Wformat=2', '-Werror',
                 '-fsanitize=address,undefined', '-I', str(USER_HEADER.parent)]
        subprocess.run(['clang', *flags, str(cfile), '-o', str(exe)], check=True)
        subprocess.run(['clang', *flags, '-fsyntax-only', str(cfile)], check=True)
        subprocess.run([str(exe)], check=True)
    result = {
        'passed': True,
        'source': str(SOURCE),
        'functions': ['bridge_accept_locked', 'sweet_audio_bridge_consume',
                      'bridge_ioctl', 'sweet_audio_bridge_muted'],
        'checks': ['fifo', 'zero-fill', 'overwrite accounting',
                   'volume clamp', 'stats ioctl'],
        'scope': 'host locks/logs mocked; no DSP, TDM, amplifier or device access',
    }
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
