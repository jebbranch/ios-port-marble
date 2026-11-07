# Questions for an AudioReach / Qualcomm SPF expert — XNU on SM7475 (marble)

Date: 2026-11-07. Written to be **self-contained**: you do not need our repo to answer.
Everything under "Observed" is a real measurement from this phone, not a hypothesis.

## 0. What we are doing, and the constraints

- Porting **XNU 8796 (iOS 16 era)** to a **Redmi Note 12 Turbo** (`marble`, **SM7475**, Qualcomm SoC name **UKEE**, Adreno 725, **AW882xx** speaker amps on **TERTIARY TDM**, no WSA codec).
- The phone boots our kernel **purely from RAM** (`fastboot boot` + a shim). **No flashing, no partition writes, no GPT/persist/modemst changes.** So please do not suggest anything that writes to disk.
- The **ADSP is already running** (loaded by the Android bootloader before we take over). We do **not** load ADSP firmware ourselves. We talk to it over **GLINK → GPR → APM/SPF (AudioReach)**.
- **Control plane is fully green**: `GRAPH_OPEN`, `PREPARE`, `START`, `SET_CFG`, `SHM_MAP` all return `status=0` on our accepted path, and `DATA_BUFFER_V2` gets `DATA_BUFFER_DONE`.
- **There is no audible sound.** That is the whole problem. 60 RAM boots spent so far (budget 120).
- Apps-smmu **S1 translation works** (`ATS1PR` → `PAR=0`, `CBA2R=1`, no new fault bits). **S2 bypass is not available** (`S2CR.BYPASS` gets rewritten to FAULT), so we cannot do what some other ports do (pass-through).
- We can run more experiments cheaply **if you tell us exactly what to send** (opcode, dst, body words). Please be concrete: we will ship it and report the raw reply.

## 1. Wire facts we have established (so you don't have to re-derive them)

- GPR header 24 B; `apm_cmd_header_t {payload_address_lsw, msw, mem_map_handle, payload_size}` 16 B.
- Opcodes: `GRAPH_OPEN 0x01001000`, `PREPARE 0x01001001`, `START 0x01001002`, `STOP 0x01001003`, `CLOSE 0x01001004`, `SET_CFG 0x01001006`, `GET_CFG 0x01001007`, `SHM_MAP 0x0100100C`, `SHM_UNMAP 0x0100100D`, **`REGISTER_MODULE_EVENTS 0x0100100E`**, `PRM_REQ_HW_RSC 0x0100100F`, `DATA_BUFFER_V2 0x0400100A`, `DATA_BUFFER_DONE_V2 0x05001004`, basic result `0x02001005`, `GET_SPF_STATE 0x01001021` → `0x02001007`.
- `SHM_MAP`: `mem_pool_id=3`, `property_flag=4` (= `1 << (pool-1)`), region size `0x5000` (20480 B). The address we advertise is an **IOVA** (`0x1FFE8000`) with **`msw=1`**, which we determined is a **stream index**, not a 33-bit address (setting `msw=0` used to hang the whole SoC).
- **One mapping is used for both the graph descriptor and the PCM buffers** (descriptor at region offset 0, buffers at 0/0x1000/0x2000/0x3000).
- Module ids we use: `WR_SHARED_MEM_EP 0x07001000`, `PCM_CNV 0x07001003`, `PCM_DEC 0x07001005`, `TDM_SINK 0x0700100E`, `SAL 0x07001010`, `SPLITTER 0x07001011`, `MFC 0x07001015`, `DATA_LOGGING 0x0700101A`, `MUX_DEMUX 0x07001098`, plus an AW-specific `0x10013D01`.
- Our instance ids: WR `0x40BD`, SAL `0x4012`, MFC `0x48BA`, TDM `0x428F`, AW `0x42D3`, SPLITTER `0x418E`, MUX_DEMUX `0x4006`, DATA_LOGGING `0x4007`.

## 2. The single most important anomaly (Q1)

There are **two body shapes** for `GRAPH_OPEN`, and they behave completely differently on our path.

**A form — flat parameter records.** 16-byte headers `{iid=1, param_id, param_size, error_code}`, bodies 8-aligned. Our accepted body is 760 B and walks exactly:

