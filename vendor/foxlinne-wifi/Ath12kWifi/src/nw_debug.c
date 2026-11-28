/* The driver's message functions, printing through the module log. */
#include "lx_pci.h"
#include "lx_plat.h"
#include "core.h"
#include "debug.h"

static void emit(int level, const char *fmt, va_list ap) {
  char b[300]; vsnprintf(b, sizeof b, fmt, ap); size_t n = strlen(b); if (n && b[n - 1] == '\n') b[n - 1] = 0;
  lx_plat->log(level, b); if (lx_log_tap) lx_log_tap(level, b); }
void ath12k_info(struct ath12k_base *ab, const char *fmt, ...) { (void)ab; va_list ap; va_start(ap, fmt); emit(6, fmt, ap); va_end(ap); }
void ath12k_err(struct ath12k_base *ab, const char *fmt, ...) { (void)ab; va_list ap; va_start(ap, fmt); emit(3, fmt, ap); va_end(ap); }
void __ath12k_warn(struct device *dev, const char *fmt, ...) { (void)dev; va_list ap; va_start(ap, fmt); emit(4, fmt, ap); va_end(ap); }
void __ath12k_dbg(struct ath12k_base *ab, enum ath12k_debug_mask mask, const char *fmt, ...) { (void)ab; (void)mask; va_list ap; va_start(ap, fmt); emit(7, fmt, ap); va_end(ap); }
void ath12k_dbg_dump(struct ath12k_base *ab, enum ath12k_debug_mask mask, const char *msg, const char *prefix, const void *buf, size_t len) {
  (void)ab; (void)prefix; if (!(ath12k_debug_mask & mask)) return; if (msg) lx_plat->log(7, msg);
  const u8 *p = buf; char l[100];
  for (size_t i = 0; i < len; i += 16) { int o = snprintf(l, sizeof l, "%04zx:", i); for (size_t k = i; k < i + 16 && k < len; k++) o += snprintf(l + o, sizeof l - o, " %02x", p[k]); lx_plat->log(7, l); } }
