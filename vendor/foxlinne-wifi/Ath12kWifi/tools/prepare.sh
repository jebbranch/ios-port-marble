#!/bin/bash
set -euo pipefail
# Lays out the generated include trees the sources are built from: out/pinned (public headers used as they are), out/ath12k-src and out/mhi-src
# (links to the driver sources with the trace points replaced by empty ones).
cd "$(dirname "$0")/.."
bash tools/pin-headers.sh
L=$PWD/deps/linux
A=$L/drivers/net/wireless/ath/ath12k; O=out/ath12k-src
rm -rf $O; mkdir -p $O/wifi7
for f in $A/*.c $A/*.h; do ln -s $f $O/; done
for f in $A/wifi7/*.c $A/wifi7/*.h; do ln -s $f $O/wifi7/; done
rm -f $O/trace.h
{ echo '#ifndef LX_ATH12K_TRACE_H'; echo '#define LX_ATH12K_TRACE_H'; grep -ohE "trace_ath12k_[a-z_0-9]+" $A/*.c $A/*.h $A/wifi7/*.c | sort -u | sed 's/.*/#define & (...) ((void)0)/' | sed 's/ (\.\.\.)/(...)/'; echo '#endif'; } > $O/trace.h
H=$L/drivers/bus/mhi/host; M=out/mhi-src
rm -rf $M; mkdir -p $M
for f in $H/*.c $H/internal.h; do ln -s $f $M/; done
cp shim/mhi/trace.h $M/trace.h
# One call is added to the generated copy of boot.c: the chip's boot loader is given its MHI contexts (and some time after the reset) before the image is offered.
rm -f out/mhi-src/boot.c; cp $H/boot.c out/mhi-src/boot.c
python3 - <<'PY'
p='out/mhi-src/boot.c'; s=open(p).read()
mark='\tret = request_firmware(&firmware, fw_name, dev);'
assert mark in s
if 'nw_mhi_pre_load' not in s:
    s=s.replace(mark,'\t{ extern void nw_mhi_pre_load(struct mhi_controller *); nw_mhi_pre_load(mhi_cntrl); }\n'+mark,1)
open(p,'w').write(s)
PY
# The refill ring of the receive buffers is the largest user of the endpoint's DMA window; the copy of dp.h makes it 512 buffers instead of 4096.
# The firmware spreads those buffers over one ring per radio MAC, and the ring of the MAC that receives nothing keeps what it gets: at 2048
# entries it ends up holding all 512 and the other MAC stops receiving after some hundred frames. At 128 entries it can hold at most a quarter.
rm -f $O/dp.h; sed -e 's/^#define DP_RXDMA_BUF_RING_SIZE\t\t4096/#define DP_RXDMA_BUF_RING_SIZE\t\t512/' -e 's/^#define DP_RX_MAC_BUF_RING_SIZE\t\t2048/#define DP_RX_MAC_BUF_RING_SIZE\t\t128/' $A/dp.h > $O/dp.h
grep -q 'DP_RXDMA_BUF_RING_SIZE.*512' $O/dp.h && grep -q 'DP_RX_MAC_BUF_RING_SIZE.*128' $O/dp.h || { echo "dp.h patch missed" >&2; exit 1; }
# Sending a command is the condition of the wait in ath12k_wmi_cmd_send: that wait must evaluate it once per round and never after it has succeeded.
rm -f $O/wmi.c; sed '/^int ath12k_wmi_cmd_send(/,/^}/ s/wait_event_timeout(/wait_event_timeout_once(/' $A/wmi.c > $O/wmi.c
grep -q 'wait_event_timeout_once' $O/wmi.c || { echo "wmi.c patch missed" >&2; exit 1; }
# This board has no radio switch on the chip's pin 48: the WCN7850 entry of the copy of wifi7/hw.c names no rfkill pin, so no switch is configured.
rm -f $O/wifi7/hw.c; sed 's/^\t\t\.rfkill_pin = 48,/\t\t.rfkill_pin = 0,/' $A/wifi7/hw.c > $O/wifi7/hw.c
grep -q 'rfkill_pin = 48' $O/wifi7/hw.c && { echo "wifi7/hw.c patch missed" >&2; exit 1; }
# The WCN7850 firmware takes its MLO peer count from the MLO fields of the host capability message and fails an allocation in WMI_INIT
# when they are missing, so the copy of qmi.c always sends them. That is only about the message: whether this board does MLO stays what
# the firmware's PHY capability reply says (it says no here), so the host keeps a wiphy per radio and creates scan interfaces without
# links.
python3 - "$A/qmi.c" "$O/qmi.c" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
old = ('\tif (!ag->mlo_capable) {\n'
       '\t\tath12k_dbg(ab, ATH12K_DBG_QMI,\n'
       '\t\t\t   "MLO is disabled hence skip QMI MLO cap");\n'
       '\t\treturn 0;\n'
       '\t}\n\n')
assert s.count(old) == 1, "qmi.c mlo mark"
s = s.replace(old, '', 1)
# The PHY capability reply can say the firmware takes host memory on demand (TLV 0x17); the host capability message then says the host
# offers it (TLV 0x33). Without that answer the firmware leaves its host memory segment unused. This copy offers it whatever the reply said.
def elem(tlv, struct, field, flag):
    return ('\t{\n\t\t.data_type\t= %s,\n\t\t.elem_len\t= 1,\n\t\t.elem_size\t= sizeof(u8),\n\t\t.array_type\t= NO_ARRAY,\n'
            '\t\t.tlv_type\t= %s,\n\t\t.offset\t\t= offsetof(%s, %s),\n\t},\n'
            % ('QMI_OPT_FLAG' if flag else 'QMI_UNSIGNED_1_BYTE', tlv, struct, field + ('_valid' if flag else '')))
for mark, tlv, struct, field in (('\t\t\t\t\t   feature_list),\n\t},\n', '0x33', 'struct qmi_wlanfw_host_cap_req_msg_v01', 'nw_on_demand_mem'),
                                 ('\t\t\t\t\t   single_chip_mlo_support),\n\t},\n', '0x17', 'struct qmi_wlanfw_phy_cap_resp_msg_v01', 'nw_on_demand_ddr')):
    assert s.count(mark) == 1, "qmi.c element mark " + tlv
    s = s.replace(mark, mark + elem(tlv, struct, field, True) + elem(tlv, struct, field, False), 1)
old = ('\tif (resp.single_chip_mlo_support_valid && resp.single_chip_mlo_support)\n'
       '\t\tab->single_chip_mlo_support = true;\n')
assert s.count(old) == 1, "qmi.c phy cap mark"
s = s.replace(old, old + '\tnw_on_demand_ddr = resp.nw_on_demand_ddr_valid && resp.nw_on_demand_ddr;\n'
              '\tath12k_dbg(ab, ATH12K_DBG_QMI, "phy capability: host memory on demand %u\\n", nw_on_demand_ddr);\n', 1)
old = '\tret = ath12k_host_cap_parse_mlo(ab, &req);\n'
assert s.count(old) == 1, "qmi.c host cap mark"
s = s.replace(old, '\treq.nw_on_demand_mem_valid = 1;\n\treq.nw_on_demand_mem = 1;\n\n' + old, 1)
old = '#include "qmi.h"\n'
assert s.count(old) == 1, "qmi.c include mark"
s = s.replace(old, old + '\n/* the firmware said it takes host memory on demand */\nstatic u8 nw_on_demand_ddr;\n', 1)
if os.path.lexists(dst): os.remove(dst)
open(dst, 'w').write(s)
PY
python3 - "$A/qmi.h" "$O/qmi.h" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
for old, new in (('\tu64 feature_list;\n\n};', '\tu64 feature_list;\n\tu8 nw_on_demand_mem_valid;\n\tu8 nw_on_demand_mem;\n};'),
                 ('\tu8 single_chip_mlo_support;\n};', '\tu8 single_chip_mlo_support;\n\tu8 nw_on_demand_ddr_valid;\n\tu8 nw_on_demand_ddr;\n};'),
                 ('QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN\t\t261', 'QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN\t\t265'),
                 ('QMI_WLANFW_PHY_CAP_RESP_MSG_V01_MAX_LEN\t\t18', 'QMI_WLANFW_PHY_CAP_RESP_MSG_V01_MAX_LEN\t\t22')):
    assert s.count(old) == 1, "qmi.h mark " + old[:30]
    s = s.replace(old, new, 1)
