#include "nw_crypto.h"

/* ---- SHA-1 ---- */
struct sha1 { uint32_t h[5]; uint8_t buf[64]; uint64_t total; size_t fill; };
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_block(struct sha1 *s, const uint8_t *p) {
  uint32_t w[80], a, b, c, d, e, t;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
    else { f = b ^ c ^ d; k = 0xca62c1d6u; }
    t = ROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = ROL(b, 30); b = a; a = t; }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; }
static void sha1_init(struct sha1 *s) { s->h[0] = 0x67452301u; s->h[1] = 0xefcdab89u; s->h[2] = 0x98badcfeu; s->h[3] = 0x10325476u; s->h[4] = 0xc3d2e1f0u; s->total = 0; s->fill = 0; }
static void sha1_update(struct sha1 *s, const uint8_t *d, size_t n) {
  s->total += n;
  while (n) { size_t c = 64 - s->fill; if (c > n) c = n; for (size_t i = 0; i < c; i++) s->buf[s->fill + i] = d[i];
    s->fill += c; d += c; n -= c; if (s->fill == 64) { sha1_block(s, s->buf); s->fill = 0; } } }
static void sha1_final(struct sha1 *s, uint8_t out[20]) {
  uint64_t bits = s->total * 8; uint8_t pad = 0x80, z = 0;
  sha1_update(s, &pad, 1); while (s->fill != 56) sha1_update(s, &z, 1);
  uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha1_update(s, l, 8);
  for (int i = 0; i < 5; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; } }
void nw_sha1(const uint8_t *d, size_t n, uint8_t out[20]) { struct sha1 s; sha1_init(&s); sha1_update(&s, d, n); sha1_final(&s, out); }

/* ---- HMAC-SHA1 ---- */
void nw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *data, size_t len, uint8_t out[20]) {
  uint8_t k[64], pad[64], inner[20]; struct sha1 s;
  for (int i = 0; i < 64; i++) k[i] = 0;
  if (klen > 64) nw_sha1(key, klen, k); else for (size_t i = 0; i < klen; i++) k[i] = key[i];
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha1_init(&s); sha1_update(&s, pad, 64); sha1_update(&s, data, len); sha1_final(&s, inner);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha1_init(&s); sha1_update(&s, pad, 64); sha1_update(&s, inner, 20); sha1_final(&s, out); }

/* ---- PBKDF2 ---- */
void nw_pbkdf2_sha1(const char *pass, size_t plen, const uint8_t *salt, size_t slen, unsigned iterations, uint8_t *out, size_t olen) {
  for (uint32_t block = 1; olen; block++) {
    uint8_t msg[64 + 4], u[20], t[20]; size_t n = slen > 64 ? 64 : slen;   /* the salt of WPA2 is an SSID: at most 32 bytes */
    for (size_t i = 0; i < n; i++) msg[i] = salt[i];
    msg[n] = block >> 24; msg[n + 1] = block >> 16; msg[n + 2] = block >> 8; msg[n + 3] = block;
    nw_hmac_sha1((const uint8_t *)pass, plen, msg, n + 4, u);
    for (int i = 0; i < 20; i++) t[i] = u[i];
    for (unsigned it = 1; it < iterations; it++) { nw_hmac_sha1((const uint8_t *)pass, plen, u, 20, u); for (int i = 0; i < 20; i++) t[i] ^= u[i]; }
    size_t c = olen > 20 ? 20 : olen; for (size_t i = 0; i < c; i++) *out++ = t[i]; olen -= c; } }

#define ROL8(x, n) ((uint8_t)(((x) << (n)) | ((x) >> (8 - (n)))))
/* ---- AES-128 (tables made at first use from the field arithmetic, so no big constant to mistype) ---- */
static uint8_t g_sbox[256], g_inv[256]; static int g_aes_ready;
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
static void aes_tables(void) {
  if (g_aes_ready) return;
  uint8_t p = 1, q = 1;
  do {   /* p walks the multiplicative group by 3, q is its inverse */
    p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0);
    q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
    uint8_t x = q ^ ROL8(q, 1) ^ ROL8(q, 2) ^ ROL8(q, 3) ^ ROL8(q, 4);
    g_sbox[p] = x ^ 0x63; } while (p != 1);
  g_sbox[0] = 0x63;
  for (int i = 0; i < 256; i++) g_inv[g_sbox[i]] = (uint8_t)i;
  g_aes_ready = 1; }
