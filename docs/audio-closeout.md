# 音频收尾（2026-10-27）

> 结论先说：**原先"hypervisor 锁死 apps-smmu，所以 DSP 一读主存就死"的判断是错的。**
> 真正的第一块多米诺是 `SHM_MAP` 地址的高 32 位：厂商发的是 **`msw=1`**，我们一直发
> `msw=0`。把它改成 1 之后，整机**不再挂总线**，控制面全部 status 0。
> 现在停在"DATA_BUFFER 发出去、DSP 不回 DONE"，剩下的是配置完整性问题，不是硬件权限问题。

## 1. 语义核对：厂商 SHM_MAP 到底发了什么

从 `logs/gpr-20261027-090802.txt` 里逐字解码（`gpr_send_pkt` kprobe，48 kHz/16-bit WAV）：

```
SHM_MAP 0x0100100c  len=44  src=0x2002 -> 0x0001  token=0x117
  w6  = 0x00010003   mem_pool_id=3, num_regions=1
  w7  = 0x00000004   property_flag=4
  w8  = 0x1ffd0000   addr_lsw
  w9  = 0x00000001   addr_msw     <-- 关键
  w10 = 0x00005000   size
```

- 如果把它当成 64 位地址：`0x1_1FFD0000` 超出 IOVA 池 `0x10000000..0x20000000`，也超出 32 位。
- 我们试过 33 位 IOVA（`0x1_1FFE8000`，T0SZ=31）：DSP 报 **translation fault**，但整机活着。
  ⇒ DSP 把地址截断到 32 位，**高 32 位不是地址**。
- sheng 的音频记录写明：*"The DSP takes buffers as a 32-bit address with the stream index
  (0x1001 & 0xf) in the upper word."* `0x1001 & 0xf = 1`，和厂商的 `msw=1` 完全对上。

⇒ `0x1_1FFD0000` = **`{stream_index=1, iova=0x1FFD0000}`**，不是 33 位地址。

## 2. 改一个高字，挂死消失

新增 `sweetshmhi=<n>`，把 n 放进 `SHM_MAP` 地址的 bit 32+，其余不变。
公共参数：`sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1`

| `sweetshmhi` | 结果 |
|---|---|
| 0（旧行为） | `data_buffer send` 之后**整机挂死**，日志没有下一行 |
| **1** | `data_buffer sent; gfsr=0 fsynr1=0` → `no rsp (dst=0x40bd)` → **正常 `===== done =====` 并 reset** |

同一轮里所有控制面命令都是 status 0：

```
graph_open/vp-wr        status=0
vp/wr_mf (0x40bd)       status=0
vp/tdm_rate/cfg/ep_mf/frame/power/clk  status=0 ×6
graph_prepare           status=0
graph_start             status=0
data_buffer send        -> sent, gfsr=0 fsynr1=0, no rsp
```

再加 pacing（`sweetbufs=40 sweetbufgap=4`）也一样：发出去了，DSP 不回。
厂商的 4 子图 blob（`sweetvgraph=1`）在 `msw=1` 下**不再挂死**，但 `GRAPH_OPEN` **无响应**。

## 3. 对 `smmu-dead-ends.md` 的修正

原结论把"DSP 一读主存就死"归因于 hypervisor 不让 AP 拥有 apps-smmu。
现在看，**S1 翻译本身是成功的**（SMMU 寄存器回读、CB5 走表都正常），
挂死发生在 DSP 侧对共享内存的解释上：`msw=0` 让它用了错误的流。

- `docs/smmu-dead-ends.md` 里"SMR/S2CR/CB 配置"那些数据仍然有效，
  但"因此停止 apps-smmu 路线"这个结论**不再成立**。
- 我们仍然没有拿到 BYPASS（`S2CR 0x10000 -> 0x20000` 是事实），
  但音频不需要 BYPASS：32 位 IOVA + S1 翻译已经工作。

## 4. 还差什么（下次继续的入口）

对照厂商的 8 条 SET_CFG，我们缺的是**配置内容**，不是寄存器权限：

1. **APM 级 SET_CFG（最大缺口）**：厂商发往 `dst=1`，`mem_map_handle=0xb0e36868`，
   `payload_size=18128`。这是一整块 **18 KB 的图/校准配置**，在共享内存里，
   我们的 `gpr_send_pkt` kprobe 只抓到了包头，没抓到内容。
   没有它，我们的手绘图虽然能 open/start，WR 可能没有真正接到 sink。
