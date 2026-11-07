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

## 12. G1（第 53 枪）0279 已落，判据开机前写死

补丁 `xnu/patches/0279-sweet-register-module-events.patch`（1 hunk，+44 行）；
`tools/ci_replay.sh`：**applied=220**（0278 时是 219）且
`OK: CI post-image == chain-tree HEAD for bsd/dev/sweet_glink.c` ⇒ 重放等价测试通过。
代码做一件事：在**已经拿 status=0** 的 vp-wr 路径、`g_audio_tone()` 之前，
带内发 `APM_CMD_REGISTER_MODULE_EVENTS (0x0100100e) → dst=0x40bd`，
正文 24 B、前 12 字节照抄厂商带内观测 `{0x40bd, 0x0800103f, 1}`（其余置零）。
门控 `sweetregevt=<0|1>`（0 = 对照，什么都不发）；witness
`SWEET MARK: regevt v1 on=%d dst=%#x param=%#x bytes=%u` 打印它**实际用的**状态。

| 门 | 期望 | 不成立的含义 |
|---|---|---|
| W0 身份 | 产物内 `regevt v1 `×1、`amptone v1 `×1、`dblat v2 `×1、`mfcget v4 `×5；ring 内 `sweettime=<本轮>`；窗口 total==解码字节 | 少任何一条 ⇒ 仪器未生效，**不许**判命令 |
| W1 证人 | `regevt v1 on=1 dst=0x40bd param=0x800103f bytes=24` 恰 1 行 | 缺 ⇒ 分支没跑，本轮作废 |
| W2 发送 | `APM reg_events/wr src=0x2010 dst=0x40bd (40 bytes)` 一类的发送行在应答之上 | 缺 ⇒ 没发出去，不算负结果 |
| **W3 主判据** | `reg_events/wr rsp op=0x02001005 status=0` | `0x3` ⇒ 这颗 SPF 不支持，**永久划掉该命令族**；`0x1/0x2` ⇒ 正文/目标形状不对，**只允许再改一个字段**重发；无响应 ⇒ 同镜像重放一次再判（M1r 规矩） |
| W4 回归 | `graph_open/vp-wr status=0`、`graph_start … status=0`、`pcm tone done (120 buffers)`、`dblat v2 tone … timeout=0`、`no rsp` 增量 ≤ 1（就是这条新命令）、panic 0 | 老四行变了 ⇒ 新命令扰了既有路径，记为仪器读数 |
| W5 耳朵 | `sweetvol=16`、120×6 ms（用户已给的额度） | 次要；W3 才是这一枪买的东西 |

预算 53/120。这一枪**不直接买声音**，买的是"缺失命令族"这条假设的真/伪，且三支都有归宿。

## 13. 目标第 1 项判完：**2724 那份是上一代的图**，而且结论与我们的猜测相反

厂商带内 `PREPARE/START` 的正文把**当代**真正的子图 key 名单直接给了出来（同一份 11-05 抓包）：

| src 端口 | payload | 正文点名的子图 key |
|---|---|---|
| `0x2010` | 24 | `0xb00000d5` |
| **`0x2011`** | **40** | **`0xb0000002, 0xb0000025, 0xb0000024, 0xb0000023`** |
| `0x2012` | 32 | `0xb0000025, 0xb0000024, 0xb0000023` |

⇒ **当代用的是 `0xb0000025`。** 这正是我们 ACDB 重建那份 `g_graph_ukee_current`(2960) 里的 key，
而且它点名的**四个 key、数目、顺序**与我们 2960 的 entry 表**逐一对应**
（`b0000002 / b0000025 / b0000024 / b0000023`）。
⇒ 我们那份 `g_graph_vendor4`(2724) 里是 **`0xb0000081`，在当代任何一条 start 名单里都不存在**
⇒ **判定：2724 = 上一代（陈旧）描述符。** 目标第 1 项到此结束，**禁止再把它当厂商正文发**。

**三条旧结论要作废/改写（都在我名下）：**
1. `K13` 的 `status=0x2` **不判内容**：发出去的既陈旧、又比自己声明短 4 字节。带内那条传输规矩仍成立
   （`payload_size ≤ 256` 独立支持），但"厂商正文被拒"这句话没有证据基础。
2. 上一条我排队的"把 `0xb0000025` 换成 `0xb0000081`"是**反的** —— 当代就是 `0xb0000025`，
   我们重建里已经是它。该候选**撤回**。
3. `0xb0000081 absent from all ACDBs` 那条旧注记继续有效（陈旧 key 找不到是应该的）。

**于是新出现的单变量（G2 候选）：** 厂商发这份流图描述符时用的是 **src 端口 `0x2011`**，
而我们所有 GRAPH_OPEN 都从 `0x2010` 发（`port = 0x2010u`；`g_stream_port` 我们只用它推 DATA_BUFFER）。
⇒ **"当代内容 + 我们已被接受的信封，只把 src 换成 0x2011"** 是一个真正单变量、
且不依赖任何陈旧 blob 的枪。它比"再改正文"更有资格当第 2 枪。

## 14. 资产缺口结论（目标要求的交付物，全部由实测数字构成）

我们那份**当代**容器 `g_graph_ukee_current`（2960）与厂商当代流图（src `0x2011`）对表：

| 项 | 我们（2960） | 厂商当代（同一轮抓包两次） |
|---|---|---|
| entry 数 / key 序列 | 4：`b0000002 b0000025 b0000024 b0000023` | 4：**完全相同、同顺序** |
| 走链自洽性 | `8 + Σ(8+size) = 2960`，**slack = 0**（精确到末尾） | n/a（正文带外，抓不到） |
| 正文总长 | 2960 | **2872**、下一轮 **2904** |
| 每张图 | 只开 1 张（src 0x2010） | 开 **3 张**：`0x2010`=496(1 子图 `b00000d5`)、`0x2011`=2872/2904(4 子图)、`0x2012`=2032(3 子图) |

