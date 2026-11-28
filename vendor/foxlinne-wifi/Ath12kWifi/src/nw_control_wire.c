/* Allocation-free framing shared by the kernel control endpoint and supplicant. */
#include "nw_control.h"
#include <string.h>
static uint16_t read16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t read32(const uint8_t *p) { return (uint32_t)read16(p) | (uint32_t)read16(p + 2) << 16; }
static uint64_t read64(const uint8_t *p) { return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32; }
static void write16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void write32(uint8_t *p, uint32_t value) { write16(p, (uint16_t)value); write16(p + 2, (uint16_t)(value >> 16)); }
static void write64(uint8_t *p, uint64_t value) { write32(p, (uint32_t)value); write32(p + 4, (uint32_t)(value >> 32)); }
static int operation_valid(unsigned kind, unsigned operation) {
  if (kind == NW_CONTROL_EVENT) return operation >= NW_CONTROL_READY && operation < NW_CONTROL_EVENT_MAX;
  return (kind == NW_CONTROL_REQUEST || kind == NW_CONTROL_REPLY) && operation >= NW_CONTROL_INFO && operation < NW_CONTROL_COMMAND_MAX;
}
int nw_control_begin(struct nw_control_builder *builder, void *buffer, size_t capacity,
                     unsigned kind, unsigned operation, uint32_t sequence, uint64_t session, uint64_t generation, int status) {
  if (!builder || !buffer || capacity < NW_CONTROL_HEADER_SIZE || !operation_valid(kind, operation) ||
      status < 0 || status >= NW_CONTROL_STATUS_MAX || (kind == NW_CONTROL_REQUEST && status)) return NW_CONTROL_CODEC_INVALID;
  uint8_t *p = buffer;
  write32(p, NW_CONTROL_MAGIC); write16(p + 4, NW_CONTROL_VERSION); write16(p + 6, (uint16_t)kind);
  write16(p + 8, (uint16_t)operation); write16(p + 10, 0); write32(p + 12, sequence);
  write32(p + 16, NW_CONTROL_HEADER_SIZE); write32(p + 20, (uint32_t)status); write64(p + 24, session); write64(p + 32, generation);
  *builder = (struct nw_control_builder){ .bytes = buffer, .length = NW_CONTROL_HEADER_SIZE,
    .capacity = capacity > NW_CONTROL_MAX_MESSAGE ? NW_CONTROL_MAX_MESSAGE : capacity };
  return 0;
}
int nw_control_put(struct nw_control_builder *builder, unsigned attribute, const void *bytes, size_t length) {
  if (!builder || !builder->bytes || attribute == 0 || attribute >= NW_CONTROL_ATTR_COUNT ||
      (!bytes && length) || builder->length < NW_CONTROL_HEADER_SIZE || builder->length > builder->capacity ||
      length > UINT16_MAX - 4u) return NW_CONTROL_CODEC_INVALID;
  uint64_t mask = (uint64_t)1 << attribute;
  if (builder->attributes & mask) return NW_CONTROL_CODEC_DUPLICATE;
  size_t size = (length + 7u) & ~(size_t)3u;
  if (size > builder->capacity - builder->length) return NW_CONTROL_CODEC_FULL;
  uint8_t *p = builder->bytes + builder->length;
  write16(p, (uint16_t)attribute); write16(p + 2, (uint16_t)(length + 4));
  if (length) memcpy(p + 4, bytes, length);
  memset(p + 4 + length, 0, size - 4 - length);
  builder->length += size; builder->attributes |= mask;
  write32(builder->bytes + 16, (uint32_t)builder->length); return 0;
}
int nw_control_decode(const void *bytes, size_t length, struct nw_control_view *view) {
  if (!bytes || !view || length < NW_CONTROL_HEADER_SIZE || length > NW_CONTROL_MAX_MESSAGE || (length & 3)) return NW_CONTROL_CODEC_INVALID;
  const uint8_t *p = bytes;
  if (read32(p) != NW_CONTROL_MAGIC) return NW_CONTROL_CODEC_INVALID;
  if (read16(p + 4) != NW_CONTROL_VERSION) return NW_CONTROL_CODEC_VERSION;
  struct nw_control_view parsed = { .bytes = bytes };
  parsed.header = (struct nw_control_header){ .magic = NW_CONTROL_MAGIC, .version = NW_CONTROL_VERSION,
    .kind = read16(p + 6), .operation = read16(p + 8), .sequence = read32(p + 12),
    .length = read32(p + 16), .status = (int32_t)read32(p + 20), .session = read64(p + 24), .generation = read64(p + 32) };
  if (read16(p + 10) || parsed.header.length != length || parsed.header.status < 0 || parsed.header.status >= NW_CONTROL_STATUS_MAX ||
      (parsed.header.kind == NW_CONTROL_REQUEST && parsed.header.status) || !operation_valid(parsed.header.kind, parsed.header.operation)) return NW_CONTROL_CODEC_INVALID;
  for (size_t offset = NW_CONTROL_HEADER_SIZE; offset < length;) {
    if (length - offset < 4) return NW_CONTROL_CODEC_INVALID;
    unsigned attribute = read16(p + offset), size = read16(p + offset + 2);
    if (!attribute || attribute >= NW_CONTROL_ATTR_COUNT || size < 4 || size > length - offset) return NW_CONTROL_CODEC_INVALID;
    uint64_t mask = (uint64_t)1 << attribute;
    if (parsed.attributes & mask) return NW_CONTROL_CODEC_DUPLICATE;
    parsed.attributes |= mask;
    size_t padded = (size + 3u) & ~(size_t)3u;
    if (padded > length - offset) return NW_CONTROL_CODEC_INVALID;
    for (size_t i = size; i < padded; i++) if (p[offset + i]) return NW_CONTROL_CODEC_INVALID;
    offset += padded;
  }
  *view = parsed; return 0;
}
const void *nw_control_get(const struct nw_control_view *view, unsigned attribute, size_t *length) {
  if (length) *length = 0;
  if (!view || !attribute || attribute >= NW_CONTROL_ATTR_COUNT || !(view->attributes & ((uint64_t)1 << attribute))) return NULL;
  for (size_t offset = NW_CONTROL_HEADER_SIZE; offset < view->header.length;) {
    unsigned type = read16(view->bytes + offset), size = read16(view->bytes + offset + 2);
    if (type == attribute) { if (length) *length = size - 4; return view->bytes + offset + 4; }
    offset += (size + 3u) & ~(size_t)3u;
  }
  return NULL;
}
void nw_control_clear(void *bytes, size_t length) { volatile uint8_t *p = bytes; while (length--) *p++ = 0; }
int nw_control_put_u32(struct nw_control_builder *builder, unsigned attribute, uint32_t value) {
  uint8_t bytes[4]; write32(bytes, value); return nw_control_put(builder, attribute, bytes, sizeof(bytes));
}
int nw_control_put_u64(struct nw_control_builder *builder, unsigned attribute, uint64_t value) {
  uint8_t bytes[8]; write64(bytes, value); return nw_control_put(builder, attribute, bytes, sizeof(bytes));
}
int nw_control_get_u32(const struct nw_control_view *view, unsigned attribute, uint32_t *value) {
  size_t length; const uint8_t *bytes = nw_control_get(view, attribute, &length);
  if (!bytes || length != 4 || !value) return NW_CONTROL_CODEC_INVALID;
  *value = read32(bytes); return 0;
}
int nw_control_get_u64(const struct nw_control_view *view, unsigned attribute, uint64_t *value) {
  size_t length; const uint8_t *bytes = nw_control_get(view, attribute, &length);
  if (!bytes || length != 8 || !value) return NW_CONTROL_CODEC_INVALID;
  *value = read64(bytes); return 0;
}
void nw_control_reply_state(struct nw_control_builder *builder, uint64_t generation, int status) {
  write32(builder->bytes + 20, (uint32_t)status); write64(builder->bytes + 32, generation);
}