```
SG_CONFIG        size= 48   words: 1 b0000002 3 | 800100e 4 2 | 800100f 4 2 | 8001010 4 ffffffff
CONTAINER_CONFIG size=100   words: 1 e0000002 7 | 8001011 8 1 b001001 | 8001012 4 4 | 8001013 4 1000 | 8001014 4 2 | …
MODULE_LIST      size= 80   words: 1 b0000002 e0000002 8 | 7001010 4012 | 700101a 4007 | 7001011 418e |
                                   700100e 428f | 7001098 4006 | 7001015 48ba | 10013d01 42d3 | 7001000 40bd
MODULE_PROP      size=196   words: 8 | (iid, 1, 08001015, 8, in, out) x 8
MODULE_CONN      size=244   words: 15 | (src_iid, sport, dst_iid, dport) x 15
```
This body returns **`status=0` on every boot it has ever run**, then PREPARE/START succeed and 120 `DATA_BUFFER_V2`s are acknowledged.

**B form — ACF graph index.** `{u32 total_size, u32 subgraph_count}` then per subgraph `{u32 graph_key, u32 size, body}`. Our current-generation copy is 2960 B, keys `b0000002 / b0000025 / b0000024 / b0000023`, sizes `852 / 800 / 300 / 968`, and it walks with **zero slack**.

**Observed, five separate boots, all with the same result:**

| body sent (B form) | reply |
|---|---|
| 2960 B, region offset 0 | `no response` |
| 2960 B again, with a liveness probe added | `no response` |
| 1536 B (same prefix, truncated) | `no response` |
| 1024 B (same prefix, shorter) | `no response` |
| **760 B** (same prefix, same length as the *accepted* A-form body) | `no response` |

And in every one of those boots, **the APM stopped answering everything afterwards**:
```
graph_open/ukee2960: no response
SWEET MARK: livechk v1 rc=-1
SWEET GLINK: spf_state post-vgraph: no rsp        <- GET_SPF_STATE also unanswered
APM graph_open/vp-wr …                            <- our control open, which answers status=0 in every other boot
graph_open/vp-wr: no response                     <- also dead
```
`no rsp` counter = 0 in the healthy boots, so this is not a logging artefact.

**The vendor's own accepted traffic uses B form.** From a kprobe capture of one accepted Android playback (both directions, timestamps in ms):
```
GRAPH_OPEN src=0x2010 dst=1  addr=0      handle=0xb0d89128 payload_size=496    -> RX basic result status=0
GRAPH_OPEN src=0x2011 dst=1  addr=0      handle=0xb0d89128 payload_size=2872   -> status=0   (next playback: 2904)
GRAPH_OPEN src=0x2012 dst=1  addr=0x6000 handle=0xb0d89128 payload_size=2032   -> status=0
```
All three are **out-of-band** (`handle != 0`) and **share the handle used by the 95 `DATA_BUFFER_V2`s**. Their in-band `PREPARE`/`START` bodies name the subgraphs:
```
src 0x2010: {1, 0x08001005, 8,  1, b00000d5}
src 0x2011: {1, 0x08001005, 20, 4, b0000002, b0000025, b0000024, b0000023}
src 0x2012: {1, 0x08001005, 16, 3, b0000025, b0000024, b0000023}
```

### Q1 — Why does a B-form body make the APM go completely silent instead of returning an error?
A malformed or unsupported body should produce `AR_EBADPARAM`/`AR_EUNSUPPORTED`, not kill the service and every subsequent command including `GET_SPF_STATE`.
- Is there a known failure mode where the **OOB payload read** faults inside the DSP and takes down the APM service or the GPR channel?
- Does the OOB region have to be in a **specific memory pool**, or does `property_flag` have to match the mapping type?
- **Is it legal to reuse one mapping for both the graph descriptor and the PCM data buffers** (we do), or must the descriptor have its own `SHM_MAP`?
- Does B form require the graph key to be **pre-registered/loaded** on the DSP (ACDB graph key, or a preceding command) before `GRAPH_OPEN` will parse it?
- Are A form and B form handled by **different validation paths** inside APM?

**What would settle it:** one sentence like "the descriptor must be mapped separately / in pool X / after command Y" — that would invalidate five of our boots and give us a one-line fix.

## 3. Q2 — Preconditions for each `GRAPH_OPEN` body form
Same evidence as above. Specifically:
- What must be true **before** a B-form (ACF index) `GRAPH_OPEN` is sent, that is not required for an A-form (client-described topology) one?
- The vendor opens **three graphs on three source ports** (`0x2010`, `0x2011`, `0x2012`) and interleaves an **18504-byte APM-level `SET_CFG`** (out-of-band, its **own** handle `0xb0d92ac8`) between the stream-graph open and its PREPARE/START. Is that ordering mandatory? Is the third graph (`0x2012`, 3 subgraphs) required for playback, or is it something else (e.g. a feedback/echo-reference path)?
- We open **one** graph from `0x2010` only. We have also booted twice with the open coming from `0x2011`; both returned `status=0`, so the source port alone is not the difference.