⇒ **缺口不是"框架/代次"，我们框架已经对上了**（同 key、同序、自洽到 0 字节余量）。
⇒ **缺口是三样具体的东西**：
1. **每个 entry 正文里那 56–88 字节**：同一台机、相邻两轮播放的流图正文长度差 **32 B**
   ⇒ 每个 entry 的正文是**按会话合成**的（校准/设备数据填充），
   **不存在一份"正确字节"可以复制** —— 任何"抓一份厂商正文回放"的路线在原理上就到此为止；
   可执行的是**从本机 ACDB + 校准对象自己 builder**（我们已有 ACDB 全部 6 份与 TLV 走链工具）。
2. **第三张图**（src `0x2012`，2032 B，子图 `b0000025 b0000024 b0000023`）：我们**从来没开过**任何
   第二、第三张图。这一格与"少 cal"是两个可分开的假设，G2（只把 src 换 `0x2011`）能先把"端口"切掉。
3. **18504 的 APM 级 SET_CFG**（带外、自带 handle `0xb0d92ac8`）：一个字节都没拿到过，
   而且按第 1 条推论，它同样是会话内合成的 ⇒ 只能重建，不能复制。

**可执行下一步（按性价比）**：G1 = `REGISTER_MODULE_EVENTS`（已上路，CI 构建中，不依赖以上任何一条）；
G2 = 同一份当代容器**只把 src 端口换成 `0x2011`**（单变量、零资产依赖）；
G3 = 若 G2 被拒 ⇒ 内容轴只剩"重建 entry 正文"，那需要先把 ACDB 里的 per-module cal 记录
与 entry 长度做对表（零开机可做，工具已有 `tools/acdb_graph_rebuild.py`）。

## 15. G2 在零成本阶段被判为**重复**（省下一枪）

我上一条把"同一份当代容器、只把 src 端口换成 `0x2011`"排成 G2。回源后**取消**：
- 代码里早有开关：`:7771 sweetsgraphport`（`graph port override %#x`，`:7779 g_stream_port = port`）。
- **已经开过两次**：`logs/ring-g1-graph-2011.txt`、`logs/ring-j2-port-2011.txt`，
  两枚 witness 都在（`graph port override 0x2011` + `APM graph_open/vp-wr src=0x2011 dst=0x1 (800 bytes)`），
  应答都是 **`status=0`**，`graph_start` 也 `status=0`。
⇒ **端口这一格早已关闭**：从 `0x2011` 开图 DSP 完全接受，声音也没有变 ⇒
"厂商用第二个端口开流图"不是差异所在。G2 **不排队、不开机**。
（这也是为什么"新格"必须先对旧 ring 查重再登记：这一枪省下来的是真子弹。）

**剩下的枪序**：G1（0279，CI 构建中）→ 若 G1 不给出分辨力，就只剩
**G3 = 用本机 6 份 ACDB 重建当代 entry 正文里那 56–88 字节**（先零开机做对表，
不许先开机要读数）。

## 16. 资产缺口现在是一张具名清单（零开机，全部来自厂商带内包体）

把 11-05 抓包里厂商**实际配置过**的 iid 全提出来，与"我们已 status=0 的图的 iid 集合"做差：

| 厂商配置的 iid | 它用的 param | 在我们图里？ |
|---|---|---|
| `0x40bd` WR | `0x0800100c` | **在** |
| `0x428f` TDM_SINK | `0x08001017`、`0x0800101b` | **在** |
| `0x42d3` AW | `0x10013d2a` | **在** |
| `0x4308` （第二个 HW EP） | `0x08001017`、`0x0800101b` | **不在** |
| `0x4001` | `0x0800100c` | 不在 |
| `0x4003`、`0x4004`、**`0x40bf`、`0x40c0`** | **`0x08001008` = PARAM_PCM_OUTPUT** | 不在 |
| `0x4163`、`0x4165` | `0x080010c4` | 不在 |
| `0x4019`、`0x40c4` | `0x0800103f`（这是 REGISTER_MODULE_EVENTS 的 param，不是配置） | 不在 |

⇒ **13 个被配置的 iid 里，我们图里只有 3 个。**
⇒ 关键的宽慰：这些 SET_CFG 的 payload 都是 **32/48 字节的带内包**，
**正文我们已经逐字拿到了**（例如 `0x4308` 的 `{0x4308, 0x08001017, 12, 96000, 0x20020, 1}`
与 `0x0800101b` 那条 28 字节 TDM_INTF_CFG）。
⇒ 所以缺口**不是"拿不到字节"**，而是**"这些模块不在我们打开的那张图里"**：
拓扑缺 8 个模块（其中 6 个的正文已在手，2 个 `0x080010c4` 的正文也在带内包里可核）。
⇒ 那条"必须拿到 18504 那块 cal 才能动"的旧前提，**在这张表面前降级**：
先按当代 key 把缺的模块写进图 + 用已抄到的正文配置它们，才是最短路径。

**G3 的具体形状（零开机可先行）**：以我们 2960 那份**当代、自洽**的容器为底，
在 stream 子图 `0xb0000023` 的 module list / port table / connection 里补上
`0x40be(0x0700101a) → 0x40bf(PCM_DEC) → 0x40c0(PCM_CNV) → 0x40c1(0x0700101a)`
这一段（表 4 里厂商的连接是 `0x40bd:1→0x40be:2→0x40bf→0x40c0→0x40c1→0x40c2→0x4008→0x40c4→0x40c6→0x4165`），
再把上面这些带内正文原样发过去。第一步是纯离线把容器重新走链自洽（slack=0），
第二、三步才各需要一发。

## 17. G1′（第 53 枪，改为这一发）：当代容器的**正确 framing**，从未发过

