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

---

## 8. 第 52 枪 K13 判决（`logs/ring-k13-vendor4-inband.txt`，`sweettime=1793982907`）
## **落在表里第三格，并且顺手打掉了一条我自己写错的既有断言**

窗口/身份：`vginl v1 vg=3 off=12 len=2712 form=inband`（证人自己算的 off/len）、
发送行 `APM graph_open/vendor4-inband src=0x2010 dst=0x1 (2752 bytes) inline-big`
（2752 = 24 + 16 + 2712 ✓ 算术对得上）、ring 以 `===== done =====` 正常收尾。

| 门 | 实测 | 判读 |
|---|---|---|
| GRAPH_OPEN | **`rsp op=0x1001000 status=0x2`** | **第三格成立**：ACF 图正文**不能**走带内。这与 §6 里那条既有规矩（`payload_size ≤256` 才带内）**一致**，所以这不是一次意外，是一次**预测被真机兑现** |
| 后续 | `graph_open/vp-wr … status=0`（回落到我们自己的图）→ `graph_start … status=0` → `pcm tone done (120 buffers)` | 回落脚本按事先写的规矩执行；本轮主判据已在上表拿到 |
| 顺带的新读数 | `dblat v2 tone n=120 min=5 avg=8 max=10`（K12 同一颗内核是 `min=avg=max=5`） | 回执节律**动了**。但本轮同时多了"一次被拒的 GRAPH_OPEN + 回落到另一张图"，所以**不能归因给图内容**；记为观测，不记为结论 |

### 查证时被打掉的那条断言（重要，因为它在两份文档里活着）
`docs/reference-vendor-tx-timeline` 与本文 §6 都写过"厂商 GRAPH_OPEN 是**带内** 2712"。
回源 `logs/gpr-20261027-090802.txt` 那一行逐字重读：

```
p5=16781312 (=0x01001000 GRAPH_OPEN)  p6=0  p7=0  p8=2966982840 (=0xb0d7f408)  p9=2712
```

`apm_cmd_header_t = {payload_address_lsw, msw, mem_map_handle, payload_size}` ⇒
**p8 ≠ 0**：厂商那 2712 字节的正文**在它自己映射的一块区里**（handle `0xb0d7f408`），
`addr=0` 是"在这块区的偏移 0"。⇒ 厂商的 GRAPH_OPEN 是**带外**，不是带内；
"带内 2712"这个标签是错的，它把 `payload_size` 字段当成了"正文在包内"的证据。

### 同时排掉的一个我自己提的便宜假设
我本来准备写"三次 SHM 形式之所以没响应，是因为 `g_apm_graph_open_shm` 发的是**捕获里那个陈旧的厂商 handle**"。
回源 `bsd/dev/sweet_glink.c:5457`：那个位置传的是 **`g_shm_handle`——我们自己 SHM_MAP 拿到的句柄**，
旧 ring 打印的 `0xb0d7f408` 只是数值长得像。**该假设作废**，没有这个 bug。

## 9. 问答：我们的图构造和厂商一致吗？（台账，实测，不推测）

**不一致，而且差距比"内容差一点"大。** 厂商一次播放的建图（同一份抓包、同一轮，逐条顺序）：

| 厂商 | 我们（已知-good 路径） |
|---|---|
| **3 次 GRAPH_OPEN**，src 端口 **0x2010 / 0x2011 / 0x2012** | **1 次**，src 端口 **0x2010** |
| 正文大小 **496 / 2872 / 2032**（第三张放在区内 **offset 0x6000**） | **760** |
| 每张各自 PREPARE+START；**stream 那张的 PREPARE/START 正文 = 40 B / 8 个字**（多子图列表），device 那张 = 24 B / 5 个字 | PREPARE/START 走 4 个 sg id 的列表 |
| 每张之后 `REGISTER_MODULE_EVENTS`（0x0100100e，dst=0x40c4 / 0x4019，24 B，正文 `{iid, 0x0800103f, 1}`）| **0 次**（`grep -c 0x100100e = 0`） |
| stream 图之后紧跟 **18504 的 APM SET_CFG，用自己的 handle 0xb0d92ac8**（另一块区） | 从未发出（正文一个字节都没拿到过） |

