// SPDX-License-Identifier: BSD-2-Clause-Patent
// DMA allocator from UniXNU-sheng WifiProbe.cpp.
struct Arena {uint8_t *va; uint64_t iova; size_t bytes; struct Block {uint32_t off, size; bool used;} *blocks; unsigned count; size_t used, peak; unsigned failures;} gArena;
bool arenaInit() {
    if (!native_ufs_extra_va || !native_ufs_extra_bytes || !native_ufs_extra_uncached) return false;
    if (!gArena.blocks) gArena.blocks = static_cast<Arena::Block *>(IOMallocData(kArenaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    gArena.va = native_ufs_extra_va; gArena.iova = native_ufs_extra_iova; gArena.bytes = native_ufs_extra_bytes;
    gArena.blocks[0] = {4096, (uint32_t)(gArena.bytes - 4096), false}; gArena.count = 1;   // the first page is the interrupt sink
    gArena.used = gArena.peak = 4096; gArena.failures = 0;
    bzero(gArena.va, gArena.bytes);
    return true;
}
// Buffers the endpoint reads or writes sit at addresses aligned to their size rounded up to a power of two, with no upper limit: the boot loader
// refuses an image address that is only page aligned, before it makes any transfer (the registers that name the address and size of a failed transfer
// stay zero), the firmware's host memory regions (several MiB) go through the same address translation, and an IOMMU-backed DMA allocator hands out
// addresses aligned that way.
void pLog(int level, const char *line);
void *arenaAlloc(size_t n, uint64_t *bus) {
    n = (n + 63) & ~(size_t)63;
    if (n >= 4096) n = (n + 4095) & ~(size_t)4095;
    uint32_t align = 64;
    if (n >= 4096) {align = 4096; while (align < n) align <<= 1;}
    for (unsigned i = 0; i < gArena.count; ++i) {
        Arena::Block &b = gArena.blocks[i];
        if (b.used) continue;
        const uint32_t start = (b.off + align - 1) & ~(align - 1), pad = start - b.off;
        if (b.size < pad + n) continue;
        const bool tail = b.size > pad + n;
        if (gArena.count + (pad ? 1 : 0) + (tail ? 1 : 0) > kArenaBlocks) continue;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        const Arena::Block whole = b;
        memmove(&gArena.blocks[i + extra], &gArena.blocks[i], (gArena.count - i) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        if (pad) {gArena.blocks[at] = {whole.off, pad, false}; ++at;}
        gArena.blocks[at] = {start, (uint32_t)n, true};
        if (tail) gArena.blocks[at + 1] = {start + (uint32_t)n, whole.size - pad - (uint32_t)n, false};
        uint8_t *p = gArena.va + start;
        gArena.used += n; if (gArena.used > gArena.peak) gArena.peak = gArena.used;
        bzero(p, n);
        if (bus) *bus = gArena.iova + start;
        return p;
    }
    ++gArena.failures;
    return nullptr;
}
void arenaFree(void *p) {
    if (!p || (uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return;
    const uint32_t off = (uint32_t)((uint8_t *)p - gArena.va);
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used) continue;
        gArena.used -= gArena.blocks[i].size;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {gArena.blocks[i].size += gArena.blocks[i + 1].size; memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2], (gArena.count - i - 2) * sizeof(Arena::Block)); --gArena.count;}
        if (i > 0 && !gArena.blocks[i - 1].used) {gArena.blocks[i - 1].size += gArena.blocks[i].size; memmove(&gArena.blocks[i], &gArena.blocks[i + 1], (gArena.count - i - 1) * sizeof(Arena::Block)); --gArena.count;}
        return;
    }
}

