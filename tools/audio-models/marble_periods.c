#include "marble_periods.h"
#include <string.h>

#define MP_WRITE_V2 0x0400100au
#define MP_DONE_V2 0x05001004u
#define MP_APPS 3u
#define MP_ADSP 2u
#define MP_TOKEN 0x80000000u
#define MP_TICKET_MAX 0x1fffffffu

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int
mp_init(struct mp_queue *q, uint64_t base, uint32_t region, uint32_t desc,
    uint32_t desc_bytes, uint32_t handle, uint32_t src, uint32_t dst,
    uint32_t ticket)
{
	if (q == NULL || !handle || !src || !dst || !ticket ||
	    ticket > MP_TICKET_MAX || region < MP_PERIODS * MP_BYTES ||
	    desc < MP_PERIODS * MP_BYTES || desc > region ||
	    desc_bytes > region - desc || base > UINT64_MAX - region) {
		return MP_BAD;
	}
	memset(q, 0, sizeof(*q));
	q->wire_base = base;
	q->handle = handle;
	q->src_port = src;
	q->dst_port = dst;
	q->ticket = ticket;
	q->divisor = MP_SAFE_DIVISOR;
	q->ceiling = MP_SAFE_PEAK;
	return MP_OK;
}

int
mp_output(struct mp_queue *q, uint32_t divisor, uint32_t ceiling)
{
	if (q == NULL || divisor == 0u || ceiling == 0u || ceiling > 16384u ||
	    q->submissions != 0u || q->source_frames != 0u) {
		return MP_BAD;
	}
	q->divisor = divisor;
	q->ceiling = ceiling;
	return MP_OK;
}

int
mp_stage(struct mp_queue *q, uint32_t p, const int16_t *input,
    uint32_t frames, int muted)
{
	uint32_t peak_in = 0, peak_out = 0;
	struct mp_period *period;

	if (q == NULL || p >= MP_PERIODS || frames > MP_FRAMES ||
	    (frames != 0 && input == NULL)) {
		return MP_BAD;
	}
	if (q->fault) {
		return MP_FAULT;
	}
	if (q->submissions >= MP_MAX_SUBMISSIONS) {
		return MP_BUDGET;
	}
	period = &q->period[p];
	if (period->owner != MP_FREE) {
		return MP_BUSY;
	}
	if (q->ticket > MP_TICKET_MAX) {
		q->fault = 1;
		return MP_FAULT; /* no token wrap/reuse */
	}
	for (uint32_t i = 0; i < frames * 2u; i++) {
		int32_t value = input[i];
		uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
		int32_t output = muted ? 0 : value / (int32_t)q->divisor;
		uint32_t out_magnitude = (uint32_t)(output < 0 ? -output : output);

		if (out_magnitude > q->ceiling) {
			output = output < 0 ? -(int32_t)q->ceiling :
			    (int32_t)q->ceiling;
			out_magnitude = q->ceiling;
		}
		if (magnitude > peak_in) {
			peak_in = magnitude;
		}
		if (out_magnitude > peak_out) {
			peak_out = out_magnitude;
		}
		period->samples[i] = (int16_t)output;
	}
	memset(period->samples + frames * 2u, 0, (MP_FRAMES - frames) * 4u);
	period->token = MP_TOKEN | (q->ticket++ << 2) | p;
	period->owner = MP_STAGED;
	if (peak_in > q->peak_in) {
		q->peak_in = peak_in;
	}
	if (peak_out > q->peak_out) {
		q->peak_out = peak_out;
	}
	q->source_frames += frames;
	q->silence_frames += MP_FRAMES - frames;
	return MP_OK;
}

int
mp_packet(struct mp_queue *q, uint32_t p, uint64_t now,
    uint64_t timeout, uint8_t out[MP_PACKET_BYTES])
{
	struct mp_period *period;
	uint64_t address;

	if (q == NULL || p >= MP_PERIODS || out == NULL || timeout == 0 ||
	    now > UINT64_MAX - timeout) {
		return MP_BAD;
	}
	if (q->fault) {
		return MP_FAULT;
	}
	if (q->submissions >= MP_MAX_SUBMISSIONS) {
		return MP_BUDGET;
	}
	period = &q->period[p];
	if (period->owner != MP_STAGED) {
		return MP_BUSY;
	}
	address = q->wire_base + p * MP_BYTES;
	memset(out, 0, MP_PACKET_BYTES);
	put32(out, (6u << 4) | (MP_PACKET_BYTES << 8));
	put32(out + 4, MP_ADSP | MP_APPS << 8);
	put32(out + 8, q->src_port);
	put32(out + 12, q->dst_port);
	put32(out + 16, period->token); /* full token, never old low-16 truncation */
	put32(out + 20, MP_WRITE_V2);
	put32(out + 24, (uint32_t)address);
	put32(out + 28, (uint32_t)(address >> 32));
	put32(out + 32, q->handle);
	put32(out + 36, MP_BYTES);
	period->deadline = now + timeout;
	period->owner = MP_INFLIGHT; /* BEFORE caller hands packet to transport */
	q->submissions++;
	return MP_OK;
}

int
mp_done(struct mp_queue *q, const uint8_t *packet, uint32_t bytes)
{
	uint32_t token, p;
	struct mp_period *period;
	uint64_t address, expected;

	if (q == NULL || packet == NULL || bytes < 40 ||
	    (le32(packet) & 0xffu) != 0x60u || (le32(packet) >> 8) != bytes) {
		return MP_BAD;
	}
	if (q->fault) {
		return MP_FAULT;
	}
	if (le32(packet + 4) != (MP_APPS | MP_ADSP << 8) ||
	    le32(packet + 8) != q->dst_port ||
	    le32(packet + 12) != q->src_port ||
	    le32(packet + 20) != MP_DONE_V2) {
		q->rejected++;
		return MP_STALE;
	}
	token = le32(packet + 16);
	p = token & 3u;
	period = &q->period[p];
	address = le32(packet + 24) | (uint64_t)le32(packet + 28) << 32;
	expected = q->wire_base + p * MP_BYTES;
	if (period->owner != MP_INFLIGHT || token != period->token ||
	    address != expected || le32(packet + 32) != q->handle) {
		q->rejected++;
		return MP_STALE;
	}
	if (le32(packet + 36) != 0) {
		q->fault = 1;
		return MP_FAULT; /* keep DMA-owned memory pinned on error */
	}
	period->owner = MP_FREE;
	q->completions++;
	return MP_OK;
}

int
mp_expire(struct mp_queue *q, uint64_t now)
{
	if (q == NULL) {
		return MP_BAD;
	}
	if (q->fault) {
		return MP_FAULT;
	}
	for (uint32_t p = 0; p < MP_PERIODS; p++) {
		if (q->period[p].owner == MP_INFLIGHT && now >= q->period[p].deadline) {
			q->fault = 1;
			return MP_FAULT;
		}
	}
	return MP_OK;
}

void
mp_abort(struct mp_queue *q)
{
	if (q != NULL) {
		q->fault = 1;
	}
	/* Never free/rewrite outstanding DMA. Owner must STOP/unmap first. */
}
