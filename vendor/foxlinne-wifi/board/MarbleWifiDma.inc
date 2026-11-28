// SPDX-License-Identifier: BSD-2-Clause-Patent
// PCIe0 DMA setup is called by the UFS owner while its SMMU is accessible.
extern "C" {
uint8_t *native_ufs_extra_va;
volatile uint32_t native_ufs_extra_stream, native_ufs_extra_iova, native_ufs_extra_bytes, native_ufs_extra_uncached;
volatile uint32_t native_ufs_extra_atos[8];
uint32_t native_marble_wifi_smmu[16];
uint32_t native_marble_wifi_fault[8];
void marble_wifi_dma_capture(void);
}
static uintptr_t gWifiSmmu, gWifiContext;
extern "C" void marble_wifi_dma_capture(void) {
    if (!gWifiContext) return;
    const uint32_t offsets[]={0x58,0x60,0x64,0x68,0x6c};
    for (unsigned i=0;i<5;++i) native_marble_wifi_fault[i]=read32(gWifiContext,offsets[i]);
    native_marble_wifi_fault[5]=read32(gWifiSmmu,0x48);
    native_marble_wifi_fault[6]=read32(gWifiSmmu,0x50);
    native_marble_wifi_fault[7]=read32(gWifiSmmu,0x54);
}
static void marbleWifiDma(const uint32_t report[3]) {
    unsigned requested=0;
    if (!PE_parse_boot_argn("marble-wifi",&requested,sizeof(requested)) || requested<3) return;
    static OSSharedPtr<IOMemoryMap> extraMap;   // kept: the fault registers are read from it later
    uint32_t extraReport[3] = { 0, 0, 0 };
    smmuBypassStream (0x1c01, extraMap, extraReport, 0x9c900000ULL, 8ULL << 20);
    // The endpoint only produces 32-bit addresses and the memory here is above 4 GiB: give its context bank a stage 1 table (4 KiB granule,
    // 32-bit input, L1 -> L2 with 2 MiB blocks) that maps kExtraIova upward onto one contiguous buffer. Own bank only: refused when it is the storage's.
    // Mainline ath12k owns firmware memory, CE, DP refill and monitor rings.
    // The former 48 MiB window exhausted before HTC startup on WCN7850.
    constexpr uint32_t kExtraIova = 0xa0000000U;
    constexpr uint32_t kExtraBlocks = 64, kBlock = 2U << 20;
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
      static_cast<IOOptionBits> (kIOMemoryMapperNone) , (kExtraBlocks + 1) * kBlock, kBlock);
    IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
      static_cast<IOOptionBits> (kIOMemoryMapperNone), 8192, 4096);
    if (buffer && tables && (extraReport[0] >> 16) != (report[0] >> 16)) {
      uint8_t *bva = static_cast<uint8_t *> (buffer->getBytesNoCopy ());
      uint64_t bpa = buffer->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
      uint64_t shift = (kBlock - (bpa & (kBlock - 1))) & (kBlock - 1);
      bva += shift; bpa += shift;
      bzero (buffer->getBytesNoCopy (), (kExtraBlocks + 1) * kBlock);
      uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ());
      uint64_t *l2 = l1 + 512;
      bzero (l1, 8192);
      const uint64_t tablesPa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
      l1[kExtraIova >> 30] = (tablesPa + 4096) | 3;
      uint32_t attr = 0;
      PE_parse_boot_argn ("sheng-wifi-attr", &attr, sizeof (attr));   // 1: normal non-cacheable, non-shareable memory for the endpoint's accesses
      const uint64_t share = attr == 1 ? 0 : (3ULL << 8);
      for (uint32_t i = 0; i < kExtraBlocks; ++i)
        l2[((kExtraIova >> 21) & 511) + i] = (bpa + i * kBlock) | (1ULL << 11) | (1ULL << 10) | share | (1ULL << 6) | 1;   // block, non-global, AF, inner shareable, readable and writable by unprivileged requesters, attribute index 0
      for (uint32_t off = 0; off < 8192; off += 64) __asm__ volatile ("dc civac, %0" :: "r" (reinterpret_cast<uint8_t *> (l1) + off) : "memory");
      __asm__ volatile ("dsb sy" ::: "memory");
      uintptr_t smmuBase = extraMap->getVirtualAddress ();
      unsigned pages = 1U << (((read32 (smmuBase, 0x24) >> 28) & 7) + 1);
      uintptr_t cb = smmuBase + (pages + (extraReport[0] >> 16)) * 4096;
      gWifiSmmu=smmuBase; gWifiContext=cb;
      write32 (cb, 0x0, 0);                               // stage 1 off while the table is set
      const unsigned bank=extraReport[0]>>16;
      write32 (smmuBase,0x1800+4*bank,1);                // AArch64 context
      write32 (smmuBase,0x1000+4*bank,0x1f300);          // stage 1, stage 2 bypass attributes
      write32 (cb, 0x10, (7u<<15) | ((read32(smmuBase,0x28)>>4)&15u) | (1u<<4)); // upstream address extension, output width, 16-bit ASIDs
      write32 (cb, 0x30, (2u<<30) | (2u<<12) | 32U | (1U << 23)); // 4 KiB granules, outer-shareable non-cacheable walks, TTBR0 only
      *reinterpret_cast<volatile uint64_t *>(cb + 0x20) = tablesPa;
      *reinterpret_cast<volatile uint64_t *>(cb + 0x28) = 0;
      write32 (cb, 0x38, attr == 1 ? 0x44 : 0xff);        // MAIR attribute 0: write-back (or non-cacheable)
      write32 (cb, 0x3c, 0);
      write32 (cb, 0x58, 0xffffffffu);                   // clear the previous context's faults
      write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);     // invalidate context translations, sync
      __asm__ volatile ("dsb sy" ::: "memory");
      for (unsigned spin=0; (read32(cb,0x7f4)&1) && spin<10000; ++spin) IODelay(1);
      if (read32(cb,0x7f4)&1) return;
      write32 (cb, 0x0, 0x1067);                         // native stage 1 access/attribute controls and fault reporting
      __asm__ volatile ("dsb sy; isb" ::: "memory");
      // Ask the context bank to translate every page of the window (stage 1, privileged read): count the pages that fault or land elsewhere than the table says.
      {
        uint32_t tested = 0, faults = 0, wrong = 0, first = 0;
        const bool hardwareAtos = (read32(smmuBase,0x20) & (1u<<30)) && !(read32(smmuBase,0x20) & (1u<<26));
        for (unsigned page = 0; hardwareAtos && page < kExtraBlocks * 2; ++page) {
          const uint64_t iova = kExtraIova + static_cast<uint64_t>(page / 2) * kBlock + (page & 1 ? kBlock - 4096 : 0);
          *reinterpret_cast<volatile uint64_t *>(cb + 0x800) = iova;
          __asm__ volatile("dsb sy" ::: "memory");
          uint32_t spin = 0;
          while ((read32 (cb, 0x8f0) & 1) && ++spin < 100000) { }
          if (spin >= 100000) { native_ufs_extra_atos[3] = 0xdead0000U | tested; break; }
          const uint64_t par = *reinterpret_cast<volatile uint64_t *>(cb + 0x50);
          const uint32_t parLo = static_cast<uint32_t>(par), parHi = static_cast<uint32_t>(par >> 32);
          ++tested;
          if (tested == 1) { native_ufs_extra_atos[4] = parLo; native_ufs_extra_atos[5] = parHi; native_ufs_extra_atos[6] = static_cast<uint32_t> (bpa); native_ufs_extra_atos[7] = static_cast<uint32_t> (bpa >> 32); }
          if (parLo & 1) { if (!faults++) first = static_cast<uint32_t> (iova); continue; }
          const uint64_t pa = ((static_cast<uint64_t> (parHi) << 32) | parLo) & 0x0000fffffffff000ULL;
          if (pa != ((bpa + (iova - kExtraIova)) & ~0xfffULL)) { if (!wrong++ && !first) first = static_cast<uint32_t> (iova); }
        }
        native_ufs_extra_atos[0] = tested; native_ufs_extra_atos[1] = faults; native_ufs_extra_atos[2] = wrong;
        if (native_ufs_extra_atos[3] == 0) native_ufs_extra_atos[3] = first;
      }
      const uint32_t offsets[] = {0, 0x10, 0x20, 0x24, 0x30, 0x38, 0x50, 0x54, 0x58, 0x8f0};
      for (unsigned i=0; i<10; ++i) native_marble_wifi_smmu[i] = read32(cb, offsets[i]);
      native_marble_wifi_smmu[10] = read32(smmuBase, 0x1000+4*(extraReport[0]>>16));
      native_marble_wifi_smmu[11] = read32(smmuBase, 0x1800+4*(extraReport[0]>>16));
      native_marble_wifi_smmu[12] = read32(smmuBase, 0x800+4*(extraReport[0]&0xffff));
      native_marble_wifi_smmu[13] = read32(smmuBase, 0xc00+4*(extraReport[0]&0xffff));
      native_marble_wifi_smmu[14] = read32(smmuBase, 0x20);
      native_marble_wifi_smmu[15] = read32(smmuBase, 0x28);
      // The CPU reaches the buffer through a second mapping that is not cached (the buffer's own kernel mapping is cached), so what the CPU writes is in
      // memory when the endpoint reads it and what the endpoint writes is seen at once, whether or not the link is cache coherent. The cached lines are
      // written back and dropped first, and that mapping is not used again.
      for (uint64_t off = 0; off < (kExtraBlocks + 1) * static_cast<uint64_t> (kBlock); off += 64)
        __asm__ volatile ("dc civac, %0" :: "r" (static_cast<uint8_t *> (buffer->getBytesNoCopy ()) + off) : "memory");
      __asm__ volatile ("dsb sy" ::: "memory");
      IOMemoryMap *wc = buffer->createMappingInTask (kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
      native_ufs_extra_va = wc ? reinterpret_cast<uint8_t *> (wc->getVirtualAddress ()) + (bva - static_cast<uint8_t *> (buffer->getBytesNoCopy ())) : bva;
      native_ufs_extra_uncached = wc != nullptr;
      native_ufs_extra_bytes = kExtraBlocks * kBlock - 0;
      native_ufs_extra_iova = kExtraIova;
    }
    __atomic_store_n(&native_ufs_extra_stream, 0x80000000U | extraReport[0], __ATOMIC_RELEASE);
}
