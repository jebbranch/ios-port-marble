# Ath12kWifi

Linux ath12k, mac80211 and cfg80211 on XNU, with userspace authentication
through wpa_supplicant 2.12. Board callbacks provide power, PCIe, BAR access,
MSI, DMA, firmware loading and the native network interface.

[Component documentation](https://unixnu.com/drivers/ath12k-wifi) describes
board integration; the [catalog](https://unixnu.com/reference/components)
records build inputs and dependencies.

## marble ath11k backend

The marble build selects `tools/sources-ath11k.txt` and prepares the pinned
Linux ath11k sources with `tools/prepare-ath11k.py`. It targets WCN6855 hw2.1
(PCI 17cb:1103), sharing mac80211, cfg80211, MHI, QRTR and the native control
endpoint. The device build entry point is `scripts/build_marble_wifi.py` in
UniXNU-marble. Board callbacks and firmware inputs belong to that repository.
Firmware bootstrap and hardware scan results are recorded in its `CURRENT.json`.

## Runtime

`tools/source-lock.json` pins Linux v7.0.14, matching wireless headers,
FreeBSD headers and the supplicant archive. `tools/fetch-sources.sh` verifies
these inputs; generated sources and binaries live in `deps/` and `out/`.

Production startup initializes cfg80211/mac80211 before ath12k. The manifest
selects `src/nw_mainline.c`; the retired `nw_sta.c`, `nw_mac80211.c` and
`nw_crypto.c` remain available to legacy fixtures.

The native endpoint `org.unixnu.wifi.control` queues requests on the wireless
worker. `src/nw_cfg80211_control.c` maps scan, association, key, EAPOL and
radio commands to upstream interfaces. Owner IDs and connection generations
identify active requests. Radio off closes the interface; power-on waits
for a network selection from wifid. Owner teardown releases its interface.

`userspace/driver_sheng.c` connects the original supplicant SME/WPA state
machine to this endpoint. Settings supplies profiles through ADD/SET/
SELECT_NETWORK. The active profile lives in RAM; saved credentials come
from the OS. Supported profiles are open and WPA2-PSK with PMF.

Each accepted join has a 30-second deadline. Cancellation completes a
pending request once; success follows port authorization. Association RSSI
comes from the returned BSS. The worker refreshes cached peer RSSI once per
second and publishes changes of at least 3 dB.

Portable AES/CCM/GCM/CMAC/SIV providers come from the pinned supplicant.
`src/nw_crypto_mac80211.c` supplies mac80211 crypto interfaces; ath12k
hardware offload handles ordinary traffic.

## Packet handling

- TX retains the original mbuf until a direct copy into DMA; RX copies skb
  fragments into aligned mbufs and submits batches of up to 64 frames.
- skb clones and A-MSDU fragments share reference-counted DMA storage.
  Copy-on-write detaches writable storage. Six reuse classes cache up to 1 MiB.
  A DMA allocation holds packet bytes only; its descriptor, reference counts
  and fragment table are heap memory, so a board that maps the window
  uncached pays that cost for packet bytes alone.
- mac80211 supplies aggregation, reordering, FQ and AQL. IFF_NO_QUEUE
  enters its scheduler directly; bounded fallback queues retain busy frames.
- NAPI rotates through contexts with a 256-packet/2 ms budget. Work dispatch
  has a 64-item/2 ms budget, with bottom-half priority and ordinary-work fairness.
- MSI delivery wakes the worker; `nw_wifi_wait_ms` supplies timer deadlines.
  Monotonic clocks and hardware busy delays use board callbacks.
- Atomic locks protect shared state. The single wireless executor serializes
  mutex/RTNL work while completion queues and NAPI continue firmware progress.

`nw_wifi_buffer_stats_get` exposes allocation, copy, clone and reuse counters.
`nw_wifi_link_state` reads cached PHY rate, MCS, stream count, bandwidth and RSSI.
Both use existing driver state without firmware statistics requests.

While joined, the worker logs the driver's receive counters every 30 s in
which frames arrived: `wifi: rx reo ...` (frames per destination ring,
exception and error pushes by code; `reo0` counts frames delivered without
a reorder queue) and `wifi: rx rings ...` (free refill entries, waiting
entries, frames dropped at the boundary). With `nw_board.diagnostics` set
and at least 20000 frames in that time it also requests the firmware's
receive statistics, at most once in two minutes, logged as
`wifi: fw stats t<tag>: <word>=<value>`.

## sheng integration

WifiProbe owns power, PCIe, MSI and the 128 MiB WCN7850 DMA window. WifiNet
binds en0 and Apple80211 to `include/nw_abi.h`. The board emits the 540-byte
24A446 asynchronous association result and the legacy result-query events.

The signed service runs as `/usr/libexec/unixnu-wpa_supplicant` with its
configuration under `/private/var/ngpu/etc/wifi/`. Packaging merges its own
job into the OS launchd cache, serializes it with Apple `plutil binary1`,
signs the cache and executable, and collects their trust hashes.

Firmware loads through VFS from
`/private/var/ngpu/firmware/ath12k/WCN7850/hw2.0/` after addon installation.
`package/WifiFirmware.h` in the board repository defines the loader/packer path;
a local manifest pins the firmware sizes and SHA-256 hashes.

## Build and verification

```sh
python3 tools/fetch-sources.py --check
python3 tools/build-supplicant.py --target ios
```

The daemon output is `out/wpa-ios/bin/wpa_supplicant`. The board builder signs
and packages it. The consuming kernel build compiles the production manifest.

| Check | Command |
| --- | --- |
| Core lifecycle, radio transitions, join deadlines, RSSI and packet ownership | `python3 tools/test-mainline-runtime.py` |
| Original SME/WPA handshake and rekey | `python3 tools/test-supplicant.py` |
| Native cfg80211 command mapping | `python3 tools/test-cfg80211-control.py` |
| Buffers, scheduling and locks | `python3 tools/test-buffers.py`, `python3 tools/test-runtime.py`, `python3 tools/test-locks.py` |
| Crypto and notification encoding | `python3 tools/test-crypto.py --kernel`, `python3 tools/test-wireless-events.py` |
| Exact production objects and XNU imports | `python3 tools/check-mainline-link.py --build-report FILE --kernel FILE` |

Host fixtures use ASan/UBSan with simulated radio operations. The lifecycle
check releases all 294 tracked allocations. The production audit covers
125 objects and 13 XNU imports, with zero missing imports or duplicate definitions.
Run checks affected by changes and retain their reports in `out/`.

On sheng with 24A446, device checks cover Wi-Fi toggling, SSID scanning,
WPA2 association, DHCP, traffic and join-result delivery. Current device
checks pending: cancellation/timeout completion, runtime signal display,
Data firmware startup, throughput, jitter and heat.