if os.path.lexists(dst): os.remove(dst)
open(dst, 'w').write(s)
PY
# The WCN7850 firmware of this board reports an all-zero address (a scan interface made with it stops the firmware's replies) or, from other
# builds, a random one under the chip maker's default prefix 00:03:7f that changes at every start: the copy of mac.c asks the board
# (nw_board_mac) for the address of a radio whose address is not valid or carries that prefix.
python3 - "$A/mac.c" "$O/mac.c" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
old = '\t\t\tar->mac_addr[4] += ar->pdev_idx;\n\t\t}\n'
assert s.count(old) == 1, "mac.c address mark"
s = s.replace(old, old + '\t\tif (!is_valid_ether_addr(ar->mac_addr) ||\n\t\t    (!ar->mac_addr[0] && ar->mac_addr[1] == 0x03 && ar->mac_addr[2] == 0x7f)) {\n\t\t\tnw_board_mac(ar->mac_addr);\n\t\t\tar->mac_addr[4] += ar->pdev_idx;\n\t\t}\n', 1)
s = s.replace('#include "mac.h"\n', '#include "mac.h"\nvoid nw_board_mac(u8 *a);\n', 1)
# A station without links has no scan link to speak of: the copy scans on the interface's own link (0) so the scan vdev is made the way the
# interface's ordinary vdev is.
old = '\tscan_links_map = ~ahvif->links_map & ATH12K_SCAN_LINKS_MASK;\n'
assert s.count(old) == 1, "mac.c scan link mark"
s = s.replace(old, '\tif (!ahvif->links_map)\n\t\treturn 0;\n' + old, 1)
assert 'void nw_board_mac' in s, "mac.c include mark"
if os.path.lexists(dst): os.remove(dst)
open(dst, 'w').write(s)
PY
# The receive reorder queue descriptors are read by the endpoint, so the copies of dp_rx.c and wifi7/dp_rx.c take them from the DMA window
# (ordinary heap memory is not reachable by it) and give them back to it.
python3 - "$A/wifi7/dp_rx.c" "$O/wifi7/dp_rx.c" "$A/dp_rx.c" "$O/dp_rx.c" <<'PY'
import os, sys
for src, dst, old, new in ((sys.argv[1], sys.argv[2], 'vaddr = kzalloc(hw_desc_sz + HAL_LINK_DESC_ALIGN - 1, GFP_ATOMIC);', 'vaddr = lx_dma_zalloc(hw_desc_sz + HAL_LINK_DESC_ALIGN - 1);'),
                           (sys.argv[3], sys.argv[4], '\t\tkfree(tid_qbuf->vaddr);', '\t\tlx_dma_kfree(tid_qbuf->vaddr);')):
    s = open(src).read()
    assert s.count(old) == 1, "reoq mark " + old[:20]
    s = s.replace(old, new, 1)
    if dst == sys.argv[2]:
        # Buffers taken from a destination ring go back to the refill ring even when none of them completed a frame (all pushed with an error,
        # or the last one continues): skipping the refill then loses them for good, and after some hundred frames the device has none left.
        old = '\tif (!total_msdu_reaped)\n\t\tgoto exit;\n\n\tfor (device_id = 0; device_id < ATH12K_MAX_DEVICES; device_id++) {\n\t\tif (!num_buffs_reaped[device_id])\n\t\t\tcontinue;\n\n\t\tpartner_dp = ath12k_dp_hw_grp_to_dp(dp_hw_grp, device_id);\n\t\trx_ring = &partner_dp->rx_refill_buf_ring;\n\n\t\tath12k_dp_rx_bufs_replenish(partner_dp,'
        assert s.count(old) == 1, "refill mark"
        s = s.replace(old, old.replace('\tif (!total_msdu_reaped)\n\t\tgoto exit;\n\n', ''), 1)
        assert s.count('exit:\n\treturn total_msdu_reaped;') == 1, "refill exit mark"
        s = s.replace('exit:\n\treturn total_msdu_reaped;', '\treturn total_msdu_reaped;', 1)
    if os.path.lexists(dst): os.remove(dst)
    open(dst, 'w').write(s)
