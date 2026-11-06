# UniXNU / sheng 的音频源码：这一轮读到什么、哪些我们已经付过钱、哪些是新格

日期 2026-11-07（第 51 枪之后）。触发：用户"实在不行你拉一下 unixnu 的仓库看看他们怎么修的
蓝牙 wifi 还有音频之类的东西？但他们是依靠 UEFI 启动的"，随后指出 **"sheng 那条路我们试过了"**。
这两句都要正面回答，所以本文分四块：普查方法 / 已付过钱的部分 / 没读过的部分（新）/ UEFI 依赖。

本地副本在 `work/refs/unixnu-sheng/`（23 个文件 239 KB，未纳入 git；拉取方式见文末配方）。

---

## 1. 普查：32 个仓库、4266 个 blob，按**路径**筛音频

```
GET /orgs/UniXNU/repos?per_page=100            -> 32 repos（UniXNU 主仓 2072★）
GET /repos/UniXNU/<r>/git/trees/HEAD?recursive -> 4266 blobs
path ~ /audio|gpr|adsp|lpass|spf|sound|alc|codec|speaker|microphone|hapt|vibra/i
```

命中只有三处：`UniXNU-sheng/hal/ShengAudioPlugIn.c` 等 12 个音频文件、`UniXNU` 主仓一个
backboard 震动等待检查脚本、`uefi-sheng` 的 EDK2 平台文件。

**先纠正我自己在本轮开头形成的一个错误判断**：我一度根据"QcomBaseband / ArmDTPlatform /
ArmSmmu4K / QcomGeniSpi 四个仓里没有 audio 字样"就准备写"UniXNU 没有音频实现"。
**那句话是错的**，错因是只抽了 5 个仓、没做全组织普查。真实结论相反：
**sheng 有一份完整的 AudioReach 客户端 + 一份 iOS AudioServerPlugIn。**
（同一课的第 N 次：范围没普查完就下"没有"的结论。`absence may be my filter`。）

---

## 2. 我们已经付过钱的 sheng 部分（用户这句是对的，不要重走）

| 借用的东西 | 用在哪 | 现状 | 文档位置 |
|---|---|---|---|
| `SHM_MAP` 地址高字 = **流序号**（`0x1001 & 0xf`），不是 33 位地址 | 把 `msw` 从 0 改 1 | **这是本项目音频线最大的一次解锁**：整机不再挂总线，控制面全绿 | `docs/audio-closeout.md` §1–§2 |
| SMMU **pass-through** 给 DSP 映射音频流 | 想照抄 | **marble 拿不到**：`S2CR.BYPASS` 被改成 FAULT（run71/73）；AP 直驱 LPAIF 也不成立 | `docs/speaker-path.md:47` |
| AMP 调度器要 CLPC/Edge policy（`zzyf-sheng-amp.patch`） | CPU 拓扑 | 试过，`git revert` 回 v88 | `docs/status-log.md:2111` |

⇒ 所以本文**不提议**任何"照 sheng 改传输/改 SMMU/改启动"的动作，也**不需要**开机去证明它不行。
下面第三条才是新的。

---

## 3. 从没读过的部分：sheng 的**命令正文**写法

`package/ShengAudioReach.c`（17 KB）、`ShengGpr.h`、`ShengAudioAbi.h`、
`hal/ShengAudioPlugIn.c`（43 KB）、`package/AudioProbe.cpp`（65 KB，bring-up 线程）、
`package/AdspProbe.cpp`（34 KB，GLINK/SMEM/SMP2P/PAS）、`ShengAudioService.cpp`、
`package/tests/{gpr,audioreach,qcom_adsp}_host.c` + `scripts/check-*.py`。

对我们当前那一格有意义的六条（都能对行）：