**而且"逐字节一致"这个目标本身不可达**：同一次抓包里相邻两轮播放的 stream 正文
**2872 → 2904**（差 32 B），第三轮又是 2032 ⇒ 正文是**运行时合成**的，
不是可以从堆里复制的静态 blob。我们手里那份 2724 来自 AGM 堆、长度 2712 属于**另一天的会话**，
把它当"厂商的图"来回放，本身就是拿陈旧会话的字节去撞今天的 DSP。

**这条同时解释了为什么"完全一致也过不去"的直觉只对了一半。**
按证据看，DSP 判的是**内容**（同一个 `g_apm_graph_open_shm`、同 handle、同区内偏移 0、
同 token：我们的 760 B 拿 `status=0`，那份 2712 拿 `0x2`），所以"结构一致"是能过门的；
过不去的是**复制**这条路 —— 正确的目标是**按上面这张表把结构造出来**
（3 张图 / 3 个端口 / 事件注册 / 配套 cal），而不是搬运一次抓包。

⇒ 因此第 2 节的枪单次序改一下：**A/B 两枪（对 0x4308 发厂商抄得的 TDM 正文、
补 REGISTER_MODULE_EVENTS）仍然是单变量、仍然先开**，但它们的定位从"补内容"降为
"**先把缺失的命令族补上、看 status 会不会动**"；真正决定性的工作是那张 3-graph 构造（C），
而 C 的判据必须能区分"少一张图"与"少一个 cal 对象"，否则开了也读不出东西。

## 10. 一次零开机的对表，把两件事同时改写了（2026-11-07，第 52 枪之后）

外层索引按 `{key, size}` + 8 字节头走链，两份容器**各自走到底**，于是第一次能一格一格比：

| entry | 厂商堆拷贝 `g_graph_vendor4` (2724) | 我们的重建 `g_graph_ukee_current` (2960) | 差 |
|---|---|---|---|
| 0 | `0xb0000002` size **860** | `0xb0000002` size **852** | 8 |
| 1 | **`0xb0000081`** size **560** | **`0xb0000025`** size **800** | **key 不同 + 240 B** |
| 2 | `0xb0000024` size 300 | `0xb0000024` size 300 | **完全相同** |
| 3 | `0xb0000023` size 968 | `0xb0000023` size 968 | **完全相同** |
| 走链终点 | **`0xaa8` > 数组末 `0xaa4` ⇒ 少 4 字节（截断）** | **`0xb90` = 数组末 ⇒ 自洽** |

**改写了第一件事：所有"回放厂商正文"的实验都无效。**
我们手里那份厂商拷贝**被自己的索引声明超出 4 字节** —— 它是一份**畸形的描述符**。
所以 K13 那句"厂商正文带内被拒 `0x2`"要收窄成：**"一份截断 4 字节的正文带内被拒"**；
带内这条传输规矩仍然成立（`payload_size ≤ 256` 独立支持它），
但**内容那一格 K13 没有判、也无从判**。回放路线到此结束：不是"不像"，是**原件残缺**。

**改写了第二件事：差异被缩到唯一一格。**
两份容器在 entry[2]/entry[3] 上逐字节等长（300 / 968），
唯一实质差异是 **entry[1]：key `0xb0000081` vs `0xb0000025`，且 size 差 240 B**
（240 = 10 × 24，正是"一个子图的 PORT_INFO 表 10 条"的那个尺寸）。
而我们用 `0xb0000025` 的理由只是"ACDB 普查里找不到 `0xb0000081`"。

