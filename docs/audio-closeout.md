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

## 2026-11-02（下午）：§128 那条"读厂商 shm"的路判死；正文只剩一个前置条件

全部只读，设备 2aa26095，Android 会话 uptime ≈2100 s。

### 1. shm 读不出来 —— 不是权限，是映射类型
AGM 服务（`vendor.qti.hardware.AGMIPC@1.0-service`，pid 1700）里唯一的共享映射：

```
723c97a000-723c982000 rw-s 00000000 00:09 266   /dmabuf:     ← 0x8000 = 32768 B
/proc/1700/fd/23 -> /dmabuf:   ino: 266          ← 同一个 buffer
```
三条读法全部失败，且失败方式各不相同：

| 尝试 | 结果 |
|---|---|
| `dd if=/proc/1700/mem bs=4096 skip=… count=8` | `read error: I/O error`（0 字节） |
| `PTRACE_ATTACH` + `PTRACE_PEEKDATA` | attach **成功**，peek 第一字即 `I/O error` |
| `dd if=/proc/1700/fd/23` | `No such device or address`（dma-buf 的 anon_inode 不支持 reopen） |

对照组（同一进程、同一工具、换一个普通 anon `rw-p` 页 `71abfcc000`）：
`read via /proc/pid/mem` **成功拿到 64 B**。⇒ EIO 不是 SELinux、不是 uid，
而是这块 VMA 是 **VM_PFNMAP/special pte**（`dma_buf_mmap` 用 `remap_pfn_range`），
`__access_remote_vm()` 拿不到 page 引用。**任何基于 VA 的用户态读接口都拿不到它**，
§128 的"root 下读 /proc/<pid>/mem"作废；`/dev/mem`、`/dev/kmem`、`/proc/kcore` 在本机都不存在，
所以也换不到"从物理页侧读"的替代路。

### 2. 文件路线与 ACDB 路线都排除
- `aw882xx_acf.bin` = **2672 B**（`/vendor/firmware/`，需 root 才能取，`adb pull` 直接 Permission denied）。
  2712 = 2672 + 40（24 B GPR 头 + 16 B `apm_cmd_hdr`）——**此前标为"未验证"的那条算式，
  第一次由文件大小本身证实**，也说明厂商是把整个文件当正文塞进 shm。
- 全量按尺寸筛 `/vendor{,/etc,/firmware} /odm /apex /data/vendor`（18050..18200 B）：
  只有 `libplatformconfig.so`(18132) 和两个 vndk `.so`(18164)，**无音频件**。
- 正在用的 ACDB = `/vendor/etc/acdbdata/Forte/Forte_acdb_cal.acdb`（1634478 B，AGM fd 11）
  + `/data/vendor/audio/acdbdata/delta/Forte_acdb_cal.acdbdelta`（**0 字节**，没有 delta 校准）。
  整文件按 u32 LE 扫（`tools/acdb_size_scan.py`）：18128 / 18088 / 18112 / 2712 / 2672 **各 0 次命中**，
  只有 20480 命中 1 次（`0x17c8e4`）。⇒ 正文不是 ACDB 里一条"以长度为键"的现成切片。
  （顺带修正 §119：第 14 行 `p8=536674304` = **0x1FFD0000**，不是 0x1ffe0000。）

### 3. 真正的卡点是"触发一次真实播放"
| 触发方式 | `gpr_send_pkt` 命中 | 结果 |
|---|---|---|
| `tinyplay tone.wav` | **0** | dmesg `check_userspace_service_state: pcm_id 0 state 0` → 90 ms 内 `msm_common_snd_shutdown`，tinyplay 报 `Error playing sample`。本机 ASoC 把 PCM 让给userspace 服务，**tinyplay 不是有效触发器** |
| `ctl.restart audioserver` | 15 | 解码后全是小包：`SET_CFG` 只有 dst=模块、size=24/32/48 的内联包，**没有 dst=1 的 18128** |
| `ctl.restart vendor.audio-hal` | 15 | 同上，只到 core/register 层 |
| `input keyevent 24/25`（root 和 shell 两种 uid） | **0** | 没有产生 device open |
| `cmd notification post` | 0 | shell 通道 `sound=null`，本来就不响 |
| `agmplay -i 3` | 0 | `smartpa_type = Entry not found` → `Invalid input, entry not found for : 3`；它查的是 `/vendor/etc/backend_conf.xml`（只有 `name/rate/ch/bits`，没有 id），Mi 的 AGM 表对不上 |