2. **TDM 端口映射** `dst=0x4165, param=0x080010c4`，payload 只有 `{0x428f}` 一个模块。
3. 另外两条 WR/转换器格式：`dst=0x40bf`、`dst=0x40c0`，`param=0x08001008`。
4. 厂商是 **4 个子图分别 START**；我们只有一个合并图。`sweetvgraph` 的 blob
   在 `msw=1` 下不再挂死但无响应，说明 blob 本身还需要核对（偏移/长度/来源）。

最值得做的下一步：**在 Android 上抓那 18 KB 的 APM 配置块**。
抓法参考我们之前从 AGM 服务堆里抠出 2712 字节图定义的做法：
在 `gpr_send_pkt` 命中 `dst=1, opcode=SET_CFG, handle=0xb0e36868` 时，
从 `mem_map_handle` 对应的 ION/ion-mapped 缓冲区 dump 18 KB。
拿到它以后，`sweetvgraph=1` + `sweetshmhi=1` 才是厂商的完整复刻。

## 5. 为什么现在停

- 用户决定：音频先收尾，转去修别的。
- 剩下的不是"再调一个寄存器"，而是**一次新的抓包/逆向循环**（18 KB 配置 + 4 子图 blob 核对）。
- 但现在的状态比之前好得多：**不再挂死**，控制面全绿，失败模式是可观测的 "no rsp"。
  下次回来时不需要再碰 SMMU。

## 6. 复现命令

```bash
# 构建（CI 需要 v84+）
GPU_PROBE_LEVEL=0 ./scripts/mkimage.sh \
  sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 \
  sweetnolpass=1 sweetshmhi=1

# 期望日志
#   SWEET GLINK: shm high word 0x1
#   SWEET GLINK: graph_start rsp ... status=0
#   SWEET GLINK: data_buffer sent; gfsr=0 fsynr1=0
#   SWEET GLINK: data_buffer: no rsp (dst=0x40bd)
#   SWEET GLINK: ===== done =====
```

## 2026-11-02：18 KB 的说法第一次在原始报文里核对（附包字段布局）

`logs/gpr-20261027-090802.txt:15` 就是那条 APM 级 `SET_CFG`：

```
p0=10336 p1=770 p2=8208 p3=1 p4=280 p5=16781318 p6=0 p7=0
p8=2967693416 (= 0xb0e36868, mem_map_handle)
p9=18128      (= payload_size，此前只在文档里被引用，未对过原始行)
p10..p31=0
```
第 16 行是同 handle 的后续包（`p4=281`、handle 落在 `p6`）；第 13 行是**已经拿到过的 2712 B 图 blob**
（`p8=2966982840 p9=2712`）——同一种"按 handle 去抠 buffer"的做法在厂商身上成功过一次，
这条路线不是猜测。第 14 行的 `p10=20480` 像 SHM 区域大小，`p8=536674304`(`0x1ffe0000`)
与我们 XNU 侧用的 IOVA `0x1FFE8000` 同一区域。

两条否证式结论：
1. `grep -c b0e36868 docs/traces/trace_ui.txt logs/gpr-20261027-090802.txt` → 前者 0、后者 2，
   且两处都只出现在**包头字段**；两份 trace 都不含 18128 字节正文（kprobe 每包只打 96 个字）。
   ⇒ "18 KB 可能已经在仓库里"这个可能性排除，必须重抓一次。
2. 此前 `handle=0xb0e36868 / payload_size=18128` 只被文档转引，没人对过原始行；现在对上了，
   数字没错。

### 抓正文的方法学要点（下一步）
包里没有正文，只有 `{offset, handle, size}`。要拿到 18128 字节，必须走
handle → 该 handle 对应的已映射区域：厂商的 SHM_MAP 报文给出区域大小/IOVA，
而同一块 buffer 在用户态 HAL 进程里有映射，root 下可读 `/proc/<pid>/mem`。
两条都要在**同一次开机窗口**里抓：kprobe `gpr_send_pkt`（现有脚本
`gprpkt`/`gprsend` 两种格式，后者字段更多）+ 记录发该包的进程与它的映射。