⇒ **下一枪（0279，最小改动）**：发**我们这份自洽的 2960**，但把 entry[1] 的 key 在运行期
替换成 `0xb0000081`（新增 `sweetsgkey1=<hex>`，只在发送前改写那一个 u32，别的一个字节都不动）。
单变量、可回退、能一次判定"这一格到底值不值得过门"。判据先写：
`status=0` ⇒ key 就是那道门，随后再处理 240 B 的表；`0x2`/无响应 ⇒ key 这一格被排除，
剩下的只有"厂商运行时合成的那 144 B"，那时才轮到三图构造（C）与 cal 资产线。

## 8b. K14 的前提在开机之前就被两份证据打掉了（零成本，记下来防重走）



K14 原定义 = "给图描述符单独 SHM_MAP 一块区、取新句柄、带外发"。两条证据各自独立地否掉它：

1. **厂商自己就是同一块区。** 逐字重读 `logs/gpr-20261027-090802.txt`：
   GRAPH_OPEN 行 `handle(p8)=0xb0d890b8 addr_lsw=0 len=2712`，
   而其后 **95 条 DATA_BUFFER_V2 的 p8 全是同一个 `0xb0d890b8`**（addr_lsw 依次 0/0x1000/0x2000/0x3000）。
   ⇒ 描述符与数据缓冲**共用一个映射**，offset 0 处临时放描述符。没有"第二个句柄"这回事。
2. **我们的代码本来就是这么发的。** `:7882` `g_apm_graph_open_shm(port, body + vgoff, vglen, **0**, tag)`
   —— 第四个参数（区内偏移）**写死 0**，源指针是 `body+12`，句柄是我们自己的。
   ⇒ 带外那三枪的包形状已经等于厂商的形状。

**更强的内部对照（同一支函数、同一种信封）**：我们自己那张 760 B 的 `g_graph_vp_wr`
走的**就是同一个 `g_apm_graph_open_shm`**（同一个 token `0x400` @ `:5455`、同一个 handle、
同一个区内偏移 0），它拿 `status=0`。⇒ **传输、句柄、偏移、token、信封全部被同一轮的内部对照排除**，
唯一变量只剩**正文内容**。

**并且厂商那份正文确实被这块 DSP 接受过**（RX 方向实测，不是假设）：
`logs/gpr-rx-20261105-223253.txt` / `gpr-both-20261105-222736.txt` 里
`BASIC_RESULT p6=0x1001000 p7=0x0` 出现**两次**（token 38→dst 0x2010、token 44→dst **0x2011**），
`PREPARE/START` 也 `p7=0x0` ⇒ **厂商在这台机上开了两张图、两条 src port（0x2010 与 0x2011），
都成功**；还有一条 `0x100100d SHM_UNMAP` 回到 dst 0x2002。
我们从 0x2010 复刻同一份 2712 字节 ⇒ `0x2`。

⇒ 现在唯一站得住的解释是：**我们手里那份 2724 的堆拷贝不是厂商真正发出去的字节**
（文档里早就写着它"比自己最后一条记录短 4 字节"）。EBADPARAM 正好是"描述符畸形"的答话。
⇒ **下一枪不是旋钮、也不是传输，而是内容重做**：用已经解码可读的真实 key
（`0xb0000023 / 0xb0000002 / 0xb0000024 / 0xb0000081`、container `0xe000002c/02/30/6e`、
模块表与 9 条连接）在**我们已被接受的容器格式**里自己写一份 GRAPH_OPEN（即 sheng 客户端做的事），
而不是回放一份来源不明的堆拷贝。工作量：一个 builder（不是回放开关），需要新补丁。
另一条并行、零成本的取证：**厂商用两条 src port（0x2010 + 0x2011）开两张图**，我们只用了 0x2010
——这两件事谁负责"第二张图"，值得在下一次读 ACDB/AGM 资产时一起回答。


