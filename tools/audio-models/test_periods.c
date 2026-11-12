#include "marble_periods.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static struct mp_queue queue;
static int16_t input[MP_FRAMES * 2];
static uint32_t cases;

#define CHECK(expr) do { assert(expr); cases++; } while (0)

static void
put(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint32_t
get(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
fresh(uint32_t seed)
{
	CHECK(mp_init(&queue, 0x11ffd0000ull, 0x5000, 0x4000, 856,
	    0x12345678, 0x2011, 0x40bd, seed) == MP_OK);
}

static void
done(uint32_t p, uint8_t packet[40])
{
	uint64_t address = queue.wire_base + p * MP_BYTES;

	memset(packet, 0, 40);
	put(packet, 0x60 | (40u << 8));
	put(packet + 4, 3u | 2u << 8);
	put(packet + 8, queue.dst_port);
	put(packet + 12, queue.src_port);
	put(packet + 16, queue.period[p].token);
	put(packet + 20, 0x05001004);
	put(packet + 24, (uint32_t)address);
	put(packet + 28, (uint32_t)(address >> 32));
	put(packet + 32, queue.handle);
}

static void
send_one(uint32_t p)
{
	uint8_t wire[MP_PACKET_BYTES];

	CHECK(mp_stage(&queue, p, input, MP_FRAMES, 0) == MP_OK);
	CHECK(mp_packet(&queue, p, 1000, 5000, wire) == MP_OK);
	CHECK(get(wire) == (0x60 | MP_PACKET_BYTES << 8));
	CHECK(get(wire + 20) == 0x0400100a);
	CHECK(get(wire + 16) == queue.period[p].token);
	CHECK(get(wire + 36) == MP_BYTES);
	for (unsigned i = 40; i < MP_PACKET_BYTES; i++) {
		CHECK(wire[i] == 0); /* no metadata / timestamp / gain payload */
	}
}

static void
amplitudes(void)
{
	/* Exhaust the complete S16 domain, including -32768. */
	for (int start = INT16_MIN; start <= INT16_MAX; start += MP_FRAMES * 2) {
		fresh(1);
		for (unsigned i = 0; i < MP_FRAMES * 2; i++) {
			input[i] = (int16_t)(start + (int)i);
		}
		CHECK(mp_stage(&queue, 0, input, MP_FRAMES, 0) == MP_OK);
		for (unsigned i = 0; i < MP_FRAMES * 2; i++) {
			int32_t value = queue.period[0].samples[i];
			int32_t magnitude = value < 0 ? -value : value;

			CHECK(value == (int32_t)input[i] / MP_SAFE_DIVISOR);
			CHECK(magnitude <= (int32_t)MP_SAFE_PEAK);
		}
	}
	fresh(1);
	input[0] = INT16_MIN;
	input[1] = INT16_MAX;
	CHECK(mp_stage(&queue, 0, input, 1, 0) == MP_OK);
	CHECK(queue.period[0].samples[0] == -1024);
	CHECK(queue.period[0].samples[1] == 1023);
	for (unsigned i = 2; i < MP_FRAMES * 2; i++) {
		CHECK(queue.period[0].samples[i] == 0);
	}
	CHECK(queue.source_frames == 1 && queue.silence_frames == MP_FRAMES - 1);
	fresh(1);
	CHECK(mp_stage(&queue, 0, input, MP_FRAMES, 1) == MP_OK);
	for (unsigned i = 0; i < MP_FRAMES * 2; i++) {
		CHECK(queue.period[0].samples[i] == 0);
	}
}

static void
ownership(void)
{
	uint8_t replies[MP_PERIODS][40], stale[40];
	int16_t owned[MP_FRAMES * 2];
	const unsigned order[] = { 2, 0, 3, 1 };

	fresh(1);
	for (unsigned p = 0; p < MP_PERIODS; p++) {
		send_one(p);
		done(p, replies[p]);
		memcpy(owned, queue.period[p].samples, sizeof(owned));
		CHECK(mp_stage(&queue, p, input, MP_FRAMES, 1) == MP_BUSY);
		CHECK(!memcmp(owned, queue.period[p].samples, sizeof(owned)));
	}
	for (unsigned i = 0; i < MP_PERIODS; i++) {
		CHECK(mp_done(&queue, replies[order[i]], 40) == MP_OK);
	}
	CHECK(queue.completions == 4);
	CHECK(mp_done(&queue, replies[0], 40) == MP_STALE); /* duplicate */
	send_one(0);
	CHECK(mp_done(&queue, replies[0], 40) == MP_STALE); /* prior generation */
	CHECK(queue.period[0].owner == MP_INFLIGHT);
	done(0, stale);
	put(stale + 24, get(stale + 24) + MP_BYTES);
	CHECK(mp_done(&queue, stale, 40) == MP_STALE); /* wrong address */
	done(0, stale);
	put(stale + 32, queue.handle ^ 1u);
	CHECK(mp_done(&queue, stale, 40) == MP_STALE); /* wrong map */
	done(0, stale);
	put(stale + 8, queue.dst_port ^ 1u);
	CHECK(mp_done(&queue, stale, 40) == MP_STALE); /* wrong route */
	done(0, stale);
	put(stale + 20, 0x02001005);
	CHECK(mp_done(&queue, stale, 40) == MP_STALE); /* basic ACK != DONE */
	done(0, stale);
	CHECK(mp_done(&queue, stale, 36) == MP_BAD);
	put(stale, 0x60 | (36u << 8));
	CHECK(mp_done(&queue, stale, 40) == MP_BAD);
	done(0, stale);
	CHECK(mp_done(&queue, stale, 40) == MP_OK);
	CHECK(queue.period[0].owner == MP_FREE);
}

static void
failures(void)
{
	uint8_t wire[MP_PACKET_BYTES], reply[40];

	fresh(1);
	CHECK(mp_stage(&queue, 0, NULL, 1, 0) == MP_BAD);
	CHECK(mp_stage(&queue, 4, input, 1, 0) == MP_BAD);
	CHECK(mp_stage(&queue, 0, input, MP_FRAMES + 1, 0) == MP_BAD);
	send_one(0);
	CHECK(mp_expire(&queue, 5999) == MP_OK);
	CHECK(mp_expire(&queue, 6000) == MP_FAULT);
	CHECK(queue.period[0].owner == MP_INFLIGHT);
	CHECK(mp_stage(&queue, 1, input, MP_FRAMES, 0) == MP_FAULT);
	fresh(1);
	send_one(0);
	done(0, reply);
	put(reply + 36, 3);
	CHECK(mp_done(&queue, reply, 40) == MP_FAULT);
	CHECK(queue.period[0].owner == MP_INFLIGHT);
	fresh(1);
	send_one(0);
	mp_abort(&queue); /* failed send: do not assume no DMA access */
	CHECK(queue.period[0].owner == MP_INFLIGHT);
	CHECK(mp_packet(&queue, 0, 10, 10, wire) == MP_FAULT);
	fresh(1);
	CHECK(mp_stage(&queue, 0, input, MP_FRAMES, 0) == MP_OK);
	CHECK(mp_packet(&queue, 0, UINT64_MAX, 1, wire) == MP_BAD);
	CHECK(queue.period[0].owner == MP_STAGED);
}

static void
budgets_and_geometry(void)
{
	uint8_t reply[40];

	fresh(1);
	for (uint32_t n = 0; n < MP_MAX_SUBMISSIONS; n++) {
		uint32_t p = n % MP_PERIODS;

		send_one(p);
		done(p, reply);
		CHECK(mp_done(&queue, reply, 40) == MP_OK);
	}
	CHECK(queue.submissions == MP_MAX_SUBMISSIONS);
	CHECK(mp_stage(&queue, 0, input, MP_FRAMES, 0) == MP_BUDGET);
	CHECK(mp_init(&queue, 0, 0x5000, 0, 856, 1, 1, 2, 1) == MP_BAD);
	CHECK(mp_init(&queue, 0, 0x4000, 0x4000, 856, 1, 1, 2, 1) == MP_BAD);
	CHECK(mp_init(&queue, UINT64_MAX, 0x5000, 0x4000, 856, 1, 1, 2, 1) == MP_BAD);
	CHECK(mp_init(&queue, 0, 0x5000, 0x4000, 856, 0, 1, 2, 1) == MP_BAD);
	CHECK(mp_init(&queue, 0, 0x5000, 0x4000, 856, 1, 1, 2, 0) == MP_BAD);
	fresh(0x1fffffff);
	send_one(0);
	done(0, reply);
	CHECK(mp_done(&queue, reply, 40) == MP_OK);
	CHECK(mp_stage(&queue, 0, input, MP_FRAMES, 0) == MP_FAULT);
}

int
main(void)
{
	amplitudes();
	ownership();
	failures();
	budgets_and_geometry();
	printf("{\"passed\":true,\"assertions\":%u,\"s16_values_tested\":65536,"
	    "\"peak_ceiling\":1024,\"transport\":\"mock-only; no hardware\"}\n", cases);
	return 0;
}
