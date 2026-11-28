#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "lx_skb.h"

enum { THREADS = 6, ITERATIONS = 12000, PRODUCERS = 4, PACKETS = 6000 };
static _Thread_local unsigned preemption_depth;
static unsigned long enters, leaves;
void lx_spin_enter(void) { preemption_depth++; __atomic_fetch_add(&enters, 1, __ATOMIC_RELAXED); }
void lx_spin_leave(void) { assert(preemption_depth); preemption_depth--; __atomic_fetch_add(&leaves, 1, __ATOMIC_RELAXED); }
static spinlock_t spin;
static rwlock_t rwlock;
static unsigned spin_count, stamp, inverse;
static void *spin_writer(void *context) {
  (void)context;
  for (unsigned i = 0; i < ITERATIONS; i++) { lx_shared_spin_lock(&spin); assert(preemption_depth); spin_count++; lx_shared_spin_unlock(&spin); }
  return NULL;
}
static void *rw_writer(void *context) {
  (void)context;
  for (unsigned i = 0; i < ITERATIONS; i++) {
    write_lock(&rwlock); stamp++; lx_cpu_relax(); inverse = stamp ^ 0xa5a55a5a; write_unlock(&rwlock);
  }
  return NULL;
}
static void *rw_reader(void *context) {
  (void)context;
  for (unsigned i = 0; i < ITERATIONS; i++) {
    read_lock(&rwlock); unsigned first = stamp; lx_cpu_relax(); assert((first ^ 0xa5a55a5a) == inverse); read_unlock(&rwlock);
  }
  return NULL;
}
static struct sk_buff_head queue;
static struct sk_buff *packets;
static u8 *seen;
static void *producer(void *context) {
  unsigned base = (unsigned)(uintptr_t)context * PACKETS;
  for (unsigned i = 0; i < PACKETS; i++) { struct sk_buff *packet = &packets[base + i]; packet->mark = base + i; skb_queue_tail(&queue, packet); }
  return NULL;
}
static void *consumer(void *context) {
  (void)context;
  unsigned received = 0;
  while (received < PRODUCERS * PACKETS) {
    struct sk_buff *packet = skb_dequeue(&queue);
    if (!packet) { lx_cpu_relax(); continue; }
    assert(packet->mark < PRODUCERS * PACKETS && !seen[packet->mark]); seen[packet->mark] = 1; received++;
  }
  return NULL;
}
int main(void) {
  pthread_t threads[THREADS];
  spin_lock_init(&spin); assert(lx_shared_spin_trylock(&spin) && preemption_depth == 1 && !lx_shared_spin_trylock(&spin) && preemption_depth == 1); lx_shared_spin_unlock(&spin);
  assert(!preemption_depth);
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_create(&threads[i], NULL, spin_writer, NULL));
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_join(threads[i], NULL));
  assert(spin_count == THREADS * ITERATIONS && !spin_is_locked(&spin));
  rwlock_init(&rwlock); inverse = 0xa5a55a5a;
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_create(&threads[i], NULL, i < 2 ? rw_writer : rw_reader, NULL));
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_join(threads[i], NULL));
  assert(stamp == ITERATIONS * 2 && !rwlock.state && !rwlock.writers);
  packets = calloc(PRODUCERS * PACKETS, sizeof(*packets)); seen = calloc(PRODUCERS * PACKETS, 1); assert(packets && seen);
  skb_queue_head_init(&queue);
  assert(!pthread_create(&threads[0], NULL, consumer, NULL));
  for (unsigned i = 0; i < PRODUCERS; i++) assert(!pthread_create(&threads[i + 1], NULL, producer, (void *)(uintptr_t)i));
  for (unsigned i = 0; i <= PRODUCERS; i++) assert(!pthread_join(threads[i], NULL));
  assert(skb_queue_empty(&queue) && !skb_queue_len(&queue));
  for (unsigned i = 0; i < PRODUCERS * PACKETS; i++) assert(seen[i]);
  free(seen); free(packets);
  assert(enters == leaves && !preemption_depth);
  puts("PASS: six-thread spinlock increments, concurrent RW consistency, four-producer skb queue with 24000 packets and no missing or duplicate owners");
}
