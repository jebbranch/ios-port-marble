/*
 * Portable, offline-verified candidate for the marble PCM feeder.
 * Not yet linked into XNU. Caller serializes access; no transport/hardware calls.
 */
#ifndef MARBLE_PERIODS_H
#define MARBLE_PERIODS_H
#include <stdint.h>

#define MP_PERIODS 4u
#define MP_FRAMES 1024u
#define MP_BYTES 4096u
#define MP_PACKET_BYTES 68u
#define MP_MAX_SUBMISSIONS 30u
#define MP_SAFE_DIVISOR 32
#define MP_SAFE_PEAK 1024u

enum mp_result {
	MP_OK = 0, MP_BAD = -1, MP_BUSY = -2, MP_STALE = -3,
	MP_FAULT = -4, MP_BUDGET = -5
};
enum mp_owner { MP_FREE, MP_STAGED, MP_INFLIGHT };

struct mp_period {
	int16_t samples[MP_FRAMES * 2];
	uint32_t owner, token;
	uint64_t deadline;
};
struct mp_queue {
	struct mp_period period[MP_PERIODS];
	uint64_t wire_base;
	uint32_t handle, src_port, dst_port, ticket, submissions;
	uint32_t completions, rejected, fault, peak_in, peak_out;
	uint32_t divisor, ceiling;
	uint64_t source_frames, silence_frames;
};

int mp_init(struct mp_queue *q, uint64_t wire_base, uint32_t region_bytes,
    uint32_t descriptor_offset, uint32_t descriptor_bytes, uint32_t handle,
    uint32_t src_port, uint32_t dst_port, uint32_t first_ticket);
int mp_stage(struct mp_queue *q, uint32_t p, const int16_t *samples,
    uint32_t frames, int muted);
int mp_output(struct mp_queue *q, uint32_t divisor, uint32_t ceiling);
int mp_packet(struct mp_queue *q, uint32_t p, uint64_t now_us,
    uint64_t timeout_us, uint8_t out[MP_PACKET_BYTES]);
int mp_done(struct mp_queue *q, const uint8_t *packet, uint32_t bytes);
int mp_expire(struct mp_queue *q, uint64_t now_us);
void mp_abort(struct mp_queue *q);

#endif
