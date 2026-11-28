#ifndef LX_U64_STATS_SYNC_H
#define LX_U64_STATS_SYNC_H
#include "lx.h"
typedef struct { u64 v; } u64_stats_t;
struct u64_stats_sync { unsigned seq; };
static inline void u64_stats_init(struct u64_stats_sync *s) { s->seq = 0; }
static inline void u64_stats_update_begin(struct u64_stats_sync *s) { __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL); }
static inline void u64_stats_update_end(struct u64_stats_sync *s) { __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE); }
static inline unsigned u64_stats_fetch_begin(const struct u64_stats_sync *s) {
  unsigned v; do { v = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE); } while (v & 1); return v;
}
static inline bool u64_stats_fetch_retry(const struct u64_stats_sync *s, unsigned start) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE); return __atomic_load_n(&s->seq, __ATOMIC_RELAXED) != start;
}
static inline u64 u64_stats_read(const u64_stats_t *p) { return __atomic_load_n(&p->v, __ATOMIC_RELAXED); }
static inline void u64_stats_set(u64_stats_t *p, u64 n) { __atomic_store_n(&p->v, n, __ATOMIC_RELAXED); }
static inline void u64_stats_add(u64_stats_t *p, unsigned long n) { __atomic_fetch_add(&p->v, n, __ATOMIC_RELAXED); }
static inline void u64_stats_inc(u64_stats_t *p) { u64_stats_add(p, 1); }
#endif