## 4. Q3 — How do we observe *real consumption* from the AP side? (highest practical value)
**Observed:** our `DATA_BUFFER_DONE_V2` comes back in **1–2 ms** for a **1152-byte / 6 ms** frame, while the vendor spaces its buffers **20.5 ms** apart for **4096 B / 21.3 ms** (i.e. real time). So our DONE is a **queue receipt**, not proof of consumption. Historic "`not consumed = 0`" therefore proved nothing.
We also cannot read the amplifier: after our GPIO bit-bang phase, every AW882xx register read over GENI I2C returns NACK (`se15 err irq=0x409`), while reads *before* the bit-bang return real values (`0x2032 / 0xffff / 0x4003 / 0x38`).

**Questions:**
- Is there a **statistics / counters parameter** on `WR_SHARED_MEM_EP`, `PCM_DEC` or `PCM_CNV` readable with `GET_CFG` (e.g. `PARAM_ID_MODULE_STATISTICS`, frames/bytes processed, underruns)? Please give the **param id and body layout** if you know it.
- Is `DATA_BUFFER_DONE` documented as a receipt or as consumption? What is the correct AP-side way to know the DSP actually pulled and processed a buffer?
- Any DSP-side counter reachable from AP (SMEM item, shared register, `GET_CFG` on APM) that reports processed frames?

**What would settle it:** any readable "processed" counter. Without one, "no sound" has ~5 indistinguishable causes and every experiment is a guess.

## 5. Q4 — Is a single merged subgraph legal?
**Observed (boot 59, the most recent):** we took the accepted 760-byte A-form body and edited it **in place** (SG_CONFIG and CONTAINER_CONFIG asserted byte-identical) to add the converter stage:
```
MODULE_LIST  80 -> 96   count 8 -> 10, added (0x07001005, 0x40BF) PCM_DEC and (0x07001003, 0x40C0) PCM_CNV
MODULE_PROP 196 -> 244  added their two PORT_INFO records (in=1, out=1)
MODULE_CONN 244 -> 276  the single link 0x40BD:1 -> 0x4012:2 became
                        0x40BD:1 -> 0x40BF:2, 0x40BF:1 -> 0x40C0:2, 0x40C0:1 -> 0x4012:2
total 760 -> 856 B
```
Result:
```
SWEET MARK: dcnv v1 on=1 bytes=856
APM graph_open/dec-cnv src=0x2010 (shm handle=0xb0d7f408 len=856)
graph_open/dec-cnv rsp op=0x1001000 status=0        <-- ACCEPTED
SWEET GLINK: dec-cnv did not start                  <-- our own bug: we started 4 subgraph keys
                                                        while this body declares only b0000002
ring "no rsp" count = 0                             <-- channel alive, unlike the B-form boots
```
So: **`GRAPH_OPEN` accepts `WR_SHARED_MEM_EP + PCM_DEC + PCM_CNV + SAL + MFC + AW + SPLITTER + MUX_DEMUX + DATA_LOGGING + TDM_SINK` all in ONE subgraph/container.**

