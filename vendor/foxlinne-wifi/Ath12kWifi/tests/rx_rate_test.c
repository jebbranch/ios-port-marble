/* Included after the extracted production rate parser and upstream helpers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned warnings;
void __ath12k_warn(struct device *device, const char *format, ...) { (void)device; (void)format; warnings++; }
int main(void) {
  struct ath12k_base *base = calloc(1, sizeof(*base));
  struct ath12k_dp *dp = calloc(1, sizeof(*dp));
  struct ath12k_pdev_dp *pdev = calloc(1, sizeof(*pdev));
  assert(base && dp && pdev); dp->ab = base; pdev->dp = dp;
  struct ieee80211_rx_status status;
  struct hal_rx_desc_data info = { .rx_status = &status, .pkt_type = RX_MSDU_START_PKT_TYPE_11AC,
                                  .bw = ATH12K_BW_80, .nss = 2, .sgi = 1 };
  for (unsigned mcs = 0; mcs <= 11; mcs++) {
    memset(&status, 0, sizeof(status)); info.rate_mcs = mcs; warnings = 0;
    ath12k_dp_rx_h_rate(pdev, &info);
    assert(status.encoding == RX_ENC_VHT && status.rate_idx == mcs && status.nss == 2);
    assert(status.bw == RATE_INFO_BW_80 && (status.enc_flags & RX_ENC_FLAG_SHORT_GI) && !warnings);
    if (mcs >= 10) {
      memset(&status, 0, sizeof(status)); warnings = 0;
      ath12k_dp_rx_h_rate_before(pdev, &info);
      assert(status.nss == 0 && warnings == 1);
    }
  }
  memset(&status, 0, sizeof(status)); info.rate_mcs = 12; warnings = 0;
  ath12k_dp_rx_h_rate(pdev, &info); assert(warnings == 1);
  free(pdev); free(dp); free(base);
  puts("PASS: VHT MCS 0-11 retain NSS, width and GI; original MCS 10/11 lose NSS, patched MCS 10/11 emit no warning; MCS 12 still reports invalid");
}