查旧 ring 得到一条被埋掉的事实：2960 那份容器上过线 **5 次**，但**每一次 framing 都是错的**：
```
4 × APM graph_open/ukee2960 src=0x2010 (shm handle=… len=2944)   → status=0x2 ×3、无响应 ×1
1 × graph_open/ukee2960-inband (2984 bytes) inline-big           → status=0x2
```
`len=2944` 来自代码默认 `vgoff=16 / vglen=2944`。而本轮实测走链证明
**这份容器的正文就从头开始**：`8 + Σ(8 + size) = 2960`，**slack = 0** ⇒ 正确的 payload 是
**`off=0 / len=2960`**，跳过前 16 字节等于把第一个 entry 的头削掉 —— 那是一份**必然**畸形的正文。
⇒ 所以历史上那 5 个 `0x2` **不能算对"内容假设"的判决**，与 2724 那三次同理。

**这一发不需要新代码**：`sweetvgoff` / `sweetvglen` 是现成的 boot-arg
（`:7838` 之后被 `PE_parse_boot_argn` 覆盖；守卫 `vgoff>64 || vgoff+vglen>body_sz`
在 `0+2960>2960` 为假 ⇒ 放行），内核就用已在手的 0278。
cmdline = K13 那行把 `sweetvgraph=3` 换成 `=2`（等长）再加 ` sweetvgoff=0 sweetvglen=2960`
⇒ **预计 582 B < 590**（打包时实测，不许继承）。

判据（开机前写死）：
| 门 | 期望 | 不成立的含义 |
|---|---|---|
| W0 身份 | ring 内 `sweettime=<本轮>`；`ring_window` total==解码字节；产物内 `vginl v1 `×1、`dbb v1 `×1、`amptone v1 `×1 | 少一条 ⇒ 本轮不作数 |
| W1 证人 | `SWEET MARK: vginl v1 vg=2 off=0 len=2960 form=shm` **恰 1 行**（证人自己算 off/len） | 缺/值不同 ⇒ framing 没生效，**不许**判内容 |
| W2 发送 | `APM graph_open/ukee2960 src=0x2010 (shm handle=<非0> len=2960)` 恰 1 行，且 `len` 是 **2960** | 缺 ⇒ 没发出去 |
| **W3 主判据** | `graph_open/ukee2960 rsp op=0x1001000 status=0` | `0x2` ⇒ **当代正文本身被拒**，内容假设获得第一次干净否定 ⇒ 按目标止损条款转资产线；无响应 ⇒ 同镜像重放一次再判 |
| W4 连带 | 若 W3=0：`graph_start … status=` 与 `not consumed` 计数、`pcm tone done` | 图开成后能不能 start / 数据面是否照旧，这一格顺带白拿 |
| W5 耳朵 | `sweetvol=16`、120×6 ms | 次要；W3 才是这一枪买的东西 |

## 18. 我自己 §16 的表用错了对照对象 —— 更正，并且缺口因此变具体

§16 那张"13 个被配置 iid 里我们只有 3 个"是拿 **`g_graph_vp_wr`（我们平时能 status=0 的那张 760 B 合并图）**
的 iid 集合去比的。而**正在开的这一枪用的是 `g_graph_ukee_current`（2960）**，两者不是一张图。
按 2960 自己的 module list 重算：

| 子图 | 2960 里的模块 | 厂商同一 key 的模块（走链自洽的 entry[0]/[2]/[3]） |
|---|---|---|
| `0xb0000023` stream | WR 0x40bd、DL 0x40be、**PCM_DEC 0x40bf、PCM_CNV 0x40c0**、DL 0x40c1 | **完全一致** |
| `0xb0000024` | MFC 0x40c8 | MFC 0x40c8 ✓ |
| `0xb0000002` device | SAL 0x4012、DL 0x4007、SPLITTER 0x418e、**CODEC_DMA_SINK 0x43b1**、**0x700109d/0x4586** | SAL 0x4012、DL 0x4007、SPLITTER 0x418e、**TDM_SINK 0x428f**、MUX_DEMUX 0x4006、**MFC 0x48ba**、**0x10013d01/0x42d3** |

⇒ **§16 的"缺 DEC/CNV"对 2960 不成立**（它有，而且与厂商逐 iid 一致）；我把两张图混为一谈了。
⇒ **真正的缺口换成了更硬的一条**：2960 的**设备子图 `0xb0000002` 里没有 TDM_SINK(0x428f)、
没有 MFC(0x48ba)、没有 AW(0x42d3)**，放的是 CODEC_DMA_SINK 0x43b1 与 0x700109d/0x4586
—— 也就是**这份容器从 WR 一路到 PCM_CNV 之后，没有任何一段接到 TDM 硬件**。
而我们平时那张 760 B 图反过来：有 0x428f/0x48ba/0x42d3，却没有 DEC/CNV。
⇒ **两份容器各缺一半，拼起来才是厂商那张**：
`0x40bd → 0x40be → 0x40bf(DEC) → 0x40c0(CNV) → 0x40c1 → … → 0x4012(SAL) → 0x48ba(MFC)
 → 0x42d3(AW) → 0x418e(SPLITTER) → 0x4006(MUX_DEMUX) → 0x4007 → 0x428f(TDM_SINK)`。
**这就是可执行的资产缺口结论**：不是"拿不到某个大 blob"，而是
**把 2960 的 entry[0] 换成厂商 entry[0] 的那 7 个模块与其 port/conn（这些字节我们手上都有，
来自 2724 走链自洽的前三个 entry —— 它们的 key 与当代 start 名单一致）**，
然后按已抄到的带内正文配置 `0x428f/0x48ba/0x42d3`。

正在飞的第 53 枪（2960 正确 framing）不管 W3 落在哪一格，**都不改变上面这条结论**：
它判的是"这份缺硬件段的容器能不能被接受"，而答案若为 `0x2`，
说明连**结构自洽但内容不完整**的当代容器也被拒 ⇒ 那就必须先把 entry[0] 拼对再谈出声。

