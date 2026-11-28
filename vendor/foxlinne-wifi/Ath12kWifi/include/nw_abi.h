/* The boundary between the board code and the WiFi module: what the board provides, and the calls into the module. Plain C types only. */
#ifndef NW_ABI_H
#define NW_ABI_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lx_platform {
  uint64_t (*now_ms)(void);
  void (*sleep_ms)(unsigned int ms);        /* let the clock advance while nothing is ready */
  void (*poll)(void);                       /* gather device events */
  void (*log)(int level, const char *line);
  void (*halt)(void);
  void *(*alloc)(size_t n);
  void (*free)(void *p);
  void *(*dma_alloc)(size_t n, uint64_t *bus);     /* zeroed memory the endpoint reaches at *bus */
  void (*dma_free)(size_t n, void *p, uint64_t bus);
  uint64_t (*dma_map)(void *p, size_t n, int dir); /* bus address of memory that lies in the DMA window, 0 if it does not */
  void (*dma_sync)(uint64_t bus, size_t n, int dir);
  int (*fw_get)(const char *name, const uint8_t **data, size_t *size);   /* firmware file by its last path component */
  uint64_t (*now_ns)(void);                 /* optional monotonic clock */
  uint64_t (*boottime_ns)(void);            /* optional clock including system sleep */
  void (*busy_delay_us)(unsigned int us);   /* optional hardware delay; never dispatch callbacks */
  void (*preempt_disable)(void);           /* paired native hooks for shared control/status locks */
  void (*preempt_enable)(void);
};
struct nw_board {
  void *bar; size_t bar_len;                         /* register window of the endpoint, mapped */
  uint16_t device_id, msi_cap; int irq_base;         /* PCI device id; offset of the MSI capability; first irq number given to vectors */
  int (*cfg_read)(int off, int size, uint32_t *v);   /* config space; offsets with bit 31 set are inside the PCIe capability */
  int (*cfg_write)(int off, int size, uint32_t v);
  int (*msi_target)(uint32_t *lo, uint32_t *hi, uint32_t *data);   /* where the endpoint writes to raise vector 0 */
  void *(*map_phys)(uint64_t pa, size_t n);
  unsigned diagnostics;                              /* nonzero: also ask the firmware for its receive statistics while frames arrive */
};
struct nw_wifi_stats { uint32_t stage, scans, frames, beacons, bss_seen; int last_err; };
/* Cumulative buffer counters; snapshots do not reset live ownership counts. */
struct nw_buffer_stats {
  uint64_t skb_allocations, skb_frees, dma_allocations, dma_frees;
  uint64_t live_dma, peak_dma, clones, cow, expansions, copied_bytes, allocation_failures;
  uint64_t pool_hits, pool_misses, pool_cached_bytes, pool_cached_blocks;
};
void nw_wifi_buffer_stats_get(struct nw_buffer_stats *out);
void nw_wifi_buffer_pool_configure(size_t maximum_bytes);
void nw_wifi_buffer_pool_trim(void);
/* ---- station interface, as the board's network code uses it ----
 * Calls may come from any board thread; each only queues a request (or copies a snapshot) and returns. The work runs, and the events below
 * are called, from whichever thread calls nw_wifi_step, so event handlers must not block. */
struct nw_bss_info {
  uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32];
  uint32_t freq_mhz; int8_t signal_dbm; uint16_t caps, beacon_int; uint32_t age_ms;
  uint16_t ie_len; uint8_t ie[384];                  /* the information elements as the access point sent them */
};
enum { NW_STA_OFF, NW_STA_IDLE, NW_STA_SCANNING, NW_STA_AUTH, NW_STA_ASSOC, NW_STA_KEYS, NW_STA_UP };
enum { NW_RES_OK = 0, NW_RES_NOT_FOUND = 1, NW_RES_TIMEOUT = 2, NW_RES_REJECTED = 3, NW_RES_BAD_KEY = 4, NW_RES_UNSUPPORTED = 5, NW_RES_FAILED = 6 };
struct nw_wifi_status { uint32_t state; int32_t result; uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32]; uint32_t freq_mhz; int8_t signal_dbm; uint8_t mac[6]; uint32_t channels, tx_rate_100kbps; };
typedef int (*nw_packet_reader)(void *context, size_t offset, void *destination, size_t length);
struct nw_wifi_events {
  void (*scan_done)(void);
  void (*connect_done)(int result);                  /* NW_RES_* */
  void (*link_lost)(int reason);
  void (*rx)(const uint8_t *eth, size_t len);        /* a frame for the host network stack: Ethernet header and payload */
  void (*rx_flush)(void);                          /* end of a bounded receive batch */
  void (*rx_packet)(void *context, size_t length, nw_packet_reader reader); /* synchronous borrowed fragments */
  void (*link_update)(const struct nw_wifi_status *status); /* cached peer signal changed */
};
void nw_wifi_set_events(const struct nw_wifi_events *ev);
int nw_wifi_scan(const uint8_t *ssid, size_t len);                       /* ssid may be NULL (all networks) */
int nw_wifi_bss_count(void);
int nw_wifi_bss_get(int i, struct nw_bss_info *out);                     /* 0 on success */
int nw_wifi_connect(const uint8_t *ssid, size_t len, const uint8_t *bssid, const char *pass, size_t plen);   /* pass NULL: open network */
void nw_wifi_disconnect(void);
void nw_wifi_power(int enabled);                  /* queue radio on/off on the wireless worker */
int nw_wifi_tx(const uint8_t *eth, size_t len);                          /* 0 queued */
/* A successful queue takes ownership; release runs once on the wireless worker.
 * A rejected queue leaves ownership with the caller. No payload copy here. */
int nw_wifi_tx_packet(void *context, size_t length, nw_packet_reader reader, void (*release)(void *));
void nw_wifi_status_get(struct nw_wifi_status *st);
void nw_wifi_tx_reasons(char *text, int size);                          /* the driver's transmit completion counts, as text */
void nw_wifi_link_state(char *text, int size); /* worker only; reads cached peer rates, no firmware request */
int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b);
void nw_wifi_set_platform(const struct lx_platform *p); /* before exposing native socket callbacks */
int nw_wifi_step(void);                                                   /* nonzero while frames move: call again without sleeping */
/* Board poll delivers observed MSI bits from the worker, including nested waits. */
void nw_wifi_irq_mode(int delivered);
void nw_wifi_irq_pending(uint32_t vectors);
void nw_wifi_poll(void);                    /* worker poll, including nested firmware waits */
unsigned nw_wifi_wait_ms(unsigned maximum);
void nw_wifi_stop(void);
extern struct nw_wifi_stats nw_wifi_stats;
/* What the dispatcher runs now: kind (1 interrupt handler, 2 timer, 3 work, 4 board poll, 0 nothing), the function as an offset from
 * lx_now_ms, the nesting depth and the millisecond it started. A board can show it when the module stops making progress. */
extern volatile uint64_t nw_wifi_where[4];
#ifdef __cplusplus
}
#endif
#endif
