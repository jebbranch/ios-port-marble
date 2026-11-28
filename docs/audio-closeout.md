# 音频收尾：当前判决与证据索引

> **当前状态（2026-11-28）**：音频暂停在 **73/120**。数据面、图控制、HAL 连续流都已通；
> 最后一次有效电声负读数是「功放没有 TDM 时钟」。第 73 轮是 PRM 回包解析失配，
> 0298 已离线修好绑定，但**尚无完整内核 / 镜像 / 开机**。

## 1. 已经证明的

| 层 | 结论 | 证据 |
|---|---|---|
| ADSP 控制面 | `GRAPH_OPEN` / `PREPARE` / `START`、TDM `SET_CFG` 全 `status=0` | `status-log.md`、`vendor-gpr-packets.md` |
| 音频数据面 | S1 叶子 `AP` 位修正后 `DATA_BUFFER done status=0`，1200 buffer 正常复位 | `status-log.md` 2026-11-02 |
| 图内容 | 856 B A 形式（含 `PCM_DEC` + `PCM_CNV`）能开、能起、能推 120 帧 | `unixnu-sheng-audio-2026-11-07.md`、`status-log.md` |
| iOS 音频桥 | `mediaserverd` 会 open 我们发的壳并调 `method_Open` | `audio-hal-bridge.md` §11 |
| HAL 捕获 | 524288 帧、500234 非静音帧、peak 32768；只证明入流 | `audio-hal-live-2026-11-12.md` |
| HAL → AudioReach | 30-period 连续流 `30/30/30`，六条控制应答全 0，安全 teardown 通过 | `audio-halstream-0294-2026-11-13.md` |
| 功放寄存器 | 0290/0291 修掉诊断复位与三字节 WRITE，两颗表值 `b440`/`05a9` 读回一致 | `audio-ampguard-2026-11-08.md` |

## 2. 仍然没证明的

- **最终渲染 / 人耳出声**：三次有效听测均无声。
- **EOS 事件**：`EOS_RENDERED` 在收包仪器被证明可用的前提下从未到达。
- **PRM 时钟票**：第 73 轮没有取得 `0x204` 的 status，不能判 PRM 接受/拒绝。
- **0298 实机**：只完成离线验证，未完整构建、未打包、未开机。

## 3. 关键负读数

### 3.1 856 B 图：三次有效无声

图能 open/start、能推 120 帧、DSP 归还缓冲，但人耳三次都没听到。
`EOS_RENDERED` 在 137 包、预期 opcode mix 的日志里为 0，且 EOS 发送后没有任何后续包。
⇒ AP 侧事件路线按止损关闭；不能把「缓冲归还」当「消费完成」，更不能当「已渲染」。

### 3.2 第 72 轮：功放没有 TDM 时钟

两颗 AW88266 的 profile / ID / vol 都正确，清 PWDN 后 `SYSST=0`、PLL 全 0；
安全门在任何 period 提交前拒绝并双关断。这是有效负读数，失败点在功放时钟，不在 HAL 或 PCM。

### 3.3 第 73 轮：PRM 回包解析失配

0297 发的是厂商精确 `0x204@12.288MHz attr4 root0`，但等待的是 APM BASIC_RSP
`0x02001005`；第 64 轮全量 RX 已证明 PRM 专用应答是
`0x02001002` + `param=0x0800102c`。5 秒 timeout 后安全门不 OPEN、不碰功放、不发 PCM，UNMAP=0。
⇒ 不能把这次 timeout 命名为「PRM 拒绝」。

### 3.4 R1：身份成立，但不是电声测试

120 秒内 HAL 全静音，`attempted/sent/done=0`，安全 teardown 后才出现 PCM；
功放 `pre_play` 从未调用。R1 只能记「未触发」，不能记「无声」。

## 4. 已关闭 / 不要重走