## 19. 第 53 枪判决（`logs/ring-g1p-2960-off0.txt`，`sweettime=1794031385`）
| 门 | 实测 | 判读 |
|---|---|---|
| W0 身份 | cmdline 回显含 `sweetvgraph=2 sweetvgoff=0 sweetvglen=2960`；ring 88379 B | 过 |
| W1 证人 | `vginl v1 vg=2 off=0 len=2960 form=shm` ✓ **framing 这次真的生效了** | 过 |
| W2 发送 | `APM graph_open/ukee2960 src=0x2010 (shm handle=0xb0d7f408 len=2960)` ✓（不是历史的 2944） | 过 |
| **W3** | **`graph_open/ukee2960: no response`** | 落在"重放一次"那一格，**不判内容** |
| **W4 回归** | **`APM graph_open/vp-wr … : no response`**（我们平时必回 status=0 的那张控制图！）、**无 `graph_start`、无 `pcm tone done`**，ring 到 `===== done =====` 正常收尾 | **这一格才是本轮真正的读数**：发出 2960 的带外正文之后，**APM 对随后所有命令都不再应答** |

**这不是"内容被拒"，是"通道被打死"**：同一次开机里，控制图从 `status=0` 变成 `no response`，
说明 APM/GPR 在那一发之后不再回话。历史线索对上了：`docs/START-HERE.md:193` 早写过
e1（2724 那份）**"run115 挂死形态，不是被拒"** —— 同一族现象，今天用正确 framing 复现了一次。
⇒ 机制候选（**未证实**，只列）：(a) 2960 B 正文写进了与 DATA_BUFFER 共用的那块 20 KB 区的 offset 0，
把区搞乱；(b) DSP 去读这块 OOB 正文时卡在翻译/长度上；(c) 一发未答的长命令让 APM 线程停摆。
⇒ 已排：重放同镜像确认（第 54 枪，`b5yy5l1jj` 在取 ring）。
⇒ 由 W4 得出的**下一步仪器**（比"再试内容"优先）：把这张容器发出去之后，
**先发一条已知-good 的小命令探活**（`get_spf_state`），把"APM 是否还活着"与"正文是否被拒"分开。
没有这个探活，`no response` 永远有两种读法。

## 20. G1 判决（第 54 枪，`logs/ring-g1-regevt.txt`，`sweettime=1794032001`）—— 目标止损格到达
| 门 | 实测 |
|---|---|
| W0 身份 | `sweettime=1794032001` 在 ring 内 ✓；ring 85646 B |
| W1 证人 | `regevt v1 on=1 dst=0x40bd param=0x800103f bytes=24` ✓ |
| W2 发送 | `APM reg_events/wr src=0x2010 dst=0x40bd (64 bytes)` ✓ |
| **W3 主判据** | **`reg_events/wr rsp op=0x100100e status=0`** ⇒ **这颗 APM 接受这条命令**，我们此前从未发过它 |
| W4 回归 | `graph_open/vp-wr status=0`、`graph_start status=0`、`pcm tone done (120 buffers)`、`dblat v2 tone n=120 min=5 avg=8 max=10 timeout=0`、`===== done =====` ⇒ **全部照旧，通道没死**（与 53 形成干净对照） |
| W5 耳朵 | 用户报"没有" —— 且本轮 tone 被证明跑完 120 帧 ⇒ **这是一条有效的负读数** |

**结论：`REGISTER_MODULE_EVENTS` 不是缺失的那一环。** 它被接受、流照旧、仍然无声 ⇒ 从嫌疑名单划掉。
**目标止损格已到**（本轮预算内 3 枪：52 K13、53 framing、54 G1）：
按登记时的条款 —— 停止 AP 侧一切实验，转资产/内容线。

## 21. A2（第 55 枪）判决：`no response` 是**通道被打死**，不是内容判决
`logs/ring-a2-2960-livechk.txt`，`sweettime=1794032879`，args 与第 53 枪逐字符相同（唯一变化 = 带探活的 0280）：
```
vginl v1 vg=2 off=0 len=2960 form=shm
APM graph_open/ukee2960 src=0x2010 (shm handle=0xb0d7f408 len=2960)
graph_open/ukee2960: no response
SWEET MARK: livechk v1 rc=-1
SWEET GLINK: spf_state post-vgraph: no rsp        <-- 探活：APM 已经不回答任何东西
APM graph_open/vp-wr … : no response              <-- 我们的控制 open 同样死
```
⇒ **两次逐条复现**（53、55），不需要 M1r 式"重放确认"。
⇒ 探活这一件仪器把两格彻底分开了：**我们此前所有对大正文的 `no response` 读数都不能算内容判决**。
⇒ 已知：760 B 走同一个 `g_apm_graph_open_shm`（同函数、同 handle、同区内偏移 0）每次 `status=0`；
2960 B 每次都打死 ⇒ **界限在 760 与 2960 之间**，而 `sweetvglen` 是现成旋钮 ⇒ 可以二分。

## 22. G-a（第 56 枪）判据 —— 零新代码，开机前写死
`sweetvgraph=2 sweetvgoff=0 sweetvglen=1536`（正文在此截断 ⇒ **形状故意不完整**；
这一发的读数**只是 `spf_state post-vgraph` 有没有应答**，不是内容）。
| 观察 | 判读 |
|---|---|
| `spf_state post-vgraph` **有应答**（`len=` 那种行） | 1536 B 打不死 ⇒ 上限 >1536 ⇒ 下一刀切 2048/2560 |
| `spf_state post-vgraph: no rsp` | ≤1536 就死 ⇒ 上限在 760–1536 之间 ⇒ 下一刀切 1024，并把结论改指向**映射布局**（区 0x5000、与 PCM 共用 offset 0） |
| `graph_open/ukee2960 rsp … status=…`（任意非 0） | **额外收获**：这一格才第一次真正判到正文（能被回答就说明通道活着） |
身份门：`livechk v1 rc=` 必须在 ring 内；`sweettime=1794033…` 对得上；窗口 total==解码字节。

