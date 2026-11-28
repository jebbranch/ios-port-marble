#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nw_control.h"
int main(void) {
  uint8_t storage[1025], malformed[1024], key[32], eapol[127];
  for (unsigned i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i * 7);
  for (unsigned i = 0; i < sizeof(eapol); i++) eapol[i] = (uint8_t)(i ^ 0xa5);
  struct nw_control_builder builder;
  assert(!nw_control_begin(&builder, storage + 1, 1024, NW_CONTROL_REQUEST, NW_CONTROL_SET_KEY, UINT32_MAX, 0x123456789abcdef0ULL, UINT64_MAX, NW_CONTROL_OK));
  assert(!nw_control_put(&builder, NW_CONTROL_ATTR_KEY, key, sizeof(key)));
  size_t unchanged = builder.length;
  assert(nw_control_put(&builder, NW_CONTROL_ATTR_KEY, key, sizeof(key)) == NW_CONTROL_CODEC_DUPLICATE && builder.length == unchanged);
  assert(!nw_control_put(&builder, NW_CONTROL_ATTR_EAPOL, eapol, sizeof(eapol)));
  struct nw_control_view view;
  assert(!nw_control_decode(builder.bytes, builder.length, &view));
  assert(view.header.sequence == UINT32_MAX && view.header.generation == UINT64_MAX && view.header.session == 0x123456789abcdef0ULL);
  size_t length;
  assert(!memcmp(nw_control_get(&view, NW_CONTROL_ATTR_KEY, &length), key, sizeof(key)) && length == sizeof(key));
  assert(!memcmp(nw_control_get(&view, NW_CONTROL_ATTR_EAPOL, &length), eapol, sizeof(eapol)) && length == sizeof(eapol));
  assert(!nw_control_get(&view, NW_CONTROL_ATTR_REASON, &length) && !length);
  for (size_t size = 0; size < builder.length; size++) assert(nw_control_decode(builder.bytes, size, &view) < 0);
  memcpy(malformed, builder.bytes, builder.length); malformed[4]++;
  assert(nw_control_decode(malformed, builder.length, &view) == NW_CONTROL_CODEC_VERSION);
  memcpy(malformed, builder.bytes, builder.length); malformed[NW_CONTROL_HEADER_SIZE + 2] = 0xff; malformed[NW_CONTROL_HEADER_SIZE + 3] = 0xff;
  assert(nw_control_decode(malformed, builder.length, &view) < 0);
  memcpy(malformed, builder.bytes, builder.length); malformed[NW_CONTROL_HEADER_SIZE + 4 + sizeof(key)] = NW_CONTROL_ATTR_KEY;
  assert(nw_control_decode(malformed, builder.length, &view) == NW_CONTROL_CODEC_DUPLICATE);
  memcpy(malformed, builder.bytes, builder.length); malformed[builder.length - 1] = 1;
  assert(nw_control_decode(malformed, builder.length, &view) < 0);
  uint32_t random = 0x78508550;
  for (unsigned iteration = 0; iteration < 30000; iteration++) {
    memcpy(malformed, builder.bytes, builder.length);
    for (unsigned i = 0; i < 5; i++) {
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      malformed[random % builder.length] ^= (uint8_t)(random >> 16);
    }
    size_t size = random % (builder.length + 1);
    if (!nw_control_decode(malformed, size, &view)) {
      for (unsigned i = 1; i < NW_CONTROL_ATTR_COUNT; i++) {
        const uint8_t *value = nw_control_get(&view, i, &length);
        if (value) assert(value >= malformed + NW_CONTROL_HEADER_SIZE && value + length <= malformed + size);
      }
    }
  }
  uint8_t tiny[NW_CONTROL_HEADER_SIZE];
  assert(!nw_control_begin(&builder, tiny, sizeof(tiny), NW_CONTROL_REPLY, NW_CONTROL_INFO, 1, 3, 7, NW_CONTROL_NOT_READY));
  assert(nw_control_put(&builder, NW_CONTROL_ATTR_KEY, key, sizeof(key)) == NW_CONTROL_CODEC_FULL && builder.length == NW_CONTROL_HEADER_SIZE);
  nw_control_clear(storage, sizeof(storage)); for (unsigned i = 0; i < sizeof(storage); i++) assert(!storage[i]);
  puts("PASS: unaligned control packets, key/EAPOL spans, full-width generations, truncation, duplicates, padding, overflow, 30000 mutated packets and payload wiping");
}
