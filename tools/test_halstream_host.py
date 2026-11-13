#!/usr/bin/env python3
"""Run actual 0293 core/runtime/adapter functions against host-only transport."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
STREAM = ROOT.parent / "chain-tree-halstream/bsd/dev"
GLINK = ROOT.parent / "chain-tree/bsd/dev/sweet_glink.c"


def masked(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                  lambda match: re.sub(r"[^\n]", " ", match.group()),
                  source, flags=re.S)


def function(source, name):
    clean = masked(source)
    match = re.search(
        r"^(?:static\s+)?(?:uint32_t|uint64_t|int|void)\s*\n" +
        re.escape(name) + r"\([^;]+?\)\s*\{", clean, re.M)
    if match is None:
        raise ValueError("missing actual function " + name)
    begin = clean.index("{", match.start())
    depth = 0
    for end in range(begin, len(clean)):
        depth += (clean[end] == "{") - (clean[end] == "}")
        if depth == 0:
            return source[match.start():end + 1]
    raise ValueError("unclosed " + name)


def body(source):
    return "\n".join(line for line in source.splitlines()
                     if not line.startswith(("#include", "#if", "#endif")))


SUPPORT = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CMD_RX_INTENT_REQ 7u
#define G_VP_WR_IID 0x40bdu
#define APM_PARAM_ID_SUB_GRAPH_LIST 0x08001005u
typedef uintptr_t vm_offset_t;
static uint64_t virtual_us;
static uint8_t fake_memory[0x5000] __attribute__((aligned(16)));
static vm_offset_t g_shm_va;
static uint64_t g_shm_pa;
static int g_shm_mapped, g_shm_offset_mode;
static uint32_t g_shm_handle, g_tx_len=8192, g_desc_off, g_lcid=4;
static uint8_t g_rsp[4096];
static uint32_t g_rsp_len;
static int g_rsp_ready;
static uint32_t g_hal_ctrl_token, g_hal_ctrl_source, g_hal_ctrl_opcode, g_hal_ctrl_status;
static int g_hal_ctrl_pending, g_hal_ctrl_ready;
static int g_hal_started_once;
struct g_riid { uint32_t id; int in_use; };
static struct g_riid remote={112,0};
struct message { uint32_t bytes; uint8_t packet[68]; };
static struct message events[256];
static unsigned event_read,event_write,wire_data,wire_controls,case_count;
static int mode, clock_failure, ctrl_failure, muted, supply, full_fifo;
static uint32_t fail_opcode, drop_opcode;
static int listen_start_calls,listen_stop_calls,listen_start_failure;
static const char *bad_flag;
static int bad_flag_value;
static char logs[65536];
static size_t log_length;
static uint32_t g_le32(const uint8_t *p) {
 return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void g_put32(uint8_t *p,uint32_t v) {
 for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static void sweet_log_sync(void) {}
void sweet_log_append(const char *text,int length) {
 assert(length>0 && length<220);
 if(log_length+(size_t)length+1<sizeof(logs)) {
  memcpy(logs+log_length,text,(size_t)length);log_length+=(size_t)length;logs[log_length]=0;
 }
}
uint32_t sweet_audio_bridge_muted(void) { return (uint32_t)muted; }
uint32_t sweet_audio_bridge_consume(int16_t *out,uint32_t count) {
 assert(count==1024);
 uint32_t n=supply ? count:0;
 if(mode==4 && n) n=128;
 for(uint32_t i=0;i<n;i++){out[2*i]=INT16_MIN;out[2*i+1]=INT16_MAX;}
 memset(out+2*n,0,(count-n)*4u);return n;
}
static uint64_t mach_absolute_time(void) {return virtual_us*1000u;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out){*out=value;}
static void g_wait(unsigned ms){virtual_us+=(uint64_t)ms*1000u;}
static int g_drain(unsigned budget);
static struct g_riid *g_gpr_riid(uint32_t bytes){(void)bytes;return remote.in_use?NULL:&remote;}
static void g_send(uint16_t cmd,uint16_t p1,uint32_t p2,const void *data,uint32_t n) {
 (void)p1;(void)p2;(void)data;(void)n;assert(cmd==CMD_RX_INTENT_REQ);
}
static int g_wait_riid(uint32_t bytes,unsigned ms) {
 (void)bytes;(void)ms;g_drain(0);remote.in_use=0;return 1;
}
static uint32_t g_rd(uint32_t offset) {
 if(full_fifo && offset==g_desc_off+4u)return g_tx_len-16u;
 if(offset==g_desc_off+4u || offset==g_desc_off) return 0;
 assert(0);return 0;
}
static int g_prm_vote_clock(uint32_t id,uint32_t hz,uint32_t root) {
 (void)id;(void)hz;(void)root;return clock_failure?-1:0;
}
static int g_hal_listen_start(void) {
 listen_start_calls++;return listen_start_failure?-1:0;
}
static void g_hal_listen_stop(void) {listen_stop_calls++;}
static int g_apm_shm_map(uint32_t src) {
 assert(src==0x2002);g_shm_mapped=1;return 0;
}
static uint32_t g_shm_region_size(void) {return 0x5000;}
static void g_audio_graph_cfg_vp(uint32_t source) {assert(source==0x2010);}
static void g_send_tx_data(uint32_t iid,const uint8_t *packet,uint32_t bytes);
static int PE_parse_boot_argn(const char *name,void *out,int size) {
 static const char *ones[]={"sweethalbridge","sweetpcm","sweetdescsep","sweets1","sweetiova","sweetshmhi"};
 int value=0;assert(size==4);
 for(unsigned i=0;i<sizeof(ones)/sizeof(ones[0]);i++) if(!strcmp(name,ones[i]))value=1;
 if(!strcmp(name,"sweettdmvendor"))value=7;
 if(!strcmp(name,"sweetshmflag"))value=4;
 if(!strcmp(name,"sweets1ap"))value=3;
 if(bad_flag && !strcmp(name,bad_flag))value=bad_flag_value;
 memcpy(out,&value,4);return 1;
}
'''

MOCKS_AND_TESTS = r'''
static void enqueue(const uint8_t *packet,uint32_t bytes) {
 assert(event_write<256 && bytes<=68);
 events[event_write].bytes=bytes;
 memcpy(events[event_write++].packet,packet,bytes);
}
static int g_drain(unsigned budget) {
 (void)budget;
 int n=0;
 while(event_read<event_write) {
  struct message *m=&events[event_read++];
  remote.in_use=0;
  if(!g_hal_receive(m->packet,m->bytes)) {
   memcpy(g_rsp,m->packet,m->bytes);g_rsp_len=m->bytes;g_rsp_ready=1;
  }
  n++;
 }
 return n;
}
static void g_send_tx_data(uint32_t iid,const uint8_t *packet,uint32_t bytes) {
 assert(iid==112 && bytes>=28);
 uint32_t op=g_le32(packet+20),src=g_le32(packet+8),dst=g_le32(packet+12);
 uint32_t token=g_le32(packet+16);
 uint8_t reply[68];memset(reply,0,sizeof(reply));
 g_put32(reply+4,0x203);g_put32(reply+8,dst);g_put32(reply+12,src);g_put32(reply+16,token);
 if(op==0x0400100a) {
  wire_data++;
  assert(bytes==68 && g_le32(packet+36)==4096);
  uint64_t address=g_le32(packet+24)|(uint64_t)g_le32(packet+28)<<32;
  assert(address<0x4000 && !(address&4095));
  const int16_t *samples=(const int16_t *)(fake_memory+(uint32_t)address);
  for(unsigned i=0;i<2048;i++)
   assert(samples[i]>=-(int32_t)stream_queue.ceiling &&
          samples[i]<=(int32_t)stream_queue.ceiling);
  assert(!memcmp(fake_memory+0x4000,g_graph_vp_wr_dec_cnv,856));
  g_put32(reply,0x60|(56u<<8));g_put32(reply+20,0x05001004);
  memcpy(reply+24,packet+24,12);
  if(mode==2)g_put32(reply+36,3);
  if(mode==3)return; /* DSP never returns the memory */
  if(mode==5){remote.in_use=0;assert(g_hal_receive(reply,56)==1);return;}
  if(mode==6) {
   uint8_t wrong[56];memcpy(wrong,reply,56);g_put32(wrong+16,token^4u);
   enqueue(wrong,56);
  }
  enqueue(reply,56);
 } else {
  wire_controls++;
  assert(dst==1);
  if(op==0x01001000) {
   assert(bytes==40 && g_le32(packet+24)==0x4000 && g_le32(packet+36)==856);
  } else if(op==0x0100100d) {
   assert(bytes==28 && src==0x2002 && g_le32(packet+24)==g_shm_handle);
  } else {
   assert(bytes==64 && g_le32(packet+36)==24 && g_le32(packet+40)==1);
   assert(g_le32(packet+44)==APM_PARAM_ID_SUB_GRAPH_LIST);
   assert(g_le32(packet+48)==8 && g_le32(packet+56)==1 && g_le32(packet+60)==0xb0000002);
  }
  g_put32(reply,0x60|(32u<<8));g_put32(reply+20,0x02001005);
  g_put32(reply+24,op);g_put32(reply+28,(ctrl_failure || op==fail_opcode)?3:0);
  if(op==drop_opcode){remote.in_use=0;return;}
  uint8_t noise[32];memcpy(noise,reply,32);g_put32(noise+16,token^0x10u);
  enqueue(noise,32);enqueue(reply,32);enqueue(noise,32); /* mailbox must survive later traffic */
 }
}
static void reset(int new_mode) {
 memset(&stream_queue,0,sizeof(stream_queue));
 stream_active=stream_once=0;stream_sent=0;
 virtual_us=0;event_read=event_write=wire_data=wire_controls=0;
 g_shm_va=(vm_offset_t)(uintptr_t)fake_memory;
 g_shm_pa=0x11ffd0000ull;g_shm_mapped=0;g_shm_offset_mode=1;g_shm_handle=0xb0d7f408;
 remote.in_use=0;mode=new_mode;muted=0;supply=1;clock_failure=ctrl_failure=0;
 fail_opcode=drop_opcode=0;full_fifo=0;
 listen_start_calls=listen_stop_calls=listen_start_failure=0;
 g_hal_ctrl_pending=g_hal_ctrl_ready=0;g_rsp_ready=0;g_rsp_len=0;
 g_hal_started_once=0;
 log_length=0;logs[0]=0;bad_flag=NULL;bad_flag_value=0;g_tx_len=8192;
 memset(fake_memory,0xa5,sizeof(fake_memory));
}
static void ok(int condition){assert(condition);case_count++;}
int main(void) {
 reset(0);ok(g_hal_guards()==1);g_hal_bringup();
 ok(wire_data==30 && stream_queue.completions==30 && stream_sent==30);
 ok(stream_queue.peak_in==32768 && stream_queue.peak_out==1024);
 ok(g_shm_mapped==0 && !stream_active);
 ok(strstr(logs,"end result=0")!=NULL);
 ok(strstr(logs,"raw frame=0 off=0 bytes=64")!=NULL);
 g_hal_bringup();ok(wire_data==30 && wire_controls==6); /* no hidden restart */

 reset(1);supply=0;g_hal_bringup();
 ok(wire_data==0 && virtual_us>=120000000 && virtual_us<121000000);
 ok(strstr(logs,"no non-silent")!=NULL && g_shm_mapped==0);

 reset(2);g_hal_bringup();
 ok(stream_queue.fault && wire_data==1 && stream_sent==1);
 ok(strstr(logs,"end result=-1")!=NULL && g_shm_mapped==0);

 reset(3);g_hal_bringup();
 ok(stream_queue.fault && wire_data==4 && stream_queue.completions==0);
 ok(g_shm_mapped==0 && !stream_active);

 reset(4);g_hal_bringup();
 ok(wire_data==30 && stream_queue.source_frames==30u*128u);
 ok(stream_queue.silence_frames==30u*(1024u-128u));

 reset(5);g_hal_bringup();
 ok(wire_data==30 && stream_queue.completions==30 && stream_sent==30);

 reset(6);g_hal_bringup();
 ok(wire_data==30 && stream_queue.rejected==30 && stream_queue.completions==30);

 reset(0);ctrl_failure=1;g_hal_bringup();
 ok(wire_data==0 && g_shm_mapped==1); /* absent ack => no pretend release */

 reset(0);drop_opcode=0x01001000;g_hal_bringup();
 ok(wire_data==0 && wire_controls==1 && g_shm_mapped==1);

 reset(0);fail_opcode=0x01001003;g_hal_bringup();
 ok(wire_data==30 && wire_controls==5 && g_shm_mapped==1 && stream_active);

 reset(0);fail_opcode=0x01001004;g_hal_bringup();
 ok(wire_data==30 && wire_controls==5 && g_shm_mapped==1 && stream_active);

 reset(0);fail_opcode=0x0100100d;g_hal_bringup();
 ok(wire_data==30 && wire_controls==6 && g_shm_mapped==1 && stream_active);

 reset(0);full_fifo=1;g_hal_bringup();
 ok(wire_data==0 && wire_controls==0 && g_shm_mapped==1);

 reset(0);bad_flag="sweetlisten";bad_flag_value=1;g_hal_bringup();
 ok(wire_data==30 && listen_start_calls==1 && listen_stop_calls==1);
 ok(stream_queue.divisor==1 && stream_queue.ceiling==16384);
 ok(stream_queue.peak_out==16384);

 reset(0);bad_flag="sweetlisten";bad_flag_value=1;listen_start_failure=1;g_hal_bringup();
 ok(wire_data==0 && listen_start_calls==1 && listen_stop_calls==0);

 reset(2);bad_flag="sweetlisten";bad_flag_value=1;g_hal_bringup();
 ok(wire_data==1 && listen_start_calls==1 && listen_stop_calls==1);

 reset(3);bad_flag="sweetlisten";bad_flag_value=1;g_hal_bringup();
 ok(wire_data==4 && listen_start_calls==1 && listen_stop_calls==1);

 reset(0);clock_failure=1;g_hal_bringup();
 ok(wire_data==0 && wire_controls==0);

 reset(0);g_shm_mapped=1;
 int16_t data[2048];memset(data,0,sizeof(data));
 ok(g_hal_store(0x4000,data,4096)==-1);
 ok(g_hal_store(1,data,4096)==-1);
 ok(g_hal_store(0,data,4092)==-1);
 bad_flag="sweetamp";bad_flag_value=1;ok(g_hal_guards()==0);
 bad_flag="sweetbatt";ok(g_hal_guards()==0);
 bad_flag="sweetdescsep";bad_flag_value=0;ok(g_hal_guards()==0);
 bad_flag="sweetshmflag";bad_flag_value=0;ok(g_hal_guards()==0);
 bad_flag="sweetshmsize";bad_flag_value=0x8000;ok(g_hal_guards()==0);
 bad_flag="sweetgmuboot";bad_flag_value=1;ok(g_hal_guards()==0);
 bad_flag="sweets1ap";bad_flag_value=1;ok(g_hal_guards()==0);
 bad_flag_value=0;ok(g_hal_guards()==0);
 printf("{\"passed\":true,\"checks\":%u,\"actual_adapter\":true,\"dsp\":\"mock-only\"}\n",case_count);
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    source = GLINK.read_text()
    match = re.search(
        r"static const uint8_t g_graph_vp_wr_dec_cnv\[856\]\s*=\s*\{.*?\};",
        source, re.S)
    if match is None:
        raise ValueError("actual 856B graph declaration missing")
    names = [
        "g_hal_receive", "g_hal_now_us", "g_hal_wait_ms", "g_hal_poll",
        "g_hal_tx_free", "g_hal_send", "g_hal_store", "g_hal_xfer",
        "g_hal_graph_ctrl", "g_hal_unmap", "g_hal_guards", "g_hal_bringup",
    ]
    code = SUPPORT + "\n"
    code += body((STREAM / "sweet_audio_periods.h").read_text()) + "\n"
    code += body((STREAM / "sweet_audio_stream.h").read_text()) + "\n"
    code += body((STREAM / "sweet_audio_periods.c").read_text()) + "\n"
    code += body((STREAM / "sweet_audio_stream.c").read_text()) + "\n"
    code += match.group() + "\n"
    code += "\n".join(function(source, name) for name in names)
    code += "\n" + MOCKS_AND_TESTS
    with tempfile.TemporaryDirectory(prefix="halstream-host-") as folder:
        cfile, exe = Path(folder) / "test.c", Path(folder) / "test"
        corefile, coreexe = Path(folder) / "core.c", Path(folder) / "core"
        core_test = (ROOT / "tools/audio-models/test_periods.c").read_text()
        core_test = re.sub(r"\bmp_", "marble_pcm_", core_test)
        core_test = re.sub(r"\bMP_", "MARBLE_PCM_", core_test)
        core_code = (
            "#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n"
            "#include <limits.h>\n#include <string.h>\n" +
            body((STREAM / "sweet_audio_periods.h").read_text()) + "\n" +
            body((STREAM / "sweet_audio_periods.c").read_text()) + "\n" +
            body(core_test)
        )
        corefile.write_text(core_code)
        subprocess.run([
            "clang", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
            "-fsanitize=address,undefined", str(corefile), "-o", str(coreexe),
        ], check=True)
        core_run = subprocess.run([str(coreexe)], check=True, capture_output=True, text=True)
        cfile.write_text(code)
        subprocess.run([
            "clang", "-std=gnu11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
            "-fsanitize=address,undefined", str(cfile), "-o", str(exe),
        ], check=True)
        completed = subprocess.run([str(exe)], capture_output=True, text=True)
        if completed.returncode:
            raise RuntimeError(
                "adapter host failed (%d)\nstdout:\n%s\nstderr:\n%s" %
                (completed.returncode, completed.stdout, completed.stderr))
    result = json.loads(completed.stdout.splitlines()[-1])
    result["production_core"] = json.loads(core_run.stdout)
    result["scope"] = "actual new core/runtime/adapter; old transport/clock and HAL ring mocked"
    result["functions"] = names
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "adapter-host.log").write_text(completed.stdout)
        (args.out_dir / "adapter-host.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
