# 厂商 GPR 报文格式（本机 kprobe 实测）

`docs/traces/trace_ui.txt` 是在**本机 Android** 上对 `gpr_send_pkt()` 下 kprobe
抓到的 534 个包（播放一次系统音效）。解码用
`scripts/parse_gpr_trace.py`。

> ⚠️ 抓包里的 `p0..p23` 是**十进制**（kprobe 默认输出）。按十六进制解析会
> 得到一堆"不认识的 opcode"——本项目在这上面浪费过一整天。

## 报文头

所有消息都是 `cmd = 6`：

```
w0 = (cmd << 4) | (len << 8)
w1 = domains
w2 = src_port        w3 = dst_port
w4 = token           w5 = opcode
w6 = payload_addr_lsw   w7 = payload_addr_msw
w8 = mem_map_handle     w9 = payload_size
w10.. = 内联正文（当 payload 不在共享内存里时）
```

## 两种形态

| 形态 | 判据 | 例子 |
|---|---|---|
| **内联**（payload 在包里） | `handle = 0`，正文从 `w10` 起 | 发往模块的 `SET_CFG`（dst = 模块 iid，72/88 字节） |
| **共享内存**（payload 在映射区里） | `handle ≠ 0`，`w6` 是**区域内的偏移** | `GRAPH_OPEN`、发往 APM 的 `SET_CFG`（dst = 1，都是 40 字节） |
| **无描述符** | 正文直接从 `w6` 开始 | `SHM_MAP` / `SHM_UNMAP` |

### SHM_MAP（`0x0100100c`，44 字节，无描述符）

```
w1 dom = 0x10302   （bit16 = src_port_valid，只有 MAP/UNMAP 会置）
w2 src = 0x2002    w3 dst = 1    w4 token = 递增序号
w6 = 3 | (1 << 16)      pool 3 = APM_MEMORY_MAP_SHMEM8_4K_POOL，1 个区域
w7 = 4                  property_flag = 4（厂商自定义；上游只用 0 和 0x2）
w8 = 0x1ffe8000  w9 = 1 → 地址 0x1_1FFE8000（33 位 IOVA）
w10 = 0x5000            size
```

### GRAPH_OPEN（`0x01001000`，40 字节，共享内存）

```
w6 = 0            w8 = handle    w9 = 0x1f0（流图）/ 0xb38（设备图）
```
图定义本身在映射区的 offset 0 处，`GRAPH_OPEN` 只引用它。

### SET_CFG

- 发往 **APM**（dst = 1）：40 字节，`w6 = 0x400` / `0x1000` / `0`，
  `w9 = 0x108` / `0xb0` / `0x70` —— 参数块在映射区里。
- 发往 **模块**（dst = 模块 iid，如 `0x428f` TDM / `0x42d3` AW / `0x40bd` WR）：
  72/88 字节，`handle = 0`，参数**内联**。

#### 内联正文的定长规律（2026-11-02 由 6 条包独立验证）

```
w10 = 目标 iid        w11 = param_id
w12 = data_len        w13 = flags（实测恒为 0）
w14.. = data
w9  = round_up(16 + data_len, 8)
```

⇒ 之前把 `w10` 当成"包头里重复出现的 dst"是误读，它就是正文第一个字。
⇒ 同一形状也能装进 `dst=1` 的批量包里由 APM 转发（UI 抓包 `size=64` 那条的正文首字是模块 iid
   `0x40ca`），所以"直接发"与"经 APM 发"两种路由都合法。
⇒ 完整的"厂商配过哪些模块 × 哪些 param × 原字节"表在
   [audio-closeout.md](audio-closeout.md) §3（2026-11-02 晚 2）。

### DATA_BUFFER_V2（`0x0400100a`，68 字节）

```
w2 src = 0x2011     w3 dst = 0x40bd（WR_SHARED_MEM_EP 实例）
w4 token = 0 / 1
w6 = 0 或 0x800    缓冲区的**区内偏移**
w8 = handle        与 GRAPH_OPEN 同一个 handle
w9 = 0x480         长度
其余（时间戳 / flags / metadata 六个字）全 0
```

#### 但几何随场景变（2026-11-02 两份抓包对表）

| 场景 | src | handle | token | 偏移 | size | 包数 |
|---|---|---|---|---|---|---|
| UI 音效（本文上面的表，`trace_ui.txt`） | `0x2011` | `0xb0d890e8` | 0/1 | `0x0`/`0x800` | `0x480`=1152 | 479 |
| 媒体播放（`gpr-20261027-090802.txt`） | `0x2010` | `0xb0d890b8` | 0..3 | `0x0`/`0x1000`/`0x2000`/`0x3000` | 4096 | 95 |

⇒ 恒等式（两份都成立）：**`token == 槽号`，`偏移 == 槽号 × size`，`msw = 0`，一整个 handle 管到底**。
⇒ 我们用的 1152/2 槽那一档**就是厂商 UI 场景那一档的逐字复制**，
所以"缓冲区几何"不是可变量（详见 `audio-closeout.md` §2，2026-11-02 晚 2）。

## 一个 handle 装下所有东西

**所有** `GRAPH_OPEN` 与**全部 479 个** `DATA_BUFFER` 用的都是同一个
`handle`。区域内的偏移分配：

```
0x000   图定义（0x1f0 流图 / 0xb38 设备图）
0x400   APM 参数块（0x108）
0x1000  APM 参数块（0xb0）
0x000   PCM buffer 0（0x480，GRAPH_OPEN 之后复用这块）
0x800   PCM buffer 1
```

⇒ 区域大小至少要 `0x5000`，与 `SHM_MAP` 报的 size 一致。

## 相关文档

- 图拓扑（模块 / 端口 / 连接）见 [vendor-graph.md](vendor-graph.md)
- 当前进度与卡点见 [audio-status.md](audio-status.md)