## 23. G-a（第 56 枪）判决：1536 也打死 ⇒ 界限在 (760, 1536]
`logs/ring-ga-2960-len1536.txt`（117051 B），`sweettime=1794033200` 身份 ✓：
```
vginl v1 vg=2 off=0 len=1536 form=shm
APM graph_open/ukee2960 src=0x2010 (shm handle=0xb0d7f408 len=1536)
graph_open/ukee2960: no response
SWEET MARK: livechk v1 rc=-1
spf_state post-vgraph: no rsp
```
**必须同时记下一条混淆，G-b 就是为它设计的**：
- `len=2960` 是**完整、自洽**的容器（slack 0）；`len=1536` 是从同一份容器**中间截断**（切在 entry[1] 里）；
  两者都打死，而 760（另一份**完整**容器）能答。
- ⇒ 现在不能区分"**尺寸**超过某个上限"与"**这份容器的内容**本身有问题"。
- ⇒ **G-b = `sweetvglen=1024`（同一份容器、同一个前缀，只是更短）**：
  若 1024 **有应答** ⇒ 打死通道的是**尺寸**（阈值落在 (1024,1536]），
  因为 1024 与 1536 唯一的差别是同一个前缀的长度；
  若 1024 **仍无应答** ⇒ 尺寸解释被削弱，转 G-c：**映射布局**（区 0x5000、描述符与 PCM 缓冲共用 offset 0）。

## 24. G-b（第 57 枪）判决：1024 也打死 ⇒ **止损格触发，这条路停**
`logs/ring-gb-2960-len1024.txt`，`sweettime=1794033495` ✓：
```
vginl v1 vg=2 off=0 len=1024 form=shm
graph_open/ukee2960: no response
livechk v1 rc=-1   +   spf_state post-vgraph: no rsp
```
三档同一份前缀：**2960 打死、1536 打死、1024 打死**；而 760（**另一份**容器，完整）每次都答。
⇒ 按登记时的止损条款（"3 枪内若所有档位都无应答 ⇒ 停止用这条路发大正文"）**这条路到此为止**。
⇒ 同时**尺寸解释已被削弱**：如果真是"长度 > 某阈值"，1024 与 760 只差 264 字节却一档死一档活，
更像**这份容器前 760 字节里就有让 APM 卡住的东西**（而不是长度）。
⇒ **仍未做的最后一刀（若将来回到这条路）**：拿 `ukee_current` 自己的 **len=760 / len=512** 前缀发一发
—— 同一份字节、长度降到"已知能活"的量级，那才能把"尺寸"与"这份字节本身"彻底分开。
**本轮不开**（止损已到，且 57/120 之内我不擅自加枪）。
⇒ 转 **B 线**：ACDB 重建 per-entry 正文（会话合成：2872 vs 2904）、18504 cal 对象、从未开的第三张图 `0x2012`。

## 25. G-c（第 58 枪）判据 —— 用户点头后加的这一发，开机前写死
`sweetvgraph=2 sweetvgoff=0 sweetvglen=760`：**同一份 `ukee_current` 字节、同一个前缀**，
长度降到"另一份容器每次都活得下来"的量级（760）。它把 G-b 留下的那条混淆切成两半：
| 观察 | 判读 |
|---|---|
| `graph_open/ukee2960 rsp … status=<任意>`（**有应答**） | **是尺寸**：阈值落在 (760,1024]；并且这同时是**第一次真正判到正文**（通道活着，DSP 回答了这份字节） |
| `no response` **且** `spf_state post-vgraph: no rsp` | **不是尺寸，是这份字节**：`ukee_current` 开头 760 字节里就有让 APM 停摆的东西 ⇒ "大正文"这条通道判死，转 B 线 |
| `no response` 但 `spf_state post-vgraph` **有应答** | 第三种读法：**单条命令**被吞、APM 本身没事 ⇒ 与"打死通道"不同因，下一枪改查命令本身 |
身份门：`vginl v1 vg=2 off=0 len=760 form=shm`、`livechk v1 rc=`、`sweettime=<本轮>`、窗口 total==解码字节；
回归门：本轮后面的 `graph_open/vp-wr` 是否恢复 `status=0`（53/55/57 三发里它都被连带打死）。
预算 58/120（用户点头的第 1 项，goal 已在其止损格收口，不占旧 goal）。

## 26. G-c（第 58 枪）判决：**落在第二格 —— 是字节，不是尺寸**
`logs/ring-gc-2960-len760.txt`，`sweettime=1794033872` ✓：
```
vginl v1 vg=2 off=0 len=760 form=shm
graph_open/ukee2960 (shm handle=0xb0d7f408 len=760): no response
livechk v1 rc=-1  +  spf_state post-vgraph: no rsp
```
**这发之所以干净：同一次开机里长度相同、传输相同的对照已经存在** ——
`g_graph_vp_wr` 也是 760 B、也走 `g_apm_graph_open_shm`、同 handle 同区内偏移 0，**每次都拿 status=0**；
`ukee_current` 截到 760 B 就把 APM 打死。
⇒ **尺寸轴彻底排除**（阈值说被 760=活 / 760=死 直接否证）；
⇒ **`ukee_current` 开头 ≤760 字节里就含有让 APM 停摆的东西**；
⇒ "发大正文 OOB 描述符"这条路**判死**，B 线成为唯一方向。
⇒ 下一件**零开机**可做的判别：把 `ukee_current` 前 760 B 与 `g_graph_vp_wr` 前 760 B 做**逐块结构对照**
（外层头、第一个 entry 的 key/size、第一个 param 块的头格式），因为差异一定在其中之一里 ——
最可疑的是**外层 `{total,count}` 索引**：vp_wr 根本没有这层（它直接以 16 B 参数头开始，iid=1）。

