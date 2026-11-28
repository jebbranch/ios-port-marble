/* mac80211 crypto platform wrappers. Algorithms are the locked hostap sources. */
#include "lx_pci.h"
#include "aes_wrap.h"
#include "aead_api.h"
#include "aes_cmac.h"
#include "aes_gmac.h"

struct crypto_aead { u8 key[32]; size_t key_len, mic_len; bool ccm; };
struct nw_cmac_key { struct crypto_shash base; u8 key[32]; size_t key_len; };

static void wipe(void *memory, size_t len) {
  volatile u8 *p = memory;
  while (len--) *p++ = 0;
}

struct crypto_aead *aead_key_setup_encrypt(const char *algorithm, const u8 key[],
                                          size_t key_len, size_t mic_len) {
  bool ccm = !strcmp(algorithm, "ccm(aes)");
  if ((!ccm && strcmp(algorithm, "gcm(aes)")) ||
      (key_len != 16 && key_len != 32) ||
      (ccm ? mic_len != 8 && mic_len != 16 : mic_len != 16))
    return ERR_PTR(-EINVAL);
  struct crypto_aead *tfm = kzalloc(sizeof(*tfm), GFP_KERNEL);
  if (!tfm) return ERR_PTR(-ENOMEM);
  memcpy(tfm->key, key, key_len);
  tfm->key_len = key_len; tfm->mic_len = mic_len; tfm->ccm = ccm;
  return tfm;
}

/* Upstream hostap GCM requires separate input/output; both modes access words.
 * Keep scratch aligned and publish plaintext only after tag verification.
 * Ordinary data encryption remains in ath12k hardware offload. */
static int aead_crypt(struct crypto_aead *tfm, const u8 *iv, const u8 *aad,
                      size_t aad_len, u8 *data, size_t len, u8 *mic, bool decrypt) {
  if (!tfm || IS_ERR(tfm) || len > 65535 || aad_len > 30 ||
      (tfm->ccm && (iv[0] & 7) != 1)) return -EINVAL;
  size_t stride = ALIGN(len ? len : 1, 16);
  u8 *scratch = kmalloc(stride * 2 + 32, GFP_ATOMIC);
  if (!scratch) return -ENOMEM;
  u8 *input = scratch, *output = scratch + stride, *associated = output + stride;
  u8 tag[16] __aligned(8);
  if (len) memcpy(input, data, len);
  if (aad_len) memcpy(associated, aad, aad_len);
  if (decrypt) memcpy(tag, mic, tfm->mic_len);
  int result;
  if (tfm->ccm) {
    if (decrypt)
      result = aes_ccm_ad(tfm->key, tfm->key_len, iv + 1, tfm->mic_len,
                          input, len, associated, aad_len, tag, output);
    else
      result = aes_ccm_ae(tfm->key, tfm->key_len, iv + 1, tfm->mic_len,
                          input, len, associated, aad_len, output, tag);
  } else {
    if (decrypt)
      result = aes_gcm_ad(tfm->key, tfm->key_len, iv, 12, input, len,
                          associated, aad_len, tag, output);
    else
      result = aes_gcm_ae(tfm->key, tfm->key_len, iv, 12, input, len,
                          associated, aad_len, output, tag);
  }
  if (!result) {
    if (len) memcpy(data, output, len);
    if (!decrypt) memcpy(mic, tag, tfm->mic_len);
  }
  wipe(tag, sizeof(tag)); wipe(scratch, stride * 2 + 32); kfree(scratch);
  return result ? (decrypt ? -EBADMSG : -EIO) : 0;
}

int aead_encrypt(struct crypto_aead *tfm, u8 *iv, u8 *aad, size_t aad_len,
                  u8 *data, size_t len, u8 *mic) {
  return aead_crypt(tfm, iv, aad, aad_len, data, len, mic, false);
}
int aead_decrypt(struct crypto_aead *tfm, u8 *iv, u8 *aad, size_t aad_len,
                  u8 *data, size_t len, u8 *mic) {
  return aead_crypt(tfm, iv, aad, aad_len, data, len, mic, true);
}
void aead_key_free(struct crypto_aead *tfm) {
  if (!tfm || IS_ERR(tfm)) return;
  wipe(tfm, sizeof(*tfm)); kfree(tfm);
}

struct crypto_shash *ieee80211_aes_cmac_key_setup(const u8 key[], size_t len) {
  if (len != 16 && len != 32) return ERR_PTR(-EINVAL);
  struct nw_cmac_key *tfm = kzalloc(sizeof(*tfm), GFP_KERNEL);
  if (!tfm) return ERR_PTR(-ENOMEM);
  memcpy(tfm->key, key, len); tfm->key_len = len;
  return &tfm->base;
}
int ieee80211_aes_cmac(struct crypto_shash *base, const u8 *aad, const u8 *data,
                       size_t len, u8 *mic, unsigned mic_len) {
  if (!base || IS_ERR(base) || (mic_len != 8 && mic_len != 16) || len < mic_len)
    return -EINVAL;
  const struct nw_cmac_key *tfm = container_of(base, struct nw_cmac_key, base);
  static const u8 zero[16];
  u8 tag[16]; const u8 *parts[4]; size_t lengths[4], count = 0;
  parts[count] = aad; lengths[count++] = 20;
  /* Same IEEE 802.11w masking as upstream aes_cmac.c. */
  if ((aad[0] & 0xfc) == 0x80) {
    if (len < mic_len + 8) return -EINVAL;
    parts[count] = zero; lengths[count++] = 8; data += 8; len -= 8;
  }
  if (len > mic_len) { parts[count] = data; lengths[count++] = len - mic_len; }
  parts[count] = zero; lengths[count++] = mic_len;
  int result = omac1_aes_vector(tfm->key, tfm->key_len, count, parts, lengths, tag);
  if (!result) memcpy(mic, tag, mic_len);
  wipe(tag, sizeof(tag));
  return result ? -EIO : 0;
}
void ieee80211_aes_cmac_key_free(struct crypto_shash *base) {
  if (!base || IS_ERR(base)) return;
  struct nw_cmac_key *tfm = container_of(base, struct nw_cmac_key, base);
  wipe(tfm, sizeof(*tfm)); kfree(tfm);
}
struct crypto_aead *ieee80211_aes_gmac_key_setup(const u8 key[], size_t len) {
  return aead_key_setup_encrypt("gcm(aes)", key, len, 16);
}
int ieee80211_aes_gmac(struct crypto_aead *tfm, const u8 *aad, u8 *nonce,
                       const u8 *data, size_t len, u8 *mic) {
  if (!tfm || IS_ERR(tfm) || len < 16 || len > 65535) return -EINVAL;
  u8 *associated = kmalloc(20 + len, GFP_ATOMIC);
  if (!associated) return -ENOMEM;
  memcpy(associated, aad, 20); memcpy(associated + 20, data, len - 16);
  memset(associated + 20 + len - 16, 0, 16);
  if ((aad[0] & 0xfc) == 0x80) {
    if (len < 24) { kfree(associated); return -EINVAL; }
    memset(associated + 20, 0, 8);
  }
  u8 tag[16] __aligned(8);
  int result = aes_gmac(tfm->key, tfm->key_len, nonce, 12, associated, 20 + len, tag);
  if (!result) memcpy(mic, tag, sizeof(tag));
  wipe(tag, sizeof(tag)); wipe(associated, 20 + len); kfree(associated);
  return result ? -EIO : 0;
}
void ieee80211_aes_gmac_key_free(struct crypto_aead *tfm) { aead_key_free(tfm); }