PY
# The copy also shows the bytes of an event it does not know.
python3 - "$O/wmi.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
mark = '\t\tath12k_dbg(ab, ATH12K_DBG_WMI, "Unknown eventid: 0x%x\\n", id);\n'
assert mark in s, "unknown event mark"
s = s.replace(mark, mark + '\t\tath12k_dbg_dump(ab, ATH12K_DBG_WMI, NULL, "", skb->data, skb->len);\n', 1)
open(p, 'w').write(s)
PY
# Without debugfs the firmware's extended statistics are thrown away: the copy of dp_htt.c hands them to nw_htt_ext_stats, which logs them.
rm -f $O/dp_htt.c; sed 's/^\t\tath12k_debugfs_htt_ext_stats_handler(ab, skb);/\t\t{ extern void nw_htt_ext_stats(struct ath12k_base *, struct sk_buff *); nw_htt_ext_stats(ab, skb); }/' $A/dp_htt.c > $O/dp_htt.c
grep -q 'nw_htt_ext_stats' $O/dp_htt.c || { echo "dp_htt.c patch missed" >&2; exit 1; }
ln -sf $L/drivers/bus/mhi/common.h out/common.h
python3 tools/apply-ath12k-patches.py
python3 tools/apply-os-patches.py
python3 tools/prepare-mac80211.py
python3 tools/prepare-mac80211.py --subsystem cfg80211
python3 tools/prepare-crypto.py
python3 tools/prepare-cfg80211-native.py
python3 tools/prepare-cfg80211-events.py