## 27. 正式结论（本文件收口）：GRAPH_OPEN 有两种合法正文形式，我们只用对过一种
**结论（实测，非推）**：`APM_CMD_GRAPH_OPEN` 的正文可以是
- **A 形式｜平铺参数记录**：开头就是 `apm_module_param_data_t{iid=1, param, size, err}` 的序列
 （`g_graph_vp_wr` 760 B 就是它）。**这种形式我们每次都拿 `status=0`**，能 PREPARE/START/推 120 帧。
- **B 形式｜ACF 图索引 blob**：开头是 `{u32 total, u32 count}` + 每子图 `{u32 key, u32 size, 正文}`
 （`g_graph_ukee_current` 2960、`g_graph_vendor4` 2724 都是它，厂商 11-05 抓包那三次带外也是它）。
 **这种形式我们发了 5 次（2960/1536/1024/760 + 老的 off=16 系列），没有一次拿到应答，
 且它会把 APM 打死到连 A 形式的控制 open 都不回。**

**边界已经钉住的三件事**（以后不许重走）：
1. **不是尺寸**：同为 760 B，A 形式活、B 形式死（第 58 vs 历次 vp_wr）。
2. **不是传输/句柄/偏移/token**：两者走**同一个函数** `g_apm_graph_open_shm`（同 handle、区内偏移 0、同 token `0x400`），A 形式 status=0。
3. **不是 src 端口**：0x2011 已开过两次，A 形式在两个端口下都 status=0（`ring-g1-graph-2011`、`ring-j2-port-2011`）。
⇒ **所以 B 形式在我们这里的失败是"形式本身没被这条路吃下"**：要么它的语义要求我们先做某件我们没做的事
（厂商那边是**先 SHM_MAP 一块专门给描述符的区**、再按 `addr=0 handle=<该次映射>` 发；
我们复用 PCM 那块 0x5000 区），要么这份 blob 需要我们没有的运行时配套（会话合成正文 / 18504 cal / 第三张图）。
**这两支都归入 B 线，且都有零开机第一步**，不再靠开机试探。

## 28. 下一枪（H1）的取材方式：A 形式 + 当代内容
既然 **A 形式是我们唯一验证过能被吃下的 GRAPH_OPEN 正文**，H1 = 用 A 形式（平铺 `iid=1` 参数记录）
把**当代内容**写出来：外层不要 ACF 索引，直接
`SG_CONFIG(0x08001001) / CONTAINER_CONFIG(0x08001000) / MODULE_LIST(0x08001002) /
 MODULE_PROP(0x08001003) / MODULE_CONN(0x08001004)`，
里面填**当代 key** `{b0000002, b0000025, b0000024, b0000023}` 与**缺的硬件段模块**
`TDM_SINK 0x428f / MFC 0x48ba / AW 0x42d3`（这些字节我们都已从 2724 的自洽 entry[0]/[2]/[3] 与
厂商带内 SET_CFG 正文里拿到）。**先零开机把 `tools/splice_graph.py` 改成产出 A 形式并走链自洽**，
再谈打包。

## 29. H1（第 59 枪）判据 —— 开机前写死（0281，ci_replay applied=222，post-image==链树 HEAD）
`sweetvgraph6=1`：A 形式、784 B、当代内容，走 `g_apm_graph_open_shm`（区内偏移 0、我们自己的句柄）——
**与"每次都 status=0"的 760 B 控制相比，只换正文。**
| 门 | 期望 | 不成立的含义 |
|---|---|---|
| W0 身份 | ring 内 `sweettime=<本轮>`；窗口 total==解码字节；产物内 `aform v1 `×1、`livechk v1 `×1、`regevt v1 `×1 | 少一条 ⇒ 不开机/不作数 |
| W1 证人 | `SWEET MARK: aform v1 on=1 bytes=784` 恰 1 行 | 缺 ⇒ 分支没跑，**不许**判正文 |
| W2 发送 | `APM graph_open/aform-cur src=0x2010 (shm handle=<非0> len=784)` | 缺 ⇒ 没发出去（`aform open refused`/`no shared memory` 另记） |
| **W3 主判据** | **任何应答**：`graph_open/aform-cur rsp op=0x1001000 status=0` 或 `status=0x2/0x1/0x3` | 有应答 ⇒ 通道活着 ⇒ **这是本项目第一次真正判到 GRAPH_OPEN 正文**；`no response` ⇒ A 形式承载当代内容也打死 ⇒ 按止损判死"正文"这一族，转"描述符专用区 / 0x2012 / 18504 cal" |
| W4 连带 | 若 W3=0：`graph_start status=`、`not consumed`、`dblat v2 tone … timeout=0`、`pcm tone done (120 buffers)`；`spf_state` 若被后续路径调用应仍有应答 | 图开成但 start/数据面异常 ⇒ 内容仍有缺口，H2 按块 diff（MODULE_LIST→CONN→CONTAINER） |
| W5 耳朵 | **仅当 W3=status=0** 才请用户听（`sweetvol=16`、120×6 ms ≈0.7 s） | 其他情况这条不算读数 |
预算 59/120。cmdline 长度打包实测，不许继承。

## 30. **H1 暂缓**：H0 那份 784 B 不能就这么开机（自己抓到自己一个错）
把 `g_graph_vp_wr`（已知 status=0）与 `g_graph_aform_cur` 按块并排一比：
| 块 | vp_wr（被接受） | 我生成的 | 差 |
|---|---|---|---|
| SG_CONFIG | 48 | **44** | **少一个 word** |
| CONTAINER_CONFIG | 100 | 60 | 少 3 个属性 |
| MODULE_LIST | 80 | 108 | 多 4 个模块 |
| MODULE_PROP | 196 | 292 | 多 6 条端口记录 |
| MODULE_CONN | 244 | 180 | 少 4 条连接 |
⇒ **SG_CONFIG 48 vs 44 是要害**：被接受的那份比"我以为的字段表"多一个字（很可能是
`{count=1, …}` 那个前导字），也就是说 **我的 emitter 又在按猜测重排正文结构** ——
这正是我们踩过的老坑（"never decode a body shape we only assumed"）。
而且这份正文**一次改了五块**，即便拿到 `0x2` 也归不了因。
⇒ **修正做法（仍然零开机）**：**不要重新生成**，而是
**以 `g_graph_vp_wr` 的字节为底，逐块就地改字段**（先把 `MODULE_LIST` 里的模块换成当代内容，
其余四块一字节不动）—— 这样天然保证布局与被接受的那份完全一致，而且天然就是单变量。
⇒ H1 的 0281 补丁先不打包开机；等我按上面方式重出正文（H0′），再走同一判据。
（今天不浪费这一枪：CI 产物本来就还没出来，marker 计数仍为 0。）