- 厂商 18 KB 校准不是数据面主因（S1 叶子 `AP` 位才是）。
- B 形式 ACF 索引不是 APM wire payload；旧 2724 是上一代且自身越界。
- 旧 2960 五枪用默认 `off=16/len=2944` 削掉了 entry[0] 的头，那些拒绝不作内容判决。
- 「图里缺 DEC/CNV」两次被逐字 dump 推翻；真正缺的是 `PCM_DEC` + `PCM_CNV`。
- `DATA_BUFFER_DONE` 只证明缓冲归还，不证明消费或渲染。
- 「EOS 无应答是 100 ms 窗太短」已撤回：`g_apm_xfer` 等 5 秒。
- 动态通道 discard 路径已证伪（`dynrx v1` 零行）。
- 逐记录 `error_code` 在当前协议形态下不可观察（N1 哨兵证明 `err=0` 是没人写过）。
- 不要再用 `DATA_BUFFER` 当探针；不要用尺寸匹配认领 ACDB 记录组。

## 5. 证据索引

| 文档 | 内容 |
|---|---|
| [`status-log.md`](status-log.md) | 按轮次的完整实验日志 |
| [`audio-hal-listen.md`](audio-hal-listen.md) | 0293 → 0298 / R1 合并记录 |
| [`audio-halstream-0294-2026-11-13.md`](audio-halstream-0294-2026-11-13.md) | 第 70 轮 HAL 连续流 |
| [`audio-hallisten-0298-2026-11-15.md`](audio-hallisten-0298-2026-11-15.md) | 0298 离线修正 |
| [`audio-hal-bridge.md`](audio-hal-bridge.md) | iOS 音频架构、ABI 与桥接路线 |
| [`audio-preflight-2026-11-07.md`](audio-preflight-2026-11-07.md) | 描述符污染 PCM 的离线复现 |
| [`audio-descsep-live-2026-11-08.md`](audio-descsep-live-2026-11-08.md) | 描述符隔离开/关对照 |
| [`audio-eos-wire-2026-11-08.md`](audio-eos-wire-2026-11-08.md) | 厂商 EOS 28 B 封装证据 |
| [`audio-ampguard-2026-11-08.md`](audio-ampguard-2026-11-08.md) | 功放两处真实缺陷与第 67/68 轮 |
| [`audio-hal-live-2026-11-12.md`](audio-hal-live-2026-11-12.md) | HAL capture 第 69 轮 |
| [`unixnu-sheng-audio-2026-11-07.md`](unixnu-sheng-audio-2026-11-07.md) | 第三方音频源码复核与「不要重走」记录 |
| [`vendor-gpr-packets.md`](vendor-gpr-packets.md) | 厂商 GPR 报文与 opcode |
| [`vendor-graph.md`](vendor-graph.md) | AGM 图内存提取与拓扑 |
| [`data/audioreach-graph-format-2026-11-06.md`](data/audioreach-graph-format-2026-11-06.md) | 图序列化、MFC/TDM 分析 |
| [`data/vendor-accepted-setcfg-2026-11-05.md`](data/vendor-accepted-setcfg-2026-11-05.md) | 被接受的 SET_CFG 字节 |
| [`data/vendor-tx-timeline-2026-11-06.md`](data/vendor-tx-timeline-2026-11-06.md) | TX 时间线与 cadence 统计 |

## 6. 如何恢复音频

1. 用 0298 重新构建完整内核，确认 PRM 应答绑定与所有安全门；
2. 冻结镜像身份（SHA / `sweettime` / cmdline / marker / 符号 / 8 核门）；
3. 只在 `sweetlisten=1` 下按厂商顺序 request `0x204@12.288MHz attr4 root0`；
4. request 失败不 OPEN、不启功放；STOP/CLOSE 或 release 缺 ACK 不假释放 DMA；
5. 首次峰值不得高于历史 `sweetvol=16`（约 −6 dBFS），扬声器与听筒分开；
6. 判决必须同时有：HAL 非静音及时到达 + PRM status=0 + 双 AW PLL locked + period 实际提交；
7. 没满足上述任一条，都不记「出声」。
