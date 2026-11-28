// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7475/QCA6490 board bring-up. PCIe sequencing follows UniXNU-sheng
// WifiProbe.cpp and Xiaomi marble-s-oss pci-msm.c / gcc-waipio.c.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "MarbleWifi.h"
#include "MarbleWifiPhy.h"
#include "QcomRpmh.h"
#include "MarbleWifiFirmware.h"

extern "C" {volatile uint32_t native_marble_wifi_stats[24];}
namespace {
struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    bool open(uint64_t address, uint64_t bytes) {
        IODeviceMemory *memory = IODeviceMemory::withRange(address, bytes);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (memory) memory->release();
        va = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
    ~Window() {if (map) map->release();}
    uint32_t read(uint32_t offset) const {return va[offset / 4];}
    void write(uint32_t offset, uint32_t value) {
        va[offset / 4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void set(uint32_t offset, uint32_t mask) {write(offset, read(offset) | mask);}
    void clear(uint32_t offset, uint32_t mask) {write(offset, read(offset) & ~mask);}
};
enum {StMapped=1, StRails=2, StClocks=3, StPhy=4, StLink=5, StEndpoint=6, StIdentified=7};
constexpr uint32_t kBranches[] = {0x7b020, 0x7b028, 0x7b030, 0x7b034, 0x7b03c, 0x7b01c, 0x7b08c, 0x7b044, 0x9d098, 0x9d094};
void stage(unsigned value) {
    native_marble_wifi_stats[0] = value;
    IOLog("MarbleWifi stage=%u\n", value);
}
void fail(unsigned value, int error) {
    native_marble_wifi_stats[1] = error;
    native_marble_wifi_stats[0] = -value;
    IOLog("MarbleWifi failed stage=%u error=%d\n", value, error);
}
bool waitBits(const Window &w, uint32_t offset, uint32_t mask, uint32_t value, unsigned ms) {
    for (unsigned i=0; i<ms; ++i) {
        if ((w.read(offset) & mask) == value) return true;
        IOSleep(1);
    }
    return (w.read(offset) & mask) == value;
}
void pin(Window &tlmm, unsigned gpio, uint32_t config, int level) {
    if (level >= 0) tlmm.write(gpio * 0x1000 + 4, level ? 2 : 0);
    tlmm.write(gpio * 0x1000, config);
}
uint32_t halted(const Window &gcc) {
    uint32_t bits=0;
    for (unsigned i=0; i<sizeof(kBranches)/sizeof(kBranches[0]); ++i)
        if (gcc.read(kBranches[i]) & (1u<<31)) bits |= 1u<<i;
    native_marble_wifi_stats[5]=bits;
    return bits;
}
// TCS0 belongs to Wi-Fi; ADSP uses TCS1. Poll the TCS command completion
// without enabling a second owner on the ADSP interrupt line.
bool vote(Window &rsc, const QcomRscLayout &layout, uint32_t address, uint32_t value) {
    constexpr unsigned tcs=0;
    QcomRpmhWrite writes[16];
    unsigned n=qcom_rpmh_active_request(&layout,tcs,&address,&value,1,writes,16);
    for (unsigned i=0; i<n; ++i) rsc.write(0xd00+writes[i].offset,writes[i].value);
    bool done=waitBits(rsc,0xd00+layout.cmd_msgid+0xc,1u<<16,1u<<16,100);
    native_marble_wifi_stats[3]=rsc.read(0xd00+layout.cmd_msgid+0xc);
    if (!done) return false;
    n=qcom_rpmh_finish(&layout,tcs,writes,16);
    for (unsigned i=0; i<n; ++i) rsc.write(0xd00+writes[i].offset,writes[i].value);
    return true;
}
bool power() {
    Window database, rsc;
    if (!database.open(0x80860000,0x20000) || !rsc.open(0x17a20000,0x10000)) return false;
    auto *copy=static_cast<uint8_t *>(IOMallocData(0x20000));
    if (!copy) return false;
    for (unsigned i=0; i<0x20000; i+=4) {uint32_t v=database.read(i); memcpy(copy+i,&v,4);}
    QcomRscLayout layout=qcom_rsc_layout(rsc.read(0));
    struct Rail {const char *name; unsigned mv;};
    // DT minimum setpoints. PCIe analog supplies are L5B=0.88 V, L10C=1.2 V.
    constexpr Rail rails[]={{"smpe2",1012},{"smpb11",966},{"smpb10",1800},
        {"smpc1",1900},{"smpb12",1350},{"ldoe7",2800},{"ldob5",880},{"ldoc10",1200}};
    bool ok=true;
    for (unsigned i=0; ok && i<sizeof(rails)/sizeof(rails[0]); ++i) {
        QcomRpmhResource res={};
        ok=!qcom_cmd_db_find(copy,0x20000,rails[i].name,&res);
        if (ok) ok=vote(rsc,layout,res.address+8,7) && vote(rsc,layout,res.address,rails[i].mv)
            && vote(rsc,layout,res.address+4,1);
        if (ok) native_marble_wifi_stats[2] |= 1u<<i;
        IOLog("MarbleWifi rail=%s address=%x mv=%u result=%u\n",rails[i].name,res.address,rails[i].mv,ok);
    }
    const char *arcs[]={"cx.lvl","mx.lvl","xo.lvl"};
    for (unsigned i=0; ok && i<3; ++i) {
        QcomRpmhResource res={}; uint32_t index=3;
        ok=!qcom_cmd_db_find(copy,0x20000,arcs[i],&res);
        if (ok && i<2) ok=!qcom_rpmh_level_index(&res,0x100,&index);
        if (ok) ok=vote(rsc,layout,res.address,index);
    }
    QcomRpmhResource bcm={};
    if (ok) ok=!qcom_cmd_db_find(copy,0x20000,"SN7",&bcm);
    if (ok) ok=vote(rsc,layout,bcm.address,(1u<<30)|(1u<<29)|(0x400u<<14)|0x400u);
    IOFreeData(copy,0x20000);
    return ok;
}
bool atu(Window &a, unsigned region, uint32_t type, uint32_t cpu, uint32_t bytes, uint32_t target) {
    const uint32_t r=region*0x200;
    a.write(r+8,cpu); a.write(r+12,0); a.write(r+16,cpu+bytes-1);
    a.write(r+32,0); a.write(r+20,target); a.write(r+24,0);
    a.write(r,type); a.write(r+4,1u<<31);
    return waitBits(a,r+4,1u<<31,1u<<31,10);
}
bool endpoint(Window &a, Window &dbi, unsigned requested) {
    Window cfg;
    if (!atu(a,0,4,0x60100000,0x100000,1u<<24) || !cfg.open(0x60100000,0x1000)) return false;
    uint32_t id=cfg.read(0);
    native_marble_wifi_stats[9]=id;
    native_marble_wifi_stats[10]=cfg.read(0x2c);
    if ((id&0xffff)!=0x17cb) return false;
    // Memory decoding is enabled after BAR assignment. DMA waits for its
    // SMMU and interrupt integration in the ath11k stage.
    const uint32_t command=cfg.read(4)&0xffff;
    cfg.write(4,command & ~6u);
    const uint32_t saved=cfg.read(0x10);
    cfg.write(0x10,0xffffffffu);
    const uint32_t mask=cfg.read(0x10);
    cfg.write(0x10,saved);
    const uint32_t size=~(mask&~15u)+1;
    if ((mask&1) || !(mask&~15u) || !size || size>0x2000000) {cfg.write(4,command); return false;}
    const uint32_t base=(0x60300000u+size-1)&~(size-1);
    if (base+size>0x64000000u) {cfg.write(4,command); return false;}
    if (!atu(a,1,0,base,size,base)) {cfg.write(4,command); return false;}
    dbi.write(0x20,((base+size-1)&0xfff00000)|((base>>16)&0xfff0));
    cfg.write(0x10,base|(saved&15));
    if ((mask&6)==4) cfg.write(0x14,0);
    cfg.write(4,(command&~4u)|2u);
    native_marble_wifi_stats[11]=cfg.read(0x10);
    native_marble_wifi_stats[12]=size;
    native_marble_wifi_stats[19]=cfg.read(4)&0xffff;
    native_marble_wifi_stats[22]=cfg.read(0x34)&0xff;
    Window bar;
    if (!bar.open(base,size)) return false;
    native_marble_wifi_stats[13]=bar.read(0x224);
    native_marble_wifi_stats[15]=bar.read(8);
    native_marble_wifi_stats[16]=bar.read(0x48);
    const uint32_t bhi=bar.read(0x28);
    native_marble_wifi_stats[17]=bhi;
    if (bhi && bhi<=size-0x30) native_marble_wifi_stats[18]=bar.read(bhi+0x28);
    if ((id>>16)==0x1103 && size>=0x100000) {
        const uint32_t savedWindow=bar.read(0x310c);
        bar.write(0x310c,(1u<<30)|((0x1910010u>>19)&0x3f));
        (void)bar.read(0x310c);
        native_marble_wifi_stats[14]=bar.read(0x80000+(0x1910010u&0x7ffff));
        bar.write(0x310c,savedWindow);
    }
    IOLog("MarbleWifi endpoint=%08x subsystem=%08x BAR=%08x/%x SoC=%08x subversion=%08x\n",
        id,native_marble_wifi_stats[10],base,size,native_marble_wifi_stats[13],native_marble_wifi_stats[14]);
    stage(StIdentified);
    if (requested>=3) marble_wifi_runtime(bar.va,size,cfg.va,dbi.va,requested);
    return true;
}
void run(void *arg, wait_result_t) {
    thread_set_thread_name(current_thread(),"marble-wifi");
    const unsigned requested=static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
    Window gcc,tlmm;
    if (!gcc.open(0x100000,0x100000) || !tlmm.open(0xf100000,0x100000)) {fail(StMapped,ENOMEM); return;}
    stage(StMapped);
    native_marble_wifi_stats[4]=gcc.read(0x7b004);
    halted(gcc);
    native_marble_wifi_stats[20]=tlmm.read(80*0x1000+4);
    native_marble_wifi_stats[21]=tlmm.read(94*0x1000+4);
    if (requested==1) return;
    if (!power()) {fail(StRails,EIO); return;}
    stage(StRails);
    pin(tlmm,94,1u|(1u<<9),0);
    pin(tlmm,95,3u|(1u<<2),-1);
    pin(tlmm,80,3u|(7u<<6)|(1u<<9),0);
    IOSleep(150);
    // QCA6490 requires XO asserted before WLAN_EN, then latched for 2–5 ms.
    pin(tlmm,204,1u<<9,1);
    IODelay(200);
    pin(tlmm,80,3u|(7u<<6)|(1u<<9),1);
    IOSleep(4);
    pin(tlmm,204,1u<<9,0);
    IOSleep(60);
    gcc.clear(0x62128,1u);
    if (!waitBits(gcc,0x7b004,1u<<31,1u<<31,10)) {fail(StClocks,ETIMEDOUT); return;}
    gcc.write(0x7b068,0); gcc.set(0x7b064,1);
    // PHY rate-change uses the 19.2 MHz TCXO entry from gcc-waipio.c.
    gcc.write(0x7b04c,0); gcc.set(0x7b048,1);
    if (!waitBits(gcc,0x7b064,1,0,10) || !waitBits(gcc,0x7b048,1,0,10)) {fail(StClocks,ETIMEDOUT); return;}
    gcc.write(0x7b060,(gcc.read(0x7b060)&~3u)|2u);
    gcc.set(0x62008,0x3fu);
    gcc.set(0x62000,(1u<<11)|(1u<<12)|(1u<<19)|(1u<<22));
    gcc.set(0x9c004,1);
    IOSleep(1);
    if (halted(gcc)&0x2f) {fail(StClocks,EIO); return;}
    const uint32_t resets[]={0x7b000u,0x7c01cu};
    for (uint32_t reset : resets) {
        gcc.set(reset,1); IODelay(1000); gcc.clear(reset,1); IODelay(1000);
    }
    stage(StClocks);
    Window parf,phy,dbi,a;
    if (!parf.open(0x1c00000,0x3000) || !phy.open(0x1c06000,0x2000)) {fail(StPhy,ENOMEM); return;}
    parf.write(0x1000,4); parf.clear(0x40,1);
    parf.write(0x350,0); parf.write(0,0x365e);
    parf.set(0x174,1u<<4); parf.write(0x358,0x4000000); parf.set(0x1a8,1u<<31);
    for (const auto &entry : marble_wifi::kPhy) {
        phy.write(entry.offset,entry.value);
        if (entry.delay_us) IODelay(entry.delay_us);
    }
    gcc.clear(0x7b060,3);
    const bool ready=waitBits(phy,0x214,1u<<6,0,100);
    native_marble_wifi_stats[6]=phy.read(0x214);
    if (!ready) {fail(StPhy,ETIMEDOUT); return;}
    stage(StPhy);
    for (unsigned i=0; i<256; ++i) parf.write(0x2000+4*i,0);
    uint8_t hash=0;
    const uint8_t requester[]={1,0};
    for (uint8_t byte:requester) {
        hash^=byte;
        for (unsigned bit=0; bit<8; ++bit) hash=(hash&0x80) ? (hash<<1)^7 : hash<<1;
    }
    parf.write(0x2000+4*hash,(0x100u<<16)|(1u<<8));
    parf.clear(0x2c00,1);
    if (!dbi.open(0x60000000,0x1000) || !a.open(0x60001000,0x1000)) {fail(StLink,ENOMEM); return;}
    dbi.write(0x18,0x10100); dbi.write(4,(dbi.read(4)&0xffff)|6u);
    dbi.write(0xa0,(dbi.read(0xa0)&~15u)|2u); // Gen2, per CNSS QCA6490 enumeration.
    IOSleep(100);
    pin(tlmm,94,1u|(1u<<9),1); IOSleep(10);
    parf.set(0x1b0,1u<<8);
    const bool link=waitBits(dbi,0x80,1u<<29,1u<<29,1000);
    native_marble_wifi_stats[7]=dbi.read(0x728);
    native_marble_wifi_stats[8]=dbi.read(0x80);
    native_marble_wifi_stats[4]=gcc.read(0x7b004);
    native_marble_wifi_stats[20]=tlmm.read(80*0x1000+4);
    native_marble_wifi_stats[21]=tlmm.read(94*0x1000+4);
    if (!link) {fail(StLink,ETIMEDOUT); return;}
    stage(StLink);
    stage(StEndpoint);
    if (!endpoint(a,dbi,requested)) {fail(StEndpoint,ENXIO); return;}
    stage(StIdentified);
}
}
extern "C" void marble_wifi_start(void) {
    unsigned stage=0;
    if (!PE_parse_boot_argn("marble-wifi",&stage,sizeof(stage)) || stage<1 || stage>4) return;
    if (native_marble_wifi_stats[23]) return;
    native_marble_wifi_stats[23]=stage;
    thread_t thread=THREAD_NULL;
    if (kernel_thread_start(run,reinterpret_cast<void *>(static_cast<uintptr_t>(stage)),&thread)==KERN_SUCCESS)
        thread_deallocate(thread);
    else fail(StMapped,ENOMEM);
}
extern "C" unsigned marble_wifi_report(char *out, unsigned capacity) {
    unsigned at=0;
    if (capacity<16) return 0;
    at=snprintf(out,capacity,"MarbleWifi");
    for (unsigned i=0; i<24 && at+12<capacity; ++i)
        at+=snprintf(out+at,capacity-at," %08x",native_marble_wifi_stats[i]);
    if (at+1<capacity) out[at++]='\n';
    return at;
}