## 31. H0′ 的真相：把被接受的那份正文**逐字读出来**之后，缺口比我以为的小得多
`g_graph_vp_wr` 的两个关键块（原样，字）：
```
SG_CONFIG   size=48 words=12: 1 b0000002 3 | 800100e 4 2 | 800100f 4 2 | 8001010 4 ffffffff
MODULE_LIST size=80 words=20: 1 b0000002 e0000002 8 |
   7001010 4012 | 700101a 4007 | 7001011 418e | 700100e 428f |
   7001098 4006 | 7001015 48ba | 10013d01 42d3 | 7001000 40bd
```
⇒ **我们唯一被接受的图里同时有 `WR_SHARED_MEM_EP 0x40bd` 和硬件段
`TDM_SINK 0x428f / MFC 0x48ba / AW 0x10013d01→0x42d3 / SAL 0x4012 / SPLITTER 0x418e / MUX_DEMUX 0x4006 / DATA_LOGGING 0x4007`。**
⇒ 所以 H1 的真实 delta **只有两个模块**：`PCM_DEC 0x07001005/0x40bf` 与 `PCM_CNV 0x07001003/0x40c0`
（以及它们的 port 记录 + 把 `0x40bd→0x4012` 那条连接改成 `0x40bd→0x40bf→0x40c0→0x4012`）。
⇒ 我 §30 担心的"布局猜错"当场被这份原文坐实了两处：SG_CONFIG 第 1 个字确实是**前导 count=1**
（我的 emitter 漏了），而最后一个属性值是 **`0xffffffff`**（scenario 未设），我却写了 `1`。
⇒ **H0′ 的做法**：在 `g_graph_vp_wr` 的字节上就地加这两个模块 + 两条 port + 改连接，
其余一字节不动 —— 这样布局天然与被接受的那份一致，改动面也小到只有一个主题。

## 32. H0′ 完成：就地改 `g_graph_vp_wr` 的字节，门 = 只允许三块变
`tools/patch_aform_body.py` → `docs/data/h1p-graph-body-patched-vp-wr.txt`
```
accepted body: 760 B, 5 blocks: SG_CONFIG=48 CONTAINER_CONFIG=100
               MODULE_LIST=80 MODULE_PROP=196 MODULE_CONN=244
gate: SG_CONFIG and CONTAINER_CONFIG byte-identical to the accepted body  OK
  MODULE_LIST   80 ->  96 B     (count 8 -> 10, +PCM_DEC 0x07001005/0x40bf
                                 +PCM_CNV 0x07001003/0x40c0)
  MODULE_PROP  196 -> 244 B     (count 8 -> 10, 两条 PORT_INFO in=1 out=1)
  MODULE_CONN  244 -> 276 B     (15 -> 17 条：0x40bd:1->0x4012:2 改成经
                                 0x40bf、0x40c0 两跳)
total: 760 -> 856 B (+96)
```
⇒ 与 0281 用的那份 784 B 相比，这一份**结构不是我以为的**，而是**被接受那份的字节**：
`SG_CONFIG`、`CONTAINER_CONFIG` 由脚本断言逐字节相同，任何越界改动都会让脚本**拒绝输出**。
⇒ 856 B 走 A 形式：尺寸上比 760 大 96 B，而我们已证明 760 的 B 形式才会打死通道，
所以这个尺寸不构成新变量（A 形式 2960 B 都没试过，但那条已由"形式"解释）。
⇒ **0281 不动**（已推的补丁不回头改），H1 改用 **0282**：新增这张数组 + `sweetvgraph7=1` 走
同一个 `g_apm_graph_open_shm`（区内偏移 0、我们自己的句柄、同 token）。
判据沿用 §29：**主判据 = 有没有任何应答**；`status=0` 才请耳朵。

## 33. H1 改为 0282（判据沿用 §29，开机前写死）
`sweetvgraph7=1` 发 `g_graph_vp_wr_dec_cnv`（**856 B**，由 `tools/patch_aform_body.py` 在
`g_graph_vp_wr` 的字节上就地改出：`SG_CONFIG`/`CONTAINER_CONFIG` 断言逐字节不变）。
证人 `SWEET MARK: dcnv v1 on=%d bytes=%u`；发送行 `APM graph_open/dec-cnv … len=856`。
**主判据 = 有没有任何应答**：`status=0/1/2/3` 都算通道活着（第一次真正判到正文）；
`no response` ⇒ 按目标止损判死正文族，转三件资产工作。
附带读：`dec-cnv started` / `graph_start status=` / `not consumed` / `dblat v2 tone` / `pcm tone done (120 buffers)`；
**耳朵只在 `status=0` 时请**（`sweetvol=16`、120×6 ms ≈0.7 s）。

## 34. H1（第 59 枪）判决：正文被接受，start 失败是**我们的工具缺陷**
```
dcnv v1 on=1 bytes=856 | APM graph_open/dec-cnv … len=856
graph_open/dec-cnv rsp op=0x1001000 status=0      <- 接受！
dec-cnv did not start                            <- start 名单 4 个 key，正文只声明 1 个
ring 内 no rsp = 0（通道活着，不是 53/55/57/58 那种打死）
```
⇒ **本项目第一次真正判到 GRAPH_OPEN 正文**：A 形式 + PCM_DEC/PCM_CNV = **被接受**；
此前所有拒绝属于 **ACF 索引形式**发不出去，不是内容不对。
⇒ 下一枪（第 60 枪，0283）= `g_apm_graph_start(port, 0xb0000002u)`（正文自己声明的那个 key），
证人 `dcnv v2 on=%d bytes=%u start=%#x`。判据：start `status=0` ⇒ 进 not consumed/dblat/
`pcm tone done (120 buffers)` 并**请耳朵**（sweetvol=16）；仍拒 ⇒ 合并子图到顶，转厂商多图/描述符专用区。