1. **GRAPH_OPEN 是带内、≤512 B、客户端自己描述拓扑**：
   `ShengGpr.h:22` `GPR_MAX_PACKET 512u`；`ShengAudioReach.c:86-122` 一次 open 写五块
   `SG_CONFIG 0x08001001` / `CONTAINER_CONFIG 0x08001000` / `MODULE_LIST 0x08001002` /
   `MODULE_PROP 0x08001003` / `MODULE_CONN 0x08001004`，属性 id `0x0800100e..0x08001015`
   （perf/direction/scenario/caps/position/stack/domain/**port_info{in,out}**）。
   ⇒ **没有任何 18 KB 校准对象参与他们的出声路径**。这不能证明 marble 不需要，
   但它把"必须有 cal"从"公认前提"降级成"未检验假设"。
2. **两个容器：stream 图 + device 图，stream 图的 open 里带跨图连接**
   （`ShengAudioReach.h` 顶部注释；`AR_SG_PLAY 0x4001` / `AR_SG_SPEAKER 0x4005`）。
   这正是我们唯一缺的形状：我们成功打开的是**一张单容器图**。
3. **`PARAM_ID` 表里有两个我们当成"自创"的 id**：
   `0x08001176 = PARAM_HW_EP_POWER_MODE`（`ShengAudioReach.c:27`，:200 发 body `{0}`）、
   `0x08001008 = PARAM_PCM_OUTPUT`（:24，:208-215 给出**完整正文布局**：
   `{1, MEDIA_PCM 0x09001000, size=(16+ch+3)&~3, pcm_t{bits,bit_width,msb,lsb,encoding,
   interleaving,reserved,channels}, channel_map}`）。
   ⇒ 我们从厂商流量里看到、但一直解不开的 `0x40bf/0x40c0 + param 0x08001008 + psize 30`
   **第一次有了逐字段的解码**（30 = 12 + 16 + 2，双声道正好）。
4. **回执节奏**：`AudioProbe.cpp:12` "keeps **four 10 ms** periods queued at the DSP, each
   copied from the shared playback ring **as the previous one returns**"。
   ⇒ 与 K10 实测"DATA_BUFFER_DONE ≈1 ms = 入队回执而非消费"**独立互证**。
5. **PRM 时钟 id ≠ DT 时钟索引**：`ShengAudioReach.h:87` `AR_CLK_TX_CORE_MCLK 0x30c`
   注释写明 *"not the device tree indices (57 was refused)"*。
   ⇒ 我们 TDM 位时钟用的 `0x204` 是从厂商包抄来的，这条继续成立，不用重测。
6. **iOS 侧出声的两个坑**（与我们"mediaserverd 开壳不出流"那一格同题材，见
   `docs/audio-hal-bridge.md`）：`ShengAudioPlugIn.c` 注释 —— time stamp 周期必须
   > safety offset + 一个 IO cycle，否则 audio server **一次 IO 都不做**（i543: 8192 → 0 writes）；
   CoreAudio 把 host clock 当 **24 MHz**，板级 19.2 MHz 计时器会让它每周期落后直到被时间戳
   拉回（i546: 1474 周期里 858 次间隔 25–40 ms、skew −13052 帧）。
   `ShengAudioAbi.h:18,22` 给出环几何：48 kHz、`RING_FRAMES 65536`（1.37 s，
   "装得下 300 ms lead 和 audio server 迟到的 IO"）。

---

## 4. 用 sheng 的词表重读**厂商自己的** 2724 B 正文（零开机）

`tools/decode_graph_blob.py`（本轮新增，只读，循环全部由字面量 bound，退出码 0）
的输出归档在 **`docs/data/vendor4-graph-decode-2026-11-07.txt`**。要点：

* 18 个块，id 全是 sheng 那五个，顺序也一致；每个 MODULE_CONN 的自检算术相等：
  `4 + 16×9 = 148 = declared`；**最后一块正好落在正文末尾**
  （`0xa08 + 8 + 148 = 0xaa4 = 2724 = 正文自己声明的 size`）⇒ 走链没有错位。
* 厂商一次 GRAPH_OPEN 里开 **4 个子图**：
  `0xb0000002`（device，7 模块：SAL 0x4012 / DATA_LOGGING 0x4007 / SPLITTER 0x418e /
  **TDM_SINK 0x428f** / MUX_DEMUX 0x4006 / **MFC 0x48ba** / `0x10013d01` 0x42d3）、
  `0xb0000081`（5：DATA_LOGGING 0x42a1、0x42a4 / SYNC 0x42a6 / MFC 0x42a7 / SAL 0x42ad）、
  `0xb0000024`（1：MFC 0x40c8）、
  **`0xb0000023`（stream，5：`WR_SHARED_MEM_EP 0x40bd` / DATA_LOGGING 0x40be /
  `PCM_DEC 0x40bf` / `PCM_CNV 0x40c0` / DATA_LOGGING 0x40c1）**。
* stream 子图的 9 条连接：
  `0x40bd:1→0x40be:2→0x40bf→0x40c0→0x40c1→0x40c2→0x4008→0x40c4→0x40c6→0x4165`。
* 我们**当前能 status=0 打开**的那张图（`g_graph_vp_wr`，760 B，用
  16 B `apm_module_param_data_t` 头、iid=1，走链同样精确到 0x2f8=760 末尾）只有
  **8 模块 / 15 连接**，其中 WR 的那条是 **`0x40bd:1 → 0x4012:2`（WR 直连 SAL）**。

⇒ **新格（这就是"图内容"轴上第一次能指着两个具体 iid 说话）**：
厂商在 WR 端点后面挂了 **9 跳**，含 `PCM_DEC(0x40bf)` 与 `PCM_CNV(0x40c0)`；
我们挂了 **0 跳**（直连 SAL）。我们曾经抓到过厂商发给 0x40bf/0x40c0 的
`0x08001008` 记录却"没地方放"，原因在这里：**我们的拓扑里没有这两个模块**。
这与 §3 第 3 条合起来，第一次让"补哪两个模块、发哪一条正文"变成可以写下来的东西。

同时**记下两条不越过证据的话**：
1. 我们自己的 `g_graph_ukee_current`（2960）与厂商的 2724 在**子图 key 集合**上差一个：
   厂商用 `0xb0000081`，我们用 `0xb0000025`。而 memory/AGENTS 里那句
   "`0xb0000081` 在所有 ACDB 里都不存在"**只说过 ACDB**——它确实在厂商自己的 GRAPH_OPEN 正文里。
   ⇒ 这是我们那发 `status=0x2`（带内 2944）目前**唯一有名字的候选**，但也只是候选。
2. 块头的字长（8 / 12 / 16）在这份正文里**没有定论**（间隔 +60/+116/+84/+188 不一致）。
   所以本文只引用"自证的形状"（模块对、连接四元组、末尾对齐），不引用属性字段的解释。

---

## 5. UEFI 依赖：哪些是能搬的，哪些搬不动（回答用户的第二个问题）

读 `UniXNU/ArmDTPlatform/ArmDTPlatform.cpp` 与 `QcomBaseband/README.md`、
`UniXNU-sm8550-common/dts/sm8550.dtsi`：

* **驱动本体不依赖 UEFI**。`ArmDTPlatform` 是一个真正的 `IODTPlatformExpert` 子类，
  吃的是一份 **Apple 格式 device tree**（要求 `/chosen` 带 CHRP NVRAM proxy
  （`nvram-bank-size` + `nvram-proxy-data`）、`/options`、`/cpus`、`/arm-io`、`/defaults`），
  `deleteList()/excludeList()` 返回 `"()"` 以保住整棵树，再用 `createNub()` 把 `arm-io`
  的每个真实子节点挂上；资源（reg/ranges）由 XNU 原生 `getNubResources` 沿 DT 解析。
  注释里明写"不把板名/SoC 地址/厂商标识编进源码"。
* 依赖 UEFI 的是**喂给它的那棵树和装载过程**：`DarwinBoot`（UEFI 引导器）+ `uefi-sheng`
  （Mu-Silicium fork）负责产生 DT、加载 prelinked kernel。
* ⇒ 对我们（纯 shim + `fastboot boot`）的含义很具体：**他们没有任何"必须由 UEFI 提供"的运行时服务**。
  我们能搬的是 `ShengGpr.c`/`ShengAudioReach.c` 这类**纯编码 + MMIO + IOKit** 的代码；
  搬不动的是 `AdspProbe.cpp` 里 PAS 加载 ADSP 那一半（我们从不需要：DSP 本来就在跑，
  控制面每轮都回 status=0）与 `ArmDTPlatform` 那种"用 DT 提供资源"的前提
  （我们的地址来自 shim 里硬编码 + 手工 ADT）。
* 蓝牙/WiFi/基带的对照价值也在同一处：`QcomBaseband` 的 README 明写
  "A board provides modem power/firmware startup, SMEM/SMP2P, GLINK/QRTR transport and rmtfs"
  ——**板级那段它不给我们**，跟 memory 里"assumes an already-loaded modem"一致；
  `Ath12kWifi` 是把 Linux ath12k/mac80211/cfg80211 整株搬进 XNU 再加 `userspace/driver_sheng.c`
  接 wpa_supplicant 2.12，这条成本与我们 WiFi 停摆的判断没有冲突。

---

## 6. 第 52 枪的候选（**开机前**写死；未开，预算 51/120）

新格是"我们图里没有 DEC/CNV"，最便宜的判法是**先把厂商那 2724 个字节用能用的传输发一次**：

**K13 = `sweetvgraph=5`：`g_graph_vendor4` 正文原样、带内、`off=0 len=2724`，走 0275 的
`g_apm_cmd_big`（4 KB inline）**。理由（全部实测过）：`logs/*.txt` 里 `graph_open/vendor4`
出现过 **7 次，全是 SHM 形式**（`shm handle=0xb0d7f408 len=2712`：1×`status=0x2`、2×`no response`），
而两次带内形式（`vginl v1 vg=4`）发的是**我们自己的 2944**，不是厂商的 2724。
⇒ 这一格**从没开过枪**。

判据（四种，先写后开）：

| 观察 | 结论 |
|---|---|
| `graph_open/vendor4-inband rsp op=0x1001000 status=0` 且 `graph_start … status=0` 且耳朵听到 | 内容就是那 2724 B；音频线到此**出声**，转入音质/多设备 |
| 同上 status=0、start=0、`not consumed`=0，但**仍无声** | 拓扑到位了；从此可以**第一次对 0x40bf/0x40c0 发 GET_CFG**（DEC/CNV 现在在我们的图里，读得到），cal 轴才有可测的落点 |
| `status=0x2` | ACF 图正文**必须**走带外/SHM，带内这条对图描述符关闭（不是对 SET_CFG 关闭） |
| `no response` | 按 M1 的先例，**同镜像重放一次**再判（一次性早退已被 M1r 证明会伪装成结果） |

**这一枪的代价与污染，先写清楚**：换了图之后，驱动后面那些发给
`0x40bd / 0x428f / 0x48ba` 的单模块 SET_CFG 就不再是单变量（它们点名的是我们旧图里的 iid）。
所以 K13 只判上表四格，`mfcget v4` 的读回**记录但不作一阶结论**。
字节账：现在 581 B，` sweetvgraph=5`(15) 替换 `sweetvgraph` 现有取值不改长度；
不过 cmdline 长度必须打包实测，不许继承。
改动量：`sweet_glink.c` 里 vg 分支加一个 `== 5` 的（正文/偏移/长度 + 一条 witness
`SWEET MARK: vginl v2 vg=5 off=0 len=2724 form=inband`），marker 进 workflow。

---

## 7. 复现配方（零开机、只读）

```bash
export https_proxy=http://127.0.0.1:7890
cd work/refs/unixnu-sheng && B=https://raw.githubusercontent.com/UniXNU/UniXNU-sheng/main
for f in package/ShengAudioReach.c package/ShengGpr.h package/AudioProbe.cpp; do
  curl -sS --max-time 45 -o "$f" "$B/$f"; done
cd ../../xnu-gh && python3 -u tools/decode_graph_blob.py > /tmp/decode.txt 2>&1; echo $?
```

许可：sheng 全部文件头为 `SPDX-License-Identifier: BSD-2-Clause-Patent` ⇒
可按条款带出处复用；搬代码时保留 SPDX 头并在 `docs/` 记录来源 rev。