void nw_aes128_expand(const uint8_t key[16], uint32_t rk[44]) {
  aes_tables(); uint8_t rcon = 1;
  for (int i = 0; i < 4; i++) rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
  for (int i = 4; i < 44; i++) {
    uint32_t t = rk[i - 1];
    if (i % 4 == 0) {
      t = (t << 8) | (t >> 24);
      t = (uint32_t)g_sbox[t >> 24] << 24 | (uint32_t)g_sbox[(t >> 16) & 255] << 16 | (uint32_t)g_sbox[(t >> 8) & 255] << 8 | g_sbox[t & 255];
      t ^= (uint32_t)rcon << 24; rcon = xt(rcon); }
    rk[i] = rk[i - 4] ^ t; } }
static void add_key(uint8_t s[16], const uint32_t *rk) { for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) s[4 * c + r] ^= (uint8_t)(rk[c] >> (24 - 8 * r)); }
void nw_aes128_encrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16]; for (int i = 0; i < 16; i++) s[i] = in[i];
  add_key(s, rk);
  for (int round = 1; round <= 10; round++) {
    for (int i = 0; i < 16; i++) s[i] = g_sbox[s[i]];
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) & 3) + r];   /* shift rows */
    if (round < 10) for (int c = 0; c < 4; c++) { uint8_t *a = t + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
      s[4 * c] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3; s[4 * c + 1] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
      s[4 * c + 2] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3); s[4 * c + 3] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3); }
    else for (int i = 0; i < 16; i++) s[i] = t[i];
    add_key(s, rk + 4 * round); }
  for (int i = 0; i < 16; i++) out[i] = s[i]; }
void nw_aes128_decrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16]; for (int i = 0; i < 16; i++) s[i] = in[i];
  add_key(s, rk + 40);
  for (int round = 9; round >= 0; round--) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = s[4 * c + r];   /* inverse shift rows */
    for (int i = 0; i < 16; i++) s[i] = g_inv[t[i]];
    add_key(s, rk + 4 * round);
    if (round > 0) for (int c = 0; c < 4; c++) { uint8_t *a = s + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
      a[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9); a[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
      a[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11); a[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14); } }
  for (int i = 0; i < 16; i++) out[i] = s[i]; }

/* RFC 3394 5.2 */
int nw_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out) {
  uint32_t rk[44]; uint8_t a[8], b[16]; size_t n;
  if (len < 24 || (len & 7)) return -1;
  n = len / 8 - 1; nw_aes128_expand(kek, rk);
  for (int i = 0; i < 8; i++) a[i] = in[i];
  for (size_t i = 0; i < n * 8; i++) out[i] = in[8 + i];
  for (int j = 5; j >= 0; j--) for (size_t i = n; i >= 1; i--) {
    uint64_t t = (uint64_t)n * (uint64_t)j + i;
    for (int k = 0; k < 8; k++) b[k] = a[k] ^ (uint8_t)(t >> (56 - 8 * k));
    for (int k = 0; k < 8; k++) b[8 + k] = out[(i - 1) * 8 + k];
    nw_aes128_decrypt(rk, b, b);
    for (int k = 0; k < 8; k++) { a[k] = b[k]; out[(i - 1) * 8 + k] = b[8 + k]; } }
  for (int i = 0; i < 8; i++) if (a[i] != 0xa6) return -1;
  return 0; }

/* PRF-n: HMAC-SHA1(key, label || 0x00 || data || counter), counter from 0 */
void nw_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out, size_t bits) {
  uint8_t msg[128]; size_t ll = 0, n = 0, need = bits / 8;
  while (label[ll]) { msg[ll] = (uint8_t)label[ll]; ll++; }
  msg[ll++] = 0; for (size_t i = 0; i < dlen && ll + i < 127; i++) msg[ll + i] = data[i];
  n = ll + dlen;   /* dlen is bounded by the caller: 76 bytes for the key derivation */
  for (uint8_t c = 0; need; c++) { uint8_t h[20]; msg[n] = c; nw_hmac_sha1(key, klen, msg, n + 1, h);
    size_t k = need > 20 ? 20 : need; for (size_t i = 0; i < k; i++) *out++ = h[i]; need -= k; } }