## 35. 外部评审（GPT）回读 + 我们当场复核的两条
**复核 1（成立，而且是对着我们的字节算的）**：把 B 形式开头 16 字节当 APM 参数头读，得到
`module_instance_id=0xb90 param_id=0x4 param_size=0xb0000002 error_code=0x354`
—— **param_size = 2.75 GiB，而 payload 只有 2960 B（超读约 997564 倍）**。
这解释了为什么截到 1536/1024/760 都一样死：**毒字段就在开头 16 字节**。
⇒ B 形式（ACDB/GSL 侧的图索引封装）**从来不是 APM 的 wire payload**；我们一直把数据库侧结构当线上正文发。
**复核 2（对方建议的 OOB 测试其实早就在做）**：`g_apm_graph_open_shm` 本身就是带外
（`memcpy` 进映射区 + `{addr=off, msw=0, handle, len}`），我们那张 760 B A 形式**每枪都走它并拿 status=0**
⇒ 对方判别表第一行**已经成立**：OOB 读取路径没问题，问题在 B 形式的**格式**。
⇒ 采纳的卫生修正：descriptor 与 PCM buffer 0 都在 offset 0 **确实重叠**，应把 descriptor 挪到 `0x4000`
（region 0x5000 装得下 2960 B）。这是独立的一小步，不与 EOS 混在同一枪。
**待核实的疑点（对方提出，我们有反证）**：`0x10013D01`(AWDSP_RX) 是 AFE 模块 id、`0x42D3` 是 AFE port id，
不该出现在 APM 的 MODULE_LIST 里。但**厂商自己的线上 SET_CFG 就是 `dst=0x42d3, param=0x10013d2a`**
（11-05 抓包实测），所以这颗固件要么有 APM wrapper，要么 AFE 风格 id 在 APM 里合法。
⇒ 记为**疑点而非结论**；若第 60 枪 start 仍拒，它升为第一嫌疑。

## 36. 第 61 枪（0284）判据 —— EOS_RENDERED，开机前写死
`sweeteos=1`：tone 跑完后向 `dst=0x40BD` 发 `0x04001002`，正文 `{policy=1}`（LAST）。
| 观察 | 判读 |
|---|---|
| 收到 `0x05001001`，两字 = `{0x40bd, render_status}` | **第一次拿到样本之后的完成点**：render_status 若为 rendered ⇒ 数据走完播放路径 ⇒ 无声的原因移到 TDM/功放/供电侧；若 dropped ⇒ 数据在图里被丢 ⇒ 回到图内容 |
| 只收到 basic result / 别的 op | 该固件不支持这条 EOS，或我们 dst/时机不对 ⇒ 记为仪器读数，**不判渲染** |
| `no response` | **不判渲染**（事件可能落在通用等待窗之外，代码里已写明）；下一枪改成专门的异步监听 |
| 回归 | `graph_open/graph_start status=0`、`pcm tone done (120 buffers)`、`no rsp` 增量 ≤1、panic 0 |
**打分注意**：`g_apm_cmd` 把正文前两个字打印成 `op=/status=`，对本事件它们其实是
**`module_instance_id` 与 `render_status`** —— 别按 op/status 读。
耳朵：本枪沿用 `sweetvol=16`、120×6 ms；**EOS 与听感同轮取**，若 rendered 且仍无声，那就是最干净的一次定位。

## 37. 第 60 枪判决（0283）：**带转换级的图第一次完整跑通**
```
dcnv v2 on=1 bytes=856 start=0xb0000002
graph_open/dec-cnv rsp op=0x1001000 status=0
graph_start      rsp op=0x1001002 status=0
dec-cnv started
pcm tone done (120 buffers)
dblat v2 tone n=120 min=5 avg=8 max=10 ms timeout=0 step=5
```
⇒ 第 59 枪的 `did not start` 确认是**我们 start 名单的 bug**（4 个 key vs 正文 1 个），不是内容问题。
⇒ **含 `PCM_DEC 0x40bf` + `PCM_CNV 0x40c0` 的图现在 OPEN=0、START=0、120 帧全部推完、无 timeout。**
⇒ 耳朵这一格**现在有效**（ring 证明流真的跑完了）—— 已向用户取读数，未回前不写结论。
⇒ 下一枪 = 第 61 枪 `h3-eos.img`（`sweeteos=1`）：EOS_RENDERED 把观察点推到"样本之后"。

## 38. 第 60/61 枪判决
**第 60 枪（0283）**：`dcnv v2 on=1 bytes=856 start=0xb0000002` → `graph_open status=0` →
`graph_start status=0` → `dec-cnv started` → `pcm tone done (120 buffers)` → `dblat timeout=0`。
**用户听感：没听到** ⇒ 这是**有效负读数**（ring 证明流跑完）⇒
**含 PCM_DEC/PCM_CNV 的图完整跑通 120 帧仍无声**，成为事实。
**第 61 枪（0284）：作废，不是负结果。** ring 里有 tone、有 `sweeteos=1` 的回显，
但**没有 `eos v1` 证人、没有 `wr_eos` 发送行** ⇒ 探针被放在默认 vp-wr 路径，
而 `sweetvgraph7=1` 从自己分支 return，**根本没走到**。按 W1 门记分支未跑。
⇒ **0285** 把同一条 EOS 放进 dec-cnv 分支，新字面量 `eos v2`；第 62 枪重取。