⇒ 18 KB 那条报文只在**真实播放**时出现，而本机没有纯 shell 的播放触发器。

### 4. 仪器自检：uprobe 是好的，"0 命中"是真的没被调用
`tools/uprobe_selftest.c`（`-static`，`probe_me` 的文件偏移 = 0x171cc）挂 uprobe 后
运行：**5/5 命中**且 `%x0` 参数正确带出（`a0=0x0/0x1/0x2…`）。
所以 `gsl_write / gsl_graph_write / gsl_dp_write / gsl_shmem_map_allocation /
gsl_shmem_get_metadata` 的 0 命中是**真的没被调用**，不是仪器坏。
另：这台内核 **不接受 `$argN`**（`Invalid $-valiable specified`），必须写 `%x0..%x5`；
且 `echo > uprobe_events` 在事件仍 enable 时返回 **EBUSY**（和 kprobe_events 同一个坑），
arm 前必须先逐个 disable。

新增工具（都只读）：`tools/apm_blob_dev.sh`、`tools/apm_blob_capture.sh arm|pull|clean`、
`scripts/parse_apm_blob.py`（拿 20261027 那条老 trace 自测，正确点出
`dst=1 tok=280 handle=0xb0e36868 size=18128 <== TARGET`）、`tools/apm_shm_read.c`、
`tools/acdb_size_scan.py`。

### 5. AW882xx 收口：判定树（用已有 ring 收口，不是新功能）
语料 = `logs/ring-*`（155 个归档，含 `levels before` 的行全部取出）：

```
所有出现过的功放行只有两种，且都是成功：
  80 × "geni amp 0x34 r=0 id=0x2032"   80 × "geni amp 0x34 profile 44/44"
  80 × "geni amp 0x35 r=0 id=0x2032"   80 × "geni amp 0x35 profile 44/44"
整个语料里没有任何一条 NACK / r!=0 / "no amp"。
```
再看复位状态，语料**整齐地分成两组**：

| 组 | `levels before (rstA=…)` | 有没有"我们放复位"那行 | ACK |
|---|---|---|---|
| A（80 次成功里全部） | **rstA=1 rstB=1 spksw=1** | 有 `rstA=1 rstB=1 spksw=1 irq=1` | 2/2 |
| B（`ring-amp8`、`ring-b*/c*/d*/e*/f*/g*` 等 GPU 时代的跑） | **rstA=0 rstB=0 spksw=0** | **没有**（根本没走到） | 0（未到探测点） |

判定树（每条都对应一个可直接读的证据，不需要新代码）：

1. `levels before` 里 rstA/rstB 已经是 1 ⇒ 复位是**上一次会话留下的**，
   本次只是"接着用一个已经出复位的芯片"。**今天所有 80 次成功都属于这一类。**
2. rstA/rstB = 0 且日志里**没有** `rstA=1 rstB=1 spksw=1 irq=1` ⇒ 这台根本没走到功放探测
   （GPU 实验跑或提前死机），**不能**记为"功放不 ACK"。
   → `docs/status-log.md:722`（run67"0 个功放"）属于这一类，"I2C 无 ACK"这个前提在语料里**没有证据**。
3. 只有当日志里**同时**出现"我们放复位那行"和 `r!=0` 时，才是真的电气/时序问题，
   此时按顺序查：`scl/sda` 空闲电平（是否被拉低）→ 复位低电平保持时间 →
   先写 `0x00=0x55aa` 软复位再读 ID → 地址 7 位/8 位写法。

⇒ **唯一还没被任何一次开机验证过的叶子**：真正冷启动（rstA=0）由我们自己放复位之后，
功放到底 ACK 不 ACK。验证成本 = 1 次开机：长按电源彻底断电（或拔插电池/静置到 rst 线回到 0），
再 `fastboot boot`，只看三行——`levels before (rstA=0…)` → `rstA=1 rstB=1 spksw=1 irq=1` →
`geni amp 0x34 r=?`。出现 `r=0 id=0x2032` 即"冷启动也能自己拉起"，这条彻底收口；
出现 `r!=0` 才进入上面第 3 步。
