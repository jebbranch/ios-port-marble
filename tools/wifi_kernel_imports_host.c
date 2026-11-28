/* Host link witnesses only. Abort if a test reaches a kernel-only hook. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uintptr_t ml_io_map(uintptr_t p, size_t n)
{ (void)p; (void)n; abort(); }
uintptr_t ml_io_map_unmappable(uintptr_t p, size_t n, unsigned flags)
{ (void)p; (void)n; (void)flags; abort(); }
void ml_io_unmap(uintptr_t p, size_t n)
{ (void)p; (void)n; abort(); }
uint64_t kvtophys(uintptr_t p)
{ (void)p; abort(); }
void flush_dcache64(uint64_t p, unsigned n, int flags)
{ (void)p; (void)n; (void)flags; abort(); }
void *IOMallocContiguous(size_t n, size_t a, uintptr_t *p)
{ (void)n; (void)a; (void)p; abort(); }
void IOFreeContiguous(void *p, size_t n)
{ (void)p; (void)n; abort(); }
void *IOMallocData(size_t n)
{ (void)n; abort(); }
void IOFreeData(void *p, size_t n)
{ (void)p; (void)n; abort(); }
void IOSleep(unsigned t)
{ (void)t; abort(); }
void IODelay(unsigned t)
{ (void)t; abort(); }
void absolutetime_to_nanoseconds(uint64_t a, uint64_t *p)
{ (void)a; (void)p; abort(); }
void _disable_preemption(void)
{ abort(); }
void _enable_preemption(void)
{ abort(); }
void sweet_log_append(const char *p, int n)
{
	static const char prefix[] = "SWEET WIFI RUNTIME: ";
	if (n < (int)(sizeof(prefix) - 1) || memcmp(p, prefix, sizeof(prefix) - 1))
		abort();
}
void sweet_log_sync(void)
{ abort(); }
void panic(const char *fmt, ...)
{ (void)fmt; abort(); }
