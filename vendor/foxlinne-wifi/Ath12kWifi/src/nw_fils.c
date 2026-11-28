/* Original mac80211 FILS framing with pinned portable AES-SIV providers. */
#include "lx_pci.h"
#include "ieee80211_i.h"
#include "fils_aead.h"
#include <crypto/aes.h>
int nw_aes_siv_encrypt(const u8 *, size_t, const u8 *, size_t, size_t, const u8 **, const size_t *, u8 *);
int nw_aes_siv_decrypt(const u8 *, size_t, const u8 *, size_t, size_t, const u8 **, const size_t *, u8 *);
/* The original framing calls allow overlap. Publish the provider's scratch
 * output only on success, including authenticated decryption. */
static int aes_siv_encrypt(const u8 *key, size_t key_length, const u8 *plain, size_t length,
                           size_t count, const u8 *address[], size_t sizes[], u8 *output) {
  if (length > 65520) return -EINVAL;
  u8 *scratch = kmalloc(length + AES_BLOCK_SIZE, GFP_KERNEL);
  if (!scratch) return -ENOMEM;
  int result = nw_aes_siv_encrypt(key, key_length, plain, length, count, address, sizes, scratch);
  if (!result) memcpy(output, scratch, length + AES_BLOCK_SIZE);
  memzero_explicit(scratch, length + AES_BLOCK_SIZE); kfree(scratch);
  return result ? -EINVAL : 0;
}
static int aes_siv_decrypt(const u8 *key, size_t key_length, const u8 *cipher, size_t length,
                           size_t count, const u8 *address[], size_t sizes[], u8 *output) {
  if (length < AES_BLOCK_SIZE || length > 65536) return -EINVAL;
  u8 *scratch = kmalloc(length, GFP_KERNEL);
  if (!scratch) return -ENOMEM;
  int result = nw_aes_siv_decrypt(key, key_length, cipher, length, count, address, sizes, scratch);
  if (!result) memcpy(output, scratch, length - AES_BLOCK_SIZE);
  memzero_explicit(scratch, length); kfree(scratch);
  return result ? -EINVAL : 0;
}
#include "mac80211-fils-protocol.inc"