**Questions:**
- Is that actually legal in AudioReach, or must the **SHMEM endpoint and the HW endpoint live in different subgraphs** joined by a cross-graph connection (which is what the vendor's 3-graph layout suggests)?
- What does **`GRAPH_PREPARE` validate that `GRAPH_OPEN` does not** (port counts, media-format negotiation between connected modules, cross-subgraph connections, container capabilities)?
- For `PCM_DEC`/`PCM_CNV`, is `PARAM_ID_PCM_OUTPUT_MEDIA_FORMAT (0x08001008)` **required before PREPARE**, and what is the exact body? We have the vendor's in-band records for iids `0x40BF`/`0x40C0`/`0x4003`/`0x4004` with `param=0x08001008, param_size=30`, and an independent implementation (UniXNU-sheng) describes the body as `{1, MEDIA_PCM 0x09001000, size=(16+channels+3)&~3, pcm_t{bits,bit_width,msb,lsb,encoding,interleaving,reserved,channels}, channel_map}` — is that right?
- We feed the WR endpoint **16-bit stereo 48 kHz PCM** and set MFC output to **96 kHz / 24-bit / 2ch** (read back successfully at four phases). The TDM endpoint is configured with the vendor's own words: `0x08001017` size 12 = `{96000, 0x20020, 1}` and `0x0800101B` (TDM_INTF_CFG) size 28 = `{2, 0x10001, 3, 0x200004, …, 1}`. Is a `PCM_DEC` even appropriate for **already-PCM** input, or is `PCM_DEC` only for encoded streams (meaning we should use `PCM_CNV` alone, or `MFC` alone)?

## 6. Q5 — What is the 18504-byte APM-level `SET_CFG`, and can we build it?
**Observed:** in the vendor's accepted playback, right after the stream-graph open and its `SHM_MAP`, there is
```
SET_CFG src=0x2011 dst=1  addr=0  handle=0xb0d92ac8  payload_size=18504   (out-of-band, its OWN mapping)
SHM_UNMAP … handle=0xb0d92ac8                                            (unmapped right after)
```
The size **varies between sessions**: we have seen 18128 / 18504 / 18505 / 18506. We scanned all six ACDB files on this phone (9039 entries, entry header 44 B, invariant `entry_len = 44 + blob_len`): **no blob anywhere falls in 18000–19000**. The per-module `SET_CFG`s the vendor sends are small and **in-band**, so we hold their bodies verbatim (e.g. dst `0x4308`: `0x08001017` size 12 and `0x0800101B` size 28; dst `0x4163`/`0x4165`: `0x080010C4`; dst `0x4001`: `0x0800100C`).

**Questions:**
- What object is that ~18 KB payload — a graph-level **custom configuration / calibration container**? Which `param_id` does it carry (we suspect an APM-level id, since `dst=1`)?
- Is it **required before PREPARE/START** for a graph opened by key, or optional?
- Can it be **reconstructed** from the ACDB plus per-module calibration, or does it have to come from a runtime calibration service (i.e. it is genuinely unavailable to us)?
- Given that its length changes per session, is it composed at runtime by the client (AGM/PAL) rather than stored? If so, **what are its inputs**?

## 7. Things we have already ruled out (please don't send us back here)

- **Not amplitude/duration/pacing/token**: closed as a family; we have run at the clamp maximum (−0.27 dBFS) and at −6.0 dBFS with 120 buffers, with and without pacing, with both token forms. Still silent.
- **Not the SMMU/address path**: `ATS1PR` returns `PAR=0`, `CBA2R=1`, no new fault bits, `shm mapped … mode=offset`, and `DATA_BUFFER`s are acknowledged. (S2 bypass is unavailable, but S1 translation demonstrably works.)
- **Not the TDM bit-clock vote position**: voting `0x204 @ 12288000, attr=4` early is accepted but then `GRAPH_START` refuses; voting late is refused but `GRAPH_START` succeeds and 120 buffers stream. The vendor's own in-band vote body is `{2, 0x0800102C, 20, 1, 0x204, 0xBB8000, 4}` — **byte-identical to ours**.
- **Not the WR media format**: our WR record is byte-identical to the vendor's.
- **Not `REGISTER_MODULE_EVENTS`**: we had never sent `0x0100100E`; we sent it to `0x40BD` with the vendor's body shape `{iid, 0x0800103F, 1}` → **`status=0`**, the tone still streamed 120 buffers, **still silent**. So that family is removed.
- **Not the graph generation/keys**: our 2960 rebuild carries exactly the keys the vendor's live in-band `PREPARE`/`START` bodies name (`b0000002 b0000025 b0000024 b0000023`). An older 2724-byte blob we had been replaying carries `b0000081`, which appears in **no** live start list — it is a previous generation, and it also overruns its own array by 4 bytes. All conclusions drawn from it have been retracted.
- **Not the source port**: opening from `0x2011` has been booted twice, `status=0` both times.
- **Not the amplifier init**: both AW882xx answer `id=0x2032` and accept the full 44/44 profile write over GENI I2C before streaming.

## 8. If you can only answer one thing

**Answer Q1.** "A malformed body should return an error, not silence the service" is itself the anomaly. Either our mapping usage is wrong in a way we cannot see, or there is a DSP-side precondition we have never met. Everything else in this document becomes interpretable once that is known.

**Second choice: Q3**, because it determines whether *any* future experiment of ours can be read at all.

---
*Please answer with concrete ids/layouts/command sequences where you can. We will ship exactly what you specify on the next RAM boot and report the raw reply words, the `status`, and whether the APM stayed alive afterwards.*