### 那么现在被拒的是什么（唯一还活着的形状差）
两种传输都把同一份正文拒了：带内 `0x2`（本轮）、带外 `0x2`/无响应（历史 3 次）。
带外那三次用的句柄是我们自己的，但它们把图正文**拷进了与 PCM 缓冲区同一块 region**
（`:5445` `g_shm_va + off`，off 用 12/16）；厂商那里 GRAPH_OPEN 的句柄
（`0xb0d7f408`）与 DATA_BUFFER 的句柄**是两个不同的映射**。
⇒ 剩下唯一没试过的形状变量 = **给图描述符单独 SHM_MAP 一块区，拿一个新句柄，按 `addr=0/msw=0/len=2712` 发带外 GRAPH_OPEN**。
这是 K14 的内容，需要一发真代码（`g_apm_cmd` 之外没有别的路径），
**判据照规矩先写后开**。


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

## 11. 目标轮开始：两枪在零成本阶段被打掉，一枪存活（2026-11-07 05:50）

**登记目标**：3 枪内判定"无声"是否在图正文/图代次上（预算 52→55/120），止损=3 枪无分辨力即转资产线。

### 已判定 1：`images/e1-vgraph2724.img` **不许再开**（省下一枪）
它跑的是 `sweetvgraph=1 sweetvgoff=0 sweetvglen=2724`（判据表早已 commit：`audio-closeout.md:1963` W0 门）。
但本轮走链实测：外层 `size=2724` 与数组等长，**而 entry[3]（`0xb0000023`）自己声明 968 B，
从 `0x6d8` 起算要到 `0xaa8`=2728 ⇒ 比数组末多 4 字节**。
⇒ 无论用 2712 还是 2724 的 framing，我们发出去的都是**最后一块短 4 字节的畸形描述符**。
它的 `status` 无论回答什么都不构成对"内容假设"的判决 ⇒ **这一枪作废，不开**。

### 已判定 2：0x4308 那两条厂商 TDM 正文**不能作为单变量枪**（省下一枪）
从我们**已拿 status=0** 的那张图（`g_graph_vp_wr`）的连接表里提取全部 iid：
`0x4006 0x4007 0x4012 0x40bd 0x418e 0x4199 0x41e2 0x422c 0x428f 0x42d3 0x4333 0x4436 0x4448 0x467e 0x468d 0x48ba`
⇒ **`0x4308` 不在其中**（`0x40c4 / 0x4019 / 0x40bf / 0x40c0 / 0x4165` 也都不在）。
往一个图里不存在的 iid 发配置，被拒是必然，读不出任何新东西 ⇒ **作废，不开**。

### 存活的那一枪（0279，唯一不依赖残缺 blob 的单变量）
**`APM_CMD_REGISTER_MODULE_EVENTS = 0x0100100e` → `dst = 0x40bd`**（我们图里真实存在、
也正是 DATA_BUFFER 的接收端），正文按厂商带内形状 `{iid=0x40bd, 0x0800103f, 1}`（24 B，带内）。
厂商那边：`REGISTER_MODULE_EVENTS src=0x2011 dst=0x40c4/0x4019 payload=24 body={iid,0x0800103f,1}`，
且我们**从未发过这条命令**（`grep -c 0x100100e = 0`）。
判据（开机前写死，事后不改）：
| 观察 | 判读 |
|---|---|
| `reg_events ... rsp op=0x02001005 status=0` | 该命令被这个 APM 接受 ⇒ 命令族缺口是真的，下一枪才有资格谈"补完之后 status 变不变" |
| `status=0x3`(EUNSUPPORTED) | 这颗 SPF 不支持该命令 ⇒ **永久划掉这一族**，不再为它花任何预算 |
| `status=0x1/0x2` | 正文形状或 iid 目标不对 ⇒ 只允许再改**一处**重发（0x0800103f 字段），仍单变量 |
| `no response` | 同镜像重放一次再判（M1r 的规矩） |
**注意**：这一枪**不直接买声音**；它买的是"缺失命令族清单"里第一项的真伪，且三条分支都有确定归宿，
不会变成第 N 次旋钮。改动量：一个带 boot-arg 门控的发送 + 一条 witness；marker 进 workflow。
