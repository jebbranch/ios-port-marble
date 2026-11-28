// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include "nw_abi.h"
#include "nw_control_endpoint.h"
#include "MarbleWifi.h"
#include "MarbleWifiFirmware.h"

extern "C" {
int armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
extern uint8_t *native_ufs_extra_va;
extern volatile uint32_t native_ufs_extra_bytes,native_ufs_extra_iova,native_ufs_extra_stream,native_ufs_extra_uncached;
extern volatile uint32_t native_ufs_extra_atos[8];
extern uint32_t native_marble_wifi_smmu[16];
extern uint32_t native_marble_wifi_fault[8];
void marble_wifi_dma_capture(void);
extern volatile int native_gpu_addon_stage;
volatile uint32_t native_marble_wifi_runtime_stats[16];
char native_marble_wifi_text[128][160];
volatile uint32_t native_marble_wifi_text_count;
uint32_t native_marble_wifi_bhi[16],native_marble_wifi_pci[8];
void marble_wifi_wake(void);
}
namespace {
constexpr unsigned kArenaBlocks=16384;
#include "MarbleWifiArena.inc"
volatile uint32_t *gConfig,*gDbi;
volatile uint32_t *gBar;
IOMemoryMap *gDbiMap;
unsigned gMsiCap,gPcieCap;
volatile uint32_t gPending,gWake;
IOService *gDiagnostics;
void publishDiagnostics() {
    if (!gDiagnostics) return;
    gDiagnostics->setProperty("BoardStats",const_cast<uint32_t *>(native_marble_wifi_stats),24*4);
    gDiagnostics->setProperty("RuntimeStats",const_cast<uint32_t *>(native_marble_wifi_runtime_stats),16*4);
    gDiagnostics->setProperty("SmmuRegisters",native_marble_wifi_smmu,16*4);
    gDiagnostics->setProperty("AtosStats",const_cast<uint32_t *>(native_ufs_extra_atos),8*4);
    gDiagnostics->setProperty("DmaFault",native_marble_wifi_fault,8*4);
    gDiagnostics->setProperty("BhiRegisters",native_marble_wifi_bhi,16*4);
    gDiagnostics->setProperty("PciRegisters",native_marble_wifi_pci,8*4);
    gDiagnostics->setProperty("LogCount",static_cast<unsigned long long>(native_marble_wifi_text_count),32);
    gDiagnostics->setProperty("Logs",native_marble_wifi_text,sizeof(native_marble_wifi_text));
}
uint64_t nowNs() {uint64_t n; absolutetime_to_nanoseconds(mach_absolute_time(),&n); return n;}
uint64_t bootNs() {uint64_t n; absolutetime_to_nanoseconds(mach_continuous_time(),&n); return n;}
uint64_t nowMs() {return nowNs()/1000000;}
void sleepMs(unsigned ms) {IOSleep(ms);}
void busyUs(unsigned us) {IODelay(us);}
void logLine(int level,const char *line) {
    if (gBar && strnstr(line,"MHI did not load image",strlen(line))) {
        marble_wifi_dma_capture();
        uint32_t bhi=gBar[0x28/4];
        if (bhi && bhi<0x200000-64 && !(bhi&3))
            for(unsigned i=0;i<16;++i) native_marble_wifi_bhi[i]=gBar[bhi/4+i];
        native_marble_wifi_pci[0]=gConfig[4/4]; native_marble_wifi_pci[1]=gConfig[0x10/4];
        native_marble_wifi_pci[2]=gDbi[0x80/4]; native_marble_wifi_pci[3]=gDbi[0x830/4];
        native_marble_wifi_pci[4]=gBar[0x38/4]; native_marble_wifi_pci[5]=gBar[0x48/4];
        native_marble_wifi_pci[6]=gDbi[0x104/4]; native_marble_wifi_pci[7]=gDbi[0x110/4];
    }
    unsigned at=__atomic_fetch_add(&native_marble_wifi_text_count,1u,__ATOMIC_RELAXED)%128;
    snprintf(native_marble_wifi_text[at],160,"%llu %d %.130s",nowMs(),level,line);
    if (level<=6) IOLog("MarbleWifi %s\n",line);
}
void *heapAlloc(size_t n) {auto *p=static_cast<size_t *>(IOMallocData(n+16)); if (!p) return nullptr; *p=n+16; return reinterpret_cast<uint8_t *>(p)+16;}
void heapFree(void *p) {if (p) {auto *base=static_cast<uint8_t *>(p)-16; IOFreeData(base,*reinterpret_cast<size_t *>(base));}}
void halt() {native_marble_wifi_runtime_stats[0]=0xffffffffu; for (;;) IOSleep(1000);}
void *dmaAlloc(size_t n,uint64_t *bus) {void *p=arenaAlloc(n,bus);native_marble_wifi_runtime_stats[5]=gArena.used;native_marble_wifi_runtime_stats[6]=gArena.peak;native_marble_wifi_runtime_stats[7]=gArena.failures;return p;}
void dmaFree(size_t,void *p,uint64_t) {arenaFree(p);native_marble_wifi_runtime_stats[5]=gArena.used;}
uint64_t dmaMap(void *p,size_t n,int) {
    auto address=reinterpret_cast<uintptr_t>(p),start=reinterpret_cast<uintptr_t>(gArena.va);
    if (address<start || address-start>gArena.bytes || n>gArena.bytes-(address-start)) return 0;
    return gArena.iova+address-start;
}
void dmaSync(uint64_t,size_t,int) {__asm__ volatile("dsb sy" ::: "memory");}
struct Firmware {char name[32]; uint8_t *data; size_t size;} gFirmware[5];
int firmwareGet(const char *path,const uint8_t **data,size_t *size) {
    const char *name=path;
    for (const char *p=path; *p; ++p) if (*p=='/') name=p+1;
    if (!strcmp(name,"wlan_mac.bin")) {
        static char mac[32]; static size_t length;
        if (!length) {
            IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
            OSData *id=chosen ? OSDynamicCast(OSData,chosen->getProperty("unique-chip-id")) : nullptr;
            uint64_t seed=0;
            if (id && id->getLength()>=8) memcpy(&seed,id->getBytesNoCopy(),8);
            if (chosen) chosen->release();
            if (!seed) return -ENOENT;
            uint64_t hash=0xcbf29ce484222325ull;
            for (unsigned i=0; i<8; ++i) hash=(hash^((seed>>(i*8))&255))*0x100000001b3ull;
            uint8_t bytes[6]; for (unsigned i=0; i<6; ++i) bytes[i]=hash>>(i*8);
            bytes[0]=(bytes[0]&0xfe)|2;
            length=snprintf(mac,sizeof(mac),"address=%02x%02x%02x%02x%02x%02x\n",bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5]);
        }
        *data=reinterpret_cast<const uint8_t *>(mac); *size=length; return 0;
    }
    for (auto &f:gFirmware) if (f.data && !strcmp(name,f.name)) {*data=f.data; *size=f.size; return 0;}
    for (auto &f:gFirmware) if (!f.data) {
        int error=marble_wifi_firmware_read(name,&f.data,&f.size);
        native_marble_wifi_runtime_stats[12]=error;
        if (error) return -error;
        strlcpy(f.name,name,sizeof(f.name)); *data=f.data; *size=f.size;
        native_marble_wifi_runtime_stats[13]+=f.size;
        return 0;
    }
    return -ENOMEM;
}
int configRead(int offset,int size,uint32_t *value) {
    uint32_t at=offset;
    if (at&0x80000000u) at=gPcieCap+(at&0x7fffffffu);
    uint32_t word=gConfig[(at&~3u)/4];
    *value=size==4 ? word : (word>>((at&3)*8)) & (size==2 ? 0xffffu : 0xffu);
    return 0;
}
int configWrite(int offset,int size,uint32_t value) {
    uint32_t at=offset;
    if (at&0x80000000u) at=gPcieCap+(at&0x7fffffffu);
    if (size==4) gConfig[at/4]=value;
    else {unsigned shift=(at&3)*8; uint32_t mask=(size==2?0xffffu:0xffu)<<shift;
        gConfig[(at&~3u)/4]=(gConfig[(at&~3u)/4]&~mask)|((value<<shift)&mask);}
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int msiTarget(uint32_t *lo,uint32_t *hi,uint32_t *data) {*lo=gArena.iova; *hi=0; *data=0; return 0;}
void msiInterrupt(void *) {
    uint32_t bits=gDbi[0x830/4];
    gDbi[0x830/4]=bits;
    __asm__ volatile("dsb sy" ::: "memory");
    native_marble_wifi_runtime_stats[2]++;
    native_marble_wifi_runtime_stats[3]|=bits;
    __atomic_fetch_or(&gPending,bits,__ATOMIC_RELEASE);
    __atomic_store_n(&gWake,1,__ATOMIC_RELEASE);
    thread_wakeup(event_t(&gWake));
}
void poll() {nw_wifi_irq_mode(1);nw_wifi_irq_pending(__atomic_exchange_n(&gPending,0,__ATOMIC_ACQ_REL));nw_wifi_poll();}
void wait(unsigned ms) {
    if (__atomic_exchange_n(&gWake,0,__ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(event_t(&gWake),THREAD_UNINT,ms,NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gWake,0,__ATOMIC_ACQ_REL)) {clear_wait(current_thread(),THREAD_AWAKENED); return;}
    thread_block(THREAD_CONTINUE_NULL);
}
#include "MarbleWifiControl.inc"
}
extern "C" void native_wifi_logf(int level,const char *format,...) __attribute__((format(printf,2,3)));
extern "C" void native_wifi_logf(int level,const char *format,...) {
    char line[160]; va_list ap; va_start(ap,format); vsnprintf(line,sizeof(line),format,ap); va_end(ap); logLine(level,line);
}
extern "C" void marble_wifi_wake(void) {__atomic_store_n(&gWake,1u,__ATOMIC_RELEASE); thread_wakeup(event_t(&gWake));}
extern "C" void marble_wifi_runtime(volatile uint32_t *bar,size_t bytes,volatile uint32_t *cfg,volatile uint32_t *dbi,unsigned requested) {
    auto *st=native_marble_wifi_runtime_stats;
    st[0]=1;
    IOService *diagnostics=new IOService;
    if (diagnostics && diagnostics->init() && diagnostics->attach(IOService::getPlatform()) && diagnostics->start(IOService::getPlatform())) {
        gDiagnostics=diagnostics;
        gDiagnostics->setName("MarbleWifiDiagnostics"); publishDiagnostics(); gDiagnostics->registerService();
    } else if (diagnostics) diagnostics->release();
    // The storage owner installs the PCIe stream while its SMMU registers are accessible.
    for (unsigned ms=0; ms<15000 && !__atomic_load_n(&native_ufs_extra_stream,__ATOMIC_ACQUIRE); ms+=10) IOSleep(10);
    st[4]=native_ufs_extra_bytes; st[15]=native_ufs_extra_stream;
    if (!arenaInit() || native_ufs_extra_atos[1] || native_ufs_extra_atos[2]) {st[0]=-1; st[1]=ENXIO; return;}
    const bool hardwareAtos=(native_marble_wifi_smmu[14]&(1u<<30)) && !(native_marble_wifi_smmu[14]&(1u<<26));
    if (hardwareAtos && native_ufs_extra_atos[0]!=2*(native_ufs_extra_bytes/(2u<<20))) {st[0]=-1; st[1]=ENXIO; return;}
    st[0]=2;
    if (requested==3) return;
    gConfig=cfg;
    gBar=bar;
    for (unsigned at=cfg[0x34/4]&255,guard=0; at>=0x40 && guard<48; ++guard) {
        uint32_t v=cfg[at/4]; if ((v&255)==5) gMsiCap=at; if ((v&255)==0x10) gPcieCap=at; at=(v>>8)&255;
    }
    st[14]=gMsiCap;
    if (!gMsiCap || !gPcieCap) {st[0]=-3; st[1]=ENXIO; return;}
    IODeviceMemory *memory=IODeviceMemory::withRange(0x60000000,0x1000);
    gDbiMap=memory ? memory->map(kIOMapAnywhere|kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!gDbiMap) {st[0]=-3; st[1]=ENOMEM; return;}
    gDbi=reinterpret_cast<volatile uint32_t *>(gDbiMap->getVirtualAddress());
    gDbi[0x828/4]=0;
    int error=armcpu_spi_attach(32+141,false,msiInterrupt,nullptr,nullptr,nullptr);
    if (error) {st[0]=-3; st[1]=error; return;}
    gDbi[0x820/4]=gArena.iova; gDbi[0x824/4]=0;
    gDbi[0x830/4]=0xffffffffu; gDbi[0x82c/4]=0; gDbi[0x828/4]=0xffffffffu;
    cfg[4/4]=(cfg[4/4]&0xffffu)|6u;
    st[0]=3;
    for (unsigned ms=0; ms<30000 && native_gpu_addon_stage<1; ms+=50) IOSleep(50);
    static const lx_platform platform={nowMs,sleepMs,poll,logLine,halt,heapAlloc,heapFree,
        dmaAlloc,dmaFree,dmaMap,dmaSync,firmwareGet,nowNs,bootNs,busyUs,_disable_preemption,_enable_preemption};
    static nw_board board;
    board.bar=const_cast<uint32_t *>(bar); board.bar_len=bytes; board.device_id=cfg[0]>>16;
    board.msi_cap=gMsiCap; board.irq_base=1000;
    board.cfg_read=configRead; board.cfg_write=configWrite; board.msi_target=msiTarget;
    nw_wifi_set_platform(&platform);
    error=controlStart();
    if (error) {st[0]=-4; st[1]=error; publishDiagnostics(); return;}
    error=nw_wifi_start(&platform,&board);
    if (error) {st[0]=-4; st[1]=-error; gDbi[0x828/4]=0; cfg[4/4]=(cfg[4/4]&0xffffu)&~4u; publishDiagnostics(); return;}
    st[0]=4;
    nw_wifi_irq_mode(1);
    bool networkStarted=false;
    uint64_t slice=nowNs();
    uint64_t nextReport=0;
    for (;;) {
        bool busy=nw_wifi_step();
        st[5]=gArena.used; st[6]=gArena.peak; st[7]=gArena.failures;
        st[8]=nw_wifi_stats.stage; st[9]=nw_wifi_stats.last_err;
        st[10]=nw_wifi_stats.scans; st[11]=nw_wifi_bss_count();
        if (!networkStarted && nw_wifi_stats.stage>=4) {
            nw_wifi_status status{}; nw_wifi_status_get(&status);
            marble_wifi_net_start(status.mac);
            nw_wifi_power(1); nw_wifi_scan(nullptr,0); networkStarted=true; st[0]=5;
        }
        if (nowMs()>=nextReport) {publishDiagnostics(); nextReport=nowMs()+1000;}
        if (!busy) {unsigned ms=nw_wifi_wait_ms(1000); if (ms) wait(ms); slice=nowNs();}
        else if (nowNs()-slice>=2000000) {thread_yield_to_preemption(); slice=nowNs();}
    }
}
