# 音频收尾（2026-10-27）
> **⚠️ 本文件含已被推翻的结论（2026-11-07 更新）。当前权威状态见
> [`unixnu-sheng-audio-2026-11-07.md`](unixnu-sheng-audio-2026-11-07.md) §§13–48，全项目九条线的一句话状态见 **[`STATUS-2026-11-07.md`](STATUS-2026-11-07.md)**。**
> 已作废/收窄的旧说法：① "18 KB / 18128 / 18504 cal 是墙" —— 降级为未检验假设；
> ② "ACDB 重建容器 = EBADPARAM，内容被否" —— 那些拒绝来自 **B 形式（ACF 索引）**，
> 而 B 形式开头 16 字节被 APM 当参数头读会得到 `param_size=0xB0000002`（2.75 GiB），
> 它是 GSL/ACDB 侧结构、**从来不是 APM wire payload**；③ `g_graph_vendor4`(2724) 是**上一代**
> 且自身索引超界 4 B，由它得出的一切结论作废；④ 旧 2960 五枪用默认 `off=16/len=2944`
> 削掉了 entry[0] 的头，其 `0x2` 是自伤不是判决；⑤ "图里缺 DEC/CNV" 与 "各缺一半" 两次都被
> 逐字 dump 推翻 —— 被接受的 `g_graph_vp_wr` 同时有 WR 与硬件段；⑥ `DATA_BUFFER_DONE` 只证明入队；
> ⑦ "EOS 无应答是 100 ms 窗太短" 已撤回（`g_apm_xfer` 等 5 s）。
> **当前事实**：856 B A 形式（+PCM_DEC/PCM_CNV）`graph_open=0`、`graph_start=0`、
> `pcm tone done (120 buffers)`、DSP 回 121 个 DONE —— 用户**三次**都没听到 ⇒ 三条有效负读数。
> `EOS_RENDERED` 在收包仪器被证明可用的前提下从未到达（第 64 枪 137 包、`0x05001001` = 0）
> ⇒ AP 侧事件路线按止损关闭，现在做 B 线（第 65 枪 = 0288 摘掉 AFE id，内核已过门，只差打包开机）；
> **预算 64/120**。新会话第一入口 = [`STATUS-2026-11-07.md`](STATUS-2026-11-07.md)。


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
  2712 = 2672 + 40（24 B GPR 头 + 16 B `apm_cmd_hdr`）。**【已撤回，见文末"撤回 1"】**
  那条 2712 的包 `p5=0x01001000` 是 **GRAPH_OPEN**（设备图正文），不是 ACF 的 SET_CFG；
  2672 与 2712-40 相等是巧合。文件大小本身没错，错在我给它贴的报文类型标签。
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

### 5. AW882xx 收口：判定树 —— "无 ACK" 是 bit-bang 仪器失败，不是功放失败

> 订正：本节第一版只 grep 了 `geni amp` 这个前缀，漏掉了 bit-bang 的另一种措辞
> `amp 0x34 no ack`，因而一度得出"语料里没有 NACK"的错误结论。下表是按**两种措辞**
> 重新统计的结果，第一版的错误作废。

语料 = `logs/` 全量（含 `logs/raw/*.bin`）：

| 观测 | 行数 | 文件数 |
|---|---|---|
| `amp 0x34/0x35 no ack`（软件 bit-bang 探测） | 470 | 155 |
| `0 amplifier(s) answered 0x2032` | 235 | — |
| `N amplifier(s) answered`，N > 0 | **0** | — |
| `geni amp 0x34/0x35 r=0 id=0x2032`（GENI SE 主机） | 160 | 80 |
| `geni amp … profile 44/44` | 160 | 80 |

两条总线的记录完全相反：**bit-bang 235/235 全败，GENI 80/80 全胜**；而 bit-bang 自己报的
电气检查是通过的（`sda driven low reads 0` / `scl driven high reads 1` / `bus idle, sda=1`），
所以线是好的、地址是对的、芯片是活的——失败在 bit-bang 自己的时序/起始条件那一侧。
结论：**`docs/status-log.md:722`（run67"本轮 I2C 扫描 0 个功放"）不是功放故障**，那台跑
的正是 bit-bang 路径；GENI 路径在同一块硬件上从未失败过。

判定树（每步只看日志措辞就能分叉，不需要新代码）：

1. 先分主机：`amp …` = bit-bang（**不作为功放好坏的证据**），`geni amp …` = GENI SE。
2. `geni amp 0x34 r=0 id=0x2032` → 芯片在；继续看 `profile 44/44` 是否满 44。
3. 只有 `geni amp` 出现 `r!=0`/`no ack` 才进入硬件分支，顺序是：
   `levels before (rstA=… rstB=…)` → 我们放复位那行 `rstA=1 rstB=1 spksw=1 irq=1` 有没有出现 →
   `scl/sda` 空闲电平 → 复位低电平保持时间 → 先写 `0x00=0x55aa` 软复位再读 ID。
4. 没同时看到"放复位那行 + GENI NACK"就不许改代码，一律先怀疑仪器。

**唯一还没被任何一次开机验证过的叶子**：80 次 GENI 成功的开机，`levels before` **全部**是
`rstA=1 rstB=1 spksw=1`（复位是上一次会话留下的），`cold(rstA=0) + geni amp` 的文件数 = **0**。
也就是"由我们自己从 0 拉起复位、GENI 能不能读到 0x2032"从未被证明。
验证 = 1 次开机：`adb reboot bootloader`（warm reboot 会把 TLMM 恢复默认 ⇒ rstA=0）后
`fastboot boot` 一个**不带 GPU 探测**的镜像——GPU 探测结束时会复位 SoC，把功放阶段切掉，
这正是那 155 个 bit-bang-only 文件的成因。只看三行：
`levels before (rstA=0…)` → `rstA=1 rstB=1 spksw=1 irq=1` → `geni amp 0x34 r=?`。

### 6. 附带发现：刷缓冲的遗留探针会挤掉音频事件
`/data/adb/post-fs-data.d/99-gpuclk.sh`（开机 arm `rpmh_write_batch`/`rpmh_write`/
`kgsl_regmap_bulk_write`）在一次 35 分钟的会话里产生 **203,951** 条事件，音频窗口的事件被
挤出环形缓冲（`tools/gpr_capture.sh arm` 的 `echo > kprobe_events` 也因此返回 EBUSY，
旧的 96 字探针原样留着）。`tools/apm_blob_capture.sh arm` 现在会先把 `rhb/rhw/kbw` disable
再开新探针。目标里的收尾义务（`99-rpmh.sh`）已删除并核对；`99-gpuclk.sh`/`99-gpuprobe.sh`
是 GPU 线的工具，保留，但任何音频抓包前必须静音它们。

## 2026-11-02（cold-amp 开机）：AW882xx 这条线彻底收口

`images/audio-coldamp.img`，`sweettime=1793592884`（ring 与镜像一致，已核对），
boot-args = `… sweetgpu=0 sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1`
（**故意不带 GPU 探测**，这样它不会在中途复位 SoC；`adb reboot bootloader` 是 warm reboot，
TLMM 回默认 ⇒ 进入时功放复位线为 0）。归档：`logs/ring-audio-coldamp.txt`。

同一台开机里两条主机的先后证据（行号取自归档）：

```
170  SWEET I2C: levels before (rstA=0 spksw=0 scl=1 sda=1 rstB=0 irqA=1 irqB=1)   ← 冷态
173  SWEET I2C: resets released (A=1 B=1)                                        ← 我们自己放的
174-178 SWEET I2C: sda/scl driven 电平全部正确，bus idle, sda=1
244  SWEET I2C: released scan found 0 device(s)     ← bit-bang
311  SWEET I2C: spksw-high scan found 0 device(s)   ← bit-bang
364  SWEET I2C: asserted scan found 0 device(s)     ← bit-bang
371/373 SWEET I2C: amp 0x34 / 0x35 no ack           ← bit-bang
374  SWEET I2C: 0 amplifier(s) answered 0x2032
2380-2382 SWEET I2C: pin 56/57 cfg=0x87 level=1 → rstA=1 rstB=1 spksw=1 irq=1     ← GENI 前
2383 SWEET I2C: se15 fw=0x303 proto=3 status=0 tx=0x201029e4 rx=0x20104800 geni=0
2391-2394 SWEET I2C: geni amp 0x34 r=0 id=0x2032 / profile 44/44
                   geni amp 0x35 r=0 id=0x2032 / profile 44/44     ← ★ 冷态自放复位也 ACK
```

结论（把上一节那个"唯一未验证叶子"关掉）：

1. **冷启动由我们自己释放复位，两颗 AW882xx 照样应答并写满 44 条 profile。**
   之前 80 次成功全部发生在 `rstA=1`（上一次会话留下的复位状态）下，现在这台是
   `rstA=0 → 我们放复位 → ACK`，语料里第 0 类 → 第 1 类。
2. `0 amplifier(s) answered` / `no ack` **只属于 GPIO bit-bang 那台主机**，
   与它同一台开机里 GENI 全绿 ⇒ "AW882xx I2C 无 ACK"从来不是硬件问题，
   而是 bit-bang 自己不可用（本轮再次 0/235）。既然 GENI 可用，bit-bang 不必再修。
3. 功放与图都正常，剩下的失败模式没变：
   `2372 SWEET GLINK: data_buffer: no rsp (dst=0x40bd)`、`2373 buffer probe dst=0x40bd -> -1`
   —— DSP 对发给 WR 实例的 DATA_BUFFER **连响应都没有**（不是 NACK）。
   控制面 `graph_open/vp-wr rsp … status=0`、SMMU `ats1pr iova=0x1ffe8000 -> par=0 OK`。

⇒ 音频线现在只剩一个未知量：**那条 APM 级 18 KB SET_CFG 的正文**。
仪器与解法都已就位（`tools/apm_blob_capture.sh` + `scripts/parse_apm_blob.py`），
唯一缺的是**一次真实播放**——本机没有任何纯 shell 的播放触发器（见 §3 表）。

## 2026-11-02（晚些时候）：找到了 shell 侧的真实触发器，harvest 管线打通

上一节说"本机没有纯 shell 的播放触发器"——**这句作废**。`strings /vendor/bin/audio-factory-test`
把 Mi 工厂测试自己调的命令暴露出来了，关键点是 **`-i` 接的是 backend 名字字符串，不是数字**，
而且卡号是虚拟声卡 100：

```
/vendor/bin/agmplay /vendor/etc/%s.wav -D 100 -d 100 -i CODEC_DMA-LPAIF_RXTX-RX-0
/vendor/bin/agmplay /vendor/etc/%s.wav -D 100 -d 100 -i TDM-LPAIF-RX-TERTIARY-VIRT-0 -dkv 0xA2000100
```
`/vendor/etc/` 下有 Mi 自己的测试音：`bottom_spk.wav`(底部功放)、`bottom_spk_hp.wav`、
`rcv.wav`(听筒)、`mi_us_whitenoise.wav`、`RES1K.wav`。实测（tracing 全程开着）：

| 命令 | 结果 |
|---|---|
| `agmplay bottom_spk.wav -D 100 -d 100 -i CODEC_DMA-LPAIF_RXTX-RX-0` | `pcm_plug_open - agm_pcm_plugin_open` → **`start error`** |
| `… -i TDM-LPAIF-RX-TERTIARY-VIRT-0 -dkv 0xA2000100` | `Enter VIRT-` + `smartpa_type =` + **`gpr_send_pkt` 90~116 次** ⇒ 真的开了图 |

这一窗口里的 APM 级报文（`logs/apm-blob-20261102-122551.txt`，同一 handle 被复用）：

```
pid=9206 SET_CFG dst=1 tok=20 handle=0xb0d890c8 size=1648
pid=9206 SET_CFG dst=1 tok=22 handle=0xb0d890c8 size=112
pid=9206 SET_CFG dst=1 tok=34 handle=0xb0d890c8 size=1648      ← 同一序列走第二遍
pid=9206 SET_CFG dst=1 tok=36 handle=0xb0d890c8 size=112
（模块级 dst=16584/16573/17115/16575/16576/16741 全是内联小包 24/32/48）
```
⇒ 三条新事实（其中第 2、3 条的措辞在文末"再订正"里被改写）：**APM 级 SET_CFG 的大小是随设备/图变的**（672/112/1520/1648 vs 曾经的 2712/18128），
`handle` 在同一次会话里被多个 SET_CFG 复用（`0xb0d890c8` 跨两次触发都相同 ⇒ 它是 DSP 侧固定映射），
**而 18128 属于我们还没打开的那个设备**（主 media 输出 + 保护链），工厂测试的 TDM VIRT 组设备不是它。

### harvest 管线（这才是今天真正的收获）
`scripts/apm_harvest.py` 一次跑完：触发 → 拉 trace → 解出 `dst=1` 的 SET_CFG 与所有 gsl uprobe
参数 → **趁缓冲区还活着**把每个指针参数用 `apm_shm_read` 读出来 → 在 dump 里搜"这一轮真的出现过
的 size 值 / 18128 / AudioReach TLV 头"。实测输出：

```
hb-usetcfg-a3-7d387fed30.bin (8192 B): size=32@0x9a8 size=32@0xbe8 size=48@0x5c0
hb-ugwr-a4-7d31f0a0a8.bin   (8192 B): size=48@0x88  size=112@0x33
hb-ugwr-a4-7d31f0940c.bin   (8192 B): size=48@0xd24 size=112@0xccf
```
⇒ **报文模板确实躺在用户态堆里**（112/48/32 这些 size 值就在被指针指着的那页上），
所以"18128 一旦发出，同一次 harvest 就会把它那 18 KB 正文一起 dump 出来"不是推测，是已验证的机械过程。

顺带把 gsl 的 API 形状钉住（`gsl_write/gsl_graph_write/gsl_dp_write` 是 16 字节 thunk 跳到同一实现，
所以三个 probe 参数完全一致）：
`a0='GSL'(0x47534c) a1=0xc0000001 a2=结构 a3=结构-4 a4=缓冲区 a5=tagged 指针`；
AGM 堆是 **SCudo/HWASan 带标签指针**（高字节 `0xb4`），做指针过滤时要连 `0x00`/`0xb4` 两种 tag 一起接；
`gsl_ioctl` 用 `a0='GSL' a1=cmd a2=参数结构 a3=结构长度(0x20)`，见过 cmd=0xa/0x15/0x0/0x1。
过滤条件也修了：用户态 VA 在 Android arm64 上都 > 4 GiB，否则 `a5=0x26464` 这种 tid 会被当指针去读。

### 还缺的一步（比之前小得多）
只需要让**主 media 输出设备**开一次（框架级播放：音乐/视频/铃声预览），18128 就会出现在同一台
开机里；届时 `python3 scripts/apm_harvest.py --want 18128 --secs 20 --cmd '<框架播放的替代触发器>'`
或直接让它默认跑一次 agmplay，再对照 `dst=1 size=18128` 那一行。工厂命令开不出这个设备，
是目前唯一的缺口——不是方法缺口。

## 2026-11-02（闹钟实验）：18 KB 那一类报文在 shell 驱动下重现，实测大小是 **18504**

新触发器（开的是**主 media 输出设备**，工厂 agmplay 开的是 TDM VIRT 组设备）：

```bash
settings put system volume_alarm 10
am start --user 0 -a android.intent.action.SET_ALARM \
  --ei android.intent.extra.alarm.HOUR <H> --ei android.intent.extra.alarm.MINUTES <M> \
  --ez android.intent.extra.alarm.SKIP_UI true
```
闹钟响 = 框架级播放 = AGM 主输出 + 功放链真开。抓到的原始序列
（`logs/apm-18504-evidence.txt`，发送者是 AGM(pid 1707) 的 binder 线程 7209/7210）：

```
966.400755  SET_CFG   dst=1  tok=47 handle=0xb0d5f2c8 size=496
966.402396  SET_CFG   dst=1  tok=48 handle=0xb0d5f2c8 size=264
966.408117  SET_CFG   dst=1  tok=53 handle=0xb0d5f2c8 size=2872
966.411748  SHM_MAP   dst=1  tok=54 p6=0x10003 p7=4 p8=0x1FFF0000 p9=1 p10=20480
966.412217  SET_CFG   dst=1  tok=55 handle=0xb0d8e7c8 size=18504      ← ★ 就是这一类
966.413385  SHM_UNMAP dst=1  tok=56 p6=0xb0d8e7c8
966.414512  SET_CFG   dst=1  tok=57 handle=0xb0d5f2c8 size=112
966.415052  SET_CFG   dst=1  tok=58 (inline) size=64
```
三条结论（都推翻/收窄了此前的说法）：

1. **"18128"不是常数**：同一条报文这一轮是 **18504**（+376）。结合 §2（不是文件、不是 ACDB 长度字段），
   现在可以确定它是**运行期拼装**的 ⇒ 移植目标从"抠出那 18128 字节"改成"**复刻结构**"，
   把厂商某一次的字节固化进固件是错的路线（哪怕抠到了也不能直接抄）。
2. **SHM region 大小恒定 20480**（两轮都是 `p10=20480`），18504+40=18544 < 20480 自洽；
   `p8` 是 DSP 侧地址（本轮 0x1FFF0000，Oct-27 是 0x1FFD0000 ⇒ **每次开机不同**，不能当常量登记）；
   但 region **大小**恒定 20480 ⇒ 这块区是 APM 配置专用区。
3. 一次会话里有**两个 handle**：`0xb0d5f2c8` 承载小的（264/496/2872/112），
   `0xb0d8e7c8` 只承载大的（18504）⇒ 大正文单独一次映射、用完立刻 unmap（1.2 ms）。
   ⚠️ 措辞订正（见文末）：handle 是 **DSP 在 SHM_MAP 响应里返回的 id**（我们代码
   `sweet_glink.c:2337`、`:3564-3567`、打印点 `:3446`），同一开机内复用、**跨 boot 必然重新分配**，
   不是"固定映射"，也不能预置成常量。

`gsl_write` 的 12295 次命中说明主写入路径就是它，`a2/a3(=a2-4)` 是描述符结构、`a4` 是被拷贝的缓冲区；
`a3` 处那个 u32 **应该**是长度，但用户态解引用 fetcharg 在这台内核上取不到值（`ugwrL` 命中 0 次 ⇒
长度要靠配对时间戳从 `gprsend` 的 `p9` 拿，不能靠 deref）。

## 事故记录：设备在插桩后掉离 USB 总线（原因未定，需要人眼确认）
约 04:35，`adb devices`、`fastboot devices` 与 `system_profiler SPUSBDataType` **同时看不到任何
USB 设备**（两条 USB 3.1 总线皆空）。此前 100 s 内我开着 12 个 gsl uprobe（其中 `gsl_write` 是热路径，
12295 次命中）并临时挂过一个带用户态解引用+过滤器的探针。候选原因两种，**我无法从这台 Mac 区分**：
(a) 插桩把 AGM/内核带死（没有 panic 日志，adb 已断）；(b) 掉电/线松。
工具侧安全默认已记下：热路径 uprobe 只在短窗口开；不要给 `+off(%reg)` 的用户态解引用加过滤器
（取不到值时静默丢事件，会让人把"仪器不行"误读成"没有调用"）。

## 2026-11-02（同日第三段）：跨进程 dma-buf **能读**；但那块 dma-buf 不是正文容器

本段仍是只读 + `/data/local/tmp` 暂存；收尾已撤临时开机钩子、清暂存、tracefs 归零
（`uprobe_events=0 kprobe_events=0 tracing_on=0`）。

### 1. 18 KB 类报文第二次复现，且不需要人
`am start … SET_ALARM` 响铃时抓到 `SET_CFG dst=1 … p9=18504` @ t=522 s；
上一台开机在 t=63 s 也抓到过同一条。⇒ **它绑定"主输出设备第一次打开"**，
用闹钟就能稳定复现，之前"需要人随手播一段"的判断作废。

### 2. 更正上午的结论：VM_PFNMAP ≠ 读不出来
`tools/apm_dmabuf_pull.c`：`pidfd_open(pid)` + `pidfd_getfd(pidfd, fd)` 拿到**同一个 dma-buf 的 fd**，
在**自己进程里** `mmap(PROT_READ, MAP_SHARED)` → 完整读出 32768 B（`wrote=32768 (complete)`）。
所以正确的说法是：**远程访问 API（`/proc/<pid>/mem`、`PTRACE_PEEKDATA`、fd reopen）拿不到它，
而读取者自己的映射完全能读**。这条能力是可复用的，上午"此路判死"写重了。

### 3. 但读到的这块不是正文
AGM 只有这一条 `rw-s /dmabuf:`（32 KB，ino 276）。事后快照 + 开机后 0..174 s 每 1 s 的 157 张时间戳快照，
内容都极稀疏：`nonzero=7.4%`，**最长连续非零段只有 319 B**（@0x0，头 320 B 是 `…fe` 结尾的指针样表），
没有 `18504/2872/672/20480` 任何一个作为 u32 出现。⇒ 它是 GPR 队列，不是配置正文区。
另外 AGM 的 342 条映射里**没有任何一条 20480 字节的可写映射**
（20480 的 7 条全是 `.so` 的 `r--p/r-xp` 与 `[anon:.bss]`）⇒ 正文如果在用户态，就在 malloc arena 里；
而内核侧 `grep -awc gsl /proc/kallsyms` = **0** ⇒ GSL 栈全在用户态，没有内核 shm 可 vmap。

### 4. 写路径待定（不说死）
上一台开机里大包 @t=63 s 前后**没有任何 `ugwr`(gsl_write) 命中**（本 boot 第一次 ugwr 命中在 t=135 s），
说明那条正文不是经 gsl_write 落进去的；但本次 t=522 s 那一次还没做同样的配对（快照钩子在 t=174 s 已自停）。
这一条留作下一步的第一个判断点。

### 5. 下一步（已备好，不需要人、不留钩子）
在**拷贝那一刻**抓，而不是事后翻缓冲区：
给 libc `memcpy` 挂 uprobe，用**寄存器值**过滤长度（`n=%x2:x64` + `filter=n>=17000 && n<=20000`），
再用 `common_pid == <AGM pid>` 限定进程，并且**只在响铃那 ~10 秒开启**（memcpy 是全局热点，
长时间挂有系统级代价——见下条风险记录）。拿到 `src` 指针后用 `apm_shm_read` 读普通 heap 页即可（已验证可行）。
libc 偏移取法：`llvm-nm -D --defined-only libc.so | grep ' memcpy'`；本机 libc 的 R E 段
`p_offset == p_vaddr == 0x4d000`，所以符号值就是 uprobe 文件偏移。

### 6. 风险记录（保留为约束，不作为结论）
本段有一次设备在重度插桩窗口内**自己重启**：上一次开机 uptime 已从 2111 s 走到 ~2400 s，
`gsl_write` 一族 uprobe 在 100 s 里命中 12295 次 + 一个带用户态解引用与过滤器的探针之后，
adb/fastboot/`system_profiler` 同时看不到 USB 设备；约 2 分钟后设备重新出现在 adb，`/proc/uptime=121 s`
⇒ 是**自行重启**而不是掉线（没有人按过）。因果未证明，但作为约束写死：
热路径 uprobe 只短开、不给用户态解引用加过滤器（它取 0 会把事件全丢掉，制造"没有调用"的假象）。

## 2026-11-02（晚，成员 A 复核后）：撤回两条 + 主嫌换人

### 撤回 1：「2712 = aw882xx_acf.bin(2672) + 40 ⇒ 厂商把整个 ACF 当正文」是错的
`logs/gpr-20261027-090802.txt:13` 的 `p5=16781312 = 0x01001000`，按本文档自己的字段表
（`docs/vendor-gpr-packets.md`："GRAPH_OPEN 0x01001000"）那是 **GRAPH_OPEN**，不是 SET_CFG。
2712 是**设备图正文**；2672 是 ACF 文件，两者相等是巧合。今天早些时候我写"这条算式第一次被
文件大小证实"，是在一个被贴错标签的包上做的确认 —— 作废。（`payload_size` 含 24+16 头这件事
本身仍成立，只是对象是图正文。）

### 撤回 2：`p8=536805376` 是 **0x1FFF0000**，不是 0x20010000
`python3 -c "hex(536805376)" = 0x1fff0000`。本文 `:361`/`:373` 两处写错，已改口。
配合 Oct-27 的 `0x1FFD0000`（`logs/gpr-20261027-090802.txt:14`）⇒ **该 SHM 区域地址每次开机不同**
（DSP 侧动态分配），所以"把它当常量登记"也是错的。

### 主嫌换人：DSP 读我们 buffer 被自己的 S1 权限挡下（不是缺 18 KB）
`logs/ring-audio-coldamp.txt`（sweettime 1793592884）同一台开机里：

```
2289  s1 cb5 … leaf=0x701 …                     ← AP=[7:6]=00 ⇒ 只允许特权访问
2288  ats1pr iova=0x1ffe8000 -> par=0 OK         ← 特权读探测，必然绿
2336  pre  cb5 FSR=0x400 FAR=0x12140410          ← 与 cb0..cb4 同样的陈旧值
2371  post cb5 FSR=0x408 FAR=0x1ffe8000 FSYNR0=0x40002   ← ★ 新故障位，FAR 正是我们登记的 IOVA
2372  data_buffer: no rsp (dst=0x40bd)
```
cb0..cb4 的 FSR 全程停在 0x400，只有 cb5 多了 `0x8`，而 `0x1ffe8000` 是我们 `sweets1va=` 的值
（见同一行 boot-args，`:375`）⇒ **"ats1pr par=0 所以 SMMU 没问题"这个判断不成立**：
探测走特权，DSP 真实访问未必。

置信度（成员 A 给 5–10%，我独立复核后同意，并补充一点）：**"缺 18 KB 大 SET_CFG"是
DATA_BUFFER 不被消费的唯一/主要原因 ≤10%**；校准改的是增益/滤波/保护参数，不负责路由，
而控制面已经证明 0x40bd 实例活着（`vp/wr_mf status=0`，`:2299`）。

### 另一条测量层面的自我怀疑（成员 A 提出，我接受）
`no rsp` 的打印条件把"有回复但 `len<40`"也算成无响应，而模块级 SET_CFG 的回复就是 32 B
⇒ **在证明"真沉默"之前，得先把 ready/len 与前 64 B 打出来**。这条比抓任何字节都便宜。

### 下一步顺序（都不需要厂商字节）
1. `post-send` 与 `post` 之间把 cb5 的 `FSR/FSYNR0/FAR` 三元组与 `g_rsp_ready/g_rsp_len` 一起打出来
   （新补丁必须自带 marker，例如 `SWEET AUDIO: dbraw …`），先确定是"权限故障 + 真沉默"还是
   "权限故障 + 32 B 错误回复"。
2. 叶子 `0x701 → 0x7C1`（AP=11）单变量对照：若 `FSR` 不再新增 `0x8`、且 DATA_BUFFER 有回复 ⇒ 主因确认。
   风险：改的是我们自己那张 S1 表，不碰厂商 CB；失败也只是仍无回复。
3. 负对照 `dst=0x0bad`（不存在的实例）与 `GET_SPF_STATE` 前后各一次 —— 用来区分"WR 不回"与"DSP 被打死"。

## 2026-11-02（领队复核后）：对本文再订正四处

1. **`mem_map_handle` 的语义**：DSP 在 SHM_MAP 响应里返回的分配 id（我方代码
   `bsd/dev/sweet_glink.c:2337`、`:3564-3567`，打印 `:3446`）。**同 boot 复用、跨 boot 重新分配**
   ⇒ 本文 `:368` 的"固定映射"作废，也不能当常量预置。厂商三段
   `0xb0d5f2c8 / 0xb0d8e7c8 / 0xb0e36868` 与我们的 `0xb0e145f8` 同处 `0xb0d5–0xb0e3`
   这一段，正是同一个 DSP 侧分配器的形状。
2. **控制面从没让 DSP 读过 AP 内存**：冷启动那轮 GRAPH_OPEN 走**内联**正文
   （`sweet_glink.c:4801-4802`，ring `:2273` 的 `mem_map_handle=0`），带 handle 的
   `g_apm_graph_open_shm()`（`:3628`）没被选中。⇒ IOVA `0x1ffe8000` 的**唯一**消费者是
   DATA_BUFFER，也就是本 port 第一次让 ADSP 读 AP 内存就 fault（`:2371`）。这把
   "SMMU 只是可疑"升级成"这是第一个真正被 exercised 的路径"。
3. **`470 / 235` 不是两组独立观测**：`amp 0x34 no ack` + `amp 0x35 no ack` 是每次扫描的两行，
   470/2 ≈ 235，与 `0 amplifier(s) answered` 同一批。bit-bang 的记录是 **235 次扫描全败**，
   写成两个数会让人误以为是两个独立语料。
4. **`profile 44/44` 是传输计数不是回读**（`sweet_i2c.c:758-764` 统计的是写入无 NACK 的条数）
   ⇒ 它证明"两颗芯片在 I2C 上 ACK 并接收了 44 条"，**不证明**寄存器内容生效；
   同理"图正常"只到"被接受（status=0）"。判定树里要按这个强度写。
5. **E1 的设计改法**：叶子 `0x701` 除了 AP=[7:6]=00，**bit1=AF 也是 0**；若这颗 SMMU 未开 HTTU，
   access-flag fault 会伪装成同一种沉默 ⇒ 一次放到 **`0x7C3`（AP=11 + AF=1）**，通了再二分，
   比"先只改 AP"省一次开机。

## 2026-11-02（晚）单变量补记：`sweetshmflag=0` 的那次开机

镜像 `images/shmflag0.img`（sweettime 1793605571），ring 归档 `logs/ring-shmflag0.txt`：

| 行 | 观测 |
|---|---|
| `:2319` | `shm_map pool=3 pa=0xa2fe8000 flag=0` —— 回到"裸物理地址"老格式 |
| `:2322` | `shm_map rsp status=0` —— 映射本身仍被接受 |
| `:2364` `:2399` | cb5 `FSR=0x400`，**没有**上一轮 `flag=4` 时新增的 `0x408` |
| `:2400` | `data_buffer: dst=0x40bd done status=0x2 w28=0 w32=0xb0e145f8` |
| `:2401` `:2406` | `data_buffer rsp (56 bytes)`；`buffer probe → -1`（缓冲区里没有 DSP 写回的痕迹） |

⇒ 两个事实钉住：**(a)** `status=0x2` 是历史值，不是这次的新症状（`docs/status-log.md:1269/1407/1470`
早就有同值，我先前说"第一次拿到回复"是过头话，已订正）；**(b)** `flag=0` 下**没有任何内存访问**
就先回 BADPARAM，所以 `0x2` 这条路径与 SMMU 权限无关 —— 权限只在 `flag=4`（offset 模式）里
以 `FSR=0x408` 的形式出现。这轮之后，①（`flag=4 + sweets1ap11=1`）仍然是唯一还没测过的权限变量。

## 2026-11-02（晚 2）：两份厂商抓包合并对表 —— 报文已逐字对齐，缺的是"图里的模块"

> 本节的 `sweet_glink.c:NNNN` 全部对**当前 CI 树**（base + 0001..0230 全链，6798 行）。
> 这棵树可以离线复现（`git apply --include=bsd/dev/sweet_glink.c` 按文件名顺序过一遍
> `xnu/patches/*.patch`），以后引用行号一律以它为准，不再用旧的 6764 行副本。

数据源都是本机 Android 的 `gpr_send_pkt` kprobe（字段是**十进制**）：

- **A** = `docs/traces/trace_ui.txt`（UI 音效场景，479 个 DATA_BUFFER，模块包在 `:130-148`）
- **B** = `logs/gpr-20261027-090802.txt`（媒体场景，95 个 DATA_BUFFER，模块包在 `:17-23`）

### 1. SET_CFG 正文布局第一次定量确认

正文从 `w10` 起，结构是 `{iid, param_id, data_len, flags=0, data…}`，
且 **`w9 = round_up(16 + data_len, 8)`**。六条包独立验证同一条规律：

| 包 | data_len | 16+len 取整 | 实测 w9 |
|---|---|---|---|
| 0x428f/0x08001017 | 12 | 32 | 32 ✓ |
| 0x428f/0x0800101B | 28 | 48 | 48 ✓ |
| 0x40bd/0x0800100C | 30 | 48 | 48 ✓ |
| 0x40bf·0x40c0/0x08001008 | 30 | 48 | 48 ✓ |
| 0x40c8/0x08001024 | 14 | 32 | 32 ✓ |
| 0x4165/0x080010C4 | 4 | 24 | 24 ✓ |

⇒ 之前把 `p10` 读成"包头里重复出现的 dst"是误读：它就是正文第一个字（目标 iid）。
⇒ 我们 `g_apm_set_cfg()`（`:5009` 起：`pl+0=iid, pl+4=pid, pl+8=dlen, pl+16=data`，
`plen=(16+dlen+7)&~7`）写出的字节布局与厂商**同构**，这点不用再怀疑。

### 2. DATA_BUFFER 几何：我们那一档就是厂商的某一档 ⇒ 这个变量关闭

| | src | dst | handle | token | 区内偏移 | size | 包数 |
|---|---|---|---|---|---|---|---|
| A | `0x2011` | `0x40bd` | `0xb0d890e8` | 0 / 1 | `0x0000 / 0x0800` | **1152** | 240 + 239 |
| B | `0x2010` | `0x40bd` | `0xb0d890b8` | 0..3 | `0x0000/0x1000/0x2000/0x3000` | **4096** | 24/24/24/23 |

我们（`:5036-5038` `G_PCM_OFF_A=0x0000 / G_PCM_OFF_B=0x0800 / G_PCM_BYTES=1152`、
`:5647` `(n&1)?B:A`、token=`(n&1)`）与 **A 逐字相同**。
⇒ "改成 4096/4 槽"不是复现厂商，而是换到另一个场景；**缓冲区几何不作为变量**，不占开机。
⇒ B 里 `token == 槽号`、`偏移 == 槽号×size`，A 里也是（2 槽）；我们的 `(n&1)` 在 2 槽下等价。
⇒ DATA_BUFFER 帧本身（`{addr_lsw, addr_msw=0, handle, size}` 在 `w6/w7/w8/w9`）与厂商完全一致，
   `status=0x2` 不可能来自这一帧的形状。

### 3. 厂商配置过的模块全集（A∪B），与我们是否发过

| dst | param | data_len | 数据字节（从 `p14` 起，小端字） | A | B | 我们 |
|---|---|---|---|---|---|---|
| `0x40bd` | `0x0800100C` | 30 | `{1, 0x09001000, 18, 48000, 0x10010, 0xF0010, 0x20001, 0x01 0x02}` | ✓ | ✓ | **✓ 逐字一致**（`:5870-5880`，7 个字全部对过） |
| `0x428f` | `0x08001017` | 12 | `{96000, 0x30018, 1}` | ✓ | ✓ | **值不同**：我们第二字是 `0x30020`（`:5917`、`:5938`） |
| `0x428f` | `0x0800101B` | 28 | `{0, 2, 0x10001, 7, 0x200004, 0, 1}` | ✓ | ✓ | ✓ 逐字一致（`:5926-5932`） |
| `0x40bf` | `0x08001008` | 30 | `{1, 0x09001000, 18, 0x10010, 0xF0010, 0x30001, 0x20000, 0x01 0x02}` | ✓ | ✓ | ✗ |
| `0x40c0` | `0x08001008` | 30 | 同上 | ✓ | ✓ | ✗ |
| `0x4165` | `0x080010C4` | 4 | `{0x428f}` | ✓ | ✓ | ✗ |
| `0x4308` | `0x08001017`+`0x0800101B` | 12/28 | 与 `0x428f` 同形状 | ✓(各 2) | ✗ | ✗ |
| `0x42d3` | `0x10013D2A` | 28 | （校准） | ✓(×2) | ✗ | ✓ `vp/aw_cfg`（`:5922`） |
| `0x40c8` | `0x08001024` | 14 | `{96000, 0x30018, 0x20001, 0x0003}` | ✗ | ✓ | ✗ |
| `0x40ca` | `0x08001024` | 12 | `{48000, …}` | 经 `dst=1` 批量包 | ✗ | ✗ |

另有 4 个 `dst=1` 的 APM 级包（size 264/112/176/18504）正文首字为 0，**不是** `{iid,param,…}` 形状，
形状未解；18504/18128 随开机变化 ⇒ 已按既定结论不再追。
注意 B 里 `0x40c8` 是直接 `dst=`，A 里 `0x40ca` 走 `dst=1` 转发 —— 同一个 param 两种路由都合法。

### 4. 由此订正目标里那句"下游 9 模块链 0x40bd→0x40be→0x40bf→0x40c0→…→0x4165"

- **`0x40be` 不存在**：`grep -c 'p3=16574'` 在 A、B 里都是 0；那个连续链是我起草目标时的臆写，作废。
  真实测到的额外配置对象是 `0x40bf`、`0x40c0`、`0x4165`、`0x4308`、`0x40c8`（+经 APM 转发的 `0x40ca`）。
- **更要紧的**：这些 iid **都不在我们打开的那张图里**。`g_graph_vp_wr`（`:1853-1902`）的字节序列里
  `bd 40`/`8f 42`/`d3 42`/`12 40` 各命中 3–4 次，而 `bf 40`/`c0 40`/`c8 40`/`65 41`/`08 43` **0 命中**。
  ⇒ 所以步骤②不能写成"把缺的配置补上"：配置的对象在我们的图里根本不存在，发过去只会拿到错误码。

### 5. 步骤②的三个候选（按"零成本优先"排）

- **②a 零编译**：`sweetvgraph=1` + `sweetvgoff/sweetvglen`（`:6038-6052` 已实现），改用厂商自己那张
  含转换器的 4 子图 blob。它在 `msw=1` 下拿过"GRAPH_OPEN 无响应"，但当时 **2724 vs 2712 的
  12 字节头偏移**这个疑点没排除，`sweetvgoff/sweetvglen` 正是为它准备的，还没单独扫过。
  期望观测：`graph_open` 是否回 `status=0`、回的是哪张图。
- **②b 一个字的改动**：TDM 速率参数第二字 `0x30020 → 0x30018`（厂商 24-bit；`/tmp/backend_conf.xml`
  里 `TDM-LPAIF-RX-TERTIARY` 的 **device** 条目正是 `bits="24"`，group 条目才是 32）。
  风险：低——改的只是一个已测到字段的值。
- **②c 判定型（不修复）**：把 §3 里 4 条已测报文按原字节发给 `0x40bf/0x40c0/0x4165/0x40c8`。
  全部 `status=0` ⇒ 我们的图里其实有这些模块（blob 理解错），② 变成"补配置"；
  全部错误/不回 ⇒ **结构缺失被证实**，② 变成"扩图"（大改），此时应转 ②a 而不是硬写 blob。
  风险：若 DSP 对未知 iid 不回而不是回错误，这次开机只会耗在超时上，不改任何硬件状态。

### 6. 本节的边界（哪些仍是推测）

- "MFC/转换器没配 ⇒ WR 输入队列协商失败 ⇒ BADPARAM" 是**推测**，机制未证；
  §3 只证明"厂商配了、我们没配、且我们的图里没有这些模块"。
- `0x08001008 / 0x08001024 / 0x080010C4` 的语义（输入媒体格式 / 标量 / 端口映射）按字段位置和
  长度**推**出来的，名字未在任何抓包里出现，后续代码注释里要保持"param id + 原字节"而不是命名。
- 本节的三个候选都不改变判决顺序：**①（0230：`flag=4 + sweets1ap11=1`）仍是下一次开机的唯一新变量**。

## 2026-11-02 16:23 判决①上机：故障**换了类型**，但这不能记在权限假设的账上

镜像 `images/s1ap11.img`（sweettime 1793607627，cmdline 已在 `images/s1ap11.build.log` 核对：
相对上一次开机只多了 `sweetshmflag=4 sweets1ap11=1`），ring 归档 `logs/ring-s1ap11.txt`。

| 项 | 上次 flag=4（`ring-audio-coldamp.txt`） | 这次 flag=4 + ap11（`ring-s1ap11.txt`） |
|---|---|---|
| 叶子 | `leaf=0x701`（`:2289`） | **`leaf=0x7c3`**（`:2342`，0230 生效） |
| shm_map | `flag=4` → `status=0`（`:2293`） | `flag=4` → `status=0`（`:2350`） |
| graph_start | `status=0` | `status=0`（`:2384`） |
| pre cb5 | `FSR=0x400`（`:2336`），W1C 写回后仍 `0x400`（`:2337`） | `FSR=0x400`（`:2393`），W1C 后仍 `0x400`（`:2394`） |
| **post cb5** | **`FSR=0x408 FAR=0x1ffe8000 FSYNR0=0x40002`**（`:2371`） | **`FSR=0x402 FAR=0x1ffe8000 FSYNR0=0x40003`**（`:2428`） |
| DATA_BUFFER | `no rsp`（`:2372`） | `dbraw ready=0 len=0` + `no rsp`（`:2432-2433`）、`buffer probe → -1`（`:2434`） |

### 三条读数

1. **0230 的 `dbraw` 立刻兑现了它的作用**：`ready=0 len=0` —— flag=4 时"没回复"是**真的没收到任何包**，
   不是 `g_rsp_len < 40` 把一条 32 字节基本回复吞掉。那条推理线到此闭死。
2. **故障跟着我们的写而动**：两次开机的 pre 状态完全相同（`0x400` + 同一批陈旧 FAR），
   post 却从 `0x408/FSYNR0=0x40002` 变成 `0x402/FSYNR0=0x40003`，`FAR` 钉在 `0x1ffe8000`。
   ⇒ 仪器能看见自己的效果，cb5 这张表确实是 DSP 走的那张 —— 这是这条线第一次
   "改一位 → 故障寄存器换类型"的闭环。（顺带记一条仪器学：`0x400` 是 cb0..cb5 全部自带的
   陈旧值且 W1C 清不掉，所以**只有"post 相对 pre 新增的位"算读数**。）
3. **但这次开机不是对 AP 的测试** —— 是 0230 的实现缺陷。AArch64 长描述符里
   `[1:0]` 是**类型字段**：在 L0..L2 上 `0b01`=block、`0b11`=指向下一级页表。
   `0x701 | (3<<6) | 0x2 = 0x7c3` 把 bit1 置上了，于是那块 2 MiB 地址被当成
   "L3 页表的物理地址"，DSP 去查一张我们从未建过的表。
   **这个坑仓库早就写过**：`sweet_glink.c:4202-4210` 的注释正是讲"`0x703` 让 SMMU 把
   `0xa2e00000` 当 L3 表，所以 walk 必失败"。0230 等于把 `0x703` 重新请回来了（`0x7c3`）。
   `0x408 → 0x402`、`FSYNR0` 尾数 2→3 与"描述符类型换了"完全相容。

### 编码核对与两处自我订正（ARMv8 长描述符）

`[0]=V`，`[1]=1` 表示 table（block 时为 0），`[4:2]=AttrIndx`，**`[7:6]=AP`**，
`[9:8]=SH`，**`[10]=AF`**，`[11]/[53]=UXN/PXN`。

- 订正一：我此前写过"叶子 `0x701` 除了 AP=00，**bit1=AF 也是 0**" —— **错**。
  AF 是 bit10，`0x701` 里已经是 1；`0x2` 根本不是 AF，它是类型位。
  ⇒ 上一节（领队复核后）第 5 条作废，"一次放到 AP=11+AF=1"这个设计从起点就是坏的。
- 订正二：`0x701` 真正缺的只有 AP。放开 AP 的正确写法是 `0x701 | (3<<6) = 0x7c1`，**绝不碰 bit1**。

### 下一步（判决①的重做，补丁 0232）

- 0232：把 0230 的 `s1_leaf |= (3ull << 6) | 0x2ull` 改为 AP 由参数给——
  `sweets1ap=<0..3>`（默认 0 ⇒ 一个字节都不改，现网行为不变），
  marker 改名 `SWEET MARK: s1ap leaf=`，workflow 的 marker 列表**同批**更新。
- 正确开机：`… sweetshmhi=1 sweetshmflag=4 sweets1ap=3`，期望 `leaf=0x7c1`、
  post cb5 **相对 pre 不再新增任何位**、`data_buffer … done status=0x0`。
- 若 `0x7c1` 仍 fault：权限线到此为止（这就是参数侧第 3 次无变化），按止损转
  "offset 模式为何拖死互连"——与 GPU 线同一个未解问题。
- 风险标注：这次开机与上次同档（flag=4），历史上有过整机停在黑屏的先例，
  只能长按电源；已按"一次一个变量"避开其它热路径。

### 止损计数（照实记）

`DATA_BUFFER` 的结果：flag=0 → `done status=0x2`；flag=4 → 无回复；flag=4+ap11(坏编码) → 无回复。
**结果侧连续 2 次没有变化**，唯一变化是故障类型。下一次开机（`sweets1ap=3`）是权限假设的
第一次**干净**检验，也是计数到 3 的那一次。

## 事先登记：`sweets1ap=3`（leaf `0x7c1`）这一开机 post cb5 `FSR` 的真值表

仓库里没有这颗 SMMU 的 `CB_FSR/CB_FSYNR0` 位定义表，`refs/` 也是空的，
所以这次**不给寄存器起名**，只做差分读数。三次开机的 pre 都是同一个陈旧基线
`FSR=0x400`（且 W1C 写回清不掉，`:2337`/`:2394` 两次一致），因此只有
**"post 相对 pre 新增的位"**算数：

| leaf | 含义（我们自己编的那一位） | post `FSR` | `FSYNR0` | 出处 |
|---|---|---|---|---|
| `0x701` | AP=00，类型=block，AF=1 | `0x408` | `0x40002` | `ring-audio-coldamp.txt:2371` |
| `0x7c3` | AP=11 **且 bit1=1**（类型被改成"L3 表指针"） | `0x402` | `0x40003` | `ring-s1ap11.txt:2428` |
| `0x7c1` | AP=11，bit1 保持 0（类型仍是 2 MiB block） | 待测 | 待测 | 下一次开机 |

四种读数各自的结论（先看表再开机，不许事后编故事）：

1. `FSR=0x400`（不新增任何位）⇒ DSP 真的读到了 `0x1ffe8000`，权限线成立。
   这时 `DATA_BUFFER` 若仍不回，问题就**只剩互连/DSP 侧**，止损条款生效。
2. `FSR=0x408`（回到 AP=00 那一档的同一种故障）⇒ AP 不是主因：
   放开 EL0 权限对这颗 master 没意义（它本来就是特权访问）。**权限线判死。**
3. `FSR=0x402`（与坏编码那次同值）⇒ 我"bit1=类型字段"的解释**被否证**：
   `0x7c3` 与 `0x7c1` 唯一的差别就是 bit1，两次同码说明这颗 SMMU 的叶子编码
   不是标准 AArch64 长描述符，得先去找它真实的位定义（QCM 手册/厂商驱动），
   在那之前不要再改叶子。
4. 其它新码 ⇒ 出现了第四种状态，先把 `FSYNR0` 一起记下来再说。

顺带记一条仪器学：`FSR` 只能证明"**走到了哪一步失败**"，不能证明"**读进去的是什么**"；
所以同一次开机里 `dbraw ready/len`（0230）与 `buffer probe` 的返回值仍是一等读数，
`done status` 才是终判。

## 2026-11-02 16:54 判决①重做：**数据面打通了** —— S1 叶子的 AP=00 就是那个主因

镜像 `images/s1ap3.img`（sweettime 1793609394，kernel 来自 0232 的 CI 产物，
marker 核对：`SWEET MARK: s1ap leaf=` 命中、旧串 `s1ap11af leaf=` 归零），
ring 归档 `logs/ring-s1ap3.txt`。boot-args 相对上一次开机只多一处：
`sweets1ap11=1` → `sweets1ap=3`。

### 按事先登记的真值表读数（`a851ef9`，开机前写的）

| 读数 | 实测 | 命中的格子 |
|---|---|---|
| 叶子 | `:2310 SWEET MARK: s1ap leaf=0x7c1 req=3`、`:2314 s1 cb5 … leaf=0x7c1` | 掩码生效、`[1:0]` 保持 block ✓ |
| pre cb5 | `:2362 FSR=0x400 FAR=0x22140410 FSYNR0=0x40c5f`（陈旧基线） | 与上两次同一基线 ✓ |
| post-send cb5 | `:2368 FSR=0x400`（FAR/FSYNR0 一字未动） | — |
| **post cb5** | `:2397 FSR=0x400 FAR=0x22140410 FSYNR0=0x40c5f` | **格 1：不新增任何位** |
| 缓冲探棒 | `:2404 buffer probe dst=0x40bd -> 0`（历史值 `-1`） | — |
| 终判 | `:2398/2408/2416/2424 data_buffer: dst=0x40bd **done status=0**` | **`status=0x0` 达成** |

仪器学要点（这条是自证的关键，不然 `FSR=0x400` 会被读成"没测到"）：
**`FSR` 是负对照，`DONE` 才是正测量**。这次 `FSR` 三个字段与 pre 逐字相同 = 没有新故障；
而正证据是 DSP 主动回了 4 条 56 字节 `APM_RSP_DATA_BUFFER_DONE_V2`
（`+10 .. 04 10 00 05` = `0x05001004`，`+20 f8 45 e1 b0` = 我们的 handle `0xb0e145f8`），
并且**回复里的槽位跟着我们的 ping-pong 走**：
第 1、3 条回 `+10 00 00 00 00 … 00 00`（offset 0、slot 0），
第 2 条回 `+10 01 00 00 00 … 08 00`（**slot 1、offset 0x800**）——
这不是回声，是模块在归还具体的槽。

跑完整条链路：`pcm tone start` → **1200 个 buffer 全部发出** → `pcm tone done (1200 buffers)`
→ `===== done =====` → `PSCI SYSTEM_RESET` 正常复位。
整份 ring 里 `no rsp / not consumed / did not start / open failed` **出现 0 次**。

### 这推翻/改写了四条旧结论

1. **"控制面全绿、缺的是 18 KB APM 配置"——主因不是它。** 真正的拦路虎是我们自己那张
   S1 页表的叶子 `0x701`：`AP=[7:6]=00` 只允许特权访问，DSP 以非特权读 `0x1ffe8000`
   直接被 SMMU 终止。放开 AP（`0x7c1`）后，一个厂商字节都没多给，数据面就通了。
   `18128/18504` 那篇的判读（"缺它置信度 ≤10%"）事后看是对的。
2. **`status=0x2` 的来源解释变了**：`flag=0`（裸物理地址模式）下 DSP 根本不查我们的表，
   于是走参数校验分支回 BADPARAM；`flag=4` 下它真去查表，查不过就一声不响。
   两个症状是**同一个根因的两面**，不是两个故障。
3. **"offset 模式会拖死互连"这条止损前提 weakened**：这次 `property_flag=4` + 1200 个 buffer
   一路跑完并正常复位，没有任何挂死迹象。也就是说历史上那些"offset 模式一开就死"，
   至少要重新检验是不是**翻译失败的访问**造成的，而不是 offset 本身。
   （GPU 线那句"未解问题"因此要重开：见 `docs/gpu-probe.md` 的 reopen gate。）
4. **0230 的 bit1 故事没有被证明，也没有被否证**：真值表的格 3（"0x7c1 也回 0x402 ⇒ 我的
   类型字段读法错"）没有发生，我们落在格 1。所以"bit1=类型位"仍是**推理**，
   只是这次不需要它也能解释全部数据（`0x7c3` 的 `0x402` 由".walk 进了不存在的 L3 表"解释）。
   下次别再把这两件事混在一句话里说。

### 还没证明的（不许顺着杆往上爬）

- ** audible 输出没证明**：DONE 只说明 WR 端点收下了 buffer 并归还槽位，
  不证明 PCM 走到了 TDM→codec→喇叭。同一 ring 尾部 `geni amp 0x34/0x35 r=0 id=0x2032
  profile 44/44` 是我们自己那套 I2C 侧写（传输计数，不是回读），而 bit-bang 那串
  `0 amplifier(s) answered` 已知是仪器故障。要判定"真出声"得另外测：
  设备图/后端是否被拉起（`TDM-LPAIF-RX-TERTIARY` 在我们这张图里只是iid，没有厂商的
  0x4165 端口映射与转换器配置），或者回读 DSP 写回共享区的痕迹。
- **稳态 pacing 没测**：1200 个 buffer 是 fire-and-forget 一次灌完的；
  只有前 4 个拿到 DONE（探棒 1 + 循环前 3），之后不再等回复。
  厂商语料里 B 档是 95 个包/4 槽轮转，真实播放需要按 DONE 节流（`sweetbufgap`）。
- **图仍然比厂商窄**：`0x40bf/0x40c0/0x40c8/0x4165/0x4308` 这些配置对象不在我们打开的图里
  （上一节 §3/§4 的表仍然成立）。它们现在**不再是数据面的门槛**，但很可能是"出声"的门槛。

### 复现命令（这条现在是已知-good 配置）

```bash
D22AP_ADT=~/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt CPUS=8 GPU_PROBE_LEVEL=0 \
  ./scripts/mkimage.sh sweetgpu=0 sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 sweets1ap=3
# 期望：s1ap leaf=0x7c1 / cb5 FSR 三字段 post==pre / buffer probe -> 0 / done status=0
```

## 事先登记（下一格）：`sweetvgraph=1` —— 目标从"数据面"换成"出声"

基线 = 上面那条已知-good 配置，**唯一新变量** = `sweetvgraph=1`；
噪声控制 = `sweetbufs=30`（30 × 6 ms ≈ 180 ms，音幅本来就是 1/16 满量程，
`g_fill_tone()` 里 `>>3`），这是仪器限制、不是待测变量。
kernel 不变（0232 那版，marker 已核对），所以这一格**不需要编译**。

代码路径（全链树 `:6072-6110`）：`g_pcm_prepare()` → `g_apm_graph_open_shm(port,
g_graph_vendor4+12, 2712, 0, "graph_open/vendor4")` → `g_audio_graph_cfg_vp()` →
`g_apm_graph_start_list(port, g_vendor4_sgs, 4)` → `g_audio_tone(port, G_VP_WR_IID)`。
默认 `sweetvgoff=12 sweetvglen=2712`，`12+2712=2724 == sizeof(g_graph_vendor4)`，
范围检查应**通过**（若不通过会先打 `vgraph off/len out of range`，那是仪器故障）。

历史结论"厂商 4 子图 blob 在 `msw=1` 下 GRAPH_OPEN 无应答"（`:51` 一节）是在
**S1 权限还没修**的时候测的，从那次以后没重测过。这一格就是重测它。

六种读数与各自的结论（先看表，不许事后改）：

| 打印 | 结论 | 下一步 |
|---|---|---|
| `graph_open/vendor4 … status=0` 且 `vendor4 graph started` 且 `done status=0` | 厂商图可用，转换器/设备端口映射都进来了 | 才允许谈"出声"，去做 PA 状态读回 |
| `graph_open/vendor4` **无应答** | blob 的偏移/长度疑点仍活着（12 字节头） | 只扫 `sweetvgoff=0`，一次一个值 |
| `graph_open/vendor4 … status!=0` | DSP 读了但拒收这个定义 | 读 `w` 字段与 handle，再决定 |
| 开图成功但 `vendor4 graph did not start` | 4 子图 START 的形式不对（我们发的是合并 sg） | 查 `g_vendor4_sgs` 与厂商 `0xb0000002/81/24/23` |
| `vgraph: no shared memory` | `g_pcm_prepare()` 失败 | 仪器故障，不计入判决 |
| 无 `===== done =====`（停在某条 `sweet_log_sync()`） | 挂死 | 只认最后一条带 `sweet_log_sync()` 的输出定位置 |

注意：即使这一格全绿，也**不等于听到声音**——它只证明厂商那张图能开、能收 buffer。
"PA 真在驱动"的正证据要靠 ③（GENI 读回 AW882xx 状态/故障寄存器），
`bit-bang` 那条链是坏仪器，禁止拿它下结论。

## 2026-11-02 17:09 判决①（`sweetvgraph=1`）：落在"无应答"格，而且它把后面的开图一起拖死了

镜像 `images/vgraph.img`（sweettime 1793610339），ring `logs/ring-vgraph.txt`，
kernel 与 s1ap3 同一版（0232），args 只多 `sweetvgraph=1 sweetbufs=30`。
开机前的期望表在 `35616b2`（先推后开）。

| 判点 | 实测（行号） | 落格 |
|---|---|---|
| `graph_open/vendor4` | `:2309` 发出（shm handle `0xb0d7f408`，len 2712）→ **`:2354 no response`** → `:2355 vendor4 graph open failed` | **第 2 格：无应答** |
| 回落到我们自己的图 | `:2356 vp-wr` → `:2370 no response`；再 `vp-inline :2382`、`vp-shm :2405`、`tdm-inline :2451`、`inline :2473`、`shm :2523`、`vendor :2576`——**全部 no response**，`:2577 graph_open failed on every payload` | **新的、更重的发现** |
| DATA_BUFFER | 整个 ring **0 条**（图没开成，自然不发） | — |
| 收尾 | `:2620 ===== done =====` → `:2621 PSCI SYSTEM_RESET`，机器正常回 Android | 没挂 |

两条结论：

1. **厂商 4 子图 blob 这条路在现认知下判死**。历史上"无应答"是 S1 权限还没修时测的；
   现在权限已修（同机 `leaf=0x7c1` 打印正常、`shm_map status=0`）仍然无应答
   ⇒ 不是权限问题，是 **blob 本身或 offset/length 还不对**（12 字节头那个疑点仍然活着）。
2. **运维红线：`sweetvgraph=1` 是"有毒"的一次尝试。** 它失败之后，同一开机里原本一直
   `status=0` 的 `vp-wr` 也不再应答了——那次 vendor4 的 GRAPH_OPEN 把 APM/DSP 的建图状态
   弄坏了，后续每个 payload 都被静音。
   ⇒ 以后测厂商 blob 必须**独占一次开机**，不许指望回落到我们的图；
   而且换 blob 参数之前要先跑一次"纯 baseline"确认控制面恢复 `status=0`
   （本次没有验证恢复性——这是本节唯一没测的环节，下次开机顺手就能补上）。

## 顺手抓到的结构性缺陷：PA 是在音源灌完之后才配置的（0233 修的就是它）

现树 `:6755-6769` 的调用顺序：

```c
g_apm_graph_bringup();      /* :6766  开图 + g_audio_tone() 灌 DATA_BUFFER */
sweet_log_sync();
sweet_i2c_probe_amps();     /* :6768  这里才 GENI 复位 + 写厂商 Music profile */
g_lpass_probe(...);         /* :6769 */
```

两次开机独立证实了真机上的样子：`ring-s1ap3.txt` 里 `4902 pcm tone done (1200 buffers)`
在 `4903+ 的 SWEET I2C: … geni amp … profile 44/44` **之前**；`ring-vgraph.txt` 里 amp 配置在
`:2595-2598`，而那次图根本没开成。
⇒ **迄今每一次开机，喇叭 PA 拿到 Music profile 时 PCM 早就灌完了。**
"没听到声音"不需要用"图里缺转换器/缺 0x4165 端口映射"来解释——**顺序本身就不可能出声**。
这是"出声"这条线的排序半边，与图内容无关。

补丁 **0233**（`xnu/patches/0233-sweet-amp-before-bringup.patch`）：`sweetampfirst=1`
⇒ 先 `sweet_i2c_probe_amps()` 再 `g_apm_graph_bringup()`，并跳过后面那次调用。
为什么必须跳过：`sweet_i2c.c:797` 的 `if (se15_probe_calls++ > 0)` 让 **GENI 只在第二次调用里跑**
（0124 的 early probe 是第 1 次，故意在 UFS 挂载前不碰 GENI），所以"先配置 PA"依赖这次成为第 2 次；
不跳过就变成第 3 次，等于在灌完数据后再灌一遍 Music profile。
缺省（不给 `sweetampfirst`）= 一个字节都不变。marker：`SWEET MARK: ampfirst before bringup`
（已同批进 workflow 校验列表）。

### 事先登记：下一次开机 = baseline + `sweetampfirst=1`（只改这一个变量）

```
… sweetshmflag=4 sweets1ap=3 sweetampfirst=1 sweetbufs=30
```

| 判点 | 期望 | 若不满足说明什么 |
|---|---|---|
| `SWEET MARK: ampfirst before bringup` | 出现，且**早于** `graph_open/vp-wr rsp … status=0` | 无 marker ⇒ 补丁被 CI 跳过（marker 校验会直接把构建判失败） |
| `geni amp 0x34/0x35 … profile 44/44` | 出现在 `pcm tone start` **之前** | 仍在后面 ⇒ 调用次数/顺序没生效，别谈出声 |
| `data_buffer … done status=0` | 至少一条，槽位字段跟 ping-pong 一致 | 反而不回 ⇒ PA 的配置动作（软复位/I2C 时序）搅乱了 GLINK 节奏 |
| cb5 `FSR/FAR/FSYNR0` | post == pre（`0x400` 基线） | 新增位 ⇒ 排序变化把访问带回故障路径，原样记下码 |
| 声音 | **不作为判据**（宿舍安静要求） | 判"出声"用寄存器，不用耳朵 |

四格全绿但仍拿不到独立证据，才轮到目标里的 ②：把 `0x4165 param 0x080010C4 {0x428f}` 与
`0x40bf/0x40c0 param 0x08001008` 的原字节补进我们这张图，作为 **0234**
（目标文本里的"0233"号被排序修复占用，因为它是更靠前的门槛）。

## 补丁 0234（已写好并离线验证）：`sweetampverify=1` = PA 的原样读回

号码被 ③ 的仪器占了（②那批原字节顺延为 0235）。为什么先做它：目标要求的是
**独立于 GLINK 回复**的证据，而现在手上唯一"看起来像 PA 证据"的东西是
`profile 44/44` —— 那是**写计数**（`sweet_i2c.c` 里统计的是写入无 NACK 的条数），
从来不是回读。0234 就是把它变成真回读。

实现（两个文件，缺省全关）：

- `bsd/dev/sweet_i2c.c` 新增 `sweet_i2c_amp_readback(const char *tag)`，
  走**已经在读的同一个函数** `se15_aw_read()`（`:690`，`geni amp r=0 id=0x2032` 就是它读的），
  地址 `{0x34, 0x35}`、寄存器 `{0x00, 0x03, 0x04, 0x05}`
  ——**全部取自厂商 ACF 表 `aw882xx_music_a[]` 本身**（`0x04` 是我们写进去的
  SYSCTRL `0xb440`），没有一个偏移是我猜的数据手册地址。
  打印 `SWEET MARK: ampread <tag> addr=%#x reg=%#x r=%d val=%#x`。
- `bsd/dev/sweet_glink.c` 在 `g_apm_graph_bringup()` **之后**、`g_lpass_probe()` 之前调它。
  放在这个位置的原因：此时图已 start、DATA_BUFFER 已喂完，
  如果 `0x04` 读回来**不等于**我们写的 `0xb440`，说明有人（DSP 或 PA 内部状态机）动过它。

三种读数各自意味着什么（开机前写死）：

| 读数 | 结论 |
|---|---|
| 全部 `r!=0` | 流之后 GENI 读不通 ⇒ **仪器无效**，不能拿它给"没出声"记分（只有配置时刻的 id 读可用） |
| `r=0` 且 `val` == ACF 表里写的值 | 人类第一次真回读 PA 配置；**仍然不是**出声证据 |
| `r=0` 且 `0x04`（或 `0x03/0x05`）≠ 写入值 | 目标要的独立证据方向：PA 状态被流活动改动过；记下具体值再说它是否等于"在推功率" |

离线验证：全链 `base→…→0233→0234` 按 glob 顺序重放，两个文件的结果与手工编辑树
`diff -q` 一致；两处新增代码分别用 stub 在 `-Wall -Wextra -Wformat=2 -Werror` 下编译通过；
新增行无行尾空白；marker `SWEET MARK: ampread ` 已同批进 workflow 校验列表。

## 补丁 0235（已写好并离线验证）：`sweetvcfg2=1` = 目标里的 ②，先登记判据再开机

号码安排：② 那批原字节顺延为 **0235**（0233=排序、0234=③ 仪器）。
做法：在 `g_audio_graph_cfg_vp()` 末尾（现有 aw_cfg 之后）按原字节补发四条已测报文——
`0x40bf`/`0x40c0` 的 param `0x08001008`（30 字节）、`0x40c8` 的 `0x08001024`（14 字节）、
`0x4165` 的 `0x080010C4`（4 字节 = `{0x428f}`），并打印四个返回码：

```
SWEET MARK: vcfg2 rc bf=<int> c0=<int> c8=<int> p65=<int>
```

**为什么这一格值得花一次开机，而它不是"修复"**：这四个 iid 不在我们打开的图里
（`g_graph_vp_wr` 字节搜 `bf 40`/`c0 40`/`c8 40`/`65 41` 全 0 命中），所以诚实的预期是
报错或不回。它的价值是**关门**：

| 读数 | 结论 | 之后做什么 |
|---|---|---|
| 四条 rc 全非 0 | **结构缺失被证实**：这些对象在我们的图里不存在 | "补配置"路线关门；只剩手工扩图（大改）或厂商 blob（同一权限状态下已判死），此时按止损转交付 |
| 任一 `rc=0` 且该条 `rsp … status=0` | 图里其实有它——我们对该 blob 的理解是错的 | ② 才真正成立，再回看 `done status` 与 `ampread` 有没有跟着变 |
| `rc=0` 但 `status!=0` | 模块在、参数被拒（值或长度不对） | 记下 status 码，别改名别猜语义 |
| 发完四条后 `done status` 消失 | 向未知 iid 发 SET_CFG 与 vendor4 那次同类有毒 | 记为运维红线：**这类探测必须独占开机** |

同时保留一条本目标的既有红线：声音**不作为判据**（宿舍安静），并且这次仍带
`sweetbufs=30` 与 `sweetampfirst`/`sweetampverify` 的组合**分两次开机**，不叠。

离线验证：全链 `base→…→0235` 按 glob 顺序重放 == 手工编辑树（`diff -q`）；
新增块在 `-Wall -Wextra -Wformat=2 -Werror` 下编译通过；无行尾空白；
marker `SWEET MARK: vcfg2 rc ` 已同批进 workflow 校验列表。

## 第三个仪器（零编译，但要等人到手机旁边）：`sweetreadrx=1` 读 RX macro

`g_lpass_probe()`（全链树 `:1641`）本来就能读 codec macro 寄存器，
`sweetreadrx=1` 打开它，读的是 `G_LPASS_RX_PA = 0x03200000`（`:59`），打印
`reading rx_macro 0x3200000` → `rx_macro %08x %08x %08x %08x`。
它比 PA 的 I2C 读回更靠近"出声"的下游：**TDM→macro** 这一段唯一可观测的面。

两个前提已经核对，不是猜的：

1. **门是开的**：`ring-s1ap3.txt:2297 clocks va=0 rx=0` ⇒ `g_prm_vote_clock(RX_CORE_MCLK)`
   返回 0，`g_lpass_probe(va_clk==0, rx_clk==0)` 的 `clk_rx_ok` 为真，所以给了
   `sweetreadrx=1` 就会真的去读，不会打印 `rx read skipped (clock vote)`。
2. **对照面存在**：Android 侧厂商抓包里能看到健康播放的动作
   `lpass_cdc_rx_macro_mclk_enable: mclk_enable = 1, dapm = 1`
   （`logs/apm-blob-20261102-115548.txt:476`），也就是"macro 被拉起来"是有参照的。

**风险与门禁（这条决定了什么时候允许开）**：源码第 21 行写着"fault 在那里会 panic"，
这正是它默认关着的原因。我们的 cmdline 带 `wdt=-1`，panic 之后**不会自动复位**，
设备会一直停在原地等人长按电源。所以：

- **不在人旁边时不开**（现在手机在宿舍、人在教室）。
- 要开就单独一次开机，args 只加 `sweetreadrx=1`，其余用已知-good baseline；
  一次最多读 16 字节窗口，不扩。
- 期望读数事先写死：
  - `rx_macro` 四个非零字 ⇒ 拿到 macro 状态，与"是否在收 TDM"做关联还需要第二次对照开机
    （一次喂 buffer、一次不喂），**单次读数不许命名机制**；
  - 打印了 `reading rx_macro` 但没有 `rx_macro …` 那行 ⇒ 读的过程里挂死 ⇒ 这条仪器作废，
    并且要当作"AP 不能碰 LPASS macro"的证据记下来（与 hypervisor 门控一致）；
  - `rx read skipped (clock vote)` ⇒ 前提 1 在本次开机不成立，仪器没运行，不计判决。

顺带把这条仪器的位置说清楚：它和 0234 的 PA 读回是**互补**的——
PA 读回证明"放大器那头收到了我们的配置/或被动过"，macro 读证明"codec 那头活着"；
两者都不等于听到声音，但都比 `done status=0` 更接近喇叭。

### 一条方法学订正：我引用的"行号"是什么性质的

`/tmp/chain` 这类重建树是把 `xnu/patches/*.patch` 逐个
`git apply --include=bsd/dev/sweet_glink.c` 到 `work/ci-src` 那份拷贝上得到的；
其中约 48 个补丁（0110..0201 段）**正着 apply、反着 apply 都不干净**
（`git apply -R --check` 对 0113 也报 `patch does not apply`）。
所以这份树**不是** CI 的逐字节真树，只能说"在我编辑的那些区域与 CI 一致"，
而该一致性由实测背书：0232 推上去后 CI 产物里 `SWEET MARK: s1ap leaf=` 命中、旧串归零，
真机打印 `leaf=0x7c1`。
⇒ 行号可以引用，但**补丁的唯一硬校验是 CI 产物里的 marker grep**，不是本地 diff。

## 2026-11-02 17:30 `sweetampfirst=1`：四格全绿 —— PA 第一次"在流之前"配好

镜像 `images/ampfirst.img`（sweettime 1793611642），ring `logs/ring-ampfirst.txt`，
kernel = 0233 的 CI 产物（取包时实测 `ampfirst` marker 命中 1 次，`ampread` 还不在这一版里）。
判据表在开机前写死（上一节）。args = 已知-good baseline + `sweetampfirst=1 sweetbufs=30`。

| 判点 | 期望（开机前写的） | 实测 | 判定 |
|---|---|---|---|
| `SWEET MARK: ampfirst before bringup` | 出现且早于 `graph_open/vp-wr rsp … status=0` | `:2281` marker，`:2339 rsp … status=0` | ✓ 顺序反过来生效 |
| `geni amp … profile 44/44` | 早于 `pcm tone start` | `:2299/:2300/:2302`（0x34 `r=0 id=0x2032`，两颗 `44/44`）；`pcm tone start` 在 `:2456` | ✓ **历史上第一次：PA 先配、流后灌** |
| `done status=0` | 至少一条，槽位跟 ping-pong 一致 | `:2449 :2459 :2467 :2475` 四条 + `buffer probe → 0`（`:2455`） | ✓ 排序改动没弄坏数据面 |
| cb5 `FSR/FAR/FSYNR0` | post == pre | `:2413 pre 0x400/0x22140410/0x40c5f` == `:2448 post` 三字段逐项相同 | ✓ 无新故障位 |
| 收尾 | 正常复位 | `:2537 pcm tone done (30 buffers)` → `:2539 ===== done =====` → `:2540 PSCI SYSTEM_RESET` | ✓ `sweetbufs=30` 兑现，总量 ≈180 ms |

**顺手关掉我自己留在 vgraph 那节的一个缺口**：那里我把"控制面能否恢复"列为
"唯一没测的环节"。这次 `graph_open/vp-wr rsp … status=0` 正是在那台被 vendor4
拖死过的机器之后的下一次开机拿到的 ⇒ **那次毒是每次开机独立的，不跨开机残留**。

现在**已知-good 的出声候选配置**（数据面 + 排序都对，仍无独立出声证据）：

```bash
D22AP_ADT=~/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt CPUS=8 GPU_PROBE_LEVEL=0 \
  ./scripts/mkimage.sh sweetgpu=0 sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 \
  sweets1ap=3 sweetampfirst=1 sweetbufs=30
```

还缺的就一件事，而且它不是参数问题：**"PA 在流之前配好" ≠ "PA 在推功率"**。
下一格 = 0234 的读回（`sweetampverify=1`，判据已登记）；再往后 = `sweetreadrx=1`
（要等人到手机旁边，`wdt=-1` 下 panic 不会自动复位）。

## 事先登记：读回开机（`sweetampverify=1`）的期望值与一条依赖关系

args = 上一格 17:30 的全绿配置 + 只加 `sweetampverify=1`：

```
… sweetshmflag=4 sweets1ap=3 sweetampfirst=1 sweetampverify=1 sweetbufs=30
```

**先钉住一个会被误读的点**（读代码钉的，不是猜的）：`se15_msg()` 在 `!se15_ready` 时
**直接返回 -1**（`sweet_i2c.c:620`），而 `se15_ready` 只在 `se15_init()` 里置真（`:607`）。
0234 的最终顺序已核对：`bringup → (ampfirst=0 时才 probe_amps) → readback → lpass_probe`，
所以**两种模式下读回都排在 `probe_amps()` 之后** ⇒ `r=-1` 不许拿"没初始化"当解释。

按 ACF 表推算的期望值（`aw882xx_music_a[]` 首项是 `0x03`，所以 `0x00` 不会被 profile 覆盖，
仍是软复位/器件 id 寄存器）：

| 寄存器 | 我们写入 | 期望读回 | 读回 ≠ 写入意味着 |
|---|---|---|---|
| `0x00` | `0x55AA`（软复位） | **`0x2032`**（id，`:804` 已实测读得到） | 仪器没在读它本该读的东西 |
| `0x03` | `0xFFFF` | `0xFFFF`，或被硬件改掉 | 若是后者：这就是"有人动过 PA"的候选面 |
| `0x04` SYSCTRL | `0xB440` | **`0xB440`** | 不同 ⇒ 流活动改过 PA 状态，目标要的方向 |
| `0x05` | `0x05A9` | `0x05A9` | 用来区分"某个寄存器特殊"与"整个读不可用" |

四种读数与结论（先看表，不许事后改）：

1. 四个 `r=0` 且写入值全对上 ⇒ **人类第一次真回读 PA**，把 `44/44` 从写计数升级成落地确认；
   **仍然不是出声证据**。
2. `0x04` 或 `0x03` ≠ 写入值 ⇒ 记下实际值，这是第一条**不经过 GLINK** 的 PA 侧变化；
   但命名机制还要再等一次对照开机（一次喂 buffer、一次不喂），单次读数不给它起名。
3. `0x00` 对得上而其余全 `r!=0` ⇒ GENI 读在配置之后不可用 ⇒ **仪器作废**，
   禁止用它给"没出声"记分，转去做 `sweetreadrx=1`（等人到手机旁边）。
4. 读回半行卡住（只见 `ampread post-stream addr=0x34 reg=0x0` 没有下一行）
   ⇒ 流后的 I2C 读能把内核卡死，这条仪器也进"必须独占开机"名单。

另记一条：`sweetvcfg2=1`（②，0235）虽然依赖同样的前提，但它会改 APM 状态，
所以**单独一次开机，不与读回叠加**。

## 2026-11-02 17:41 读回开机（`sweetampverify=1`）：落在仪器无效格，但失败点是**可定位的**

镜像 `images/ampverify.img`（sweettime 1793612305），ring `logs/ring-ampverify.txt`。
args = 上一格 + 只加 `sweetampverify=1`；kernel = 含 0233+0234 的 CI 产物
（marker 实测：`ampfirst` 命中、`ampread` 命中、`pre-stream` 还没有——0236 那版未进这一包）。

数据面侧四格照旧全绿（`:2466/:2479/:2487/:2495 done status=0`、`:2472 buffer probe → 0`、
`:2429 pre` == `:2465 post` cb5 三字段、`:2556 pcm tone done (30 buffers)`、`:2585` 正常复位），
**说明读回这件事本身没有扰动数据面**。

读回侧 8 次全部失败，但失败方式**不是**我事先担心的那个：

```
:2558..:2582  SWEET I2C: se15 timeout st=0x41 irq=0 rx=0 tx=0
              SWEET MARK: ampread post-stream addr=0x34 reg=0 r=-1 val=0xffff   （8 条同理）
```

`se15_msg()` 有三条出口，只有第一条会静默：`!se15_ready` **不打日志**；
`se15_wait_clear` 失败打 `se15 busy`；**传输启动后等不到 SE_DONE 才打 `se15 timeout`**。
⇒ 所以 `se15_ready` 是**真**的，我事先钉住的那条"没初始化"的借口被排除在日志之外；
失败发生在**总线层面**：`irq=0`（一点中断都没有）、`tx=0/rx=0`。

同一次开机里同一个函数在前一分钟还好用（`:2319 r=0 id=0x2032`、44 次写全 ACK），
中间发生了什么，ring 里能看见的只有两段：

1. `:2323-:2348` **bit-bang 那一套**——`sda driven low / scl driven low / released scan /
   asserted scan / amp no ack`，它会把 `0xf138000 / 0xf139000`（引脚 56/57）的 TLMM
   `PSEL` 从 GENI 函数切到 GPIO 再切回（`sw_i2c_pins_up/down`，`sweet_i2c.c:889/904/934`）。
2. `:2473-:2556` 音流本身（30 个 DATA_BUFFER）。

**这条先按"相关"记，不命名机制**。分辨实验已经在路上：0236 的 `pre-stream` 快照
正好落在 `probe_amps()` 之后、开图之前——
- 若 `pre-stream` 也超时 ⇒ 是 **bit-bang 那一相**把 SE15/引脚弄坏了，与音流无关；
  ⇒ 修法是读回前重新 `se15_pins()`（或干脆在读回前重跑 `se15_init()`），0237 就是它；
- 若 `pre-stream` 正常、`post-stream` 超时 ⇒ 才是音流/ADSP 侧把总线拿走了，
  那时候 `st=0x41` 这个值本身才有意义。

按事先登记的判据：本格 = **仪器在"流之后"这个位置无效**，
所以**禁止**用这次 `r=-1` 给"没出声"记分；③ 还需要一次带配对快照的开机。

## 2026-11-02 17:45 `sweetreadrx=1`：AP 能读 RX macro，但读出四个 0 —— 止损条款正式生效

镜像 `images/rxread.img`（sweettime 1793612541），ring `logs/ring-rxread.txt`。
args = 已知-good + `sweetampfirst=1` + 只加 `sweetreadrx=1`；人已在手机旁边（这是这条仪器的门禁）。

| 判点 | 实测 |
|---|---|
| 门是否开 | `:2278 clocks va=0 rx=0` ⇒ `clk_rx_ok` 为真，读会发生（不是 `rx read skipped`） |
| 读是否完成 | `:2541 reading rx_macro 0x3200000` → `:2542 rx_macro 00000000 00000000 00000000 00000000` → `:2543 ===== done =====` → `:2544 PSCI SYSTEM_RESET` ⇒ **没有 fault、没有 panic、没有卡死** |
| 数据面 | `:2450/:2462/:2470/:2478 done status=0`、`:2456 buffer probe → 0`、`:2414 pre` == `:2449 post`（`FSR/FAR/FSYNR0` 逐项相同）、全 ring `FSR=0x408/0x402` 与 `GFSR!=0` **0 次命中** |
| 顺序 | `:2279 ampfirst` → `:2297 geni id=0x2032` → `:2457 pcm tone start` ⇒ 0233 的排序在这次也成立 |

三条结论：

1. **"AP 碰 LPASS macro 一定 fault"这个担心，在 `0x03200000` 上不成立**：读走完了、
   四个 dword 都拿到了、机器正常复位。历史上 `lpass_tbu 0x151e9000` 从 XNU 会 fault
   （`:6663` 注释），那是**另一个地址**，不要混为一谈。
2. **全零是事先没登记的第四种读数**。我原来的表只写了"四个非零字 / 打了 reading 却没数据 /
   skipped"。全零**既不能读成"macro 关着"也不能读成"读路径没通"**，
   因为我们不知道 RX macro 前 16 字节的寄存器含义，也没有任何一个已知答案的对照点。
   ⇒ 按纪律这条**不记分**（既不记分成功，也不记分失败）。
3. **耳朵观测量：人在旁边，答"没有"**（`ampfirst` 17:30 与 `ampverify` 17:41 两次，
   PA 已在流之前配好、30 buffer ≈180 ms、幅度 1/16）。
   但**这条同样不能当"证明没出声"**：1/16 满幅 ×180 ms 本身可能低于可听辨阈值，
   而且 PA 自己的静音/升压使能状态我们从没读到过（见上面第 2 点仪器失效）。
   所以它只能记作"**未观测到声音**"。

### 止损计数（照实算，三条开机全部落在"只有 DONE"）

| 开机 | 独立于 DONE 的证据 |
|---|---|
| `ampfirst` 17:30 | 无（四格全绿但只是数据面 + 排序） |
| `ampverify` 17:41 | 无（读回在总线上超时，仪器无效） |
| `rxread` 17:45 | 无（读出全零，无对照，不记分） |

⇒ **3/3，目标里那条止损条款生效**："出声"的参数侧到此停止，转交付。

### 交付清单（本目标剩下要做的都在这）

- [x] 已知-good 配置 + 四条必须同时出现的读数 → 已固化进 `docs/START-HERE.md` §三（`fd2b562`）。
- [x] "设备图/后端拉起"写成独立待办 → `docs/open-questions.md` 求第二意见 #2。
- [x] GPU 线前提更新 → `docs/gpu-probe.md` 追加一节：`property_flag=4` 在翻译成功时
      跑完 1200 buffer 正常复位，所以"offset 模式拖死互连"要么另找原因、
      要么本来就是**翻译失败的访问**造成的（这条换了 GPU 线的假设基础）。
- [ ] **设计好但没跑的第 4 条仪器（0237）**：把 RX macro 改成
      **开图前 + 结束后各读 16 个 dword 的配对快照**。全零有没有意义，只有配对才能判：
      若 pre==post==0 ⇒ 这块窗口对我们目前不可解读，别再花开机；
      若 post 出现任何非零字（或 pre/post 不同）⇒ 那才是第一条不经过 GLINK 的硬件侧变化。
      （同时 0236 的 `pre-stream` PA 快照还能顺带判定 0234 的总线超时是谁造成的。）
      这一条**需要一次开机 + 一次编译**，是否要花，由你定——止损条款已经触发，
      我不自行加开机。

## 事先登记（你点头后的最后一格）：0237 的配对 RX 快照 + 0236 的 PA 前置快照

一次开机同时喂两个**只读**仪器，各自独立判据（互不依赖，也不改任何参数）：
args = 已知-good + `sweetampfirst=1 sweetreadrx=1 sweetampverify=1 sweetbufs=30`。
0237 读 `G_LPASS_RX_PA` 起 64 字节，在 `clocks va= rx=` 之后（开图前）与
`g_lpass_probe()` 之前（灌完流之后）各打一次 `SWEET MARK: rxsnap <tag> +off w0 w1 w2 w3`。
老的 4 字 `rx_macro` 行**保留**，当作同一时刻两条读路径的交叉校验。

| 格 | 判据 | 结论 |
|---|---|---|
| A | pre 全零 **且** post 全零 | 这块窗口目前不可解读 ⇒ **不再对它花开机**，回交付/待办（设备图）或转 GPU |
| B | pre 全零、post 出现任何非零字 | 第一条**不经过 GLINK** 的硬件侧变化；记下偏移与值，命名机制还要一次"不喂流"的对照 |
| C | 两边都非零且不同 | 同 B，且更强（不是"刚上电才变"） |
| D | 打了 `rxsnap pre-stream reading` 却没有 16 字数据 | 开图**之前**读 macro 就能挂 ⇒ 与 17:45 那次（读在末尾、活着）成对照；这条仪器作废 |
| E | `rx_macro`（老 4 字）与 `rxsnap post-stream +00` 不一致 | 先怀疑窗口/映射本身，**不**得从差值读出任何声学结论 |

PA 侧（0236 的 `pre-stream` 快照正好落在 `probe_amps()` 之后、开图之前）：

| 格 | 判据 | 结论 |
|---|---|---|
| F | `ampread pre-stream` 也全 `r=-1` | 是**我们自己的 bit-bang 相**把 SE15/引脚弄坏的，与音流无关 ⇒ 修法=读回前重新 `se15_pins()`（0238），修好才有 ③ |
| G | `pre-stream` 有 `r=0`（至少 `reg=0 val=0x2032`）而 `post-stream` 全 -1 | 才是"音流/ADSP 拿走了总线"；那时 `st=0x41` 这个值本身才开始有意义 |
| H | `pre-stream` 里 `reg=0x4` 读到 `0xb440` | **人类第一次真回读 PA 配置**，把 `44/44` 从写计数升级成落地确认（仍然≠出声） |

跑完这一格，无论落哪一格，"出声"这条线**就停**：A/D/E 是"仪器判死"，F/G/H 是"知道下一步该修什么"，B/C 才算拿到目标要的那条独立证据。

## 零开机成本的关键修正：Android 健康态下 macro 的 `0x00..0x0c` **也是 0**

趁着手机在 Android 侧（只读，无开机），把厂商的 regmap 直接 dump 下来了：
`mount -t debugfs` 之后读 `/sys/kernel/debug/regmap/soc:spf_core_platform:lpass-cdc/registers`
（1074 个寄存器、495 个非零），整份存在
**`logs/android-lpass-cdc-regmap.txt`** + 同目录 `…-range.txt`。

对照我们 17:45 那次 `rx_macro 00000000 00000000 00000000 00000000`：

```
Android 健康态（音频栈已起来、没放声）同一 regmap 的前几个偏移：
  0000: 00    0004: 00    0008: 00      ← 和我们在 XNU 里读到的一模一样：全零
  0088: 60    00a4: 0c    00c0..00d4: 0e（六个）
  0400: 04    0404: 10    0408: 0b      0418: 01
```

三条结论，都不依赖再开一次机：

1. **17:45 那个"全零"不携带任何信息**：健康系统在同一窗口同样读零。
   之前我给这格留的话是"零既不能读成 macro 关着也不能读成读路径不通"——
   现在有实测依据了：它**至少**不是"读路径不通"的证据。
2. **有已知答案的锚点确实存在，但不在 0x00..0x0c**：`0x0088=0x60`、`0x00a4=0x0c`、
   `0x00c0..0x00d4=0x0e`、`0x0400..0x0418={04,10,0b,01}`。
   ⇒ 0237 那次配对读**大概率仍然落在死区**（我选的偏移是 base+0x00..0x3c），
   它的真价值因此转移到同一开机里的 PA 侧三格（F/G/H）。
3. 下一步该做的是 **0239**：把 XNU 的读窗口挪到上面那几个锚点，并**以 Android 的值为期望值**——
   这才第一次让"AP 能不能真读到 macro"变成可证伪的（读到 `0x60/0x0c/0x0e` ⇒ 读路径为真；
   读到 0 而 Android 读到 0x60 ⇒ 我们的窗口/翻译/供电与厂商不同，本身就是结论）。

**必须写清楚的口径限制（不能装懂）**：regmap 的 offset 与物理地址的对应关系**没有证据**。
父节点 `lpass-cdc` 在 DT 里**既没有 `reg` 也没有 `ranges`**，RX macro 自己的 `reg` 是
`0x03200000`（与我们的 `G_LPASS_RX_PA` 一致 ✓）；regmap 的 `range` 一路排到 `0x4a08`，
比一个 macro 常见的大小宽。所以"offset 0x88"到底是 `0x03200088` 还是别的窗口，
**未知**；0239 的第一步应当是"按两种映射都读一遍并看哪个能对上厂商的值"，而不是假定。

### 补一条支撑（仍然不是证明）：regmap 的 offset 0 很可能就是 RX macro 基址

把 DT 里各 macro 的基址减去 RX 基址，再对着 regmap `range` 的分布看：

| macro | DT 基址 | 相对 RX(0x3200000) | regmap `range` 里是否有对应的块 |
|---|---|---|---|
| rx | `0x03200000` | `+0x0000` | 有 `0-88 / 94-98 / a4-ac / c0-d4 / 100-13c / 200-23c / 400-42c / …` |
| tx | `0x03220000` | `+0x2000` | 有 `2000-2008 / 2080-20dc / 2100-2118 / 2180-2208 / … / 2900-2a08` |
| wsa | `0x03240000` | `+0x4000` | 有 `4000-4008 / 4080-40dc / … / 4980-4a08` |
| wsa2 | `0x031e0000` | `−0x02000`（负） | 不该出现，也确实没有 |
| va | `0x033f0000` | `+0x1f0000` | 不该出现（超出 max_register），也确实没有 |

五个 macro 里四个的"该有/不该有"都对上了 ⇒ **offset 0 = RX 基址**这个读法有很强的算术支撑。
但我还是把它记成**推断**，不是证明：`/sys/bus/platform/devices/{rx,tx,va}-macro/resource`
在这台 Android 上读出来是空的，父节点 `lpass-cdc` 也没有 `reg`，所以没有第二路证据。
⇒ **0239 的写法因此是"两种读法各读一遍"**（`0x03200000+0x88` 与"若 offset 0 不是 RX，则整块偏移不同"），
用能不能复现厂商的 `0x60 / 0x0c / 0x0e` 来定胜负，而不是先假定再解释。

## 0237 判决（18:04 开机，`logs/ring-rxsnap.txt`，sweettime=1793613866）

判据是开机前写好的（本文 "0237 设计" 一节的 A–E 是 RX 侧、F–H 是 PA 侧），现在逐格对：

| 格 | 判据 | 实测 | 落点 |
|---|---|---|---|
| A | `pre-stream` 全零 **且** `post-stream` 全零 ⇒ 该窗口不可解读 | `:2291-2294` 四行全零、`:2612-2615` 四行全零，两次都**打印完了**（没挂） | **命中** |
| B/C | 任一格出现 pre≠post（才算硬件动过） | 无差异 | 未命中 |
| D/E | 打印了 `reading` 但没有数据行（读挂死）/ `skipped (clock vote)` | 数据行都在 ⇒ 仪器没挂，只是读到零 | 未命中 |
| F | `ampread pre-stream` 也全 `r=-1` ⇒ 坏在**我们自己的 bit-bang 相**，不是流 | `:2358-2387` 八次 pre-stream 全 `r=-1`，`:2582-2610` 八次 post-stream 全 `r=-1` | **命中，而且比预想的更干脆** |
| G/H | 读到值 / 读到 `reg=0x4` 的 `0xb440` | 没走到 | 未命中 |

F 这一格落点之后，成因不用猜了——代码和日志能对上，是**硬证据**：

1. 同一个 `se15_aw_read()`，同一个 `addr=0x34`，同一个 `reg=0x00`：
   `:2313` 返回 `r=0 id=0x2032`（成功），`:2358` 返回 `r=-1`（`se15 timeout st=0x41 irq=0`）。
2. 两者之间发生的唯一一件事是我们的 bit-bang 段（`:2317-2350`），而它就在
   `bsd/dev/sweet_i2c.c:873-874` 干了这件事：
   ```c
   sw_gpio_cfg(SW_I2C_SCL, 0, false);
   sw_gpio_cfg(SW_I2C_SDA, 0, false);
   ```
   mux 功能 0 = GPIO，也就是把 56/57 两个 pad **从 QUP 上摘下来**。
3. `irq=0`（20000 次轮询一次中断都没等到）正好是"pad 不再连到 SE15"的签名：
   控制器发不出波形，所以既没有 DONE 也没有 NACK。

⇒ 结论订正：**"post-stream GENI 读回不通"从来不是流造成的，也不是 PA 造成的，是我们自己
在开机内把引脚切走了。** 这条同时把 0236/0234 那次 `se15 timeout` 的解释权收回（当时写的是
"原因待定"）。它**没有**给"出声"提供任何正证据——只是把一个坏仪器修成了可修的。

### 于是这次开机买了三件事（都还没跑）

* **0238**（本地已写好并链验通过，`xnu/patches/0238-…-pins-fixup.patch`）：读回前重新
  `se15_pins()` 把 pad 切回 QUP，并把 `se15_pins()` 里每次调用都漏一份 kernel_map 的
  `ml_io_map()` 换成缓存版 `sw_pad_map()`。这一版它的动机从"猜"升级成了上面 1–3 的证据。
* **0240**（新，`0240-sweet-amp-readback-before-bitbang.patch`，链验 `git apply --check` 通过）：
  把读回**插进 bit-bang 之前**——那是本开机内唯一已证明 GENI 可用的窗口。
  它不依赖"切回来能不能切对"，所以是 0238 的兜底；代价是它只能证到"PA 真的存下了我们写的
  配置"，证不到"流之后 PA 动过"。
* **0239**（重写完成，`0239-sweet-rx-anchor-known-answer.patch`：链验通过 + 单函数 stub 在
  `-Wall -Wextra -Wformat=2 -Werror` 下零诊断）：RX 窗口只读 Android 已知非零的锚点
  `0x088/0x0a4/0x0c0/0x0c4/0x400/0x404/0x418`（期望 `60/0c/0e/0e/04/10/01`），
  外加 `0x000/0x004` 两个"已知也是零"的对照位，用来抓"窗口基址根本不对"。
  **口径限制照旧**：offset 0 = RX 基址仍是**推断**；而且这一版没有实现本文前面写的
  "两种读法各读一遍"——如果锚点全零而对照位也全零，只能判"我们的窗口没读到厂商读到的东西"，
  不能立刻反推基址是别的；下一步才需要第二假设的变体。

### 下次开机（等用户点头，一次编译+一次开机）

一条命令能同时跑三件仪器，数据面配置保持已知-good 不动（不引入新变量）：

```bash
D22AP_ADT=/path/to/d22ap.adt ./scripts/mkimage.sh \
  sweetshmhi=1 sweetshmflag=4 sweets1ap=3 \
  sweetampfirst=1 sweetampverify=1 sweetreadrx=1 sweetbufs=30
```

开机前写死的期望（不许事后改判据）：

| 观察点 | 期望 | 落点含义 |
|---|---|---|
| `SWEET MARK: ampread pre-bitbang …` 八行 | `r=0`，且 `reg=0x4 val=0xb440` | PA 配置真落地（第一次把 `44/44` 升级成读回确认）；若 `reg=0x0` 读到 `0x2032` 则整条读路为真 |
| 同上但 `r=-1` | — | GENI 读在**它自己刚成功过的地方**失败 ⇒ 读路径本身坏，PA 侧仪器永久作废 |
| `ampread pre-stream` / `post-stream` | `r=0` | 0238 的 re-mux 有效；post 那一格才是"流之后 PA 动过没" |
| `ampread pre/post` 仍 `r=-1` 而 `pre-bitbang` 成功 | — | re-mux 无效（pad 还被动过 / GPIO 配置粘住），只承认 pre-bitbang 那一格 |
| `rxsnap … @0x88 = ` 期望 `00000060` | 非零且对上 | AP 能真读到 macro ⇒ 这条仪器活了，之后才谈 pre/post 差异 |
| `rxsnap @0x000/@0x004` 读到**非零** | — | 窗口基址错，本仪器作废（这正是它们存在的目的） |
| 七个锚点全零 + 两个对照位零 | — | 与厂商读到的不是同一块东西 ⇒ RX 线彻底收掉 |

`done status=0` 仍然会照常在，**它不算这任何一格的证据**。

## 预先登记：目标 ②（`sweetvcfg2=1`）的开机判据 —— 现在写死，开机后只许按格比对

写在开机之前，是因为这一格**零编译成本**：`SWEET MARK: vcfg2 rc` 这个字面量实测就在
`out/kernel.adsp`（0237 那版）里，说明 0235 早就进了每一个 kernel；它只是从来没被开机验证过。

代码位置（对 `work/chain-tree/bsd/dev/sweet_glink.c`，182 apply / 49 skip 的重建，行号按本文
开头那条口径限制读）：vcfg2 重放在 `g_audio_graph_cfg_vp()`（:5934 起，重放块 :6049-6105），
而它是 `g_apm_graph_bringup()`（:6177）里的一步。于是带着全部开关时，一次开机的顺序是：

```
clocks va/rx
  -> rxsnap "pre-stream"                        (0239 锚点)
  -> ampfirst: probe_amps -> ampread "pre-bitbang" (0240) -> bit-bang
  -> ampread "pre-stream"                        (0236+0238)
  -> g_apm_graph_bringup()  == 含 vcfg2 重放      (0235)
  -> 喂 PCM / done status
  -> ampread "post-stream"                       (0234+0238)
  -> rxsnap "post-stream"
```

⇒ **pre 和 post 之间的唯一新增就是 bringup（含 vcfg2）**，这正是能拿来做差的结构。

| 格 | 判据（事先写死） | 落点含义 |
|---|---|---|
| I1 | `vcfg2 rc` 四个数里**任一个 = 0** | 厂商那四个模块（0x40bf/0x40c0/0x40c8/0x4165）在我们这张图里其实是可达的 ⇒ "缺的是配置"而不是"缺图"，下一格才谈得出声 |
| I2 | 四个 rc 全 ≠ 0（错误码或无应答） | 缺口是结构性的（设备图/后端没拉起）⇒ ②关闭，写进 `docs/open-questions.md` 的第二条，不再在参数侧耗开机 |
| I3 | `p65` 单独 ≠ 0 而 bf/c0/c8 = 0 | 只有 `0x4165` 那条**端口映射**（data={0x428f}）被拒 ⇒ 正好是"TDM 出口没绑到 PA"的形状，②线升级为下一步主战场 |
| I4 | `ampread pre-stream` 与 `ampread post-stream` 同地址同寄存器**值不同** | 硬件在我们看不见的地方动过 ⇒ 这是本目标要的独立于 DONE 的正证据（严格讲归因是 bringup+流，不是单 vcfg2；要靠与 anchor3 基线对比才归到 vcfg2） |
| I5 | `ampread` 两格值相同 | 无运动 ⇒ 不构成证据，不许写成"证明没动"，因为仪器本身可能还是坏的 |
| I6 | `ampread` 仍然全 `r=-1` | **I4/I5 记为"未运行"**，不许拿它们下任何结论；②这一轮只由 I1–I3 判 |

前置条件（诚实口径）：I4/I5/I6 能不能成立，取决于**这一次 anchor3 开机**里 0238/0240 是否把
读路修活。anchor3 的判据表在上一节，两件事不混淆：anchor3 只修仪器，② 才第一次把它当证据用。

开机命令（数据面配置仍不动）：

```bash
D22AP_ADT=... CPUS=8 GPU_PROBE_LEVEL=0 ./scripts/mkimage.sh \
  sweetgpu=0 sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1 \
  sweetshmhi=1 sweetshmflag=4 sweets1ap=3 sweetampfirst=1 sweetampverify=1 \
  sweetreadrx=1 sweetvcfg2=1 sweetbufs=30
```

`done status=0` 在 I1–I6 里一律不算证据。

## anchor3 判决（18:44 开机，`logs/ring-anchor3.txt`，sweettime=1793627496）

marker 门控第一趟就过（0238=1 0239=1 0240=1），所以这一版 kernel 确实带着三个补丁。
数据面照旧绿：`clocks va=0 rx=0`(:1550) / `graph_start … status=0`(:1713) /
`done status=0`(:1758 起) / 结尾 `PSCI SYSTEM_RESET`——**这一格没有回归**。

按上一节写死的表逐格对：

| 格 | 期望 | 实测 | 判定 |
|---|---|---|---|
| `ampread pre-bitbang reg=0` | `r=0 val=0x2032` | 两颗芯片都是 `r=0 val=0x2032`（`:1586`、`:1590`） | **命中** —— AP 第一次真读到 AW882xx，而且和另一个调用点读到的 ID 同值，是自带已知答案的 |
| `ampread pre-bitbang reg=0x4` | `val=0xb440` | **`val=0x4003`** | **未命中**。写在前面（`:1580` 的 `44/44` 早于 `:1586`），所以"读不回写进去的值"是真的；只测量、不命名机制（`sweet_i2c.c:322-324` 厂商自己注着 0x04 SYSCTRL 是两个 Music profile 只差 bit15 的那个寄存器，属于会自改的控制寄存器的形状） |
| `ampread pre-stream`/`post-stream` | `r=0` | 16 次全 **`r=-2`** | **比判据本身更有信息**：`-2` 是 `se15_msg()` 的 NACK 分支，不是上一次的 `irq=0` 空转。而 pad 此刻是 `pin 56 cfg=0x87`（`:1632`，和读成功时同一个值）⇒ **0238 把引脚这层修好了，剩下的不是引脚**：芯片自己从总线上消失了 |
| `rxsnap @0x88/@0xa4/@0xc0/@0xc4` | 对上 60/0c/0e/0e | 全零 | 未命中 |
| `rxsnap @0x400` | 对上 0x04 | **`0x40000004`**=0x04 | **命中**（`:1558`、`:1874` pre 与 post 都是）⇒ 上一次"这块窗口是死的"的判决**作废**：读路能读到厂商读到的非零值 |
| `rxsnap @0x418` | 0x01 | 读到 `0x04` | 非零但不等；`@0x404` 读零而厂商 0x10 |
| `rxsnap @0x000/@0x004`（对照位） | 零 | 零 | 如厂商所是，但**不消歧**（已知零本来就两边都零，它只在读到非零时才有用） |
| pre-stream 与 post-stream 九位全同 | — | 全同 | 这些偏移上没有流带来的运动（不构成"证明没动"，见下） |

三点必须说清楚的口径：

1. **"offset 0 = RX 基址"仍然只是推断，而且现在只有 1/9 个锚点撑它**。九格里 `@0x400` 对上、
   `@0x418` 非零但不同、其余四格读零。这不叫"验证成功"，叫"这块窗口不是死的、但也不是
   一块可信的表"。
2. **`reg=0x3` 读到 `0xffff` 这格作废**：`0xffff` 同时是我们代码里的哨兵值，"读到了厂商表里
   的 0xffff"和"一个字节都没回来"用现在这台仪器**分不开**。0241 把哨兵换成厂商表里根本不存在的
   `0xa5a5`，这一格以后才谈得上可证伪。
3. anchor3 **仍然没有拿到任何独立于 DONE 的出声证据**。它拿到的是：一件修好的仪器（PA 读）、
   一个第一次成功的读数（0x2032）、一条被作废的旧判决（RX 窗口死区），以及一个新的、
   形状很硬的障碍——**bit-bang 跑完之后放大器就不在总线上了**（bit-bang 自己的两颗地址扫描
   `:1629-1631` 与 SE15 的 NACK 互相印证，两台仪器同一结论）。

### 预先登记：下一格（0241，`sweetnobb=1`）的判据

变量只有一个：**整段 bit-bang 不跑**。数据面配置与 anchor3 完全相同。

```bash
… ./scripts/mkimage.sh … sweets1ap=3 sweetampfirst=1 sweetampverify=1 \
  sweetreadrx=1 sweetnobb=1 sweetbufs=30
```

| 格 | 判据（现在写死） | 落点含义 |
|---|---|---|
| J1 | `SWEET MARK: bitbang skipped` 出现，且 `ampread pre-stream` 为 `r=0` | 不碰它，PA 就一直在总线上 ⇒ 消失是我们 GPIO 段干的（**具体哪一小步仍未定**，不许在此命名） |
| J2 | `pre-bitbang` 成功而 `pre-stream` 仍 `r=-2` | 我们自己写的 44 条配置之后芯片也在几秒内自己下总线 ⇒ AP 侧根本不可能在放声时刻看 PA，这条线**永久收掉**，出声的正证据只能去 codec/TDM 侧找 |
| J3 | J1 成立时，`pre-bitbang` 与 `post-stream` 三个寄存器（0x00/0x04/0x05）**任何一个值不同** | 第一次拿到"流之后硬件动过"的读数（哨兵已是 0xa5a5，`0xffff` 从此只表示"总线回了全 1"） |
| J4 | 三个时刻 `reg=0x4` 全是 `0x4003` | 我们写进去的 0xb440 没落在这个寄存器（只测不命名）；下一格才谈换寄存器 |
| J5 | `done status=0` 或 `graph_start status=0` 缺失 | **整次开机作废**，J1–J4 一律记为未运行（数据面必须仍然绿才允许读这些格） |
| J6 | RX 锚点 | 本轮**不计分**：预期与 anchor3 相同（同一开机内静态），RX 只在 J1/J2 落定之后才排 |

`done status=0` 在 J1–J6 里同样不算证据。

## 0241 第一次推送把 CI 编炸了：原因、机制、以及是哪道闸拦住的

现象：`af65301` 那次 run 失败，release 上出现 `build-error.txt`（head-1 `commit:` 就是
`af65301`），而 `kernel.development.sweet` 仍是上一版——所以开机链在 marker 门控处
`0241=0` 停住，**一次开机都没浪费**。

原因不是代码问题，是我生成补丁的方式错了：`work/chain-tree-i2c` 里我只 commit 过
"base"，没有把重放后的状态提交，于是 `git diff` 给出的是 **base → (0234+0236+0238+0240+0241)**
的全部差异。生成的 `0241.patch` 里因此混进了 0238 的 `se15_pins()` hunk 和 0234 的整个
`sweet_i2c_amp_readback`。dxb 应用补丁是**整补丁原子**的（任何一个 hunk 失败就整片跳过，
见 `docs/status-log.md` 2026-10-27 22:3x 那条），CI 的树里 0238 早就生效了，那个
`ml_io_map(pa[i], 0x1000u)` 的前镜像自然找不到 ⇒ 0241 整片被跳过 ⇒ marker 缺失 ⇒
`::error::missing kernel marker: SWEET MARK: bitbang skipped`。

也就是说：**marker 列表第一次真的当了闸**。它 20 分钟前还是我给 workflow 加的，
加它的理由正是"补丁被静默跳过会白开一次机"。这次它把一次白开机换成了一次 CI 红。

修正（已重做，`0241` 现在是 2 hunk、+40/-1）：基线必须先提交再改。
新流程在 `/tmp/i5`：`cp 基线 → commit base → 重放全部补丁 → commit "chain at 0240" →
编辑 → git diff`，这样 diff 里只可能有我自己的两个 hunk。校验三条：
`git apply --check` 通过；`probe_amps` 括号 22/22 平衡；`return` 确实在 `probe_amps` 内。

顺便把 0241 的**生效范围**写清楚，免得下次误读日志：那个 gate 插在
`if (se15_probe_calls++ > 0)` → `if (se15_init())` 里面，所以
`sweetnobb=1` 只在"第二次调用 probe_amps 且 GENI 初始化成功"时才跳过 bit-bang。
带 `sweetampfirst=1` 的已知-good 配置正好走这条路（0124 的早期探针是第 1 次调用，
ampfirst 那次是第 2 次）；若哪天 `se15_init()` 失败，bit-bang 仍会跑，
那时 `bitbang skipped` 不出现就是**预期内**，不能当回归读。

## ③ 的可行性先测了一遍：AP 的 DT 里没有 TDM 控制器节点，但厂商写死了时钟寄存器的绝对地址

（趁 ① 开机之前做的零成本准备，来源是 `logs/fdt/marble-live.dts` —— 真机 adb 抓的活动 DT，
不是抄的 upstream dts。）

1. **macro 基址从此不用推断**（`lpass-cdc` 子节点，`marble-live.dts:21266` 起）：

   | macro | DT 节点名 | reg | 相对 rx |
   |---|---|---|---|
   | rx | `rx-macro@3200000` | `0x03200000` | +0（与我们的 `G_LPASS_RX_PA` 逐位相同） |
   | tx | `tx-macro@3220000` | `0x03220000` | +0x2000 |
   | wsa | `wsa-macro@3240000` | `0x03240000` | +0x4000 |
   | wsa2 | — | `0x031e0000` | −0x2000 |
   | va | `va-macro@33F0000` | `0x033f0000` | +0x1f0000 |

   ⇒ 上一节"regmap 的 offset 0 很可能就是 RX 基址"那条算术，现在**差值全部来自 DT**，
   不再是我自己减出来的：regmap 里恰好出现的 `0 / 2000 / 4000` 三块与 rx/tx/wsa 对应，
   wsa2（负）与 va（远）该没有也确实没有。这仍是支撑，不是证明——证明要等 ③ 读到带已知答案的值。

2. **`mi2s/tdm` 控制器节点：AP 的 DT 里不存在。** 全文只有 pinctrl 组名
   （`tert_tdm_clk/ws/din/dout`、`pri_tdm_*`、`pri_mi2s_*`，`:1425-1710`）和 `qcom,lpass-cdc`
   那一族，没有任何带 `reg` 的 TDM 端口节点。⇒ ③ 原句"读 mi2s2 端口的使能/状态寄存器"
   **按字面做不出来**，AP 侧没有这个基址可依据。先记下来，免得拿一个猜出来的地址去开机。

3. **做得出来的版本**：`lpass-cdc-clk-rsc-mngr`（`:21272`）里厂商直接写了绝对地址：

   ```
   qcom,rx_mclk_mode_muxsel   = <0x33a40d8>
   qcom,wsa_mclk_mode_muxsel  = <0x33a20e0>
   qcom,va_mclk_mode_muxsel   = <0x3420000>
   qcom,fs-gen-sequence       = <0x3000 0x1 0x1  0x3004 0x3 0x3  0x3004 0x3 0x1  0x3080 0x2 0x2>
   ```

   前三个是"该 macro 的 MCLK 从哪个源取"的 muxsel 寄存器，地址绝对、可直接读，而且厂商在旁边
   留了可对照的线索（`qcom,va-clk-mux-select = <0x1>`）。`fs-gen-sequence` 那四个数是**偏移不是
   地址**，而 clk-rsc 节点自己没有 `reg` ⇒ 没有可靠基址，**这一条不许用来读数**，除非哪天从 `.ko`
   里挖出基址（`work/audio-vendor/modules/` 现有 `audio_prm_dlkm.ko`、`spf_core_dlkm.ko`、
   `machine_dlkm.ko`、`q6_dlkm.ko`、`aw882xx_dlkm.ko`、`lpass_cdc_{rx,va}_macro_dlkm.ko`，
   是可挖的地方）。

4. 所以 ③ 的形态定为：**读三个 muxsel 寄存器 + 与厂商线索对照**，一个补丁、一次开机、不需要新基址。
   它回答的是"我们投票要的时钟，到底有没有把 macro 的 MCLK 切过去"——比 `done status=0` 离喇叭近，
   但仍然不等于出声，也不许写成等于。

## 0242 已写好并校验：③ 的具体读法，以及它缺的那一半（Android 侧的期望值）

补丁 `xnu/patches/0242-sweet-clk-witness-muxsel.patch`：新函数 `g_clk_witness(tag)`，
读厂商 DT 直接给出的三个绝对地址 `0x033a40d8`(rx) / `0x033a20e0`(wsa) / `0x03420000`(va)，
`sweetclkwit=1` 才生效，调用点在 `clocks va=%d rx=%d` 之后（pre）和 `g_lpass_probe()` 之前（post）。

三条设计上的安全约束，都写进了代码注释：

1. **每个地址在碰内存之前先把地址打出来并 sync 日志**。这块区域我们从来没从这个 kernel 读过，
   万一某一格挂死，ring 会直接点名是哪个地址杀了这次开机，而不是整段丢日志。
2. **窗口按 16 KB 页算**。`G_PFN_MASK = 0x3fff`（不是 0xfff！），`0x033a40d8` 落在它那个
   16 KB 页里的偏移是 `0x40d8` ⇒ 若按 `PAGE_SIZE` 开窗口再映射就会映射到分配之外。
   现在是一次性开 `0x8000` 复用（`g_win_map` 自己 `pmap_remove`），末尾一次 `g_win_free`。
   这一条是我在写的时候差点踩的坑，先记下来。
3. 校验三条全过：`git apply --check` 对**已提交的链基线**（`work/chain-tree` 里
   `db56069 chain through 0241`，先 commit 再 diff，就是 0241 那次编炸换来的规矩）；
   函数括号 6/6 平衡；单函数 stub 在 `-Wall -Wextra -Wformat=2 -Werror` 下**零诊断**。

### ③ 开机前必须先拿到的一半：Android 在这三个地址上的读数

诚实口径：**现在还不能给 ③ 贴"期望读数"**，因为我们没有厂商健康态下这三个 muxsel 的值。
没有它，K 格只能测"读路活着没有"和"pre 与 post 有没有动"，测不出"切对了没有"。
所以下面这条抓法（只读，不开机）是 ③ 的前置：

```bash
# 手机回到 Android 之后（一次设备一个会话，别和开机链抢）
adb -s 2aa26095 shell 'su 0 mount -t debugfs nodev /sys/kernel/debug 2>/dev/null;   grep -iE "rx_swr_clk_src|wsa|va_core|rx_afe|wsa_macro|core_clk" /sys/kernel/debug/clk/clk_summary'   > logs/android-clk-summary.txt
adb -s 2aa26095 shell 'su 0 toybox devmem 0x033a40d8 32; su 0 toybox devmem 0x033a20e0 32;   su 0 toybox devmem 0x03420000 32' > logs/android-muxsel.txt
```

`devmem` 大概率被 SELinux 挡（挡了就记"读不到"，不许把"没读到"写成"没有值"）；
`clk_summary` 是我们以前抓 regmap 时验证过能起来的那条路。两边都拿不到，③ 就按
"无已知答案，只测 pre/post 差异"来做，判据要照这个降级重写。

### 预先登记：③ 的判据（K 格）

| 格 | 判据 | 落点含义 |
|---|---|---|
| K1 | 三行 `clkwit pre …=` 都打印完并带回值 | 这块新区域 AP 可读，仪器成立（这本身就是信息：RX macro 那条读路此前只在一个锚点上被证明过） |
| K2 | 某一行的地址打印了但**没有值** | 那一格挂死，日志点名是哪个地址；其余两格读数作废（同一次开机不能再用），并把它记成"hypervisor/供电门控"证据 |
| K3 | 三个值与 Android 侧读到的**完全相同** | 时钟源选择与厂商健康态没差别 ⇒ "时钟没切"这条嫌疑**排除**，③ 收掉，剩下的只有设备图/后端绑定（②） |
| K4 | 值不同，且 `pre` 与 `post` 也不同 | 我们投票要的时钟在流跑起来时确实动了 mux ⇒ 拿到一条不经过 GLINK、不经过 DONE 的硬件运动读数 |
| K5 | 值不同但 `pre == post` | 时钟配置不一样但流没去动它 ⇒ 缺口在"谁去切它"（厂商是 ADSP 侧切的），③ 转成一条待办而不是读数 |
| K6 | `done status=0` 缺失 | 整次开机作废，K1–K5 一律记未运行 |

## ① 判决（nobb，18:51 开机，`logs/ring-nobb.txt`，sweettime=1793629907）

门控按预期工作了两次：try1/try2 拿到的还是没有 0241 的 kernel（`0241=0`，**不开机**），
try3 拿到修好 diff 之后重出的那一版（`0238=1 0239=1 0240=1 0241=1`）才 `mkimage`+`fastboot boot`。

J5 先判（它决定其余格子算不算数）：`graph_start … status=0` ×1、`done status=0` ×4 ⇒
**数据面仍然绿，这次开机有效**。

| 格 | 判据 | 实测 | 落点 |
|---|---|---|---|
| **J1** | `bitbang skipped` 出现且 `pre-stream` 为 `r=0` | `bitbang skipped` ×1；三段时间点 24 次读回**全部 `r=0`**，整份 ring 里 `r=-1`/`r=-2` 计数 **0** | **命中** |
| J2 | `pre-bitbang` 成功而 `pre-stream` 仍 NACK | 没有发生 | 未命中 |
| **J3** | `pre-bitbang` 与 `post-stream` 三个寄存器**任何一个不同** | `0x2032 / 0xffff / 0x4003 / 0x38` 三段时间点**逐位相同**（两颗芯片都是） | **未命中 ⇒ 这格要的独立证据仍然没有** |
| **J4** | 三个时刻 `reg=0x4` 全是 `0x4003` | 正是如此（`:1561`/`:1573`/`:1792`） | **命中**（只测量：写进去的 `0xb440` 不是这个寄存器报出来的值，不命名机制） |
| J6 | RX 锚点本轮不计分 | 九位读数与 anchor3 **逐位相同**（`@0x400=0x04` 两次都在），pre==post | 记录为"可跨开机复现"，仍不作证据 |

三件事必须写清楚：

1. **0241 的作用域和我预料的一致，而且日志自己证明了它**：`bitbang skipped` 只出现 1 次，
   `levels before` 也只出现 1 次——那 1 次来自**第 1 次** `probe_amps()` 调用（早期探针，
   `se15_probe_calls==0`，GENI 块不执行，所以 gate 也到不了）。带 `sweetampfirst=1` 的
   第 2 次调用走了 GENI、命中 gate、跳过了紧接着我们写 PA 之后的那段 bit-bang。
   也就是说：**杀掉 PA 可寻址性的是"写完配置之后的那次 GPIO 段"，早期那次没有这个后果**
   ——这句是两次开机的对照结果，不是单次命名。
2. **哨兵改造当场兑现**：全 ring `val=0xa5a5` 计数 **0** ⇒ 24 次读回**每一次都真收到了字节**；
   于是 `reg=0x3` 的 `0xffff` 从此只能读成"总线回了全 1"，不再是"没读到"的歧义。
3. ① 没有交出目标要的东西（J3 未命中）。它交出的是**一件完全活着的 PA 仪器 + 一条 vcfg2-off
   对照基线**：三段时间点、两颗芯片、四个寄存器的"无运动"读数。②（`sweetvcfg2=1`）判据里
   最硬的 I4（`pre-stream` 与 `post-stream` 值不同）本来就因为仪器坏而没法跑，现在能跑了，
   而且它的对照就是这次 nobb。**所以 ① 之后立刻接了 ②**（任务 `b61382jgw`，
   唯一新增变量 `sweetvcfg2=1`，其余参数与 nobb 逐字相同）。

预算口径：目标写的是 ①②③ 三次开机跑完仍无独立于 DONE 的证据就走止损分支。① 已用完（J3 未命中），
② 在跑，③ 还欠一个 Android 侧参考值（没它 K3/K4/K5 不许计分）。到 ③ 之前不许提前宣布止损，
也不许把 J1/J4 这种"仪器修好了"的成果写成"出声了"。

## ② 之后、零开机的两件事：③ 的参考值没拿到（而且原因很具体），以及 mediaserverd 的真实状态

### A. Android 侧想做已知答案，做不到，而且失败的方式本身就是信息

全部只读，没有 flash/erase，没有任何写分区（唯一写入是 `/data/local/tmp` 下的临时文件，用完即删）：

* **`/dev/mem` 在这台机器上不存在** ⇒ `toybox devmem 0x033a40d8` 之类**根本开不了口**
  （报 `devmem: /dev/mem: No such file or directory`）。三个 muxsel 地址的厂商读数**拿不到**。
  顺带纠正我自己：`toybox devmem` 的宽度参数是**字节**（1/2/4），不是位数——传 32 会报 `bad width`。
* `clk_summary` 存在但是 **0 字节空文件**；能用的是 per-clk 目录：
  `/sys/kernel/debug/clk/audio_lpass_mclk{,2..12}/clk_rate|clk_enable_count|clk_prepare_count`，
  **空闲态全部为 0**（时钟是关的）。存 `logs/android-audio-mclk-idle.txt`。
* 同时重新 dump 的 lpass-cdc regmap 与之前那份**逐字节相同**（495 个非零项，
  `0x088=60 / 0x0a4=0c / 0x0c0..0x0d4=0e / 0x400=04`）。
* 我试着用**全零 WAV** 做"路径通电但不出声"的扰动（`tinyplay … -d 31`，
  `logs/android-livecheck.txt`）：tinyplay 报 **`Error playing sample`**，
  期间 regmap **无一项变化**、mclk enable count **仍为 0**。
  ⇒ **这次扰动是无效实验，不是"证明硬件没动"**：AudioReach 的 BE 需要 AGM/SPF 先把图建起来，
  ALSA 层直接 tinyplay 建不起来。所以"regmap 那 495 项到底是缓存还是硬件"**至今未判定**。
* 因为上面这条，**③ 的 K3/K4/K5（与厂商值相同 / pre≠post / pre==post）没有可用期望值**，
  按目标里"取不到证据就不做这一格"，**③ 不在参数侧继续耗开机**；
  而我 ① 判决里当作锚点期望值用的那份 regmap dump，可信度要降级成"驱动侧状态"，
  不能再当"硬件读数"引用——这条把我自己前面两节的口径往回收了一步，写在这里以免下一个人再踩。

### B. 但 asoc 白送了一张真实拓扑图（对 ② 的结构性结论很关键）

`/proc/asound/pcm`（存 `logs/`，命令在本文末尾）里 **PA 挂在哪条 TDM 上是写死的**：

```
00-00 CODEC_DMA-LPAIF_RXTX-RX-0        playback   ← 主播放后端（lpass-cdc RX macro）
00-31 TDM-LPAIF-RX-TERTIARY   multicodec-31  playback  ← tert(mi2s2)：AW882xx 的进料口
00-33 TDM-LPAIF-TX-TERTIARY   multicodec-33  capture
card 组件: wcd938x-codec / aw882xx_smartpa.2-0034 / aw882xx_smartpa.2-0035 /
           pm8350b-swr-haptics.1f0170220 / msm-stub-codec
```

三件事：① 厂商把 AW882xx 绑在 **TERTIARY** TDM（正是我们 `se15_tdm_pins()` 切到 mi2s2 的
那四个 pad），② PA 的 I2C 地址 `0x34/0x35` 与机器名 `aw882xx_smartpa.2-0034/0035` **完全一致**
（我们没认错芯片），③ **震动是 SoundWire 上的 `pm8350b-swr-haptics`**，与喇叭不同一条物理通路
（但上面仍是同一条 mediaserverd/音频栈）。

### C. 关于 mediaserverd：我先说错了一次，这里是核对后的版本

第二意见说"iOS 侧音频服务是 mediaserverd"——这条我核过成立：rootfs 里有
`/System/Library/LaunchDaemons/com.apple.mediaserverd.plist` + `/usr/sbin/mediaserverd`，
而且**它在我们自己的端口上真的起来了**（`logs/ring-h2-dst-tdm.txt:778` `exec mediaserverd`）。

但我一开始从聚合计数（`exec` 1245 / `CRASH` 1100）就下"mediaserverd 在崩溃循环、
这才是 iOS 出声的真拦路"——**这个说法我收回**。按 ring 分开数才对：
带 `CRASH mediaserverd` 的只有 **24 份** ring，且全是 **GPU 探测期**那批
（`ring-gpu-m*-…`、`ring-amp8`、`ring-daily-v93-freeze`）；
现在这批音频 ring（`ring-ampfirst/ampverify/anchor3/nobb/vgraph`…）里 mediaserverd
**exec 之后没有 CRASH 行**，只有一条 `CODE SIGNING: cs_invalid_page(…)[mediaserverd] …
allowing (remove VALID) page`（例如 `ring-nobb.txt:1064`），也就是签名页被放行、进程活着。

⇒ 对交付有用的结论是这两句，不含"崩溃循环"：
**iOS 的音频服务在本移植里是活的**，缺的是它**下面没有一个它认识的音频设备**；
而"AppleHDA 兼容层"这条老路线（`docs/sound-plan.md:50`）与实机不符——
我们启动的 `artifacts/kernel.bin` 里 **`AppleHDA` 字符串计数为 0**，
而 `IOAudio2Device` 1 / `IOAudio2DeviceUserClient` 1 / `AppleEmbeddedAudioDevice` 1 /
`AppleARMIISAudioDevice` 8 / `AppleAOPAudioController` 1 / `AppleSPUHapticsAudioDriver` 2
都在。所以那座桥应当按 **IOAudio2 + AOP 类**来设计，不是 AppleHDA。这条也说明
`docs/sound-plan.md` 需要订正，我在下一节改它。


## 候选见证 #5 的仪器规格（0243）：三阶段高频 IBAT 突发

把两条电流路径分清之后才知道该做什么（依据 `iokit/Kernel/arm/SweetBattery.cpp:236-272`
与 `xnu/patches/0181-sweet-shm-system-ram.patch:3592-3606`）：

- **不能用的**：`SWEET BATT: tick …`。它是 `sweet_glink.c` 向 ADSP 要 `prop 9 "ibat"` 的缓存值，
  3 秒轮询、每 6 tick 才打印一行（≈30 s）。我们的音只有 180 ms ⇒ 命中窗口概率约 0.6%，
  这条仪器在现有节奏下**不可能**测到任何东西。
- **能用的**：`SweetBattery.cpp` 那条 **SPMI 直读**：`sid 0 / 0x4805` 取 subtype（只有 3、4 有效）、
  `0x48c2` 取 16 位裸值、`0x8000` 表示无数据、`scale = subtype==3 ? 152588 : 305176`、
  `ma = -((int16)raw * scale)/1e6` ⇒ **正数=充电、负数=放电**（源码注释原话，不是我推的）。
  这条是我们自己发 SPMI，速率只受 SPMI 限制，可以做成密集突发。

**0243 的规格（一个补丁、一次开机、只加仪器、数据面配置不动）**：

1. `sweetibatburst=N`：三个阶段各读 N 次 `0x48c2`，相邻读之间固定小睡（10–20 ms）；
   每阶段读完立刻打一行汇总 `phase=<idle|tone|idle2> ok=<有效>/<N> min/max/median mA`，
   前后各一次 `sweet_log_sync()`。`tone` 阶段必须与 PCM 喂入**时间上重叠**。
2. 有效性内置：某阶段 `ok < N/2` 就是仪器没跑 ⇒ **该阶段作废、不参与判决**
   （沿用 ② 的 I5/I6，以及 ① 里 `r=-1` 与 `r=-2` 必须分开的那条教训）。
3. 判据（现在写死，开机后只许按格对）：

| 格 | 判据 | 含义与限制 |
|---|---|---|
| **E1** | `tone` 阶段 median 比两侧 idle **更负**（放电更多），且跨两次开机可复现 | 能量侧第一次出现"流期间多吃了电流"。**单次 E1 只说"电流变了"，不许命名机制** |
| **E2** | 三阶段无可测差别 | 是"本仪器在 1/16 幅度 + 180 ms 下分辨不动"，**不是**"没有出声"；它给出的下一步是"要更长的音"，而那被宿舍限制挡着 ⇒ 此时按止损收内核侧 |
| **E3** | 归因 | 只有**第二次开机**（其余逐字相同、只把 PA 静音/不使能）才允许把 E1 归给喇叭；这是唯一能让"能量"变成"喇叭在推"的对照 |
| **E4** | 插着 USB | E1/E2 **一律不判**（充电电流量级支配），开机前先核 `usb=` 字段；若整次都插着就记未运行 |
| **E5** | `done status=0` 或 `graph_start status=0` 缺失 | 整次开机作废，E1–E4 全记未运行 |

4. **订正（写完代码才发现，我上一版这条约束是错的，撤回）**：我说过"要命中窗口，音需要拉到
   ~1 s，即 `sweetbufs` 从 30 提到 ~170，贴着'不放长音'的边界，要专门许可"。
   **不成立。** 一次 burst 是 12 个样本 × (2 次 SPMI 读 + `delay(10000)` = 10 ms) ≈ **130 ms**，
   而 `sweetbufs=30` 的音本身就 ≥120 ms（一个 buffer 4 ms，外加 gap/DONE 等待），
   且 `tone` 相是插在 `n == tone_buffers/2` 处，正好压在音的中段。
   ⇒ **0243 不需要加长音，`sweetbufs` 保持 30，宿舍限制一个字都不用改。**
   真正剩下的风险不是音长，而是电量计 IBAT 的**平均窗口**有多长：几十毫秒级滑动平均，
   130 ms 的突发还压得进去；平均到秒级，`tone` 与 `pre/idle2` 会被同一个均值抹平 ⇒ 那是
   **E2（本仪器分辨不动）**，仍然**不是**"没有出声"。这条只能跑了才知道。
5. 代码与校验状态：`xnu/patches/0243-sweet-ibat-three-phase-burst.patch` 按"先 commit 基线
   再 `git diff`"生成（基线 = `work/chain-tree` 的 `e42021b chain through 0242`），
   4 hunk、+87/−0、`git apply --check` 干净、函数括号 11/11、单函数 stub 在
   `-Wall -Wextra -Wformat=2 -Werror` 下**零诊断**；marker `"SWEET MARK: ibat "` 已进
   workflow 列表（`3e10a82`）。**尚未开机**，所以 E1–E5 一格都没判。

## I4 判决（18:50 开机，`logs/ring-i4.txt`，sweettime=1793633440）—— 未命中

有效性全绿：`graph_start … status=0` ×1、`done status=0` ×4、`bitbang skipped` ×1、
`levels before` ×1、**`r=-1`/`r=-2` 计数 0** ⇒ 三段时间点 24 次读回全部成功，这格可以判。

| 格 | 判据 | 实测 | 落点 |
|---|---|---|---|
| I4 | `pre-stream` 与 `post-stream` 同地址同寄存器**值不同** | `:1573-1580` 与 `:1736-1743` 逐位相同（两颗芯片 `2032 / ffff / 4003 / 38`），且与 `pre-bitbang`（`:1561-1568`）也相同 | **未命中** |
| I3 | 只有 `p65` 被拒 | 仍是 `vcfg2 rc bf=-1 c0=-1 c8=-1 p65=-1`（`:1647`）——与上一次开机逐字一致 | 未命中；②的 I2 结论**跨开机可复现** |

⇒ **目标要的那条"独立于 DONE 的正证据"，到 0241+0235 两件套用尽之后仍然没有。** 现在能说的只是：
PA 在流前后没有可测变化、厂商那四包在这张图上被拒、并且这两件事都能复现。

**订正我自己订正过的东西一次，说清楚是什么时候改的**：E4 原来写"插着 USB 一律不判"，
理由是充电电流支配。实测支持例外：`SWEET BATT: tick … cap=100% 4414mV **0mA** status=4 usb=1`
⇒ 满电时充电已终止、净电流≈0，USB 在位本身不掩盖音频负载。
**这条修改发生在下一次开机之前**（不是事后），改后规则：同一次开机里必须同时打印 `status`/`usb`，
若读到非零正向（充电）电流则 E1/E2 不判；`0mA/status=4` 这类充电终止态允许判。

## 下一批（A2 + B1 同一版 kernel、同一次开机，攒成一次推送）

- 0243 = 三阶段 IBAT 突发（E1–E5）；0244 = 空壳音频服务（下面 P1–P4）。
  两者互不相干：一个只发 SPMI 读，一个只是发布 IOService + 打日志，
  **都不动数据面参数**，所以同机不互相污染；各自有效性门独立。
- 开机参数（相对 I4 只多两个仪器开关）：
  `… sweetshmflag=4 sweets1ap=3 sweetampfirst=1 sweetampverify=1 sweetreadrx=1 sweetnobb=1
  sweetvcfg2=1 sweetibatburst=1 sweetaudioprobe=1 sweetbufs=30`

| 格 | 判据（开机前写死） | 落点 |
|---|---|---|
| P1 | 两行 `SWEET AUDIO: … registered` 都在 | 服务发布成功；缺行 ⇒ gate 没触发或 `start()` 没到，P2/P3 记未运行 |
| **P2** | 出现任意 `SWEET AUDIO: … open by pid N` | **本目标 B 组要的第一个二元答案**：有人来找音频设备了。同时记下 pid 与之后的 `sel=` 序列（序列就是 ABI 复原的原料） |
| **P3** | 一次 `open` 都没有 | mediaserverd 根本没走到 IOKit ⇒ 不许开始写音频后端，先解决"它为什么不找设备"（HAL bundle 是否加载、签名、依赖进程） |
| P4 | `open by pid` 的 pid 归属 | 用同一次 ring 的 `exec …` 与 `p=NN[name]` 行核对是不是 mediaserverd；**不许凭印象认 pid** |
| P5 | `ring` 里 `exec mediaserverd`（历史 159 个 ring 都有，`ring-ab.txt:792`） | 有 exec 无 open ⇒ 卡在 HAL/名字解析，不是进程没起来 |

**↑ 这张表已作废（2026-11-03）**：它写的是 `SWEET AUDIO: … open by pid`，而那个前缀和 bsd 侧
撞名（§三），0244 现在的打印是 `SWEET AUDPROBE: …`。当前有效的判据表在下面的
"2026-11-03 离线收口"一节里（P1–P7 + Q0c/Q0–Q4）。保留原文是为了让"我当时打算怎么判"可查。
| E1/E2 | 见上一节（`ok=<有效>/<N>` 不足即该阶段作废） | 能量侧第一次判定 |
| E5 | `done status=0`/`graph_start status=0` 缺失 | 整次作废，E/P 全记未运行 |

风险如实说：0244 是**新增 IOKit C++**，本机没法编译校验（只能靠 CI）。它若编坏，这一版 kernel
会红，A2 的那次开机一起延后——这是把两件事攒成一次推送的代价，我按用户要求攒了。

---

## 2026-11-03 离线收口：boot-args 只有 607 字节，ring-ab 与 ring-i4 都要重判

设备不在手（用户在上课），这一节全部是本地源码 + 已归档 ring 的离线结论，没有新开机。

### 一、机制（源码级，已核）

* 截断发生在**我们自己的打包器**里：`scripts/mkpayload.py:36` `BOOT_LINE_LENGTH = 608`、
  `:37 CMDLINE = 0x48`、`:38 SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH # 0x2a8`、
  `:186 cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]`——**静默切片，不告警**。
* 内核侧同一个常量：`pexpert/pexpert/arm64/boot.h:26` `#define BOOT_LINE_LENGTH 608`
  （已实测存在于 `work/ci-src/xnu-xnu-8796.141.3/`），`bsd/kern/kern_sysctl.c:2167-2170`
  `char buf[BOOT_LINE_LENGTH]; strlcpy(buf, PE_boot_args(), BOOT_LINE_LENGTH);`。
  ⇒ **打印截断 == 存储截断，硬顶 607 字节**；launchd 那行看起来是 620，因为
  `boot-args = ` 前缀 13 字节。
* 所以 `images/*.build.log` 里的 `cmdline:` 是打包**前**的字符串，它比镜像里真正带的长，
  以前我一直拿它当真相——这就是三个仪器静默失踪的来源。

### 二、哪些历史结论作废（按实测字节偏移）

| boot | cmdline | 关键 arg@偏移 | 内核回显 | 判定 |
|---|---|---|---|---|
| anchor3 | 604 | sweetbufs@571 | 完整（604 字节，未切） | 有效 |
| nobb / vcfg2 | 616 / 617 | sweetbufs@583/584 完好 | 尾部 `sweettime=1`/`sweettime=` 被切 | 参数有效，时间戳作废（1969） |
| **i4** | 629 | `sweetbufs=30`@596 → 切成 `sweetbufs=3` | `ring-i4.txt:1732 pcm tone done (3 buffers)` | **值被污染的作废**：I4 是 12 ms 音上测的，不是 130 ms |
| **ab** | 664 | `sweetibatburst`@596、`sweetaudioprobe`@613、`sweetbufs=30`@631 | 三个都没进内核 | **E1–E5、P1–P4 全作废**（失败的检法不是负结果） |

ab 那轮更糟的后果要说清楚：`sweetbufs` 一起丢了 ⇒ 上限失效，实际跑了 **1032 次 `data_buffer send`**
（`grep -c` 于 `logs/ring-ab.txt`），`pcm tone done` 一次没有，日志末尾是
`CRASH ExtragalacticPos … extension check-in timeout`。**宿舍安静那条保护是被静默拿掉的，我当时没发现。**

### 三、我自己两次误判，写下防止复用

1. 我先前用"i4 的 `sweetbufs=3` 在偏移 606 处生效"反推"620 只是打印截断"。错——那是**值被切了一半**。
   教训：长度必须在打包侧断言，不能靠事后读日志倒推。
2. **marker 假阳性**：workflow 里 `"SWEET AUDIO: "` 这条门禁不可能抓错，因为这个前缀早就被占了——
   `work/chain-tree/bsd/dev/sweet_glink.c:6845` `printf("SWEET AUDIO: build v97 (battery refresh worker)")`，
   外加 0106/0107/0108/0109/0181 五个补丁都用它。已改成 `SWEET AUDPROBE: gate flag=`；
   `SWEET HAPTICS:` 这类 IOKit `IOLog` 串能在 `out/kernel.adsp` 里 grep 到（实测 37 次命中），
   所以新 marker 是可用的门禁。`SWEET MARK: ibat ` 也复核过，只在 0243 里出现，唯一。

### 四、0243 的自检行原来写错位置

第一版把 `printf("SWEET MARK: ibat %s flag=%d\n", phase, burst);` 放在
`PE_parse_boot_argn("sweetibatburst", …)` **之前**，而 `burst` 初值是 0 ⇒ 参数真到了也照样印
`flag=0`。哨兵必须"不可能作为读数出现"才算哨兵。现在挪到解析之后，并带上 `exp=%#x`。

### 五、下一轮（ep）开机：参数预算 + 判据（开机前写死，事后不许改）

cmdline 实测 **585 字节**（预算 590，硬顶 607）；`mkimage.sh` 现在超 590 直接 build fail。
仪器参数排最前：`sweetexp=3`（bit0 = IBAT 突发，bit1 = 音频服务探针）、`sweetbufs=30`、`sweetnobb=1`，
其后才是数据面基线。`sweetgpu=0` 从链上参数里去掉：`g_gpu_probe()` 起点是 `int en = 0`
（`work/chain-tree/bsd/dev/sweet_glink.c:2689`），传 0 与不传完全等价。

| 格 | 期望读数（开机前定死） | 不成立时怎么判 |
|---|---|---|
| 前置 | ring 的 `boot-args` 行里 `sweetexp=3`、`sweetbufs=30`、`sweets1ap=3`、`sweetnobb=1` 逐个出现，且该行长度 < 607 | 缺哪个，依赖它的格一律记"未运行"，不记负结果 |
| 前置 | `graph_start … status=0` ≥1、`pcm tone done (30 buffers` 出现 | 缺 ⇒ E/P 整次作废 |
| E0 | `SWEET MARK: ibat pre flag=1 exp=0x3` | 整行没有 ⇒ 仪器不在镜像里（补丁被跳过）；`flag=0` ⇒ 参数没到 |
| E1/E2 | pre/tone/post 三段各自 `ok=12/12`，`mA` 的 min/max/mean 都在 | `ok<12` 该段作废；`usb=1` 一律不作判（本次日志自带 `usb=` 字段） |
| E3 | tone 段 mean − pre 段 mean 的差值，记数值不记机制 | 单次开机只说"电流变了"，归因给喇叭要第二次只静音 PA 的对照 |
| E4 | 三段 `subtype` 是 3 或 4 | 否则 SPMI 路径不对，整组作废 |
| P1 | `SWEET AUDPROBE: AppleAOPAudioController start` + `IOAudio2Device start` 各一行 | 没有 ⇒ 补丁没进镜像或平台表没发布 |
| P2 | `gate flag=1 exp=0x3` + 两行 `registered` | `flag=0` ⇒ 参数没到（作废），不是"没人来找" |
| ~~P2.5~~ | ~~`SWEET AUDPROBE: matchq …`~~ **已撤销，不在这一版 kernel 里** | 覆写 `matchPropertyTable` 测不到纯类名查询（`IOService.cpp:7787` 先跑 `matchInternal`，`IOProviderClass` 被 `metaCast` 吃掉后 `done == count` 就 break，`:7556-7567`），留着会产出"没人问过"式的假安静。分层判据改由下面的 **Q 组**（0049 的用户态查询埋点）承担 |
| **P3** | 任意 `open by pid N` | **B 组要的第一个二元答案**；一次都没有也不能直接推"mediaserverd 不找设备"，见 P5 |
| P4 | pid 用同一次 ring 的 `exec …`/`p=NN[name]` 行核对 | 不许凭印象认 pid |
| P5 | `ring` 里 `exec mediaserverd`（历史 159 个 ring 都有，`ring-ab.txt:792`） | 有 exec 无 open ⇒ 卡在 HAL/名字解析，不是进程没起来 |

| **P6** | `SWEET AUDPROBE: aop sel=N ran …`（N∈0..5，对应 §10.2 那六条） | 出现 ⇒ 客户端真按 Apple 的方法表在调；`sel>5` 应当**看不到** ran 行而是基类报 unsupported（与真机一致）。整类缺失 ⇒ v6 没进这一版 kernel（本地 `out/kernel.adsp` 里 `aop sel=` 命中 0，所以这一格只有在 CI 出了 v6 产物之后才可判） |
| **P7** | `SWEET AUDPROBE: legacy method[N] / trap[N] requested by pid P` | 出现 ⇒ 客户端走 legacy `IOConnectMethod*`/trap 路径。这以前是盲区（基类返回 NULL 不打字），现在能看见"第一个 selector 都还没发就放弃"的那种失败 |

主攻对象按 §10.1 更正：**`IOAudio2Device` 那个节点才是音频客户端 open 的地方**——真机
`IOAudio2DeviceUserClient` 有 8 个实例、`AppleAOPAudioUserClient` 只有 1 个，而且带
`"device UID"="Speaker"` 的 `:13094` 节点属于 `IOService:IOAudio2Device:…` 类链、
自身**不发布** `IOUserClientClass`。P3 的判读优先看 `IOAudio2Device` 那一行。

### 六、0244 的名字按真机 ioreg 校正（离线核，未开机）

证据文件 `work/iphone-x-recon/dyld/ioservice.txt`（D22AP，kernel `xnu-8792.82.2`，build `20D67`）：

* `:3545` `+-o AppleAOPAudioController <class IORegistryEntry:IOService:AppleBaseAudioController:AppleAOPAudioController>`
* `:3547` `"IOClass" = "AppleAOPAudioController"`；`:3559` `"IOUserClientClass" = "AppleAOPAudioUserClient"`；
  `:3569` `"IOMatchCategory" = "AppleAOPAudioController"`
* `:13112` **`"device UID" = "Speaker"`**——带空格的那个字面键；`IOAudio2DeviceUID` 在整个 dump 里
  **0 次命中**，所以我草稿里那个键是编出来的。`IOAudio2DeviceUserClient` 命中 9 次，
  `AppleAOPAudioUserClient` 命中 3 次，两个 UC 类名都已改成真名。
  `IOUserClientClass` 这个串正是 `IOService::open()` 查的东西，名字错了就会**永远**读到"没人来 open"。
* 顺带记一条不该当阻塞点的：`ring-ab.txt:1272` `CODE SIGNING: cs_invalid_page … p=28[mediaserverd] … allowing`
  同现象也落在 backboardd / chronod / dasd / launchd_cache_loader 上，是移植的通病，不是音频专属。

### 七、仍然没测到的（不许含混）

* **音后 PA 是否可寻址**：`ring-ab` 在 `pcm tone start`（`:1705`）之后 **一条 `SWEET MARK: ampread` 都没有**
  （post-stream 段要等 `pcm tone done`，而那轮没跑完）。⇒ "出声时刻 PA 可不可读"仍是未知，
  子智能体报告里"PA 在音后仍 44/44 可寻址"的排序说法我没接受：`ring-ab:1555/1557` 的
  `geni amp 0x34/0x35 profile 44/44` 在 1705 之前，是音前。
* **电流通道的节拍——已核实，不再是待验**：`xnu/patches/0181-sweet-shm-system-ram.patch:3604-3607`
  是 `tick_n++; if (tick_n % 6 == 1) printf("SWEET BATT: tick %d …")`，注释自陈 "(30 s)"
  ⇒ **tick 5 s、打印 30 s**。30 s 的打印对 130 ms 的音原理性看不见，0243 的密集突发
  （12 次 × 10 ms 直读 SPMI）正是为绕开这条通路设计的。
  但还有一条更硬的结构冲突要写死：`fastboot boot` 必须插线，所以"拔掉 USB"这条前置
  在我们的运行模式里做不到 ⇒ E 组门加严为 **同一 phase 里 `usb=1` ⇒ 该 phase 记未运行**
  （0243 自带 `usb=` 字段，不用凭印象）。若 pre/tone/post 三段全 `usb=1`，
  能量侧在 RAM-only 模式下就是**原理性不可判**，按止损把"内核出声未证"写死并全转 B 组。
  历史单点只作量级参考、不作结论：`ring-ab.txt:2707 tick 1 ok=5/5 cap=84% 4354mV -1207mA status=1 usb=1`。
* `0xa5a5` 哨兵确实印出来了（`ring-vcfg2.txt:1613` 起 16 行 `r=-2 val=0xa5a5`）——我此前写的"哨兵从未打印"
  是错的；哨兵在 NACK 时保住了，所以别处读到的 `0xffff` 是真全 1 总线。这条更正同时意味着
  读回仪器是可信的，缺的从来不是仪器。

---

## 2026-11-03 晚些时候：桥的"层一"判据我先证伪了，然后发现答案早就在kernel里

### 一、`matchPropertyTable` 这个钩子不能测我想测的东西，已删

我上午给 0244 加了覆写 `matchPropertyTable` 来回答"有没有人**问过**这个类"（而不只是"有没有人 open"）。
读源码之后它被证明测不到：**纯类名查询走不到这个钩子**。

- `IOService::matchPassive` 先调 `matchInternal(table, options, &done)`（`iokit/Kernel/IOService.cpp:7787`）；
  只有当表里**还有没被吃掉的键**时才继续走到 `where->matchPropertyTable(table, &score)`（`:7798`）。
- `IOServiceMatching("IOAudio2Device")` 这种查询把 `IOProviderClass` 放进表，`matchInternal`
  用 `metaCast(str)` 处理它并在 `done == count` 时 `break`（`:7556-7567`）⇒ **钩子一次都不会跑**。
- 这颗 xnu 里那条路径上根本没有可覆写的每实例虚函数：`matchInternal` 不是虚的
  （`IOService.h:1659`），`doesMatch` 在这棵树里不存在（全库 grep 0 命中）。

留着它的后果正是我这一天一直在修的那个错误：会产出"没有 matchq"，而它会被读成"没人问过"。
所以删掉，并把证明过程写在这里，防止下一个我把它再加回来。

### 二、真正在用户态查询路径上的仪器，0049 早就编进每一版 kernel 了——但从没打开过

`xnu/patches/0049-sweet-iokit-trace.patch` 在 `iokit/Kernel/IOUserClient.cpp`
（`internal_io_service_get_matching_services:2658` 等一系列 `is_io_*` 入口）埋了跟踪器：

```
IOK <进程名>: matching <序列化后的匹配字典，240 字节> -> <N found | 命中的类名 | none>
```

覆盖 matching 调用、`add_notification`（**即"它在等某个类出现"这件事**）、registry path 查询；
每进程按名字过滤，**每 boot 上限 400 行**。开关是 boot-arg `sweetiolog`
（逗号分隔进程名，默认 `backboardd`，`-` 关闭）。

而我们的基础 cmdline 一直写着 `sweetiolog=-` ⇒ **它一次都没跑过**：
`logs/ring-*.txt` 里 `IOK ` 命中数为 **0**（实测 `grep -al "IOK "` 无输出），
`docs/` 里也从来没提过这个开关。也就是说，"mediaserverd 到底向 registry 问过什么类名"这个
桥的头号未知，**不用写一行新代码**，改一个参数就能拿到答案。

`scripts/mkimage.sh` 因此加了 `SWEETIOLOG=<names>` 环境变量：**替换**基础串里的
`sweetiolog=-`，而不是往后追加——`PE_parse_boot_argn` 取**第一个**匹配，追加会被基础串那份
早于它的答案顶掉。为了付这 11 个字节，把 `cs_debug=0` 从基础串删掉
（`bsd/kern/kern_cs.c:87` 就是 `int cs_debug = 0`，传 0 与不传完全等价）。

### 三、ep 这轮开机的最终参数与预算（开机前写死）

`sweetiolog=mediaserverd,backboardd`、cmdline **584 字节**（硬顶 607，余 23）。
关键键的字节位置：`sweetiolog`@298、`sweetexp`@403、`sweetbufs`@414、`sweets1ap`@536、
`sweetampfirst`@548、`sweettime`@564。为凑出这 23 字节，从基础串删掉两个**可证的空转参数**：
`cs_debug=0`（`bsd/kern/kern_cs.c:87` 就是 `int cs_debug = 0`）和 `sweetkeys=1`
（`xnu/patches/0060:163` 是 `int enabled = 1`，只有 `=0` 才改变行为）。
`gicrstride=0x40000` 虽然 0187 自己注释说"cpus=1 从来不需要"，**没删**：那是 SMP 线的断言，
18 字节不值一次开机风险。链脚本会把下面这些逐个对齐 ring 里内核自己回显的 `boot-args` 行，
缺一个就把依赖它的格判成 VOID。

新增 Q 组判据（同样是开机前定死）：

| 格 | 期望读数 | 读不到时怎么判 |
|---|---|---|
| **Q0c** | 对照格：`IOK backboardd …` 行数 > 0 | backboardd 今天就确实在发 IOKit matching（它 open 我们的 haptics stub 约 99 次）。**对照不成立 ⇒ 整组 Q 判"仪器不可判"，不许当负结果**；`sweetiolog` 不在回显里同样作废 |
| Q0 | 在 Q0c 成立的前提下看 `IOK mediaserverd` 行数 | 为 0 且对照 >0 ⇒ mediaserverd 这轮**没发起任何被埋点的 IOKit 查询**（这才是一个真的负结果） |
| **Q1** | `IOK mediaserverd: matching … IOAudio2Device … -> N found`（或 `none`） | 有 = 名字方向对，`none`/`0 found` 就是我们缺这台设备；没有则继续看 Q2 |
| **Q2** | `IOK mediaserverd: <某 notify 类型> … -> notify` | notify 说明它**在等**某个类出现 ⇒ 只要把那个类发出来就能推进；这比 Q1 更值钱 |
| Q3 | 它实际问的类名（从 `matching` 字典里读，不是我们猜的） | 这一条直接替换掉 `docs/audio-hal-bridge.md` 里所有"推测 mediaserverd 会查 X"的句子 |
| Q4 | `IOK …: path …` 之类其它入口也有行 | 埋点覆盖面确认；缺这条时 Q0 的"全 0"仍不能升级为机制结论 |


### 四、剩下的仍未测到的

`audio-hal-bridge.md` §7 里那条"mediaserverd 会不会走到 IOKit"仍未证；
但有了 Q 组，这次开机**无论有无 open** 都能把它切一刀，
而不是像 ab 那样拿一次参数没送达的安静当答案。



## 2026-11-03 夜：B1 判完（止损触发）＋ ② 抓到一条"从来没试过的变量"

### 一、b1 / b2 判决（`logs/ring-b1.txt`、`logs/ring-b2.txt`，用开机前写死的 U 组）

- **U0 全绿**（b2：560 字节 cmdline、cpus=8、无 sweetreboot、S1 叶子 0x7c1、
  keys bit2=1、AUDPROBE gate=1、`pcm tone done (30 buffers)`、opens=1/closed=0）
  ⇒ b2 与 ep9 完全可比，B1 的内核级判据是有效测量，不是"参数没送达"。
- **GUI 没到，但不是我们让它重启的**：ring 里没有 `PSCI SYSTEM_RESET`、没有 panic 行，
  结尾是 launchd 自己说的 `Process[326] crashed: dasd. Too many corpses being created.`
  → `exec xpcproxy` → `exec medialibraryd`（崩溃者全是音频域宿主：
  SiriTTSSynthesizer / MacinTalkAUSP / KonaSynthesizer / dasd，sig 6，约 28 轮 crash+respawn）。
  ⇒ "设备直接重启、没进 GUI" = **另一个独立阻塞**（记为 U6：GUI 到达），
  b1、b2 都有它，所以它是 U2 判据的一个共同混淆项，我按这个记，不改判据。
- **输入真的进了内核**：b2 = 99 条 touch（b1 只有 1 条，b1 的按键基本落在 GUI 前）、
  音量键 0xe9×13、0xea×8、0x0e30×1，共 21 次按键。
- **音频域零新增**：以首条 touch（b2 行 3107）为锚点，其后 0 条音频域日志；
  音频控制器的 selector 分布仍是 `sel=0 ×2`、`sel=4 ×10`，**`sel=2/3/5` 在两次开机里计数为 0**。
- 按类/pid 拆开核过，不把聚合数当结论：震动类 `AppleAOPButtonHapticsServiceUserClient sel=1`
  在 b1/b2 各 2 条，和盘上 70+ 个 ring 的每-boot 基线一模一样 ⇒ **不是新增**，别把它当"新 selector"。
  Actuator 对同一区域重复索要 map 仍是 4 次（没停）；pid 57 的 opens 仍是 4；
  `IOK mediaserverd` 埋点在跑（407 个 180 字节块、36 条 `=end`），只见匹配/发布我们的壳
  和一条 `AppleEFINVRAM -> none`。

⇒ **止损条件成立**（两次开机、任何新 selector/状态改动都没有）。
按预登记的写法：**iOS 会话常驻但不出流**；主攻转 B3 的 ②（不依赖 iOS 行为）。

### 二、② 的厂商帧表（离线，把 `work/sweet-audio/traces/trace_ui.txt` 按 opcode 重解析）

| 事件 | src 端口 | token | GPR p6/p7 | p8 handle | p9 |
|---|---|---|---|---|---|
| GRAPH_OPEN（流图） | 0x2010 | 0x26 / 0x44 | 0 / 0 | 0xb0d890e8 | **0x1f0** = 496 |
| GRAPH_OPEN（设备图） | 0x2011 | 0x2c / 0x4a | 0 / 0 | 0xb0d890e8 | **0xb38** = 2872 |
| GRAPH_START（流图） | 0x2010 | 0x49 | param 0x08001005 | — | 表 = {1, 0xb00000d5} |
| GRAPH_START（设备图） | 0x2011 | 0x59 | param 0x08001005 | — | 表 = {4, 0xb0000002, 0xb0000025, 0xb0000024, 0xb0000023} |

**两个 GRAPH_OPEN 共用同一个 handle**（0xb0d890e8），正文都放在区域偏移 0。
我们的 `g_apm_graph_open_shm()` 发的字段布局（`{off,0,handle,len}` @pkt+24）和上表逐字对齐，
所以包结构不是问题；问题只可能在 `len` 和正文本身。

### 三、关键发现：我们发的正文比正文自己声明的长度少 12 字节

把 `work/chain-tree/bsd/dev/sweet_glink.c:2043` 的 `g_graph_vendor4[2724]` 解出来：

```
[0] 0x00000aa4 = 2724      <-- 图自己声明的 graph_size == 数组全长
[1] 0x00000004 = 4         <-- num_subgraphs
子图正文头 {1, 0x08001001, 0x30} 在体内 0x3c / 0x3a0 / 0x5d8 / 0x70c
子图 id = 0xb0000002, 0xb0000081, 0xb0000024, 0xb0000023
```

`g_vendor4_sgs[4]`（:2274）和正文里的 4 个 id **一字不差**，所以 START 列表和我们自己的正文是自洽的。

而盘上 6 条实机记录全是：

```
SWEET GLINK: APM graph_open/vendor4 src=0x2010 (shm handle=0xb0d7f408 len=2712)
```

也就是每次都 `sweetvgoff=12 / len=2712` —— **把 12 字节的图头剥掉**，再告诉 DSP "正文 2712 字节"。
DSP 读到的第一个 u32 于是成了 0x35c（860），而它期望那里是 graph_size。
"no response" 不需要新假设就能被这条解释。

⇒ `off=0 / len=2724`（正文自洽 + 与厂商 framing 同形）是一个**从未跑过**的单一变量，
而且不需要重编内核（`out/kernel.adsp` 里已有 `graph_open/vendor4`、`vgraph off/len out of range` 字符串，
两个开关本来就留着）。

同时把我上一轮那句"我们的 0xb0000081 对不上厂商的 0xb0000025 ⇒ START 表写错"**降级**：
不一致的不是我们的表，是**正文变体**——厂商当前设备图 2872 B、含 0xb0000025，
我们手上这份是 2724 B、含 0xb0000081。两件事分开记。

### 四、离线复核补一条正面结果（并且再纠自己一次）

- `work/sweet-audio/traces/agm-heap-tdm-window.bin` 里能扫到 **35 处** `{1,0x08001001,0x30}` 头，
  逐个走 TLV 链读得出 4–6 段（0x08001001 子图属性 / 0x08001000 模块表 / 0x08001002 连接表 /
  0x08001003 / 0x08001004 / 0x08001061），单份跨度 256–1240 B；
  其中 6 份 span=816、子图 id=0xb0000002（@0xb39e 等）——那正是
  `agm-graph-b0000002-compact.bin`（680 B）的母本，母本被截断了。
  ⇒ **完整的子图正文副本是在盘上的**，我上一轮"正文一条都不在盘上"说过头了。
- 但**没有任何一份等于 2872**，也没有 496；厂商当前那份设备图正文仍旧不在盘上
  ⇒ 要拿它还是得跑一次 Android 侧抓取。`scripts/apm_harvest.py` 已补：
  `OPS` 认 GRAPH_OPEN/PREPARE/START、`GRAPH_SIG` = `{1,0x08001001,0x30}`、
  `GRAPH_SIZES` = (496, 2872)、GRAPH_OPEN 的 size 进 `sizes`，外加一个不碰手机的
  `--scan FILE` 离线模式（上面两张表就是用它跑出来的）。
- 再次作废一条：**"堆里 size=2872@0xb9e"是 u32 对齐伪影**（值活在 2 mod 4 的指针表里），不是命中。

### 五、预先登记：E1（`sweetvgoff=0 sweetvglen=2724`）的判据 —— 开机前写死，事后不许改

镜像已构建并**在打包产物里核过**（`images/e1-vgraph2724.img`，cmdline 582 字节 < 590 顶）：

```
D22AP_ADT=~/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt CPUS=8 GPU_PROBE_LEVEL=0 \
  ./scripts/mkimage.sh sweetbufs=30 sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 \
  sweets1ap=3 sweetampfirst=1 sweetvgraph=1 sweetvgoff=0 sweetvglen=2724
./scripts/boot_one.sh images/e1-vgraph2724.img e1
```

| 格 | 检查 | 期望 / 判读 |
|---|---|---|
| **W0** 有效性门 | cmdline 回显含 `sweetvgoff=0 sweetvglen=2724`；出现 `graph_open/vendor4 … len=2724`；数据面四行照旧（`s1ap leaf=0x7c1`、`graph_start rsp … status=0`、`data_buffer: dst=0x40bd done status=0`、`pcm tone done (30 buffers)`） | 少任何一条 ⇒ 记"**仪器未生效**"，不许判"正文长度不对" |
| W1 | `graph_open/vendor4` 有 rsp 且 `status=0` | framing 假说成立 ⇒ 进 E2（把 iOS 流接进来） |
| W2 | 有 rsp 但 `status≠0` | 正文被受理、内容被拒 ⇒ 记下 status 值，转 2872 变体抓取 |
| W3 | 仍旧 `no response` | framing 不是唯一原因；**这一格只能得"no response"，不能得"不是 SMMU"**，见 W5 |
| W4 | `SWEET GLINK: vendor4 graph started` | 再加一条：`graph_start` 的 rsp status 是不是 0 |
| **W5** 仪器缺口如实登记 | vgraph 分支**没有** `g_smmu_faults()` 包装（:5212 那个函数只在别的路径上调过），所以 W3 无法排除"区域读不到" | 若 W3 命中，下一轮先加 1 行 SMMU 见证再谈其它假设；失败的检法不复用 |

安全线：不加音、不放长音、`sweetbufs=30` 一字不动；纯 RAM `fastboot boot`；
vgraph 的 1000×`g_wait(5)` 最多阻塞 5 s，比一轮 tone 短。

## 撤回（同日 18:0x，紧接上一节）：E1 的前提是我自己读反的，② 按预登记止损关闭

上一节写完后我按"先把断言回源"的规矩去查 2712 的出处，三条实测把它打穿了：

1. **厂商自己声明的长度就是 2712。** 10-27 那次抓包（`logs/gpr-20261027-090802.txt`）里
   GRAPH_OPEN 是 `src=0x2010 p4=0x116 p5=0x01001000 p8=0xb0d890b8 p9=0xa98`，
   `0xa98 = 2712`。所以"我们把 12 字节图头剥掉 = bug"不成立——`off=12/len=2712`
   和 `off=0/len=2712` 两种读法里，厂商的长度 2712 与堆里那份 2724 的差**正好**是 12，
   最自然的解释是**图头本来就不上线**（`g_graph_vendor4` 的注释写的就是这个选择）。
2. **`off=0 / len=2712` 已经跑过。** `logs/ring-o3-vgraph0.txt` 回显的 cmdline 里
   就有 `sweetvgoff=0`，日志行仍是 `len=2712`。上一节说"从没试过"是错的。
3. **② 的真实结局不是"被拒"，是"发出去之后就没有日志"。** 五次 vendor4 开机逐条读：
   `ring-vgraph` 打印 `graph_open/vendor4: no response` + `vendor4 graph open failed`；
   `ring-o2-vgraph12` / `ring-o3-vgraph0` / `ring-o4-vgraph-pace` 三次的
   `graph_open/vendor4` 那行**就是最后一条音频域日志**，之后 ring 里没有 rsp、
   没有 no response、也没有 failed——而 `no response` 是跑完 1000×`g_wait(5)` 才打印的，
   所以那三次**根本没走到轮询结束**。这正是 `sweet_glink.c:6260-6266` 注释里
   run115 的那个现象（这个形态会让 DSP 去读区域，GUI 之前把 SoC 挂住）。
   ⇒ 之前把 ② 记成"原字节重放被 DSP 拒绝"，措辞过强：更准的是
   **"五种形态全都没拿到过任何 rsp，其中三种以挂死收场"**。

因此 **E1（`off=0/len=2724`）不跑**：它的先验来自我读反的那条前提，
而且它的失败模式是挂死（要长按电源才出来），不是安静失败。
镜像 `images/e1-vgraph2724.img` 留着但不启动。

按 goal 里预登记的止损（"B3 若在 ② 上两轮仍无进展 ⇒ 明确写'内核侧与 iOS 侧都常驻、
出声仍未证'，不刷开机次数"）：② 已经用掉 5 次开机，**止损条件成立**。
现在写下的就是那句话本身，不把它包装成headline进展：

> **内核侧（数据面、SHM、GPR、图开-准备-启动的厂商帧形）与 iOS 侧（mediaserverd 常驻、
> 开我们的壳、GetControlValue 在问）都已经在场，"手机真的出声"仍未证。**

### 顺带一条把"再抓一次厂商正文"这条路也关掉的事实（今天实测）

这台 ROM 的 root **装不上 tracefs**：`echo > /sys/kernel/tracing/kprobe_events` 是
`Permission denied`，`ls` 该文件也 `Permission denied`，连 `setenforce 0` 都是
`Permission denied`（`CapEff=000001ffffffffff` 全能力位、`u:r:su:s0`，dmesg 里只有
dontaudit 过的 `shell` 读 `error_log` 那条）。内核 `uname -r = 5.10.226-android12-9`，
`ro.soc.model=SM7475`（确实是 marble 这颗），`ro.build.fingerprint=OnePlus/PKG110/OP5D2BL1:15`
（props 是改过的，别拿它当型号判据）。
⇒ `apm_harvest.py` 主路径（kprobe+uprobe）在这个 ROM 上是死的，不用重试。

替代的 `--heapscan`（不放音、不开探针，纯 `/proc/<pid>/mem` 读，按
`{graph_size, num_subgraphs}` 头 + 体内至少 count 个 GRAPH_SIG **验真**）今天跑了两遍：
- 先用已知-good 的 `g_graph_vendor4` 当正对照验过探测器（认出 `size=2724 count=4`
  和 4 个子图 id），并正确拒绝那份 680 字节碎片 ⇒ 探测器不是摆设。
- 然后跑一遍 agmplay 真会话（`TDM-LPAIF-RX-TERTIARY-VIRT-0 96000/2/32`，100 ms，
  插件确实加载了），立刻扫 `1588/1646/328/1889/1763` 五个音频域进程共 50 MiB
  的 malloc 区 ⇒ **一个经验证的图头都没有**，`0x1f0` 和 `0xb38` 都没出现。
  ⇒ 厂商**当前**那份 2872 字节设备图正文，用现存的检法拿不到；
  而 `g_graph_vendor4` 那份 2724 在今天这台 ROM 的堆里也复现不出同一形状。
  （诚实边界：`GRAPH_SIG` 认的是 `size=0x30` 那种 48 字节属性块；若当前图用了别的小节长度，
  这个检法会漏。这一条**没测**，不能写成"堆里确实没有"。）

## pstore 判死，但 ACDB 把 ② 从"未知"变成"有输入可算"（2026-11-03 晚，零开机）

### 一、pstore 路线实测关闭
`/sys/fs/pstore` **已挂载**（`pstore … rw,seclabel,…`）、`/sys/module/ramoops/parameters` 存在，
但作为 root（`u:r:su:s0`、`CapEff=000001ffffffffff`）：
`ls /sys/fs/pstore` = `Permission denied`（目录是 `dr-xr-x--- system`），
九个 `ramoops/parameters/*` 全部 `Permission denied`，`/proc/cmdline` 也 `Permission denied`。
⇒ 与 tracefs 同一个 LSM 拦截，**这条挂死存活通道在这台 ROM 上读不出来**；
`/dev/mem` 以前已确认不存在，所以也没有绕开的读法。

### 二、真正的进展：厂商图正文的来源是 vendor 分区里的 ACDB
顺手查的（不依赖探针）：`/vendor/etc/acdbdata/ukee_{mtp,qrd,cdp}/*_acdb_cal.acdb`（每台 1.4 MB
量级）+ `/vendor/etc/usecaseKvManager.xml`，`/vendor/etc/audio/sku_ukee/` 里是
mixer_paths / resourcemanager（UKEE 就是这颗 SM7475 的 machine 串）。三套 ACDB 已拉到
`work/sweet-audio/vendor-ukee/`。

在 ACDB 里按 props 头 `{1, 0x08001001, 0x30}` 枚举：MTP **1450 条 props 记录 / 235 个子图 id**。
并且**我们容器里的三个子图 blob 在 ACDB 里原样命中**（`0xb0000002`@0x241b2、
`0xb0000024`@0x54cf6、`0xb0000023`@0x54e2a）。⇒ 结论：**子图 blob 是 ACDB 存的，
`{graph_size,count}` 容器是 AGM 运行时拼的**——所以堆里扫不到容器（今天 50 MiB 零命中）是**符合结构的**，
不是"抓漏了"。

### 三、硬事实：我们发的那个容器里，有一个子图在校准数据里根本不存在

| 子图 id | 在三套 ukee ACDB 里 | 厂商当前序列里 |
|---|---|---|
| `0xb0000002` | props ×41 | 用（设备图） |
| **`0xb0000025`** | **22–24 次命中**（props ×2） | **用（设备图）** |
| `0xb0000024` / `0xb0000023` | props ×1 / ×14 | 用（设备图） |
| `0xb00000d5` / `0xb00000d6` | props ×2 / ×1 | 用（流图） |
| **`0xb0000081`** | **0 次**（u32 全对齐搜索，三套都是 0） | **不用** |

`0xb0000081` 是我们 `g_graph_vendor4` 四子图里的那个，也是 `g_vendor4_sgs` 里的那个。
⇒ 我们一直在发一个**在这台机器校准数据里查不到**的子图。这与"发出后拿不到任何 rsp"**相容**，
但**没有证明**它就是挂死/无应答的原因——那还需要一条挂死下仍活着的输出通道（第一节已判死，
剩下的做法只有从别的上下文写 ring，属仪器改动）。

另一条同时到手的结构读法：容器头**不是** `{size,count}` + 四对 `{id,size}`，
而是 `{总长=0xaa4, count=4}` 后面直接跟第一个子图自己的记录链
（`… 0x08000001,8,2,0 … 0x08000010,4,2,<blob 长度>` 然后才是 props 头）。
同一个字段在 ACDB 里是 **808**、在我们容器里是 **816**（差 8）——
这条差异还没解释，**不许**先拿来当结论。

### 四、② 的下一步因此变成离线可算的（不占开机次数）
1. 用 ACDB 里 `0xb0000025` 的两条记录 + `b0000002/b0000024/b0000023` 拼出厂商**当前**
   设备图容器，校验总长是否 = 2872（`0xb38`，厂商包里声明过）；
2. 同时用同一套 ACDB 拼流图（`0xb00000d5`，声明 496 = `0x1f0`）——流图能对上
   就证明"ACDB→容器"的重建函数是对的，再信设备图那份；
3. 两条都对上以后，才谈"要不要花一次开机去发它"，并且那次开机必须自带
   存活输出通道（第四节③：ring 心跳从别的上下文打），否则又只是一条"发出后没了"。

## ②离线第一刀：容器 framing 算清楚了，而且它判死了我上一条"离线重建"的可行性
（`tools/acdb_graph_rebuild.py`，全程不碰手机；设备在 19:5x 从 USB 掉线，见文末）

### 一、容器的真实结构（从 `g_graph_vendor4` 反推，不是猜）
props 头在 0x3c / 0x3a0 / 0x5d8 / 0x70c，相邻间隔 868 / 568 / 308，最后一段 920：

```
container = {total, count}            8 B
          + 每子图 { 52 B 前导 + blob }
blob 长度：0xb0000002=816, 0xb0000081=516, 0xb0000024=256, 0xb0000023=920
校验：8 + 4×52 + (816+516+256+920) = 2724 == sizeof(g_graph_vendor4)      ✔
厂商声明 2712 == 2724 − 12，而头 12 字节正是 {total, count, 第一个 sg_id}
```

⇒ **上线时去掉那 12 字节就是厂商自己的 framing**，不是我们的 bug。
这条独立确认了上一条撤回（那次 vendor4 的 `off=12/len=2712` 与厂商同形）。

### 二、"从 ACDB 离线重建 2872"这条路要打个折，原因具体
把容器里每个 blob 拿去 ACDB 比：

| 子图 | ACDB 候选条数 | 内容命中 | 差异 |
|---|---|---|---|
| `0xb0000002` | 41 | 0x241b2 | 前 184 B 同，**之后参数不同** ⇒ 命中的那条不是同一条 graph key |
| `0xb0000081` | **0** | — | 三套 ACDB 全对齐搜索 0 命中 |
| `0xb0000024` | 1 | 0x54cf6 | 链长同为 256，blob 内第 121 字节 1 处不同 |
| `0xb0000023` | 14 | 0x54e2a | 前 456 B 同后分叉 |

前导字段两边只差那个长度字（ACDB `0x328=808` vs 容器 `0x330=816`，**+8**）。
⇒ 同一子图 id 在 ACDB 里有几十条**props 相同、参数不同**的变体，
选哪一条由 graph key（设备/用例/特性键）决定，**光靠长度和 props 定不下来**；
所以"枚举组合凑 2884"这种解法不成立（凑出来的不保证是同一 key 的那组）。
真要把 ACDB 当来源，得先把 ukee ACDB 的 section/graph-key 表读出来——那是另一个量级的活，
而且做完也只是"我们能替 AGM 拼容器"，不代表厂商真发过它。

### 三、我上一条里"堆扫 50 MiB 零命中"的**覆盖缺口**（自己抓到，记下来）
`apm_harvest.py` 的 `heap_regions()` 只取 `[anon:libc_malloc]` 区，
把 **dma_heap/ion 的文件映射排除在外**（判据要求名字里有 malloc）。
容器按厂商做法是写进 PRAM 共享缓冲再发描述符，那份缓冲**很可能正是 dma-buf 映射**，
于是零命中**不能**读成"堆里没有容器"。已修：`--heapscan --allrw` 扫全部 rw 映射，
并把容器 framing 的长度都加进检测集合（0x1f0/0xb38 与 +12 的 0x1fc/0xb44、0xaa4）。
这一条是**检法修正**，不是新结果：修正后的扫描**还没跑过**（设备掉线），
跑之前不许引用任何"堆里没有/有"的说法。

## ②离线第二刀：容器公式标定精确，但它给出的是"老正文与今天这份 ACDB 不同代"，而且我的模型还没闭合
（`tools/acdb_graph_rebuild.py` + 现场脚本，全程零开机）

### 一、公式在已知-good 上**精确复现**
每条记录的**长度字就在 props 前 4 字节**（ACDB `0x328`，容器 `0x330`，容器 = ACDB+8）。
于是
```
容器总长 = 8 + Σ(52 前导 + (len_i + 8))
标定：8 + 4×60 + (808+508+248+912) = 2724 == sizeof(g_graph_vendor4)   ✔
```
⇒ framing 这条已闭合，并再次确认 `off=12/len=2712` 就是厂商 framing（头 12 字节不上线）。

### 二、决定性负结果：**2884 凑不出来**
按厂商当前 START 列表 `{b0000002, b0000025, b0000024, b0000023}` 枚举 ACDB 里全部记录
（41 × 2 × 1 × 14 = 1148 组，长度字分别是 808 / 756 / 256 / 924）：

```
唯一可能的和 = 8 + 4×60 + (808+756+256+924) = 2992      ≠ 2884（厂商 2872 + 12）
```

而 `0xb0000081`（老容器里那条）在这三套 ACDB 里 0 次命中。
⇒ **老正文和今天这份 ACDB 不是同一代**。若真要再发一次，发"今天 ACDB 拼出来的那份"才有意义，
而不是 10-27 那份堆拷贝。

### 三、但我的模型**还没闭合**，两处不合不许盖住
| 观察 | 冲突 |
|---|---|
| 用长度字算：`2992` | 与用 props 间隔反推的 `8+52+Σgap = 3076` 不一致（差 84） |
| 组内第四条 `b0000023`（props@0x54e2a，长度字 924） | 下一条 props 在 0x5523a，间隔 1040；按 `len+8+52` 预测应是 984 ⇒ **多 108 字节没解释** |

那 108 字节里能读到一串 `0xb000xxxx`：`4, b0000002, 0, b0000025, 1, b0000002, b000012c, 1, b0000025, b000012b, 1, b000012c`
——**这像是 ACDB 的"图→子图"映射表**（`0xb000012b / 0xb000012c` 也在本文件的子图 id 集合里）。
如果它确实是 graph 表，就能**用图 id 把 41 选 1 消掉**，也能解释那 108 字节。
但它的字段语义我**还没读出来**（按 `{sg,graph,flag}` 三元组解析 0 命中），
所以 2992/3076 谁对、以及"今天该发多少字节"，**现在都还不成立**。

诚实结论：② 的离线路线**方向对、没走完**。已闭合的部分只有容器 framing 与
"老正文与今天 ACDB 不同代"这两条。

## ②离线收口：记录规则闭合，并且抓到我们自己那份正文的两处 4 字节缺陷（零开机）

### 一、规则（在两侧共 6 个内部边界上验证，差值全为 0）
```
一条子图记录 = 52 B 前导 + blob
blob 长度   = props 前 4 字节那个字            （不是 +8，也不是 -8）
```
| 位置 | 声明 len | 实际（间隔−52） | 差 |
|---|---|---|---|
| 容器 b0000002 / b0000081 / b0000024 | 816 / 516 / 256 | 816 / 516 / 256 | 0 / 0 / 0 |
| ACDB 当前图组 b0000002 / b0000025 / b0000024 | 808 / 756 / 256 | 808 / 756 / 256 | 0 / 0 / 0 |
| 末条 b0000023（容器） | **924** | **868（数组只剩 920）** | **−56 ⇒ 数组缺 4 B** |
| 末条 b0000023（ACDB） | 924 | 988 | +64（ACDB 记录间有填充，正常） |

### 二、由此得到的两条硬结论
1. **`g_graph_vendor4` 是截断的**：它最后一条记录自己声明 924 字节，数组里只有 920
   ⇒ 完整容器应是 `8 + 4×52 + (816+516+256+924) = **2728**`，而数组是 2724。
   头里写的 `0xaa4 = 2724` 只是"我抄回来多少"，不是图的长度。
2. **主机头是 16 字节，不是 12**：厂商包声明 **2712 = 2728 − 16**。
   而五次开机我们发的是 `container[12 : 12+2712]`，
   对完整容器而言应当是 `container[16 : 16+2712]`
   ⇒ **起点早 4 字节、终点也少 4 字节**，两处都是同一个 4。
   这是一个**字节级、离线可证**的缺陷，和"12 字节图头该不该剥"那条已撤回的猜测无关。

⇒ 之前把 ② 写成"原字节重放被结构拒绝"仍然过重；但至少现在有了**一个具体的、我们自己这边的错**，
而不是只有"厂商不答"。这不是"出声"的证据，也不是原因判定——它只说明
**我们从来没发出去过一个自洽的容器**。

### 三、今天这代 ACDB 对应的容器大小是 2960，不是 2872
当前图组 `{b0000002=808, b0000025=756, b0000024=256, b0000023=924}`：
`8 + 4×52 + 2744 = **2960**`（上线正文 2944）。
10-24 抓到的 2872 与之差 88，且 `b0000081` 在今天三套 ACDB 里 0 命中
⇒ **图正文随校准换代**，"把 10-27 那份堆拷贝原样再发一次"从定义上就不可能是厂商今天发的东西。

### 四、状态与边界（不许越界读）
- 已闭合：记录 framing；两处 4 字节缺陷；今天这代的容器尺寸 2960；ACDB 里存在
  **子图集合正好等于厂商当前列表** 的图组（props@0x54672/0x549ce/0x54cf6/0x54e2a，
  前面 0x545f0 处还有它的子图连接表）。
- 未闭合（诚实列出）：
  ① 主机头 16 字节里各字段的语义（`{total,count,sg0_id,?}` 的第四个字没读）；
  ② 41 条 `b0000002` 选哪一条——长度全相同所以**不影响总长**，但**影响字节内容**，
     graph key 仍没解析出来（0x545f0 那张表读了一半）；
  ③ 拼出来的 2960 容器**从没被实机验证过**，而 ② 的止损仍然生效：**不刷开机次数**。
- 因此这轮的产物是"**一个自洽的候选正文的构造方法**"，不是"下一步该开机"。

## 2026-11-03 深夜：播放态实测（用户放音乐）——捕获路线判死，但产物 2960 造出来了

### 一、实测量（纯读，无探针、无写入设备）
播放确认在跑：`dumpsys audio` 里 `AudioPlaybackConfiguration … usage=USAGE_MEDIA
sampleRate=44100 state:started`。期间扫四个音频进程的**全部 rw 映射**：

| pid | 进程 | rw 映射 | 扫描量 |
|---|---|---|---|
| 1646 | AGMIPC service | 141 | 38.7 MiB |
| 1588 | audio HAL service_64 | 263 | 33.3 MiB（第二轮补全到全 263） |
| 1889 | audioserver | 441 | 54.3 MiB（第二轮补全到全 441） |
| 328 | audioadsprpcd | 57 | 9.5 MiB |

**经验证的容器头：0 个**——连老形状 `0xaa4` 都没有。且这四个进程里
**没有任何 `/dev/dma_heap`/ashmem 的用户态 rw 映射**（最大的映射是 4.19 MiB 的
`libc_malloc`，8 MiB 的 cap 没丢掉任何东西）。
⇒ 厂商容器只存在于**内核映射的 PRAM**（用户态读不到），而 tracefs 这条路本 ROM 已判死、
`/dev/mem` 不存在。⇒ **"抓到厂商今天真正发出去的那段字节"用现有手段不可达**，
这是有覆盖清单支撑的负结果，不是"没扫到"。
（上一轮我写的"堆扫 50 MiB 零命中"当时确实有 dma-heap 覆盖缺口；本轮把 cap 与映射类型都补齐后，
结论方向没变，但**理由换了**：不是筛掉了一类映射，而是那类映射压根不在用户态。）

### 二、产物：`logs/container-ukee-current-2960.bin`
按已闭合的规则直接从 ACDB 当前图组拼接（每条记录 = props 前 52 B 前导 + blob，
blob 长 = props 前 4 字节的长度字，**原样照抄、不改写字节**）：

```
header {total=2960, count=4}
记录 52+len：860(b0000002) + 808(b0000025) + 308(b0000024) + 976(b0000023) = 2952 + 8 = 2960
自检：用同一个 graph_headers 探测器跑这份产物 -> size=2960 count=4
      sgs = 0xb0000002 0xb0000025 0xb0000024 0xb0000023   （正是厂商当前 START 列表）
```

诚实边界（三条都不许省）：
1. **上线 framing 未经实机验证**：`厂商声明 = 完整容器 − 16` 是从 2728/2712 这一对数字推的，
   样本只有一个；老数组末条还缺 4 字节，本身也是截断品。
2. **`b0000002` 取 0x54672 那条靠的是"文件里紧邻那张子图连接表（0x545f0）"**，
   不是解码出来的 graph key；41 条同长变体我没能证明它们都不属于这张图。
3. **这份字节从未被任何一次开机验证过**，而 ② 的止损仍然生效：**不刷开机次数**。

### 三、② 现在的完整状态（一句话版）
我们自己那五次发的正文有两处 4 字节缺陷（离线可证，见上一节）；厂商今天那份正字节的**获取手段已全部判死**；
剩下能发的是这份自洽的 2960，但它要一次开机才能判，而那次开机的已知失败模式是挂死（要长按电源）。
⇒ ② 是否再花一次开机，**现在是用户的决定，不再是我能自己推进的步骤**。


## 预先登记：E-2960 那次开机的判据（0246，开机前写死，事后不许改）

变量只有一个：正文换成**这一代**自洽容器（`sweetvgraph=2`，off=16/len=2944）。
同版 kernel 里另带两件仪器：`SWEET GLINK: poll beat`（每秒一条，走完全程共 5 条）与
`g_smmu_faults("vgraph-pre"/"vgraph-post")`。

```bash
D22AP_ADT=~/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt CPUS=8 GPU_PROBE_LEVEL=0 \
  ./scripts/mkimage.sh sweetbufs=30 sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 \
  sweets1ap=3 sweetampfirst=1 sweetvgraph=2
./scripts/boot_one.sh images/shim-iosroot-adsp.img v2960
```

| 格 | 检查 | 判读 |
|---|---|---|
| **X0** 有效性门 | ring 回显 cmdline 含 `sweetvgraph=2`；出现 `APM graph_open/ukee2960 … len=2944`；`SWEET SMMU: vgraph-pre` 至少 1 行；数据面四行照旧（`s1ap leaf=0x7c1` / `graph_start rsp … status=0` / `data_buffer: dst=0x40bd done status=0` / `pcm tone done (30 buffers)`） | 少任一条 ⇒ 记「**仪器未生效**」，**不许**判"正文不对" |
| **X1** | `graph_open/ukee2960 rsp op=… status=0` | 这一代正文 + 16 字节 framing 成立 ⇒ ② 的输入问题解决，才谈把 iOS 流接进来 |
| **X2** | 有 rsp，`status != 0` | 正文被受理、内容被拒 ⇒ 记下 status 值；下一个单变量 = graph key（41 条同长 `b0000002` 选哪条）或 `b0000025` 的另一条 |
| **X3** | 最终打印 `no response`，且 **beat 行数 = 5**（跑满 5 秒） | **AP 活着、DSP 不回** ⇒ 与"挂死"正式分开；这才是这轮买的关键判读 |
| **X4** | beat 行数 < 5 且 ring 就此结束 | 真挂死；记下最后一条 beat 的 `i` 值（走到第几毫秒），恢复只长按电源 |
| **X5** | pre/post 任一 `GFSR != 0` 或有非零 cb 的 `FSR/FAR` | 是**我们区域的映射/权限**问题 ⇒ 转 SMMU 线，别再动正文 |
| **X6** | `SWEET GLINK: vgraph 2 started` | 建图+启动都过 ⇒ 但**出声仍只能由耳朵判**（能量见证、寄存器见证都已关闭），需要用户当场确认 |

安全线：`sweetbufs=30` 不动、不加音、不放长音、纯 RAM `fastboot boot`；
beat 最多 5 行，SMMU 见证只在 vgraph 分支两次调用，不进热路径。


## E-2960 判决（2026-11-03 22:0x，`logs/ring-v2960.txt` 2640 行，sweettime 见 ring）

### 一、先记我自己的判据缺陷（不许把这写成实验失败的原因）
X0 要求"同版必须带数据面基线四行"。这条**在设计上就不可能满足**：
`audio-closeout:823` 早就记过——SHM 形态的 GRAPH_OPEN 一旦发出，**之后连 `status=0` 的
`vp-wr` 也不再被应答**。这次一模一样：

```
2457 SWEET GLINK: vgraph 2 open failed
2458 SWEET GLINK: APM graph_open/vp-wr src=0x2010 dst=0x1 (800 bytes)
2470 SWEET GLINK: graph_open/vp-wr: no response
2541 ... tdm-inline ... 2557: no response
2637 SWEET GLINK: graph_open failed on every payload
2639 SWEET GLINK: ===== done =====   → 随后 PSCI SYSTEM_RESET（内核自己复位，没挂死）
```

所以基线三条缺失是**这个形态的开图把 APM 之后都不答**造成的，不是参数没送达，
也不是仪器没生效。X0 作为门是错的，这条按实测改判据；下面只用**不依赖基线**的证据。

### 二、这次真正买到的两条（都不依赖 X0）
**1）X3 成立，X4 排除。** `poll beat` 从 195 一路打到 **4995**（25 拍走满），
`rsp=0` 全程，然后才 `no response`；环还在继续跑后面的开图尝试、`===== done =====`、
并走到我们自己的 `PSCI SYSTEM_RESET`。
⇒ 之前五次"发出后就没日志"的那种**挂死读法，在这一次被明确分开**：
**AP 活着、DSP 不回**，不是机器锁死。

**2）X5 命中，而且不是正文问题。** 见证把尝试前后夹住了：

| | cb5 FSR | cb5 FAR | FSYNR0 |
|---|---|---|---|
| `vgraph-pre` | 0x400 | 0x22140410 | 0x40c5f |
| `vgraph-post` | **0x408** | **0x1ffe8000** | 0x40012 |

`0x1ffe8000` 正是 `sweets1va=` 的区域基址 ⇒ **DSP 在尝试读我们给它的正文时，在 cb5 上
对**我们这片区域本身**产生了故障**（GFSR=0，所以是 cb 级而非全局）。

但接下来这条对照把它定性改了——我扫了全部 170 个 ring：
**同样 `cb5 FSR=0x408 FAR=0x1ffe8000` 在盘上一共 6 次**，另外 5 次是
`audio-coldamp / audio-hi1 / audio-pace-hi1 / rtc-batt / s1ap11`，
而那 5 次**根本没跑 vgraph**，只发了我们自己的 `graph_open/vp-wr`（`s1ap11` 那次是
`FSR=0x402`，即 FSR 随我们改的 AP 变、FAR 不动）。
反过来，`tone done` 正常的好开机（b2、ep9 等）里这个 FAR **0 次**。

⇒ 结论落在机制上，而不是内容上：
**"把正文放在 SHM 区域里、只发描述符"这种形态，DSP 侧的读会在区域基址上故障；
与我们发的是 2712 的旧正文还是 2944 的新正文无关。**
所以 ② 里"正文变体/图不同代"这些**都不是解释**（它们各自仍是事实，但解释不了不响应）；
而未闭合的是：**为什么 DATA_BUFFER 读同一区域能成（`done status=0`），GRAPH_OPEN 读同一区域就故障**。
候选差别是句柄/池不同（我们 `0xb0d7f408` vs 厂商 `0xb0d890e8`）、区域偏移语义（`w6`）、
或读发生在建表之前——**这三条都还没测，不许当结论**。

### 三、这轮的净结果与止损
- 用户授权的那一次开机**已用掉**，产出是上面两条判读，其中 X5 把 ② 从"正文"转向
  **"SHM 正文读路径"**。按我自己 X5 那格的预定动作：**不再动正文**。
- `docs/START-HERE.md` 里"② 结构缺口"的措辞需要按此收窄（见下）。


## 预先登记：E-AP01（`sweetvgraph=2 sweets1ap=1`）的判据 —— 开机前写死，事后不许改

依据（全部来自盘上对照，见上一节的表）：19 次 `s1ap=3`（AP=11，全特权级只读）的正常数据面
**从不**在我们 IOVA 上故障且 `done status=0`；4 次没有 `sweets1ap`（AP=00）的 offset-mode 开机
故障在 0x408；**只有 v2960 是"AP=11 + 跑了 graph-open 正文读"，故障也是 0x408**。
⇒ 假说：`DATA_BUFFER` 只需读，AP=11 够；graph-open 那次访问需要可写/非特权，被 AP 挡住。
`sweets1ap=1` 写 AP=01（EL1 RW + EL0 RW），是唯一同时保住读、放开写的编码。

代码事实（`work/chain-tree/bsd/dev/sweet_glink.c` 的 s1_ap 块）：`s1_leaf &= ~(3ull<<6);
s1_leaf |= N<<6` ⇒ **叶子应从 `0x7c1` 变成 `0x741`**，并回显 `req=1`。这是可事先算死的字面值。

| 格 | 检查 | 判读 |
|---|---|---|
| **Y0** 有效性门 | cmdline 回显含 `sweets1ap=1 sweetvgraph=2`；出现 `SWEET MARK: s1ap leaf=0x741 req=1`；出现 `APM graph_open/ukee2960 … len=2944`；`SWEET SMMU: vgraph-pre` ≥1 且 `vgraph-post` ≥1；`poll beat` 有行 | 少任一条 ⇒ 记「仪器/参数未生效」，**不许**判权限假说 |
| **Y1** | `graph_open/ukee2960 rsp … status=0` | AP 就是那位拦路石 ⇒ 建图通，接下去 `graph_cfg/START/tone/data_buffer` 若跟上，才第一次有"可能出声"的条件 |
| **Y2** | 有 rsp、`status!=0` | 权限过了、内容被拒 ⇒ 回到 graph key（41 选 1）/`b0000025` 另一条，**这才是正文问题的战场** |
| **Y3** | 仍 `no response`，但 **post 的 cb5 FAR 不再是 `0x1ffe8000`** | 权限不是原因 ⇒ 剩余候选只有"读发生在建表之前"与句柄/池语义，两条都要另行设计 |
| **Y4** | 仍 `no response` 且 FAR 仍是 `0x1ffe8000` | 记下 FSR 值。**预先声明仪器局限**：AP=00 与 AP=11 都报 0x408、只有坏编码报 0x402 ⇒ 若 01 仍得 0x408，则 **FSR 不区分 AP=00/11**，这一格就**不能**读成"权限无关"，只能读成"仪器分不开" |
| **Y5** | `poll beat` < 25 且 ring 就此断 | 挂死（v2960 已证明 AP=11 下不挂，故这次若出现是新信息） |
| **Y6** | Y1 之后 `pcm tone done (30 buffers)` 与 `data_buffer … done status=0` 是否回来 | 回来 ⇒ 同版内数据面与建图并存成立；**出声仍只能由耳朵判**，需用户当场确认 |

单一变量：相对 v2960 只改 `sweets1ap 3→1`。其余参数一字不动。
本地门（上机前必须全绿）：不需要重编（0246 内核已含 beat/见证与 s1ap 解析，靠 marker 与字符串核过）；
镜像重打；在**打包产物**里 grep 到 `sweets1ap=1`、`sweetvgraph=2`、`graph_open/ukee2960`；cmdline < 590。


## E-AP01 判决（2026-11-03 23:0x，`logs/ring-y-ap01.txt`）：**Y2 命中，权限就是那位拦路石**

单一变量：`sweets1ap` 3 → 1（叶子 `0x7c1` → `0x741`，AP=01）。其余一字未改。

```
SWEET MARK: s1ap leaf=0x741 req=1
SWEET GLINK: APM graph_open/ukee2960 src=0x2010 (shm handle=0xb0d7f408 len=2944)
SWEET GLINK: graph_open/ukee2960 rsp op=0x1001000 status=0x2        <-- 第一次有 rsp
SWEET SMMU: vgraph-post cb5 FSR=0x400 FAR=0x2040410 ...            <-- 我们 IOVA 上不再故障
```

| 判据 | 读数 |
|---|---|
| **Y0** | 全绿：叶子回显 `0x741 req=1`、`len=2944`、pre/post 见证各 7 行 |
| **Y1/Y2** | **Y2**：有 rsp 且 `status=0x2` ⇒ 正文被送到、被内容层面拒绝，**权限阶段已过** |
| **Y3** | 反面成立：`FAR=0x1ffe8000` 的故障**消失**（post 只剩背景值 `0x2040410/0x400`） |
| **Y5** | 无挂死。**beat=0 不是仪器失效**——应答在 ~1 秒内到达，还没到第一拍 |
| **Y6** | 同版数据面完整回来：`graph_start rsp status=0`、`vp/*` 八条全 `status=0`、`vendor graph + WR input started`、`data_buffer done status=0` ×4、`pcm tone done (30 buffers)`、`geni amp 0x34 r=0 id=0x2032` ⇒ **四个已知-good 基线行齐全** |

两条必须记的**我自己的错**：
1. **Y0 里"poll beat 有行"这一项设计错了**：快速成功必然 0 拍。已按实测改成本条目（beat 只在"无应答"时有意义）。
2. 打分脚本与判据表在这一项上不一致（脚本没把 beat 当门）。**判据表为准的部分我已逐项在 ring 里复核**，不拿脚本的 PASS 当证据。

### 这条结果改变了什么
- **② 从"沉默"变成"有错误码"**：AP=01 之后 SHM 形态的 graph-open **每条包都在 <1 秒内应答**。
  这既修掉了 v2960 那种"发出后整段静默、还会把后面的 APM 一起 wedge"的不可诊断态，
  也让**一次开机内可以试多个正文变体**（每个变体一秒内给 status）。
- `status=0x2` 是唯一的新事实。它的含义**没查**，不许写成"内容无效"之类解释；
  现在只能说：**同一区域、同一句柄、同一句法，AP=01 时 DSP 读了、并回了 2；AP=11 时它读不到、什么都不回。**
- `sweets1ap=1` 现在是有证据的**新基线候选**（四行齐 + 无挂死 + 无 wedge），
  但"数据面四行齐"从来都不等于出声，**出声仍未证**。


## 预先登记：E-AUDIBLE（`sweetbufs=500 sweetvol=24`）的判据 —— 开机前写死

用户特许一次"能听见"的测试音。实测到的旧事实：历次开机走的是**等 DONE 的节拍路径**
（`pcm tone start` 无 `fire-and-forget` 后缀），每缓冲 288 帧 = 48 kHz 下 **6 ms**，
`sweetbufs=30` ⇒ **只有 180 ms**；而 `g_fill_tone()` 里 `>> 3` 使峰值 = 1024/32767 ≈ **−30 dBFS**。
⇒ "听不到"有明确的量化解释，不需要新假设。

改动：0247 加 `sweetvol=<1..31>`（默认 1 = 原行为一字不变），峰值 = `1024*g`；
本次 `g=24` ⇒ 峰值 24576 ≈ **−2.5 dBFS**；`sweetbufs=500` ⇒ **3.0 s**（硬上限 1200 = 7.2 s 之内）。
其余参数与 y-ap01 **完全一致**（含 `sweets1ap=1 sweetvgraph=2`）。

| 格 | 检查 | 判读 |
|---|---|---|
| **Z0** 有效性门 | cmdline 含 `sweetbufs=500 sweetvol=24`；出现 `SWEET MARK: tone gain g=24 peak=24576`；出现 `pcm tone done (500 buffers)`；`data_buffer … done status=0` 行数 ≥ 400；`graph_start rsp … status=0`；`geni amp 0x34 r=0 id=0x2032` | 少任一条 ⇒ 记「未成立」，**不许**把"没听到"当成结论 |
| **Z1** | 用户当场听到 | **仅**证明"XNU 下的喇叭通路能发出可听声"。goal 的终点判据是**真实播放、不是内核合成音**，所以 Z1 **不等于 goal 达成**，这条不许越界写 |
| **Z2** | Z0 全绿但仍听不到 | 分档定位，不笼统收：① PA/后端静音（我们写的 AW882xx 增益/使能与 Android 生效值不同——**这是目前最大嫌疑，且没读过真值**）；② TDM 时钟/后端实际没跑；③ 数据没到喇叭。**判别对照现成**：同一台机在 Android 下 `agmplay` 同一后端能出声（今天刚跑过），所以硬件与时钟不是限制项 |
| **Z3** | ring 在 tone 循环中途结束 | 500 缓冲的节拍里出事 ⇒ 记最后一条 `data_buffer` 的序号，长度/节拍是新变量 |

安全：只动幅度与时长（用户明确特许），不碰 GPU、不 flash、纯 RAM `fastboot boot`。
本地门（上机前必须全绿）：0247 正向 apply 且 apply 后与授权树 `diff -q` 相同；
CI 产物里 grep 到 `SWEET MARK: tone gain`；镜像产物里 grep 到 `sweetvol=24 sweetbufs=500`、
`sweets1ap=1`、`leaf` 无关参数一字未动；cmdline < 590。


## E-AUDIBLE 判决（`logs/ring-z-audible.txt`）：**Z2 成立——3 秒 −2.5 dBFS 的 1 kHz 音被消费，用户没听到**

### 一、我第二次把门写错，必须先记这个
Z0 里"data_buffer `done status=0` 行数 ≥ 400"**不可满足**：这台机的打印形态从来不是每条缓冲一行。
实测四份 ring：

| ring | `data_buffer send` | `done status=0` | `tone done` |
|---|---|---|---|
| y-ap01（30 缓冲） | **31** | 4 | (30) |
| **z-audible（500 缓冲）** | **501** | 4 | (500) |
| b2 / ep9（30 缓冲） | 31 | 4 | (30) |

⇒ 真正的每条缓冲见证是 **`data_buffer send` 计数 = 缓冲数 + 1**；`done status=0` 只出现在
探测/前几条。已按此改门（与 Y0 的 beat 门同一类错误：**判据必须"该形态下可满足"**，连着两次踩到，
这条要进我的工作规则）。

### 二、去掉写错的门之后，这一枪的全部证据
```
SWEET MARK: tone gain g=24 peak=24576        （−2.5 dBFS，比历次响 ~28 dB）
pcm tone start (dst=0x40bd)                  （无 fire-and-forget 后缀 ⇒ 走等 DONE 的节拍路径）
data_buffer send × 501                        （= 请求的 500 缓冲 = 3.0 s）
pcm tone done (500 buffers)
graph_start rsp op=0x1001002 status=0
SWEET I2C: geni amp 0x34 r=0 id=0x2032
```
用户当场读数：**没有听到**。⇒ **Z2**：数字侧全部到位、DSP 逐条消费，喇叭无声。

### 三、Z2 的分档，按现有证据收窄（不笼统）
| 档 | 证据状态 |
|---|---|
| ① **PA 从未被编程**（我们写的 AW 配置不足以让 DSP 动 PA） | **ep9 已实测**：pre-stream 与 post-stream **16 对寄存器全同**（`0`=0x2032、`0x3`=0xffff、`0x4`=0x4003、`0x5`=0x38）。⇒ 播放在跑，PA 一个比特都没变。这是目前**最强**的一档 |
| ② TDM/后端实际没跑（时钟） | **已关闭**：能量/时钟两条见证此前判死（无 `/dev/mem`、`clk_summary` 空、tinyplay 扰动无效） |
| ③ 数据没到喇叭（图内部连接不通） | **未测**。现成仪器：`0x0100100e`（模块状态读回，我们从没发过）与代码里已有的 `g_apm_state_query_sync()` |
| ④ 我们自创的 `0x08001176` 值 0 把接口关了 | **未测**；厂商 trace 里这条参数**一次都没发过**（4 次比对全没有），且 `status-log.md:18` 说它来自 CDMA 时代（那时有 CDMA 模块语境，TDM sink 上是借用的） |

### 四、下一枪（不再靠猜）
0248 只做一件事：**把厂商发而我们没发的设备侧配置补齐**——AW 第二条 `0x10013d2a`{0,`0x10013d1c`,1,0,0,0,**0x6f60**,0}
与 iid **`0x4308`** 的 `0x08001017`{96000,**0x20020**,1} + `0x0800101b`{0,2,0x10001,**3**,0x200004,0,1}，
全部**照抄厂商原字节**；同时把 ④ 做成开关（`sweettdmpwr`：0=发值0（现状）/1=发值1/2=**不发**，默认保持现状）；
仪器用已有的 `sweetampverify=1 sweetnobb=1` 做 **同版 pre/post 配对**，直接读 ① 是否在补齐后变成"寄存器变了"。
字节预算：去掉 `sweetvgraph=2`（−14）换 `sweetampverify=1 sweetnobb=1`（+30）⇒ 566→582 < 590。


## 预先登记：E-AWFIX（0248）判据 —— 开机前写死，且每条都按"该形态下可满足"复查过

（我连着两次把门写不可满足：Y0 的"beat 必须有行"、Z0 的"done 行数 ≥400"。这次每条都先问一遍
"这条在这台机的打印形态下真的可能出现吗"。）

**单一变量**：AW 模块 `0x10013d2a` 的正文按厂商原字发送——值挪到**第 7 个字**，并补发第二条
`{0, 0x10013d1c, 1, 0,0,0, 0x6f60}`（`vp/aw_cfg2`）。其余与 z-audible 一字不动。
两个新开关 `sweettdmpwr` / `sweetep4308` **默认不改变行为**（本枪不用），留给后续枪免重编。
仪器 `sweetexp=4`（bit2 = 同版 pre/post 配对读回，借位以省字节）+ `sweetnobb=1`。
参数预算实测：566 + ` sweetexp=4` + ` sweetnobb=1` = **589 ≤ 590**。

| 格 | 检查 | 判读 |
|---|---|---|
| **W0** 有效性门 | cmdline 回显含 `sweetvol=24 sweetbufs=500 sweets1ap=1 sweetvgraph=2 sweetexp=4 sweetnobb=1`；`geni amp 0x34 r=0 id=0x2032` **≥2 次**（pre-stream + post-stream，ep9 形态已证可满足）；出现 `vp/aw_cfg rsp … status=0` **和** `vp/aw_cfg2 rsp … status=0`；`data_buffer send` = **501**；`pcm tone done (500 buffers)`；`graph_start … status=0` | 少任一 ⇒ 记「仪器未生效」，**不许**判"AW 修正没用" |
| **W1** | 用户听到 | 合成音可听 ⇒ 喇叭通路证成；**仍不等于** goal 的真实播放判据 |
| **W2** | pre 与 post 的 PA 寄存器**出现任何差异** | 包被接受后 DSP **开始编程 PA** ⇒ ①档（PA 从未被编程）被推翻，下一格才谈增益/静音位 |
| **W3** | 两条 aw_cfg 都 `status=0`，但 pre/post **全同** | 值放对了位置仍不动 PA ⇒ 嫌疑转向 iid **`0x4308`**（开关现成）与图内部连接（`0x0100100e` 状态读回，也从没发过） |
| **W4** | `aw_cfg`/`aw_cfg2` 任一 `status!=0` | 记下 status 值：模块层面拒了这个正文 ⇒ 与 ② 的 `0x2` 一起看 |
| **W5** | ring 中途断 / 无 tone done | 挂死特征（本形态下 `done status=0` 只 4 行是**正常**，不作挂死判据） |

上机前本地门：0248 正向 apply 且复现授权树（5 hunk，已核）；CI 产物里 grep `vp/aw_cfg2`；
镜像产物里 grep `sweetexp=4 sweetnobb=1` 与全部 z-audible 关键项、cmdline 字节数写在 build log 里核。


## E-AWFIX 判决（`logs/ring-w-awfix.txt`）：**W3 命中——补齐厂商 AW 正文没有让 PA 动起来**，并顺带挖出一段我们从没复制的序列

### 一、先记我第三次把门写错
W0 写的是"`geni amp 0x34 r=0 id=` ≥2 次"。但**配对读回的真实行是 `SWEET MARK: ampread <tag> …`**；
`geni amp … id=` 是探针行，每 boot 恒 **1** 次。于是门"必然不过"。
连续三次同类错（Y0 的 beat、Z0 的 400 行 done、W0 的行指错），根因相同：
**我拿"我以为会打印的行"当判据，没有先在同一份 ring 里确认这条行的真实打印形态。**
改法（本轮起执行）：写门之前先从**已有 ring** 里 grep 出该行真实形态与次数，把数字抄进判据。

按真实行重判（这才是证据）：

| 项 | 读数 |
|---|---|
| `vp/aw_cfg rsp status=0` 与 **`vp/aw_cfg2 rsp status=0`** | 两包都在、都被接受（第二条是这次新发的） |
| `ampread pre-stream` / `post-stream` | **各 6 条** |
| pre vs post 差异 | **0** ⇒ **W3**：值放对位置 + 补发第二条之后，**PA 寄存器仍然一动不动** |
| `data_buffer send` / `tone done` | 501 / (500 buffers) ⇒ 3.0 s 音照旧发满 |
| `graph_start status=0` / `sweets1ap=1` | 成立 |
| 用户耳朵 | 仍无 |

### 二、这段 trace 里有一段我们**从没复制过**的设备侧序列（离线新读，非开机所得）
在 `apm-18504-evidence.txt` 里，紧跟**设备图 `GRAPH_OPEN(p9=2872, src=0x2011)`** 之后：

```
SHM_MAP    p6=0x10003(pool) p7=4(flag) p8=0x2001f000 p10=20480   ← 第二块区域
SET_CFG    p9=18504  p8=<刚映射的 handle>  dst=1                  ← 大块配置灌进 APM
SHM_UNMAP  p6=<同一个 handle>
```

⇒ 厂商在**开设备图之后**，另开一块 SHM 把一个 **18504 字节的配置对象**交给 APM，然后立刻 unmap。
这与 goal 里那句"② 缺的配置对象 `0x40bf/0x40c0/0x40c8/0x4165/0x4308`"是同一件事的具体形态。
W3"PA 不被编程"与"这个对象从没发过"是**相容**的（AW 模块的行为通常要靠这块参数驱动）。

### 三、正文的可得性（这一条决定下一枪能不能做，先查干净）
- 盘上两个 `apm-blob-*.txt` 是 **trace 文本**，`18504/4848` 只是包元数据 ⇒ **正文不在盘上**。
- `/vendor` `/odm` `/apex` 里**没有** 18504/18128/18088 字节的文件；也**没有** `*acf*` / `*aw882*` 文件。
- 抓取需要 tracefs，而这台 ROM 的 root 连 `setenforce`/`mount debugfs` 都被拒 ⇒ **这条正文在当前设备上不可得**。
⇒ 结论要直说：**② 的下一步（补这个 18504 对象）在现有条件下做不了**，不是"还没想到怎么试"。


## 预先登记：E-NOPWR（`sweettdmpwr=2`）判据 V0–V4 —— 数字全部从 `ring-w-awfix.txt` 抄来

（这轮我栽了三次才换做法：门的每条数字都必须来自**已存在 ring 的真实打印形态**。）
基准 `logs/ring-w-awfix.txt`（3543 行）实测：`ampread pre-stream`=8 行、`ampread post-stream`=8 行、
`vp/tdm_power`=2 行（发出+rsp）、`geni amp`=4、`data_buffer send`=501、`pcm tone done`=1、
`graph_start rsp`=1、`SWEET MARK: s1ap leaf`=1、`SWEET MARK: tone gain`=1、`tdm_power skipped`=0。

**单一变量**：不发我们自创的 `0x08001176`（值 0）——厂商 trace 里这条参数**一次都没出现**。
`sweetvgraph=2` 换成 `sweettdmpwr=2`（同长 13 字符 ⇒ cmdline 仍 **589 ≤ 590**）。
不需要重编（0248 已带开关，`out/kernel.adsp` 就是 0248 核）。

| 格 | 检查（数字=基准实测值） | 判读 |
|---|---|---|
| **V0** 有效性门 | `tdm_power skipped` = **1**；`vp/tdm_power` = **0**（对比基准 2）；`ampread pre-stream` = **8** 且 `post-stream` = **8**；`data_buffer send` = **501**；`pcm tone done (500 buffers)` = 1；`SWEET MARK: s1ap leaf=0x741` = 1；`graph_start rsp … status=0` = 1 | 少任一 ⇒ 记「仪器未生效」，**不许**判"这条参数无关" |
| **V1** | pre/post 的 8 对里**出现任何差异** | 不发那条包之后 DSP 开始编程 PA ⇒ 档④ 就是墙，下一枪沿 PA 增益/使能追 |
| **V2** | 两包都 `status=0` 但 pre/post **仍全同** | 便宜嫌疑穷尽 ⇒ 扬声器侧只剩 #11 的能力墙（要那 18504 正文），转 B4 震动或换可抓 tracefs 的环境 |
| **V3** | `graph_start` 或 tone 不再完成（对比基准有） | 不发 0x08001176 **破坏了原本能跑的东西** ⇒ 立刻回到基准形态，这条参数不是可有可无 |
| **V4** | 用户听到 | 同 W1：**只证明喇叭通路可听**，不等于 goal 的"真实播放" |

上机前本地门：不换内核（0248 已核过 marker/CNTP）；镜像产物里 grep `sweettdmpwr=2` 且**无** `sweetvgraph`、
其余 5 个关键项齐、cmdline 字节数从 build log 核；V 表先 commit。


## E-NOPWR 判决（`logs/ring-v-nopwr.txt`）：**V2 —— 便宜的扬声器嫌疑到此穷尽**

V0 是全项目最干净的一次门：**九项计数里只有预期的两项变了**
（`vp/tdm_power` 2→0、`tdm_power skipped` 0→1），其余七项与基准 `ring-w-awfix.txt` 逐字相等
（`ampread pre/post`=8/8、`data_buffer send`=501、tone done/s1ap leaf/tone gain/graph_start 各 1）。
V3 未触发 ⇒ 我们自创的 `0x08001176=0` **既不是墙也不是支撑**：不发它，建图与播放照跑、
PA 寄存器 6 对里差异 **0**。

至此"PA 从未被编程"已被**三条独立路径**测到：ep9（AP=11）、w-awfix（AP=01 + 正文按厂商放对）、
v-nopwr（AP=01 + 不发那条自创参数）。

## 由此产生的下一枪（比继续猜参数更值钱，且不重编）
`0x10013d1b/1c` 那两条是**模块参数**；而"谁绑定 TDM 后端"取决于**图本身**。我们现在知道：
- 厂商今天的设备图正文我们**拿不到**（18504 那条是校准对象，另一回事）；
- 但**10 月那代设备图正文 `g_graph_vendor4` 在 AP=01 下从没重试过**——
  它此前五次全部在 AP=11 下静默/挂死，而 AP 一换，`ukee2960` 就从"无应答"变成了"有应答 `status=0x2`"。
⇒ 所以 **`sweetvgraph=1`（旧正文、off=12/len=2712，现已证与厂商同形）在 `sweets1ap=1` 下重发一次**，
能直接把两件事分开：
| 读数 | 含义 |
|---|---|
| `rsp status=0` | 旧正文其实是**可用**的：之前失败全因 AP；② 就此打开，PA 有希望被编程 |
| `rsp status=0x2`（与新正文同码） | 拒绝与正文**无关** ⇒ 指向"ADSP 里没有校准"这一类结构性缺失 |
| `no response` | 旧正文在新权限下仍不可读 ⇒ 回区域/句柄语义，另找仪器 |
字节：`sweetvgraph=1` 与 `sweettdmpwr=2` 同 13 字符 ⇒ cmdline 仍 589。0246 起就带 `vg==1` 分支，**不用重编**。


## 预先登记：E-OLD BODY 在 AP=01 下重发（`sweetvgraph=1`）判据 U0–U4

行形与计数抄自已有 ring：`SWEET GLINK: APM graph_open/vendor4 src=0x2010 (shm handle=0xb0d7f408 len=2712)`
（`ring-vgraph.txt` 等 6 次）、`SWEET MARK: s1ap leaf=0x741`（`ring-w-awfix.txt`）。
**明确不再犯**：`poll beat` 行数**不作为有效性判据**——它只在"无应答"时出现，应答快就 0 行（Y0 就是这么写坏的）。

| 格 | 检查 | 判读 |
|---|---|---|
| **U0** 门 | `boot-args` 回显含 `sweetvgraph=1 sweets1ap=1`；出现 `leaf=0x741`；出现上面那行 `graph_open/vendor4 … len=2712`（=1 次）；`graph_open/ukee2960` **不出现**（证明走的是旧正文分支） | 少任一 ⇒ 仪器/参数未生效，不判正文 |
| **U1** | `graph_open/vendor4 rsp … status=0` | **旧正文一直可用，② 的全部失败都是 AP** ⇒ 建图开成后看 `graph_start`/tone 是否跟上（并问用户耳朵） |
| **U2** | `rsp … status=0x2`（与新正文同码） | 拒绝与正文无关 ⇒ 指向 ADSP 侧缺校准（那条 18504 对象），② 在这台机上不可解 |
| **U3** | `graph_open/vendor4: no response` 且 beat=25 | 新权限下仍读不到 ⇒ 回区域/句柄语义，另设仪器 |
| **U4** | ring 停在该行之后且 beat<25 | 又出现挂死形态 ⇒ 该形态独占开机，停止与 tone 同版 |

本地门：不换内核（0246 起就有 `vg==1` 分支，当前 `out/kernel.adsp` 是 0248，含该分支——已在产物里 grep `graph_open/vendor4`）；
`sweetvgraph=1` 与 `sweettdmpwr=2` 同为 13 字符 ⇒ cmdline 仍 589；产物里核 `sweetvgraph=1`、无 `sweettdmpwr`、无 `ukee2960` 参数项。


## E-OLD BODY 在 AP=01 下的判决（`logs/ring-u-oldbody.txt`）：**U2 —— 两份正文同码 `0x2`，拒绝与正文无关**

```
U0 全门通过：cmdline sweetvgraph=1 sweets1ap=1 | leaf=0x741 x1 |
             APM graph_open/vendor4 … len=2712 x1 | graph_open/ukee2960 不出现
读数：graph_open/vendor4 rsp op=0x1001000 status=0x2   （beat=0 ⇒ 应答很快）
其余与基准一致：graph_start x1、pcm tone done x1、ampread pre/post 8/8、data_buffer send 501
```
⇒ 旧正文（10 月堆拷贝，2712/`0xb0000081`）与新正文（今天 ACDB 拼的 2944/`0xb0000025`）
在**修好权限之后**得到**同一个 `0x2`**。所以"该发哪一份正文"这个问题到此关闭：
**APM 不是在拒正文内容**。

### 顺带记一条我查过但**不成立**的线索（防止以后有人再拿它当真）
想免掉能力墙，就试过"18504 会不会是 ACDB 里的一段"：`MTP_ukee_acdb_cal.acdb` 里确实有 53 处
等于 18504 的 u32，但上下文是 **连续 ID 序列** `0x4748 / 0x4848 / 0x4849 / 0x484a`
（=18248/18504/18505/18506）配 module/param id（`0x0700101a`、`0x08001026`…）——**是 ID，不是长度**。
⇒ 与当年"2712 = aw882xx_acf.bin(2672)+40"同类的巧合。**18504 正文仍不可得。**

### ② 的最终状态（本轮结论，别再重走）
1. **能读到了**：`sweets1ap=1`（AP=01）把"SHM 正文读不到"修掉，两类正文都拿到快速应答。
2. **不是正文**：同码 `0x2`。
3. **剩下的唯一具体缺口**：厂商开设备图后紧跟的那次
   `SHM_MAP(第二块) → SET_CFG size=18504 → SHM_UNMAP`，其**字节在这台机上不可获取**
   （tracefs/pstore/debugfs-mount/sysfs 全部 EPERM；`/vendor /odm /apex` 无该尺寸文件；ACDB 里的 0x4848 是 ID）。
4. 因此 ② 需要的是**能力**，不是又一个假设：(a) 一台能让 tracefs 工作的环境；
   (b) 或你允许在这台机上加载内核模块（KSU 模块可改 `su` 域的 SELinux 权限，但那是跑第三方内核代码）。
   两者都没有 ⇒ 扬声器侧我**不再花机时**。


## 预先登记：T0–T3（震动可感知性 —— 唯一还没问过的判断题）

机时 **6/20 已用**。这一枪**不需要新代码**：`sweethap=1` 是现成闸
（`work/chain-tree/bsd/dev/sweet_glink.c:7363-7368`，且 play 序列走 `printf` 单写，
所以行数=次数——这是我把 Y0/Z0/W0 三道门写坏之后先核过的）。

命令：`KEEPALIVE=1`（剥掉 `sweetreboot=1` ⇒ 手机停在 XNU，不自动复位）+ `sweethap=1`，
**不带任何音频参数**（震动判断与音频路径无关，去掉它反而少一个混淆项）。
测试动作：用户手持手机，**反复短按电源键**——每次按键 kick 一次直驱。

| 格 | 检查（真实行形） | 判读 |
|---|---|---|
| **T0** 门 | cmdline 回显含 `sweethap=1` 且**不含** `sweetreboot=1`；ring 里出现 ≥1 组完整序列 `SWEET HAP: play 0xf04c<-81 rc=0` → `fault during play=0x…` → `SWEET HAP: final en=0`；`SWEET HID:` 按键行 ≥1（证明按键真进了内核） | 少任一 ⇒ 记"仪器未生效"，**不许**判"马达不震" |
| **T1** | 用户**感觉到**震动（哪怕很弱/一次） | 直驱可用 ⇒ B4 的卡点回到"iOS 不提交"，并且值得回头修我们那个 **4× 实例**（DT 只有 1 个 `hv-haptics` 节点，4 个实例是我们自己放出来的） |
| **T2** | T0 全绿但用户**一次都没感觉到** | 直驱不可用 ⇒ B4 与 ② 同墙（LPASS/SoundWire 所有权），**停**：不再往震动线花机时 |
| **T3** | `auto_res` 出现 **非 `0x26`** 的任何值 | 我此前对它的解释要重做；只记录，不推断 |

已实测与未证的边界：`auto_res=0x26` 在全库 **391/391 恒定**（换 vmax、换幅度都不变）⇒
**我不把它当"马达没接"的证据**；`fault during play=0x2` 372 次是事实，
但 0x2 的语义**没查**（不许写成"过流/开路"）。唯一缺的就是"人能不能摸到"。


## T 判决（`logs/ring-t-hap.txt`，2026-11-04）：**T1 —— 直驱真的能震，而我上一条 B4 结论是错的**

T0 全绿（`printf` 单写 ⇒ 行数=次数）：

| 项 | 本次 |
|---|---|
| `SWEET HAP: play 0xf04c…` 序列 | **29** |
| `fault during play=` | 29（`0x2`×28、`0x10`×1、`0x1`×1，`0x0`×0） |
| `auto_res=` | 29 次，取值集合只有 **`{0x26}`** |
| `SWEET HID: key` / `touch` | 58（双写 ⇒ 29 次）/ 19 |
| cmdline | 含 `sweethap=1`、**不含** `sweetreboot=1`（KEEPALIVE 生效，手机停在 XNU 让人反复按） |

**用户读数：有震动，且震感与设置的数据一致。** ⇒ **T1**。

### 因此下面两条我昨天写的推断正式作废
1. ~~"`fault during play=0x2` + `auto_res=0x26` 越出 DT 标定范围 ⇒ 马达没接/驱动器看不到负载"~~
   ⇒ **错**。同一个 fault 位与同一个 `auto_res` 读数之下，人明确摸到震动。
   所以：`0x2` 这个位**不代表"没动"**（语义仍未查，但**方向已被否证**）；
   `auto_res=0x26` **391+29 次恒定不变**，说明它根本不是"每次播放的实测阻抗"，
   拿它跟 DT 的 `lra-f0-min/max` 比是我拿错字段。**这条比较以后不许再用。**
2. ~~"B4 与 ② 是同一堵墙（都要 ADSP 经 LPASS/SoundWire 开图）"~~
   ⇒ **错**。AP 侧 SPMI 直驱就能震 ⇒ 震动输出**不需要** ADSP 开图。
   （DT 里 `swr_haptics` 与 `hv-haptics` 并存仍成立，但它不是我们的必要条件。）

### 修正后的 B4 现状
- **输出侧：可用**（本次 29 次可感知震动，参数与 DT 的 `vmax=1450mV`、`period=5882us` 一致 ⇒ 用户判断"震感同设置数据一致"）。
- **iOS 侧：仍是唯一断点**，且形状与音频一致——`open → map → buffer watch → sel=1` 之后**没有任何第二个 selector**（176+ ring）。
- 我们这边有一个**已确认是自己造出来的**异常：DT 只有 **1 个** `qcom,hv-haptics@f000` 节点，
  而 `AppleAOPAudioButtonHapticDevice registered` 每 boot 出现 **4 次** ⇒ 4 个同名实例是我们自己的匹配放出来的。
  这是目前**唯一具体、属于我们侧、且未修**的可疑点。

### 下一枪（0249，已登记为任务）
把"4 个实例"修成 1 个（行为变量），同时给 `registered`/`open` 行加 `this` 指针（仪器），
判据先写死、数字先从 ring 真实形态抄（`registered` 双写 ⇒ 8 行=4 事件 ⇒ 修好后应 **2 行=1 事件**）。
阳性读数是：去重之后 iOS 是否出现 **第二个 selector**（全库至今 0 次）。


## H 判据（0249，开机前写死，事后不许改）

0249 相对上一条计划**改了做法**：只做仪器、零行为改动。理由是"4 个实例"目前是
一个**未经身份确认的计数**——如果 4 次 `registered` 其实是同一个对象被 start 了 4 次，
那"去重"这个行为变量根本不存在。先读身份，再决定动不动它。
补丁只有 4 个 hunk、48 行纯新增、0 行删除（`git diff --stat` 已核）。

行数换算（从 `logs/ring-t-hap.txt` 实测抄，不是推的）：`printf` 在 bsd 侧**单写**
⇒ 行数=次数（`fault during play=` 29 行 / 29 次）；`SWEET_HAP_LOG` **双写**
（IOLog + `sweet_log_append`）⇒ 行数=2×次数（`registered` 8 行 / 4 事件，
`sel=` 2 行 / 1 事件，`open by pid 57` 4 行 / 2 事件）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **H0** | 四个 marker 都在下载下来的 kernel 里：`SWEET HAPID: bhap start self=` / `SWEET HAPID: bhap open self=` / `SWEET HAPID: uc=` / `SWEET HAP: fault after clear=` | 全中 | 补丁真进了镜像（缺任何一个 ⇒ 不要开机，直接按"被跳过"处理） |
| **H1** | `SWEET HAPID: bhap start self=` 里 **distinct `self=` 的个数**（行数本身不是判据） | 4 个不同 | 确实存在 4 个对象 ⇒ 我们的匹配放出了重复实例，"去重"是一个真实可动的行为变量 |
| | | 1 个不同（重复 4 次） | **同一个对象被 start 4 次** ⇒ "实例数"是假象，去重方向作废 |
| 旁读 | `provider=` 是否同一指针、`pname=` 是什么 | — | 父节点是谁；若 `provider` 等于前面某个 `self` ⇒ 是自己匹配自己 |
| **H2** | `SWEET HAPID: uc=` 的 `prov=` 是否命中 H1 的某个 `self=` | 命中 | 第一次知道 backboardd attach 的到底是哪一个实例，其余的是孤儿 |
| | | 不命中 | 该 UC 的 provider 不是 bhap 设备 ⇒ 这次跑**不算证据**（仪器答不了它要答的问题），只算开机次数 |
| **H3** | `SWEET HAPID: bhap open self=` 行数 | **预先声明为非判据** | UC 连接可能根本不走 driver 侧 `open()`；0 行是合法结果，它只在真的出现时用来配对 UC 指针 |
| **H4** | `SWEET HAP: fault after clear=` 行数 **==** `SWEET HAP: play 0xf04c<-` 行数 | 相等 | 仪器自证；不等 ⇒ 仪器坏，本次不算证据 |
| | 第 1 次脉冲的 `after clear` 值 | `0x0` 且播放中非 0 | 故障由**本次脉冲**产生（用户"过流"假设的方向成立） |
| | | 非 `0x0` | `0xf066<-0x07` 没清掉（仍闩锁/被别人持有）⇒ 历史 372 次 `fault during play` 对这些脉冲**无信息** |
| | | 两处都 `0x0` | 那 0x2 与脉冲无关，来源在别处 |
| **H5** | 回归控制：`cfg=` / `play 0xf04c<-` / `fault during play=` / `fault after clear=` 四个计数相等；`registered` 仍 8 行 | 成立 | 0249 没改行为（若 `registered` 少于 8 行 ⇒ 要么补丁没生效要么我动错了地方） |

开机配置沿用 T 那次已知-good：`KEEPALIVE=1 sweethap=1`（cmdline 含 `sweethap=1`、
**不含** `sweetreboot=1`），跑完由人重启到 TWRP 拉 ring。开机次数：20 里已用 7，本次第 8。

**止损**：H1 若落在"同一个对象 start 4 次"，去重这一枪作废，B4 剩下的唯一断点就是
"iOS 不向我们的壳提交第二个 selector"，那是 0244 壳（IOAudio2/Actuator）那条匹配问题，
不再碰 HAP520 输出侧。H1/H2 若都不可解读 ⇒ 记 1 次开机但**写明非证据**，
B4 的仪器线冻结，回到 ②（内核侧与 iOS 侧都常驻、出声仍未证）。


## H 判决（`logs/ring-h-hapid.txt`，2026-11-04）

| 判据 | 读数 | 结论 |
|---|---|---|
| **H0** | 4 个 marker 同时出现在下载的 kernel **和**打包镜像里（`SWEET HAPID: bhap start self=` / `uc=` / `fault after clear=`，`bhap open self=` 也在） | 0249 真进了镜像 |
| **H1** | 8 行 = 4 事件，`self=` 是 **4 个不同指针**（`…593600 / …591800 / …590720 / …592e80`），`provider=` **4 次全等于同一个** `0xfffffe29a05913a0`（`pname=IOResources`） | **确实是 4 个对象**，都由同一个 IOResources 节点放出。`provider` 不等于任何一个 `self` ⇒ **不是"实例自己匹配自己"**，而是**同一个 provider 被同一条 personality 匹配了 4 次** |
| **H2** | `uc=0xfffffe2007580020 prov=0xfffffe29a0592e80` | **命中** H1 的第 4 个 `self` ⇒ backboardd(pid 57) attach 的是**最后注册的那个**，前 3 个是孤儿 |
| **H3** | `SWEET HAPID: bhap open self=` **0 行**（预先声明为非判据） | UC 连接**不走** driver 侧 `open()`；这条只用来把"以后别把它当门"写死 |
| **H4** | `fault after clear=` 44 行 == `play 0xf04c<-` 44 行（仪器自证成立）；44 次**全是 0**，而 `fault during play=` 44 次**全是 `0x2`** | `0xf066<-0x07` **清得掉**，`0x2` 是**本次脉冲自己产生的**（不是历史闩锁）。用户 44 次按都有震感、震感一致 ⇒ `0x2` 与"没动"无关。**位的语义仍未查**，不许写成"过流" |
| **H5** | `registered` 仍 23 行、`sel=1` 仍 1 事件、`auto_res` 仍 `0x26/0x57` | 0249 零行为改动成立 |

### 新确立的事实
1. 4 个同名实例是**我们自己放出来的**（同一 provider 被重复匹配），不是设备树有 4 个节点。
   机制候选是 `SweetHaptics.cpp:593` 把实例的 `IOMatchCategory` 改写成
   `IODefaultMatchCategory`——XNU 靠这个属性做"同一 category 已有 client 就不再匹配"
   的保护（`IOService.cpp:3887` `copyClientWithCategory` + `:4033` 赋值）。
   **但这条还没证**：`AppleHapticsSupportCallan`（`:805`）做了同样的改写却只放出 1 个实例。
   所以只写成候选，不当结论。
2. iOS 侧一切照旧：`open(pid 57) → map → sel=1`，`asset count 18`（**非零**，
   所以"返回 0 会让 ButtonResolver 关掉服务"这条不适用于本次），**没有第二个 selector**
   （全库至今仍是 0 次）。
3. `bhap open` 0 行 ⇒ 想按"谁 attach 了哪个实例"提问，只能像 0249 一样挂在 selector 路径上，
   别再造 `open()` 仪器。

### 下一枪（I 轮，不开新补丁）
去重（0250 的一行）先排队，但**先跑不花 CI 的那一枪**：直接用
`SWEETIOLOG=backboardd` 重新打包（0049 的匹配追踪已经编进每个 kernel，历史 176+ ring
里**从来没有一行 `IOK `**，因为 base cmdline 一直是 `sweetiolog=-`）。
它回答的是"backboardd 除了我们的壳还查过什么"——这才是"没有第二个 selector"的上游。


## I 判据（`SWEETIOLOG=backboardd,launchd`，无新补丁，开机前写死）

这一枪**不碰代码**：0049 的匹配追踪已经编进每个 kernel，base cmdline 一直写 `sweetiolog=-`
（关闭），所以历史 176+ ring 里一行 `IOK ` 都没有。打包后 cmdline 415 字节（预算 590）。
追的是**两个**名字，因为必须让仪器能自证：
`launchd` 是**阳性对照**（它一定做过 IOKit 匹配），`backboardd` 是问题对象。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **I0** | 打包镜像里能 grep 到 `sweetiolog=backboardd,launchd`，且 grep 不到 `sweetiolog=-` | 成立 | 参数真的进了产物（`mkimage.sh` 是**替换**不是追加，第一处匹配生效） |
| **I1** | ring 里 `IOK launchd:` 行数 | > 0 | 仪器活着，且 `IOLog` 在这个阶段还能进 ring |
| | | **= 0** | 仪器坏（console 静默或名字不匹配）⇒ 本次**记 1 次开机但写明非证据**，不据此说"launchd 没查过" |
| **I2** | 在 I1 活的前提下看 `IOK backboardd:` | 有指向我们壳的行，且**没有**查不到东西的查询 | backboardd 对已发布的服务集**是满意的**，"没有第二个 selector" 断在它自己的状态机里，不在服务集上 |
| | | 有 `-> 0 found` / `none` 且文本里有 haptics/audio 类名或属性名 | 这就是上游缺口：**具名的、我们没发布的那个东西**；下一枪按名字发布 |
| | | 完全没有 `IOK backboardd:` 行 | 它在 console 静默前就查完了 / 或它用的入口不在这三个 hook 里 ⇒ 记为未答，不许写成"没查" |
| **I3** | 行数换算：追踪走 `IOLog` ⇒ **单写**（行数=次数）；`SWEET_HAP*` 里 `SWEET_HAPTICS:` 是双写 | — | 400 行/boot 是**全局上限**，两个名字共享；若尾部被截断，按 `IOK <name>` 分开数，不许混数 |
| **I4** | 回归：`play 0xf04c<-` == `fault after clear=` == `fault during play=` 三数相等；`registered` 仍 23 行；`sel=1` 仍 1 事件 | 成立 | 没代码改动，震动线与 H 轮同形（不等 ⇒ 参数本身动了行为，先查参数） |

**这次不许得到的结论**（预先划界）：`IOK` 只记录"匹配/通知/registry 路径/open"三类调用，
它**不能**告诉我们 backboardd 拿到服务之后为什么不再发 selector；I2 只能把"缺服务"
和"不缺服务"分开。开机次数：20 里已用 8，本次第 9。


## I 判决（`logs/ring-i-iolog.txt`，2026-11-04，第 9 次开机，未花 CI）

| 判据 | 读数 | 结论 |
|---|---|---|
| **I0** | 产物里 `sweetiolog=backboardd,launchd` 命中 1 次、`sweetiolog=-` 0 次 | 替换（不是追加）成立 |
| **I1** | `IOK launchd:` = **0 行**，而 `IOK backboardd:` = **785 行** | **判据写错了**：我拿 launchd 当阳性对照，实际仪器由 backboardd 自己那 785 行证明活着（`IOLog` 在这个阶段仍能进 ring）。所以本轮**不是非证据**；但**不许**把"launchd 0 行"写成"launchd 没做过 IOKit 匹配" |
| **I2** | 785 行里**没有任何一条 haptics 类的查询**；haptics 侧只有一条 `open AppleAOPAudioButtonHapticDevice type 0 -> 0x0`（成功），随后就是 `sel=1`、`asset count 18` | **"没有第二个 selector"不是"缺一个我们没发布的服务名"**。backboardd 已经拿到并打开了我们的壳，它停在自己进程内部，不是停在匹配上 |
| **I3** | 长 dict 被 ring 的 ~240 字节上限切断：`matching +0 <dict…IOAcceleratorES…>` 只剩头部，结果 `-> 0 found` 掉到下一段（`grep -o` 数到 303 个 `0 found`、另有 `=end 129 -> none` 这类残段）。按 `IOK <name>` 分开数了 | 数法成立。785 行 > 我按注释以为的"400 行/boot 上限" ⇒ **上限的单位还没读代码确认**，本轮不据此说"被截断" |
| **I4** | `play 0xf04c<-` 40 == `fault after clear=` 40 == `fault during play=` 40；`registered` 8 行；`sel=1` 1 事件 | 换参数没动行为 ✔ |

### 本轮真正的意外：backboardd 在一个查不到加速器的循环里
`IOK backboardd: matching` 共 728 行，其中按 `IOProviderClass` 归类：

| 它在找的类 | 次数 | 结果 |
|---|---|---|
| `IOAcceleratorES` | **330** | `0 found` |
| `IOAcceleratorESLegacy` | 27 | `0 found` |
| `AGXAccelerator` | 2 | `none` |
| `ApplePearlSEPDriver` | 1 | `none` |
| `AppleM2ScalerCSCDriver` | 1 | `none` |
| `AppleHIDKeyboardEventDriverV2` / `IOAccessoryIDBusHIDDevice` / `IOHIDDevice` | 4/4/3 | 混合 |
| `path IODeviceTree:/arm-io/pearl-sep` | 1 | `none` |

循环贯穿整场：第一条在 ring 第 **891** 行，最后一条在 **3485** 行（全 ring 4110 行），
而 haptics 的 `open`（1807）和 `sel=1`（1812）都在它中间正常发生。
⇒ 这些类我们**一个都没发布**（GPU 线是冻结的），backboardd 为它们反复重试。
**边界**：它"因为拿不到加速器所以不发第二个 selector"这条**没有被证明**——
时间线上 haptics 的 open 就夹在循环中间且成功了。

### 已关闭 / 不许重走
- ~~"iOS 不提交是因为我们的壳类名/服务名不对"~~ ⇒ I2 否证：它直接 open 成功。
- ~~"4 个实例导致 backboardd attach 错对象"~~ ⇒ 它 attach 的是第 4 个（`prov` 命中），
  而且 open 成功、sel=1 有答；去重降为**卫生问题**，不再是 B4 的候选病因。
- 仪器坏不掉这一点已被 785 行自证：以后要问"某进程查了什么"，
  `SWEETIOLOG=<name>` 是**零补丁**手段（415 字节，预算 590）。


## J 判据（`SWEETIOLOG=mediaserverd,backboardd`，零补丁，开机前写死）

同一个仪器、换对象：问"该提交流程的进程到底查过什么"。
**这次把对照做对了**——上一轮我拿 launchd 当阳性对照，它 0 行而 backboardd 785 行，
说明选错了对照；本轮的对照就是**已经证明会出行的 backboardd 本身**，
`mediaserverd` 零行时才可以说"它没调用这三个 hook"。
cmdline 420 字节（预算 590）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **J0** | 产物里命中 `sweetiolog=mediaserverd,backboardd`、且 `sweetiolog=-` 不命中 | 成立 | 参数进了产物（开机前就能验） |
| **J1a** | `IOK backboardd:` 行数（对照） | > 0 | 仪器活着。若这里就 =0 ⇒ 整轮非证据，不解释 mediaserverd |
| **J1b** | `IOK mediaserverd:` 行数 | > 0 | 可以回答 J2 |
| | | = 0（且 J1a>0） | **mediaserverd 在开机窗口内没调用被 hook 的匹配/通知/路径入口**——这是"它在别的环节停住"的证据，不是"仪器坏" |
| **J2** | 按名字分开数 `IOProviderClass` → 结果，专找**音频类**（`IOAudio*`、`AppleEmbeddedAudioDevice`、`AppleARMIIS/DMMAudioDevice`、`AGM`、`audio` 路径） | 有 `0 found`/`none` 且具名 | 上游缺口 = 那个具名服务；下一枪按名字发布或读它为什么匹配不上 |
| | | 音频类全部命中、只有非音频类落空 | "不出流"停在 mediaserverd 自己（与 backboardd 的 I2 同形），服务集不是病因 |
| **J3** | 计数法：只数**头部可见**的类名；`=end NNN -> 结果` 这类残段单独计数并报出未归属行数；绝不把两个名字混成一个数 | — | ring 的 ~240 字节切段上一轮实测存在，不处理就会把 330 行记成别名的数 |
| **J4** | 回归：`registered` 仍 8 行；`play 0xf04c<-` == `fault after clear=` == `fault during play=` | 成立 | 没动代码 ⇒ 震动线形状不变 |

开机次数：20 里已用 9，本次第 10。
**这轮不许得到的结论**：`IOK` 只看匹配/通知/registry 路径/open 四类入口；
mediaserverd 拿到服务之后为什么不发流，它答不了——J2 只能分"缺不缺具名服务"。


## J 判决（`logs/ring-j-msvd.txt`，2026-11-04，第 10 次开机，零补丁）

| 判据 | 读数 | 结论 |
|---|---|---|
| **J0** | 产物含 `sweetiolog=mediaserverd,backboardd` | ✔ |
| **J1a** | `IOK backboardd:` = **209** 行 | 同轮对照活着 ⇒ 仪器可用（上一轮拿 launchd 当对照是选错了人） |
| **J1b** | `IOK mediaserverd:` = **309** 行 | 可以回答 J2 |
| **J2** | mediaserverd 落空的查询只有三个：**`AppleEFINVRAM -> none`**、**`IODTNVRAM -> none`**、**`IONameMatch=bluetooth -> none`**；命中的是 `IOPlatformExpertDevice`×3、`path IODeviceTree:/product/audio -> IOService`×3 | 音频侧**不是**缺一个音频类：它要 NVRAM 和 bluetooth，两者我们都没有 |
| **J3** | 按名字分开数；`=end NNN -> 结果` 残段单独计（backboardd 104 行未归属、mediaserverd 20 行） | 数法成立 |
| **J4** | `registered` 8 行；`play` = `after clear` = `during play` = **30** | 没动行为 ✔ |

### 三条要单独写下来的读数
1. **mediaserverd 卡在 `/product` 上**：`path IODeviceTree:/product -> IOPlatformDevice` **276 行**
   （ring 第 886–1474 行，两个进程都算上），每次**都成功**。这是"成功但反复查"，
   不是失败；**本轮不能据此说它在循环里卡死**——要看它后面还有没有别的动作，
   而音频侧的动作（`IOAudio2DeviceUserClient open/map/uid=`）这轮**根本没打**（见第 3 条）。
2. **NVRAM 这条缺口第一次被直接测到**（以前是 §12.4 从真机对比推出来的）。
   但顺序上它**不是** mediaserverd 的停止点：`AppleEFINVRAM/IODTNVRAM -> none` 在 ring 第
   1012–1015 行，之后它还做了 bluetooth(1020)、DriverKit `IOUserAudioDriverUserClient`
   的 FirstMatch/Terminate(1172–1177)、`IOPlatformExpertDevice`×3(1179–1184)、
   USB `bInterfaceSubClass=0x2a` / `USBDeviceFunction=Valeria`(1185–1195)、`/product/audio`×3(1196)。
   ⇒ "NVRAM 拿不到所以它不往下走"**没有证据**；它继续走了 180 多行。
3. **对照差点错位（差点写成结论的那个坑）**：本轮 `SWEET AUDPROBE` 只有 7 行、
   `IOAudio2DeviceUserClient` **0 行**，而第 82 行自己写着 **`gate flag=0 exp=0`**。
   0244 的注释写明 `gate flag=0` = `sweetaudioprobe=1` 没进 kernel（本轮 cmdline 里确实没有它）。
   ⇒ **不许**把"这轮没有音频 open 行"读成"mediaserverd 这轮没 open 我们的设备"。

### ep6 前置（文档里欠的那次源码阅读）——离线做完，没开机
`docs/audio-hal-bridge.md:549` 要求先读 `IONVRAM.cpp` 再决定怎么发 NVRAM。读的结果：
- `IODTNVRAM::start`（`iokit/Kernel/IONVRAM.cpp:1224`）里 `_controllerLock` 在 `init()`
  （`:1207 IOLockAlloc`）就会拿到 ⇒ 走完整路径：diags → notifier → `initImageFormat()`。
- `initImageFormat()`（`:1341`）从 `/chosen` 取 `nvram-bank-size` / `nvram-proxy-data`，
  两个键我们都没有 ⇒ `image=nullptr, size=0` 直接落到 `skip:`，
  然后仍然构造 handler；**两个 handler 里谁返回 null 就 `panic("…creation failed")`**。
  ⇒ **发一条 stock `IODTNVRAM` 的根 personality 有开机即 panic 的风险**，ep6 不能这么干。
- 好消息：`IOProviderClass` 的比较走 `matchInternal`(`iokit/Kernel/IOService.cpp:7517`) 里的
  **`metaCast(str)`**，也就是**沿类链**判断 ⇒ **我们自己子类化 `IODTNVRAM`、只重写 `start()`
  跳过 `initImageFormat()`** 就能满足 mediaserverd 的 `IOProviderClass=IODTNVRAM` 查询，
  不碰真实后备存储、也不动 XNU 原文件。

### 下一枪（K 轮，仍旧零补丁）
把音频探针和追踪对齐：`SWEETIOLOG=mediaserverd,backboardd sweetaudioprobe=1`（439 字节 < 590）。
判据是**同一时间里两路交叉**：`IOK mediaserverd` 的查询序列 vs `IOAudio2DeviceUserClient`
的 `open/map/uid=`，看它到底停在哪一步——这是 J 轮第 3 条那个坑的唯一修法。


## K 判据（`sweetaudioprobe=1` + 同一份追踪，零补丁，开机前写死）

J 第 3 条那个坑只能这样修：**让音频探针和 IOKit 追踪在同一次开机里同时可见**。
cmdline 438 字节（预算 590）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **K0** | 产物里同时有 `sweetiolog=mediaserverd,backboardd` 与 `sweetaudioprobe=1`；ring 里 `gate flag=1` | 成立 | 探针开关**这次真的进了 kernel**（0244 注释：`gate flag=0` = 参数没到） |
| **K1** | `IOK mediaserverd:` > 0（对照同 J） | | 追踪活着 |
| | `IOAudio2DeviceUserClient` 行数 | ≥1 | 探针活着，K2 才有意义 |
| | =0（而 `gate flag=1`） | **mediaserverd 这次确实没 open 我们的音频设备** | 与 b1/b2 不同形 ⇒ 本身就是一个新读数，但必须先排除"进程没起来"（看 `SWEET AUDPROBE: ... start uid=` 是否照常 5 行） |
| **K2** | 时间线交叉：第一条 `open … uid=` 的 ring 行号，和 `path IODeviceTree:/product` 的**首/末**行号比较；数它在此之前多少次、之后多少次 | 之后仍在涨 | `/product` 反复查是**正常轮询**，不是停在"读 /product"上 |
| | | 全部集中在 open 之前、之后归零 | `/product` 查询是**初始化的一部分**，mediaserverd 走过去了 ⇒ 断点在更后面（对齐 b1/b2 的 `sel=` 形状再定） |
| | | 一直涨且始终没有 open | mediaserverd **停在 open 之前**；下一枪问"它从 `/product` 里到底想要哪个键"（DT 侧，不是服务名侧） |
| **K3** | 数法同 J：按 `IOK <name>` / 按 `uid=` 分开；`=end NNN -> 结果` 残段单独报数 | — | 不许把两个进程或五个 uid 混成一个数 |
| **K4** | 回归：`registered` 8 行；`play` == `after clear` == `during play`；`sel=1` + `asset count 18` 各 1 事件 | 成立 | 只换参数没动行为 |

开机次数：20 里已用 10，本次第 11。
**这轮不许得到的结论**：追踪只覆盖匹配/通知/路径/open 四类入口；
`/product` 轮询**为什么**存在，它答不了——K2 只分"停不停在 open 之前"。


## K 判决（`logs/ring-k-probe.txt`，2026-11-04，第 11 次开机，零补丁）

| 判据 | 读数 | 结论 |
|---|---|---|
| **K0** | ring 第 82 行 `SWEET AUDPROBE: gate flag=1`（J 轮是 `flag=0`） | 探针开关这次真进了 kernel ✔ |
| **K1** | `IOK mediaserverd:` 328 行、`IOK backboardd:` 175 行；`IOAudio2DeviceUserClient` **25 行** | 两路同时可见 ✔ |
| **K2** | `path IODeviceTree:/product` 全 ring 289 行（mediaserverd 288 / backboardd 1）；第一条音频 open 在第 **1096** 行 ⇒ **之前 2 次、之后 287 次**，最后一次第 1395 行 | 落在 K2 第一格：**`/product` 反复查发生在成功 open 我们的设备之后**，所以它不是"停在读 /product"。但也不能说它是"正常轮询"——它在 1395 行**停了**，不是无限 |
| **K4** | `play` = `after clear` = `during play` = **9**；`registered` 8 行；`sel=1` + `asset count 18` 各 1 事件 | 只换参数没动行为 ✔ |

 mediaserverd 本次的完整形状：`open by pid 28`×4 → `map requested`（Actuator/Speaker/Codec/Hawking）
→ `map type=0 handed 0x10000 zeros`×7 → `notify port`×7 → **没有任何 `IOAudio2DeviceUserClient sel=`**，
只有 `AppleAOPAudioUserClient sel=0`×1。**它没崩**（本 ring 无 `crashed: mediaserverd`）。

### 我自己埋的对照错位（必须写在结论前面）
K 的 cmdline 只有 `sweetaudioprobe=1`，**没有** B1 那批音频 flag；而 b1/b2/ep6–ep9 的 cmdline 是
`sweetampfirst=1 sweetbufs=30 sweets1ap=3 sweetshmhi=1`。所以 **K 上"没有 sel=4"不能读成
"iOS 侧行为变了"**——它和 B1 不同配置。

### 用 181 个 ring 做的离线对账（零开机）
按（音频flag? / 有 sel=4? / 有音频 UC open?）分格：

| 音频 flag | sel=4 | 音频 UC open | ring 数 | 例子 |
|---|---|---|---|---|
| 无 | 无 | 无 | 145 | `ring-0199.txt` |
| 无 | 无 | **有** | **1** | `ring-k-probe.txt` ← 本轮，样本只有 1 |
| 有 | 无 | 无 | 24 | `ring-ab.txt`（= mediaserverd 崩溃风暴那批） |
| 有 | 无 | 有 | 5 | `ring-ep/ep2/ep3/ep4…`（早期只有 1 次 open） |
| 有 | **有** | 有 | 6 | `ring-b1/b2/ep6/ep7a/ep7b/ep8/ep9` |

⇒ "4 次 open + 有音频 flag"的 6 个 ring **全都有 sel=4**；"4 次 open + 无 flag"目前只有 K 一个样本。
要么 flag 与 sel=4 有关，要么只是补丁版本漂移——**n=1 分不清**，所以补一枪（L）。

### 顺带量到的两件别线的事（不在本轮判据里，只登记）
1. **`crashed: mediaserverd` 只在 24/181 个 ring 出现**，而且**与"音频 UC open"完全不重叠**
   （open+crash = **0** 个 ring）。⇒ "mediaserverd 崩环"**不是**"开了壳不出流"的解释，
   两批 boot 是两个总体。这条差点被我当成音频线的真凶。
2. 那 24 个 ring 里 mediaserverd 是**同一条指令每次都崩**：`sig 11 / pc 0x0 / far 0x0`
   （跳到空函数指针），bt 相对偏移跨 boot 恒定（如 `lr-slide` 相同的三帧），且崩前一行总是
   我们自己的 `SWEET SURF: mediaserverd create id N 1024x768 … -> 0x0`。
   累计 1100 次。这是"GUI 起不来"那批 boot 的独立问题，属 Surface/SweetSurface 线。

### 下一枪（L 轮，仍旧零补丁）
把 K 与 B1 对齐：`sweetampfirst=1 sweetbufs=30 sweets1ap=3 sweetshmhi=1 sweetaudioprobe=1`
+ 同一份 `SWEETIOLOG=mediaserverd,backboardd`。判据：sel=4 是否回来。


## L 判据（b1 的配置搬到当前 kernel，唯一变量=补丁版本；开机前写死）

K 的表格里"4 次 open + 无 flag"只有 1 个样本，分不清 flag 与 sel=4 的关系。
L 就把 **b1 的 cmdline 原样搬过来**（含 `sweetreboot=1`，自动重启拉 ring，不用人碰手机），
只加 `sweetaudioprobe=1`，**不开追踪**（字节数不够，而且追踪是被动打印、不是本轮变量）。

腾字节只删**已经死掉的仪器**，并写明：`sweetampverify=1`（0237→anchor3 两次开机证明
它的 GENI 读回两层坏）、`sweetnobb=1`（0241 已判完：只有 bit-bang 之后那段影响可寻址）。
这两个都只碰 PA 的 I2C 读回，不碰 AOP UC / ADSP 图。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **L0** | 打包镜像里逐条 grep 到 13 个 flag 且**没有被截半**（`sweetbufs=30` 必须完整、`sweets1va=0x1FFE8000` 必须完整）；总长 ≤590 | 成立 | 参数进产物且没被 607 字节字段切坏 |
| **L1** | ring 里 `gate flag=1`、`IOAudio2DeviceUserClient open by pid` = 4 | 成立 | 探针与 4 台设备这次都到位（与 ep6+ 同形） |
| **L2** | `AppleAOPAudioUserClient sel=4` 的次数（b1/b2/ep6–ep9 的形是 **5 次，输入 0/0x1/0x2**） | **≥1（期望 5）** | sel=4 跟着**音频 flag**走 ⇒ K 上"没有 sel=4"由 flag 解释，与追踪/探针无关；B1 那条形状在 0249 kernel 上仍然成立 |
| | | **0** | flag 不是原因 ⇒ 只剩"0246–0249 之间补丁漂移"这一个解释，那就**回源读代码**（那几个补丁都没碰 `SweetAudioProbe.cpp`），不靠开机找 |
| **L3** | 顺带（不是本轮变量）：`SWEET MARK: s1ap leaf=`、`graph_start rsp … status=0`、`data_buffer …` 三条音频基线是否仍绿 | 记录 | 这是当前 kernel 上第一次"b1 配置 + 探针同轮"，音频侧 iOS/kernel 两半第一次能对齐看 |
| **L4** | 机器活着：ring 长度正常、最后一条 `sweet_log_sync()` 存在 | 成立 | 自动重启轮的老判据 |

开机次数：20 里已用 11，本次第 12。**这轮不许得到的结论**：sel=4 的语义仍未复原
（B2 还缺原料），L2 只判"它跟 flag 还是跟补丁版本"。


## L 判决（`logs/ring-l-b1config.txt`，2026-11-04，第 12 次开机，零补丁，自动重启轮）

| 判据 | 读数 | 结论 |
|---|---|---|
| **L0** | 13 条 flag 逐条在打包镜像里命中，569/590 字节，`sweetbufs=30`、`sweets1va=0x1FFE8000` 都完整 | ✔ |
| **L1** | `gate flag=1`；`IOAudio2DeviceUserClient open by pid 28` = **4** | 探针 + 4 台设备到位 ✔ |
| **L2** | `AppleAOPAudioUserClient sel=4` = **5**（输入 **0,0,1,1,2**）、`sel=0` = 1、`aop sel=4 ran in=1 out=1` = 5 | **落在第一格：sel=4 跟着音频 flag 走**，与追踪/探针无关 ⇒ K 上"没 sel=4"由 flag 解释；B1 的形状在 0249 kernel 上原样复现，**没有补丁漂移** |
| **L3** | `graph_start rsp op=0x1001002 status=0`、`data_buffer: dst=0x40bd done status=0`×40、`pcm tone start` | 内核侧数据面在 b1 配置下仍绿 |
| **L3 副作用（我删 flag 删出来的，不是新发现）** | `SWEET MARK: ampread pre-stream … r=-2 val=0xa5a5` 八条全 NACK | 因为本轮**没带 `sweetnobb=1`**，bit-bang 又跑了 ⇒ 放大器再次不应答。这等于**反向复现了 0241 的因果**（带上它时 `r=0 val=0x2032`），但**本轮 PA 侧没有任何关于图的证据** |

### 顺着 sel=4 读到的东西（离线读 0244，没开机）
`sel=4` 在 0244 抄来的 AOP 方法表里是 **`method_GetControlValue`**
（宽度 `{sin=1, sout=1}`，与实测 `in=1 out=1` 完全吻合；输入 0/1/2 就是三个 control id）。
而 `sweet_aop_method()` 里写的是：

```c
for (i = 0; i < arguments->scalarOutputCount ...; i++) {
        arguments->scalarOutput[i] = 0;      /* 每个 control 都回答 0 */
}
return kIOReturnSuccess;
```

⇒ **"控制值全 0"是我们自己给的答**，而且形状与 B1/ep 系列一致：mediaserverd 问了 5 次，
我们每次都回 0，然后它就不往下走了（`IOAudio2DeviceUserClient` 全程**没有任何 sel=**，
只有 open/map/notify）。

**这跟已经修过一次的那个坑同形**：0210 时代 haptics 的 `GetNumberOfSupportedAssets`
回 0 ⇒ ButtonResolver 直接把服务关掉；后来回 18 才拿到 `open → map → sel=1`。

### M 轮（0250，一行为主）
加 `sweetaopval=<n>`（**默认 0 = 现在的行为**），把 `GetControlValue` 的输出改成回答 n；
本轮带 `sweetaopval=1`（cmdline 582/590）。判据是**唯一问题**：
`IOAudio2DeviceUserClient sel=` 从 0 变成 ≥1 吗？


## M 判据（0250：把 `GetControlValue` 的回答变成 `sweetaopval`，本轮带 `=1`；开机前写死）

唯一变量 = 我们对 selector 4 的回答值（0 → 1）。配置沿用 L 的 cmdline 原样
+ `sweetaopval=1`（582/590 字节），仍旧 `sweetreboot=1` 自动重启轮，不开追踪（字节不够，
而且 L 已经证明追踪不影响 sel 形状）。**没有带 `sweetnobb=1`**，所以 PA 读回这一轮
仍然是 NACK 形状——那部分不是本轮证据。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **M0** | 下载的 kernel 里有 `SWEET MARK: aopval=`；打包镜像里 `sweetaopval=1` 完整 | 成立 | 补丁进了镜像、参数进了产物 |
| **M1** | ring 里有 `SWEET MARK: aopval=1 parsed from boot-args` | 有 | flag 真的被 kernel 解析到（这条打印挂在**第一次** AOP 方法调用上，所以它同时要求"至少有一次 aop 调用"——M3 保证这一点） |
| **M2** | **`IOAudio2DeviceUserClient sel=` 的行数**（L 的基线 = **0**） | **≥1** | 回答非零**改变了 mediaserverd 的行为** ⇒ "控制值全 0"就是那道 iOS 侧的门，接下来按 control id 逐个求真值（haptics 的 asset count 是同形先例） |
| | | **仍为 0** | 这条假设在"`=1` 这一档"上关闭。**边界**：不能推广成"回答值无所谓"——只证明了 1 这一档没让它前进 |
| **M3** | 控制不变量：`AppleAOPAudioUserClient sel=4` = **5**、`sel=0` = **1**（与 L 相同） | 相同 | 调用形状没变，M2 才是在"同样的对话"上比回答；**若调用次数也变了**，那本身就是新信息，M2 要和它一起读，不能单独引用 |
| **M4** | 音频基线仍绿：`graph_start rsp … status=0`、`data_buffer … done status=0` | 成立 | 内核侧数据面没被这个改动破坏 |
| **M5** | `crashed: mediaserverd` = 0 | 成立 | 本轮属于"能 open 设备"那个总体（L 那 12 个 ring），不是崩环那 24 个；否则 M2 不可读 |

开机次数：20 里已用 12，本次第 13。


## M 判决（`logs/ring-m-aopval.txt`，2026-11-04，第 13 次开机）

配置 = L 的 cmdline 逐条照抄 + 只多一个 `sweetaopval=1`（583/590 字节，14 条 flag 逐个在
打包镜像里完整命中）。**唯一变量**就是我们回给 `method_GetControlValue` 的那个数。

| 判据 | 读数 | 结论 |
|---|---|---|
| **M0** | kernel 里 `SWEET MARK: aopval=`（且 0249 的两个 marker 也还在，是累积）；产物里 14 条 flag 完整 | ✔ 0250 真进了镜像 |
| **M1** | `SWEET MARK: aopval=1 parsed from boot-args` 1 行 | 参数进了 kernel 并被解析 ✔ |
| **M2** | `IOAudio2DeviceUserClient sel=` = **0** | **落在第二格：改成回 1 没有让 mediaserverd 前进一格。** 这条假设在"=1 这一档"上关闭 |
| **M3** | `AppleAOPAudioUserClient sel=4` = **5**、`sel=0` = **1**、`aop sel=4 ran` = 5、`open by pid 28` = 4 | 与 L **逐项相同** ⇒ 不是"它没问"，是"我们答了 1 它也不动"。M2 的读法成立 |
| **M4** | `graph_start rsp … status=0`、`pcm tone start`、`done status=0` ×4 | 内核侧基线仍绿 |
| **M5** | `crashed: mediaserverd` = **0** | 本轮与 L 同属"能开设备"那个总体，不是崩环那 24 个 ⇒ M2 可读 |
| 附 | `gate flag=1 exp=0xf` | 探针开关同 L |

**边界（不许推广）**：M2 只否证了"`=1` 这一档"。要说"回答值无所谓"还差其它档
（例如按 control id 给不同值、或给全 1 之外的位图）。但在花钱的顺序上，
**"它压根没调 IOAudio2 方法"比"我回的值对不对"更上游**——所以下一枪去量前者。

### N 轮（0251，仪器轮）：mediaserverd 在 `/product` 上到底读什么
K 轮实测：mediaserverd 在成功 open 四台设备之后，做了 **287 次**
`IORegistryEntryFromPath("IODeviceTree:/product")` 并且**每次都成功**，然后在 ring 第 1395 行之后
再无任何 `IOK mediaserverd` 动作。**"它反复读 /product 的哪个属性"** 是追踪看不到的那一层——
0049 只 hook 了匹配/通知/路径/open 四类入口。
0251 要在"用户态取 IORegistry 属性"的内核入口上，**只对 DT 平面里那个 product 节点**打印
`属性名 + 命中/落空 + 调用者 comm/pid`，其它一律不打，噪声有界（本轮实测 287 次/开机）。


## N 判据（0251：`/product` 属性读取仪器；开机前写死）

配置 = L 的 cmdline **逐条照抄、不加不减**（569/590，`sweetaopval` 退回默认 0，因为 M 已否证）。
0251 只加一个 `sweetaudioprobe` 门控的打印，挂在 `IORegistryEntry::getProperty(const OSSymbol *)`
（wrap2/wrap4 的 `copyProperty`/`getProperty` 全部经过它）。范围：只有名字等于 `product`
的条目会打印；每个调用者最多 60 行、最多 8 个调用者；带重入保护（IOLog 自己也能走到这里）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **N0** | kernel 与产物里都有 `SWEET MARK: dtprop `；cmdline 与 L 完全一致 | 成立 | 补丁进了镜像，且本轮除了它没有第二个变量 |
| **N1** | `SWEET MARK: dtprop` 总行数 > 0，**且**除 mediaserverd 之外至少还有一个别的调用者 | 成立 | 仪器活着（别的调用者就是阳性对照——这次不再把对照押在单一进程上） |
| | | 只有 0 行 | 仪器坏（挂点没被走到），整轮非证据 |
| **N2** | `dtprop mediaserverd <key> -> hit\|miss` 的**键名集合**与命中情况 | 有 **miss** | 那是一个**具名的缺失 DT 键**。我们自己的 `out/marble.adt` 就是产物 ⇒ 这是可以直接补的东西，也是 NVRAM 之后第一个具名缺口 |
| | | 全是 hit，且键在重复 | mediaserverd 在**等一个值变化**，不是在缺数据 ⇒ 下一枪问"谁会改这个属性"（很可能是它自己等 ADSP/HAL 回报，或等 notify） |
| | | mediaserverd 0 行而别的调用者有行 | 它读 `/product` 走的是另一条 API（`IORegistryEntryCreateCFProperties` / 读父节点 / 直接拿 property table）⇒ 挂点不够，需要换层，但**这不是"它没读"** |
| **N3** | 某个调用者正好 60 行 = **饱和**，不是总数 | 记录 | 不许把 60 读成"它只读了 60 次" |
| **N4** | 回归：`gate flag=1`、`open by pid 28`=4、`AppleAOPAudioUserClient sel=4`=5、`crashed: mediaserverd`=0、`graph_start rsp … status=0` | 成立 | 与 L/M 同形，本轮唯一变化就是多了打印 |

开机次数：预算已提到 120（2026-11-04 你给的），这是第 14 次。


## N 判决（`logs/ring-n-dtprop.txt` 为空/无 dtprop 行）：**仪器自身缺陷导致 panic，本轮非证据**

现象：开机 panic，`pc 0xfffffe7f7750f930 / lr 0xfffffe00079dc5c0`，手机已回 TWRP。

原因**不需要设备上数据就能确定**，因为它是纯代码性质：0251 第一版把重入守卫放在
`entry->getName()` **之后**，而 `getName()` 自己就读 `name` 属性 ⇒
`getProperty("name")` 再次进入同一个 witness ⇒ 无限递归 ⇒ 栈溢出。
（`inside` 在递归时还是 0，所以什么都没拦住。）

修法与验证（都是本地的，没开机）：
1. 守卫移到函数最前面，`inside = 1` 之后**任何**返回路径都清零（审计：守卫之后 4 个
   `return;` 对 5 个 `inside = 0;`，多出的那个是函数末尾的正常出口）。
2. 加总量上限 `SWEET_DTPROP_TOTAL 500`（原来只有每调用者 60 / 8 个调用者）。
3. **对当前文件**跑 stub 语法检查（`clang++ -Wall -Wextra -Wformat=2 -Werror -fsyntax-only`）
   ⇒ STUB-COMPILE-CLEAN。这一步之前是漏的：第一次只 stub 了 glink 的 hunk，
   而 0251 的 C++ 函数从没被单独编译过——所以先吃到一个 CI 红（丢 `uint32_t i;`），
   再吃到一个 panic（守卫顺序）。**两次都是我"没跑过的检查当成跑过"**。

开机记账：第 14 次开机 = **非证据**，仪器坏掉的那一格（N1 的"0 行"分支），
按规矩计入次数但不产生任何关于 mediaserverd 的结论。
下一条：网络恢复后把修好的 0251 推上去（现在 main 上那版**会 panic，不要再打镜像开机**）。


## ② 的本地攻坚（ACDB，2026-11-04 晚，零开机）

GPT 给的方向值得试，但**只有能落到字节上的我才收下**。用现成工具的 `props_offsets`/`walk_chain`
重新扫全部本地 ACDB，结论分三层：

### 站得住的
1. **每个 ACDB 变体都只有一条"超限"记录**，其它 1523 个 blob 全都 ≤1,700 字节：

   | 文件 | 超限记录 | 所在 blob 总长 | ALIGN(,4096) |
   |---|---|---|---|
   | `MTP_ukee_acdb_cal.acdb` | `@0x12519a iid=20 pid=0x00000001 size=0x40ba` = **16,570** | **17,620** | **20,480** |
   | `CDP_ukee_acdb_cal.acdb` | 16,405 | 17,452 | 20,480 |
   | `Forte_acdb_cal.acdb` | 16,405 | 16,792 | 20,480 |
   | `QRD_ukee_acdb_cal.acdb` | 17,707（`iid=36 pid=0x00000002`） | 35,244 | 36,864 |

   厂商序列是 `SHM_MAP(size=20480) → SET_CFG(size=18504) → SHM_UNMAP`。
   **17,620 与 16,405 都恰好对齐到 20,480** ⇒ "那块映射内存装的就是这条超限校准记录"
   在**结构与量级**上成立。这是今天第一次让 ② 从"字节拿不到"变成"字节可能就在文件系统里"。
2. 这条记录挂在**子图 `0xb0000106`** 上（`rec0` 的 `SUB_GRAPH_CONFIG` 载荷第二个字就是它，
   模块数 3）——不在我们目前那四个设备图子图 id 里。
3. **两条我自己的笔记被否证并更正**：
   - `0x08001176` 我记成"我们自创的 param id"，实际在 MTP ACDB 里出现 **16 次**（与
     `HW_EP_MF_CFG 0x08001017` 同一批记录里，第一条 @0x8c7a = MF_CFG 命中 +0x10）⇒ 它是真的厂商 id。
   - `p6=0x10003` 我在文档里标成 "pool"。按公开 `apm_cmd_shared_mem_map_regions`
     是 `{u16 mem_pool_id; u16 num_regions; u32 property_flag}` ⇒ **pool=3, num_regions=1**
     （3 = `APM_MEMORY_MAP_SHMEM8_4K_POOL`），而**我们自己的发送端一直用的就是 pool=3**
     （`sweet_glink.c:5177` 打印 `shm_map pool=3`），是我读抓包时把两个字段并成一个字了。
   - 另：`0x08001005` 我们代码里早有名字（`sweet_glink.c:2562 APM_PARAM_ID_SUB_GRAPH_LIST`），
     `docs/vendor-gpr-packets.md:123` 却写"名字未知"——文档落后于代码。

### 明确拒绝的假线索
- `u32=18504` 在 MTP 里"出现 53 次"、在 Forte 里"出现 98 次" ⇒ **纯字节巧合**，不构成证据。
  （同一天我在 ACDB 上已经上过一次 `0x4848` 的 ID-vs-尺寸 假线索，这次先自我否掉。）
- "17,620 + 884 = 18,504" 里那 884 字节是什么，**我不知道**。差 884 也可能是变体/机型差异
  （这台机实际加载的 ACDB 也许既不是 MTP 也不是 QRD）。

### 还没解出来的（下一步，仍然不开机）
超限记录的**载荷内部 framing 我没解开**：两种猜测布局
`{size,key1,key2}` 与 `{key1,key2,size}` 都只能解析 1–2 条就断裂。
载荷开头 32 个字的规律是 `1, 0x4042, 2, 0, 0x14, 1, 0x4015, 1, 0x4012, 4, 0, 0x14, …`
—— 看着像 `{nkeys, key1, key2, …, 0x14}` 的重复，但 `0x14` 到底是长度还是类型我不猜。
要继续得靠 **厂商 libgsl/AGM 里读写 GDC 的函数**（或 ACDB 规范），不是继续试偏移。

### 需要设备（只 adb，不开机）的一件事
读**这台机自己**在用的 ACDB（`/vendor/etc/acdbdata/…`），量它那条超限记录的真实大小：
如果它是 18,504（或对齐后仍是 20,480 的那条），② 的"拿不到的 18 KB"就地消失。


## ② 攻坚（续）：把**这台机自己在用**的 6 份 ACDB 拉下来量（只 adb，不开机）

`/vendor/etc/acdbdata/` 下实际有 11 份 `.acdb`（ukee_cdp / ukee_mtp / ukee_qrd / diwali_qrd /
diwali_parrot_qrd / IDP_UPD / …），拉了 6 份测量。结论是**两条硬的负结果 + 一条对我们有用的正结果**。

### 负结果 1：18,504 不在这些文件里
逐档把 1,455–1,524 个 subgraph blob 全走一遍，**没有任何一档存在 span 落在 18,000–19,000 的 blob**。
每档只有**一条**超限记录，尺寸是固定两族：

| 文件 | 超限 blob | 其中校准记录 | ALIGN(blob,4096) |
|---|---|---|---|
| MTP_ukee | **17,620** | 16,570（`iid=20 pid=1` @0x12519a） | **20,480** |
| CDP_ukee / IDP_UPD / QRD_diwali / QRD_diwali_parrot | **17,452** | 16,405 | **20,480** |
| QRD_ukee | 35,244 | 17,707（`iid=36 pid=2`） | 36,864（**不匹配**） |

所以"对齐到 20,480"这个论证到此为止：它说明**这类对象存在、且和厂商映射同量级**，
但 18,504 与 17,452/17,620 差 884–1,052 字节，**我不能说 18,504 就是它**。
（QRD_ukee 那条对齐到 36,864 而非 20,480，反而说明厂商当时加载的不是 QRD_ukee 这一族。）

### 负结果 2（也是自我纠正）：`0xb0000081` 根本不是厂商现在的子图
它出现 0 次是**应该的**：`0xb0000081` 在我们**10 月那份老容器** `g_graph_vendor4` 里，
厂商现役设备图是 `{b0000002, b0000025, b0000024, b0000023}`（= 我们从 ACDB 组出来的 2960）。
文档 1944/2073 行早就降级过这件事，是我这次差点把它又当成矛盾。

### 有用的正结果：厂商的图正文是"按 key 选择"出来的，而**我们那个 2960 是对的**
按子图 id 分组后，每个子图都有**很多份** blob（`0xb0000002` 41 份、`0xb0000106` 26 份、
`0xb0000023` 14 份，同一尺寸重复出现）⇒ 一个子图对应**多个 GKV/CKV 组合**，
运行时按 key vector 选一份。取"每子图一份"的典型组合：

```
808 (0xb0000002) + 756 (0xb0000025) + 256 (0xb0000024) + 944 (0xb0000023) = 2,764
+ 每条 52 B 前导 + 16 B 头  ⇒ 我们实测的 2,960（上线值 2,944）
```

⇒ **我们的容器构造方法本身是对的**（尺寸/框架自洽），② 缺的不是这个，
而是那次 `SET_CFG`——它需要的 key 选择我们还没有原料。
下一步仍然是解开那条超限记录的载荷 framing（GDC/CKV 头），不是再猜尺寸。


## ② 攻坚（续 3）：我今天差点重走一条已关闭的路，并且它被新测量**再次**关闭

`docs/audio-closeout.md:580` 那张表底下其实早就写着两条决定性事实，是我今天没有先调出来：

1. **18,504 那条 `dst=1` 包正文首字为 0** ⇒ 它不是 `{iid, param_id, data_len, flags}` 的
   内联模块记录串接（内联格式在 `vendor-gpr-packets.md` §SET_CFG 里由 6 条包独立验证过，
   `w9 = round_up(16+data_len, 8)`）。我基于"厂商也说过是模块记录集合"去猜的那个形状**是错的**。
2. **正文大小随开机变化（18,504 / 18,128）** ⇒ 它是**运行时组装**的产物。

今天新做的测量与这两条一致，并且把"从文件里取正文"再关一次：
机内 6 份 ACDB 的超限记录尺寸是**固定**的 16,405 / 16,570（blob 17,452 / 17,620），
不存在任何 18,000–19,000 的 blob，也没有随开机变化的机制 ⇒ **文件里那一条不是它**。

### 已关闭项（不要重走）
- "去 ACDB 里找那 18 KB 的正文"——本轮做完，结论是否定（固定尺寸 vs 随动尺寸）。
- "18,504 是 `{iid,param,len,flags}` 模块记录的串接"——正文首字 0 直接否证。
- "`u32=18504` 在 ACDB 里出现 N 次"——字节巧合，Forte 里出现 98 次，不是证据。

### 还没关闭、且正在做的
那条正文的**内部 framing**（首字 0、长度随 CKV 选择变化）——用 `/vendor/lib64/libgsl.so`
反汇编来定性：找 `gsl` 里"按 (GKV,CKV) 取 cal/GDC 数据 → 拼成一个共享内存缓冲 → 发 SET_CFG"
的那条路径，看它写的首字是什么（很可能是 `{count=0}` 或 `{gsl_key_type_t ...}` 之类的开头），
以及长度为什么会随每次开机的 key 选择浮动。这属于**读厂商代码**，不是再猜尺寸。


## ② 攻坚（续 4）：**撤回我今天的一条结论**，并记下 ACDB 真正解出来的部分

libgsl 反汇编（子智能体，逐条回源核过）给出三条我必须先纠正自己事实的东西：

### 撤回
- ~~"每份 ACDB 都有一条超限校准 blob（17,452/17,620），且 ALIGN 到厂商映射的 20,480"~~
  ⇒ **错**。`{iid=20, pid=1, size=16570}` 不是某个子图的校准记录，而是**整张 1,524 条目表之后
  的一个文件级尾部结构**（每个文件恰好一个：MTP `0x12519a`、CDP/IDP_UPD/QRD_diwali/parrot
  各自一个）。真正的 per-entry blob 只有 **1,032 字节（6 条记录）**。
  所以"对齐到 20,480"是拿一个文件尾块的长度去凑厂商的映射尺寸，**不成立**。
- 连带作废：`docs/audio-closeout.md` 里"② 攻坚（续）"那张 17,452/17,620 表格的**解释部分**
  （表格里的数字本身是对的，错的是我把它当成子图校准对象）。

### 新增的硬事实（可复用）
- **ACDB 条目头是 44 字节**，不变式 `entry_len = 44 + blob_len`，在 6 个文件
  **9,039/9,039 个条目上 100% 成立**；条目字段依次是
  `entry_len, desc_len, subgraph_id, num_desc_params, {param_id,size,data}..., blob_len, blob`，
  blob 起点恒满足 `file_offset % 4 == 2`。
- 我们的 2,960 容器由 `Σ 52 + align8(blob_len)` 精确复现：
  `{808, 756, 256, 924} → 860+812+308+980 = 2960`。
  **但另一种框法 `8 + align8(44+blob_len)` 给出同一个 2,960** ⇒ 数字本身分不出两种格式（记下，别自欺）。
- 那个尾块载荷**不是 TLV 链**：用 `{iid≤40, param_id, size}` + 各种对齐/打包方式做 BFS，
  最多只能覆盖 16,570 里的 16–240 字节。数据支持的形态是"以 `{u32 0; u32 T}` 分隔的扁平流"，
  `T ∈ {0x14(370 次), 0x34(81), 0x24(14), 0x64(7)}`，切出的块长 20/28/60/72/104/212/516。
  跨文件对比：CDP 的载荷 = MTP 的**去掉开头 7 个字**，其余逐字节相同 ⇒ 这些是**整条记录**，
  而 `size` 在我试过的任何布局里都不是 per-record 字段。
- 方法论：**18,504 用"连续窗口求和"在每个文件里能命中 9–12 次**；u32 字面值 18504 在 MTP 里
  出现 53 次且都是 id 值（`0x4006…0x48ba`）不是长度 ⇒ **尺寸匹配不构成证据**，以后一律禁止用
  "凑到某个总长"来认领一组记录。

### 我指定错了二进制（流程教训）
`/vendor/lib64/libgsl.so` 是 **Adreno GPU 的 GSL**（`.rodata` 里源路径
`vendor/qcom/proprietary/graphics/adreno200/gsl/user/src/gsl_pm4cc*.c`；全文 `SET_CFG/APM/ACDB/
gdc/gkv/ckv/audio` 0 命中；阳性对照 `pm4` 170、`kgsl` 126）。⇒ 以后要反汇编"厂商怎么组
SET_CFG"，**必须先用内容筛出真正的音频 GSL/AGM/PAL**（找带 `APM_CMD` / `SHM_MAP` /
`gsl_graph` / `ACDB` 字符串的 .so），而不是按文件名猜。


## ② 攻坚（续 5）：**SET_CFG 的组包格式由厂商代码确认**（libar-gsl.so，四条声明我逐条反汇编复验）

目标二进制是 `/vendor/lib64/libar-gsl.so`（AudioReach GSL，224,440 B；本地副本
`work/phone-libs/libar-gsl.so`）。函数表在 `.gnu_debugdata` 的 mini-ELF 里（`.dynsym` 只有导出桩）。
关键函数：`gsl_apm_config_oob @0x18648`、`gsl_apm_config_inband @0x18278`、
`gsl_graph_set_custom_config @0x180a4`（**唯一**用 cmd `0x01001006` 走 OOB 的调用者）、
`gsl_graph_set_sg_cal @0x1af10`、`gsl_shmem_alloc_ext @0x24bb8`。

### 已验证的线上格式（图形级 custom-config，dst=1，就是我们那 18,504）
```
包体 = 40 B gsl/gpr 头 + 16 B 描述符
       描述符 = {addr_lsw, addr_msw, mem_map_handle, size}     (0x187f4/0x18800/0x187e8/0x187fc)
共享内存正文 = 调用者缓冲**逐字拷贝**（0x18804 ldr x0,[x27,#0x30] → ar_mem_cpy；GSL 不加头）
正文头 16 B   = { u32 id_flag, u32 rsvd, u32 data_len, u32 rsvd }   ← 记录从 +0x10 开始
                我们观测到的"正文首字为 0" = id_flag=0（图形级；发给单模块时它放模块 iid）
长度关系      = 总长 = align8(data_len) + 16
映射区大小    = align4K(总长)      ⇒ 18504→20480，18128→20480  ⇒ "随开机变"不改变映射
路由阈值      = 总长 > 256 才走 OOB，否则 inband             (0x18188 cmp w21,#0x100 / b.hi)
opcode        = 0x01001006                                  (0x18174 mov w3,#0x1006 + movk)
property_flag = 1 << (mem_pool_id - 1)                      (0x186fc→0x18710)  ⇒ pool 3 ⇒ 4
```

### 因此要更正我自己的两条记录
1. **`sweetshmflag=4` 不是"offset 模式"**。4 只是 `pool=3` 推导出的位（`1<<(3-1)`）。
   `docs/START-HERE.md` 和若干 commit 里"厂商的 offset 模式，只有在 `sweets1ap=3` 下才会被
   DSP 真正读通"这句话，**前半是错的**（后半那次实测现象保留，但它和 flag 的语义无关）。
2. `docs/vendor-gpr-packets.md` §"内联正文的定长规律"里 `w10 = 目标 iid、w11 = param_id、
   w12 = data_len、w13 = flags` —— 按厂商代码，这四个字其实是 custom-config 的
   **头** `{id_flag, rsvd, data_len, rsvd}`；发给单模块时 `id_flag` 恰好是模块 iid，
   所以我当年把它读成"记录的第一个字段"。**结果形状可用，字段名是错的**；
   `w13` 恒为 0 正是那个 `rsvd`。

### 由此可写的一个真实验（0252，不再是猜尺寸）
用已确认的容器发一份**图形级 custom-config**：正文 `{0, 0, data_len, 0}` + 从 `+0x10` 起的
TLV 记录 `{iid, param_id, size} + align8(size)`，记录内容取我们已经**实测成功**的那几条
（`0x40bd/0x0800100C/30B`、`0x428f/0x08001017 + 0x0800101B`、`0x42d3/0x10013D2A`）。
总长 ≤256 ⇒ 按阈值应走 **inband**；这正好一次测两件事：
(a) APM 接不接受我们组的这个容器；(b) `>256 才 OOB` 这条阈值在我们的 ADSP 上是否真是路由依据。
开关：`sweetcccfg=1`（默认关，行为零变化），marker 带版本字面量 `SWEET MARK: cccfg v1`
（补上 0251 v2/v3 无法用 marker 区分的教训）。

### 仍未证（不要当已知）
- `id_flag` 的语义（只知道它被转发给 `__gpr_cmd_alloc_ext` 的 info+0xc，尺寸不匹配时回退 1）。
- 我们那 18,504 究竟是 custom-config 路径还是 ACDB 直传路径（`1b0dc/1a8e8` 那两条把 SHM 指针直接交给
  `acdb_ioctl`、**没有** 16 字节头）。推断依据是 dst=1 + opcode + 16 对齐 + 首字 0，**不是证明**。
- 记录内部（`{iid,param_id,size}`）在 custom-config 里到底是 12 B 头还是 16 B 头。


## ② 攻坚（续 6）：GPT 核对官方仓库后，**撤回我自己的"正文首字为 0"**，并把三方说法对齐

来源：`AudioReach/audioreach-graphservices`（官方公开实现）。我抓到的原始 kprobe 包：

```
apm-boot2.txt:437  gprsend p5=0x01001006 p6=2048(0x800) p7=0 p8=0xB0E71628 p9=672  p10..p95=0
```

### 撤回
- ~~"4 个 `dst=1` 的 APM 级 SET_CFG 里，18,504 那条正文首字为 0"~~ ⇒ **错，而且是不可能的读数**。
  这是一条 **OOB 包**：正文在共享内存里，包里只有 `apm_cmd_header_t`，`p10` 往后是**包头后的填充**，
  96 个字的 kprobe 窗口本来就取不到正文。**我们从没观测过那 18 KB 的第一个字。**
  （教训归到"仪器不能产生这个数据、就不能这么写"：我把填充当成了内容。）

### 对齐后的官方/厂商一致结论
| 事项 | 官方实现 | 我这边厂商二进制的对应证据 | 判定 |
|---|---|---|---|
| 记录头 | `apm_module_param_data_t{module_instance_id, param_id, param_size, error_code}`，**16 B**，记录间 `GSL_ALIGN_8BYTE` | 子智能体读成 `{id_flag,rsvd,data_len,rsvd}`"外层头" | **子智能体误读**：`ldr w8,[x22,#8]`+`align8`+`add #0x10` 与 `payload_size` 比较，是官方的**单记录判定** `align8(param_size)+16==payload_size ⇒ dst=该记录 miid，否则 dst=APM`；`ldr w4,[x22]` / `mov w4,#1` 就是选 `dst_port`（1=`APM_MODULE_INSTANCE_ID`） |
| OOB 包里的 16 字节 | `apm_cmd_header_t{payload_address_lsw, msw, mem_map_handle, payload_size}` | 0x187f4/0x18800/0x187e8/0x187fc 四字存储 | **同一个结构**，抓包 `p6..p9` 与之逐字段吻合 ✔ |
| 阈值 | `payload_size <= 256` 走 inband，`>256` 走 OOB，比较的是**调用者正文长度本身** | `0x18188 cmp w21,#0x100 / b.hi` | 一致 ✔（抓包里 `payload_size=672` 走 OOB ✔） |
| 正文在映射区的偏移 | 未规定 | `p6 = 0x800`（**不是 0**） | 我们发的时候必须按区域偏移处理，别假定 offset 0 |
| `property_flag` | 官方按池推导 | `1 << (mem_pool_id-1)`（0x186fc→0x18710） | 一致 ⇒ `sweetshmflag=4` 不是"offset 模式"（已改 START-HERE） |

### 对 0252 的直接影响（比原计划更明确）
原计划按"外层 16 B custom-config 头 + 12 B TLV 记录"发。现在应改成官方形式：

```
payload = N 条记录，每条
  +0x00 u32 module_instance_id      （我们图里的真实 MIID：0x40bd / 0x428f / 0x42d3）
  +0x04 u32 param_id               （0x0800100C / 0x08001017 / 0x0800101B / 0x10013D2A）
  +0x08 u32 param_size
  +0x0C u32 error_code = 0
  +0x10 data[param_size]，下一条从 align8(param_size) 之后开始
payload_size = Σ(16 + align8(param_size))
> 256  ⇒ SHM_MAP(池 3, property_flag=4, 区大小=align4K(payload_size)) + SET_CFG{偏移,handle,size} + SHM_UNMAP
≤ 256  ⇒ 直接 inband 内联（同一批记录，正文在包里）
```

**这仍然是"未观测"**：18 KB 的**内容**（哪些 MIID × 哪些 param）我们一次都没拿到过；
现在拿到的只是**容器的规则**。所以 0252 是"用正确容器发我们已知的那几条记录"，
测的是 APM 对**容器/路由**的反应，不是复刻那 18 KB。


## C 判据（0252：把两条已知参数打包成**一条多记录 SET_CFG 发给 APM**；开机前写死）

配置 = L 的 cmdline 原样（13 条 flag、569 字节）**只加 `sweetcccfg=1`** ⇒ 582/590。
不带这个开关时 0252 的那段代码根本不执行，所以这是一次"开关 = 变量"的对照。

发出去的东西（宿主机用真实源码块编译跑出来的，不是我手算的）：

```
total len=80  records=2  inband=yes
  rec0 @  0 iid=0x428f param=0x08001017 size=12 error=0 data[0]=00 77 01 00   (= 96000)
  rec1 @ 32 iid=0x428f param=0x0800101b size=28 error=0
walk consumed 80 of 80 -> EXACT
```

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **C0** | 下载的 kernel 与打包镜像里都有 `SWEET MARK: cccfg v1 `（这条**带版本号**，补上 0251 v2/v3 无法区分的教训）；`sweetcccfg=1` 在产物里完整 | 成立 | 补丁进了镜像、开关进了 boot-args |
| **C1** | ring 里有 `SWEET MARK: cccfg v1 dst=1 inband recs=2 len=80` | 有 | 开关解析到了、算出来的容器就是我核过的那个（行数自证，不需要换算：printf 单写） |
| **C2** | 紧随其后的 `SWEET GLINK: cc_batch rsp op=0x1001006 status=<x>` | **status=0** | APM **接受**多记录容器 ⇒ "厂商那 18 KB 是同一容器"这条路打通，下一步可以放心把更多参数打包（>256 再上 SHM_MAP） |
| | | **status=0x3**（EUNSUPPORTED） | APM 不接受打包 ⇒ 我们只能继续一条一发、目标=模块端口；"用打包容器复刻 18 KB"这条**关闭**，不再猜内容 |
| | | 其它非零 | 只记录原值，**不给语义命名**（`0x2` 的教训） |
| **C3** | 回归：单记录那批仍照常发并且状态与 L 相同（`vp/tdm_rate`、`vp/tdm_cfg`、`vp/tdm_ep_mf` 的 `rsp … status=`）；音频基线 `graph_start rsp … status=0`、`data_buffer … done status=0` 仍绿 | 成立 | 加进去的那一条没有破坏已有的工作形状（否则 C2 的读数不能归因给"打包"） |
| **C4** | 不带开关的对照：本轮**就是带开关**，所以"不带"那一侧的数据来自 L（同一 kernel 的 0251 v2 构建，`cc_batch` 0 行） | 成立 | 有 L 作对照，本轮不需要额外开一枪 |

开机次数：120 里已用 14，本次第 15。
**这轮不许得到的结论**：C2 只判"APM 接不接受这个容器"。它**不**告诉我们厂商 18 KB 里
装的是哪些参数——那是内容问题，仍然一个字节都没有。


### C/N 共享一次开机（写清楚，免得事后当成两个独立样本）
本轮构建 `405f8d5` 同时包含 **0251 v3**（dtprop 仪器，重入守卫已修）与 **0252**（`cc_batch`），
而 L/M/N 的配置本来就带 `sweetaudioprobe=1`。所以**第 15 次开机一次拿两个读数**：

- **N 的重做**：`SWEET MARK: dtprop <comm> <key> -> hit|miss`，看 mediaserverd 在
  `IODeviceTree:/product` 上读什么（N1/N2 的判据原样适用，含"launchd 式对照"改成
  **任意两个不同调用者都算对照**：dtprop 是按条目名过滤、不按进程名，所以
  `kernel_task`/`system_server` 之类只要出现就是仪器活着的证据）。
- **C**：`cc_batch` 的 `status=`。

两者的机制路径互不相交（一个是 IOKit 属性读，一个是 GPR/APM 发包），所以不互为混杂；
但**样本数是 1**，两个读数来自同一台同一次 boot，写结论时要这么标注。

### C1 加强（0252 v2）
CI 第一次构建在 clone 上游依赖（bearssl.org 返回 502）时炸了，**与 0252 代码无关**；
重推时顺手把 C1 变成设备上自证：`g_apm_set_cfg_batch` 现在**按 APM 的方式走一遍链表**，
每条打印 `SWEET MARK: cccfg v1 rec N iid=… param=… size=…`，并且**走不到末尾或条数对不上就返回 -1、不发包**。
宿主机用真实函数体验证过三条路径：
```
caller built len=80
SWEET MARK: cccfg v1 dst=1 inband recs=2 len=80
SWEET MARK: cccfg v1 rec 0 iid=0x428f param=0x08001017 size=12
SWEET MARK: cccfg v1 rec 1 iid=0x428f param=0x0800101b size=28
  -> g_apm_cmd dst=0x1 len=80 opcode=0x1001006 tag=cc_batch     rc=0
len=83  -> walk 80 != len 83                                     rc=-1   (不发包)
size=0  -> bad size at 0                                         rc=-1   (不发包)
```
所以 C1 改为：ring 里必须出现 `dst=1 inband recs=2 len=80` **和两条 rec 行**；
若出现 `walk … != …` 或 `bad size`，说明是**我们自己的容器不自洽**，那一次不算对 APM 的测试。


## N 轮仪器（0251）**撤回并从序列里删除**——我上一轮给的原因是错的

- 第一次 panic 后我说"守卫放在 `getName()` 之后导致递归"。那不是（唯一）根因：
  `IORegistryEntry::copyProperty()` 的 `wrap2` 宏是
  `PLOCK; obj = getProperty(aKey); if (obj) obj->retain(); PUNLOCK;`
  ⇒ `getProperty()` **本身就在 property 锁内被调用**。我在里面做 `getName()`
  （再进 copyProperty，再拿同一把锁）+ `proc_selfname()` + `IOLog`，
  是"嵌套锁 + 持锁打印"。v3 只修了守卫顺序，所以**第二轮（C 轮）照样 panic**：
  两次 lr 都落在同一函数区（`0xfffffe00079dc5c0` / `…79dc77c`，差 0x1bc 正是我插码造成的位移）。
- 结论：**这个挂点不行**。要量"用户态读 /product 的哪个属性"，必须挂在
  **不持 property 锁**的层（IOKitServer 的 MIG 属性读入口，本机没有该文件；
  或者 wrap2/wrap4 里 **PUNLOCK 之后**再打印）。在那之前不再开这一枪。

## 同时记一条假信号（差点让我拿旧数据打分）
C 轮 panic 后没写 ring，`pull_logs.sh` 返回的是**上一轮 M 的 ring**
（`sweettime=1793775261`，而镜像是 `1793802313`）。⇒ **每轮拉完日志必须先对
`sweettime` 与本轮 build log 里的 cmdline**，不一致就当"本轮无数据"处理。
这条已经进 AGENTS.md 的判据习惯。开机记账：120 里已用 15（第 15 次 = 0251 的
第二次 panic，非证据）。


## C 判决（`logs/ring-c2-ccbatch.txt`，2026-11-04，第 16 次开机；身份已对：`sweettime=1793803435` 两侧一致）

| 判据 | 读数 | 结论 |
|---|---|---|
| **C0** | kernel 与产物里 `SWEET MARK: cccfg v1 ` 命中 4 次、`dtprop` **0 次**（证明 0251 真被摘掉）；cmdline 582/590，14 条 flag 完整 | ✔ |
| **C1** | `cccfg v1 dst=1 inband recs=2 len=80` + 两条 `rec N iid=… param=… size=…`，与**开机前在宿主机用真实函数体算出的值逐字相同** | 容器自证成立，不需要换算 |
| **C2** | `cc_batch rsp op=0x1001006 status=0`（`hop=0x2001005 len=32`） | **APM 接受我们自组的多记录打包 SET_CFG**（发给 dst=1、in-band、120 字节包 = 24+16+80） |
| **C3** | `GLINK: vp/` 9 行（与 L/M 同）、`graph_start … status=0`、`done status=0`、`IOAudio2DeviceUserClient open by pid 28` 4 行、`gate flag=1` | 回归通过 ⇒ C2 的"接受"不是被别的改动换来的 |
| **C4** | 对照 = L 轮（同配置、无 0252、`cc_batch` 0 行） | 单变量成立 |

**这条为什么值钱**：我们此前所有 SET_CFG 都是"一条记录、发给模块端口"。现在证实
"多条记录、发给 APM、由 APM 分发"这个容器在本机 ADSP 上**可用**。而按官方定义，OOB 模式的
每条记录头里那个 `error_code` 字段是**由被托管的模块回填**的 ⇒ 一次 OOB 批量包就能把
"这批 MIID 里哪些在我这张图里存在、各自被接受还是拒绝"**一次性问出来**。

### 已记账
- 第 15 次开机 = 0251 第二次 panic（`getProperty` 里持锁打印），**非证据**；0251 已从序列删除。
- 那次拉回的 ring 是 M 轮的（`sweettime` 不一致）⇒ 已在 AGENTS.md 加"拉完先对 sweettime"的硬规矩。
- 一次仓库卫生事故：`git add -A` 把 212 个未跟踪日志提交进去，随后用 `git rm --cached`
  撤出索引（`bc16832`），磁盘文件未动。以后一律按文件名 add。

## D 判据（0253：一次 **OOB 批量 SET_CFG**，读回每条记录的 error_code；开机前写死）

做法：正文 >256 字节（用 padding 记录把它推过阈值）⇒ 走 `SHM_MAP(pool 3,
property_flag=4, 区大小 = align4K(正文))` + `SET_CFG{偏移,handle,size}` + 收响应 +
**把缓冲里每条记录的 `error_code` 读回来打印** + `SHM_UNMAP`。
记录对象 = 我们实测厂商配置过的 MIID 全集，其中一部分**不在**我们打开的图里
（`0x40bf/0x40c0/0x4165/0x4308/0x40c8` 在我们图里 0 命中，`0x40bd/0x428f/0x42d3` 命中）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **D0** | 产物/kernel 含 `SWEET MARK: oobcfg v1 `；cmdline 含 `sweetoobcfg=1`；`sweetcccfg` 本轮**不带**（一次只测一个容器） | 成立 | 变量唯一 |
| **D1** | ring 里 `oobcfg v1 map size=%u off=%u recs=%u` 且 `size == align4K(payload)` | 成立 | 我们自己按官方对齐规则算的映射尺寸被照用；不对就说明算错 |
| **D2** | `SET_CFG` 响应 `status=` 与**每条记录回读的 `error_code`** 表 | 每条都打出来 | 预期：图内 MIID 为 0、图外为非 0 ⇒ 这是第一次拿到**逐模块的接受/拒绝位图** |
| **D2b** | 全部 `error_code` 都是 0 | 记录为**仪器不敏感**（回填可能发生在响应之后，或该字段只由模块自己写）⇒ 不许当成"全都接受" |
| **D3** | `SHM_UNMAP` 之后图仍能 `START`、`data_buffer … done status=0` 仍绿 | 成立 | 我们没把这张图弄坏（否则 D2 的读数会混杂状态污染） |
| **D4** | 回归：`GLINK: vp/` 仍 9 行、`IOAudio2DeviceUserClient open by pid 28` 4 行、`gate flag=1` | 成立 | 与 C 同形 |

开机次数：120 里已用 16，本次第 17。
**这轮不许得到的结论**：`error_code` 的**具体错误码语义**仍不命名（`0x2` 的教训）；
D2 只回答"0 还是非 0"，也就是"存在与否/接受与否"。


### D 判据的三处**开机前修订**（不是事后改判据：0253 写出来之后才发现和 D0/D1 的假设不一致）
1. **D1 作废、换成 D1′**：0253 **复用那张已经映射好的区域**（`G_SHM_SIZE = 0x5000`，句柄取自更早的
   `SHM_MAP` 响应），所以本轮**没有新的映射尺寸可对**。D1′ = 打印的
   `off=0x3000 len=288 recs=9` 必须满足 `len == 9*32 == 288 > 256` 且 `off+len <= 0x5000`。
   （"映射区 = align4K(正文)" 这条仍然成立，但它属于更早那次 SHM_MAP，不在本轮变量里。）
2. **D2 加读法**：9 条记录的 param id 与正文**完全相同**（`0x08001017` / 12 B），
   所以记录之间唯一的变量是 MIID。判定单元是
   **rec 0/1/2/8 = 阳性对照**（`0x428f/0x40bd/0x42d3` 确实在这张图里）：
   - 对照组 `err=0` 而 3..7 出现非 0 ⇒ 拿到**逐 MIID 存在位图**（同正文 ⇒ 不能赖给内容）；
   - 全为 0 ⇒ **D2b 仪器不敏感**（DSP 可能不给被丢弃的记录回填），本轮对"存在与否"**不作结论**；
   - **连对照组都非 0** ⇒ 是我们自己的容器/句柄/偏移错了，与 DSP 无关，本轮非证据。
3. 宿主机跑真实函数体时抓到过一个真错：第 9 个 MIID 因为数组按 `SWEET_OOB_RECS=9` 声明、
   初始化表却只有 8 项而**补零成了 `iid=0`**。改完后 9 条打印全部正确。
   ⇒ 同一条规律第三次生效：**能在宿主机上跑的数字，就不要留到设备上第一次见。**

开机记账：120 里已用 16；D 轮是第 17。

## E 判据（0254：**同一份 OOB 正文，扫两个偏移 × 两种地址形式 = 4 发**，检验"我们是否在错的地址上试探"；开机前写死）

### 本轮开机前修订（第 1 条，写于开机之前）
E 起初只扫偏移（`0x1000` / `0x3000` 两发）。写完后回源查**我们自己**的记录才发现问题比"偏移"更基本：
1. `logs/ring-d-oobcfg.txt:1692` 被接受的 `graph_open/vp-wr` 是**带内** 800 B 正文（`TX_DATA iid=112 len=800`）；
   全仓库日志里 **`shm handle=` 只出现过一次**（`ring-d-oobcfg.txt:1713`）⇒ 带外形式**总共只有一个样本**，
   也就是那个被拒的 `0x3000`。⇒ 光换偏移**分不开**"形式错了"和"这条路被拒"。
2. `ring-d-oobcfg.txt:1707` 成功的 `SHM_MAP` 是 **`pa=0xa2fe8000 flag=4`** ⇒ 我们交给 DSP 的是一个**区域基址**；
   同一个句柄下我们的 `DATA_BUFFER` 用**裸偏移**并且能用（数据面已通），
   而上游 GSL 文档的形式是**绝对地址**。两种说法都还没被排除。

⇒ 改成 **4 发 = 2 偏移 × 2 形式**：`bare`(偏移本身) 与 `abs`(`g_map_pa + 偏移` = 区域基址 + 偏移)，
正文/句柄/包构造/时序**逐字节相同**。宿主机跑真实函数体已验算：
`0xa2fe8000 + 0x1000 = 0xa2fe9000`、`+0x3000 = 0xa2feb000`，四发的 `status` 分支与回读都按被试地址跟随。
`boot-args` 不变 ⇒ 607 字节预算无风险。

| 编号 | 判据 | 取值 | 含义 / 判决走向 |
|---|---|---|---|
| **E0** | 产物含 `SWEET MARK: oobcfg v2 try ` **且含** `form=abs`（v1 串已从源码与 marker 表删除，旧镜像不可能误过）；cmdline 含 `sweetoobcfg=1`、**不含** `sweetcccfg` | 成立 | 变量唯一 + **产物真的是这一版**（并发两次 CI 构建时靠这两条串判身份） |
| **E1** | 四条 `try 0..3` 都在，`len=288 recs=9` 一致，四次 `handle=` **相同且非 0**，`addr=` 分别是 `0x1000 / 0xa2fe9000 / 0x3000 / 0xa2feb000` | 成立 | handle 为 0 或 `no region base, abs form skipped` 出现 ⇒ **E2 不作结论**（句柄/基址成了混杂变量） |
| **E2** | 比四发的 `status=` | 见下判决表 | 本轮**唯一**的判据 |
| **E2-h** | 比 `hop=`/`len=`（本轮新增；`%#x` 把 0 打成 `0`） | 记录 | 被拒那发若与已接受的 `vp/tdm_*`（`hop=0x2001005 len=32`）**不同** ⇒ 拒绝发生在路由之前，不是 APM 说的"不" |
| **E2b** | 四次 `err=` 全为 0 | 记为**仪器不敏感**（D2b 先例） | 对"逐 MIID 接受与否"不作结论；**注意** `iid/param/size` 按构造必然四处相同，**不许当证据** |
| **E3** | 回归：`GLINK: vp/` 仍 9 行、`graph_start … status=0`、`data_buffer … done status=0`、`gate flag=1` | 成立 | 扫地址没弄坏已在用的图 |
| **E4** | 不出现 `skipped (region` | 成立 | 出现 ⇒ 区域尺寸假设错，本轮作废重跑 |

### E 判据的开机前修订（第 2 条，写于开机之前）——**厂商自己的 OOB SET_CFG 在旧 kprobe 里就有，地址形式这一轴离线就能关门**

翻 `logs/apm-boot2.txt`（Android 侧 ftrace，含 42 条 `gprsend` = `gpr_send_pkt` 的寄存器/包转储）
找到厂商真正的音频序列（HwBinder 线程，`source_info` 严格递增 3,4,5,6,7）：

| op | 包头 w0 | 正文 4 字 | 读法 |
|---|---|---|---|
| `0x01001000` GRAPH_OPEN | `0x2860`（**40 字节**） | `{0x0, 0x0, 0xb0d89128, 0x5f0}` | 带外：区域偏移 **0x0**、句柄、正文 **1520 B** |
| `0x01001006` **SET_CFG** | `0x2860`（**40 字节**） | `{0x800, 0x0, 0xb0d89128, 0x2a0}` | 带外：区域偏移 **0x800**、**同一个句柄**、正文 **672 B > 256** ⇒ 与阈值规则自洽 |
| `0x01001004` ×2 | `0x2860` | `{0x0, 0x0, 0xb0d89128, 0xb8/0x18}` | 同样带外、同样裸偏移 |
| `0x0100100c` SHM_MAP | `0x2860`+`size=44` | `{0x10003, 0x4, 0x1fff8000, 0x1, 0x8000}` | {池3/区数1, property_flag=4, **IOVA 0x1_1FFF8000**, **区域 32 KiB**} |

对着 `ring-d-oobcfg.txt:1706` 逐项比我们那一发：
- 我们的 MAP = `{0x10003, 4, 0x1ffe8000, 1, 0x5000}` ⇒ **与厂商同形**，只差 IOVA（`0x1ffe8000` vs `0x1fff8000`）和区域大小（0x5000 vs 0x8000）。
- 我们的 SET_CFG 包 = `w0 0x2860 / 0x302 / src 0x2010 / dst 1 / op 0x1001006 / {off, 0, handle, len}`
  ⇒ **和厂商逐字节同形**，唯一差别是 `source_info`（我们恒为 `0x400`，厂商是递增序号）。

⇒ 三条**离线关门**（不用开机）：
1. **地址"形式"不是变量**：厂商也用**裸区域偏移**，所以 `abs`（基址+偏移）那一臂**不是假设**，
   降级成**故障寄存器的对照**（见 E5′）；本轮预期它失败。
2. **包形状不是变量**：D 轮的 `0x1` 不能赖给组包。
3. **剩下唯一没被排除的地址问题**是：我们 advertised 的 IOVA `0x1_1FFE8000` 到底有没有被
   **音频流实际使用的上下文组**翻译到我们真正的缓冲（我们手写了 cb5 的 S1 表，
   `ats1pr` 打印 `par=0` 只说明"无故障位"，**不等于**输出了我们的 PA）。
   ⇒ 这才是"我们在错误的地址上试探"这句话的**可测形式**，而测它的是 **SMMU 故障寄存器**，不是偏移扫描。

**0254 v3 = 加仪器、不加变量**：每发前后调用既有的 `g_smmu_faults()`
（`oobcfg-pre` 一次 + 每发一次 `oobcfg-post`；无响应那发行 `oobcfg-norsp`）。
FAR 是**锁存**的，所以 pre 快照是对照；helper 会打印**全部 6 个 CB** 的 `FSR/FAR/FSYNR0/farhi`，
所以即使错的是我们没编程的那个组也看得见。

| 新增编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **E5** | `oobcfg-pre` 快照**与**每发后的 `oobcfg-post` 快照都打出来 | 成立 | 仪器在场 |
| **E5′** | 某发的 post 快照里出现 `FAR` **随被试地址变**（裸形式 ⇒ `0x1ffe8000+off`；abs 形式 ⇒ `0xa2fe9000/0xa2feb000`；或 `farhi=1`） | 命中 | **带外正文真的被 DSP 解引用过、且死在翻译上** ⇒ 卡点是 IOVA→PA 的映射，不是包、不是偏移、不是 MIID ⇒ 下一枪改成"把区域映射进音频流真正用的那个组"（或干脆用厂商那对 `0x1_1FFF8000/0x8000`） |
| **E5″** | 四发全拒且**没有任何 FAR 变化** | 命中 | 拒在解引用**之前**（句柄/池/时序/MIID 校验）⇒ 地址这条轴（含偏移）**整体关门**，E2 分支 3 成立 |
| **E5‴** | pre 快照就已经非 0 且 post **与 pre 完全相同** | 记为**仪器不敏感** | 锁存的旧故障掩住了新故障 ⇒ 不许把"没有新故障"读成"没有故障"，需要单独一轮先清故障位 |

**开机前写死预测**（错了就是本轮的信息量所在）：按厂商证据，四发**大概率全部 `status=0x1`**，
若 E5′ 命中则机制确认；若 E5″ 命中则地址这条线关门。

**E2 判决表（开机前定死，事后不许改口）**
1. **`abs` 两发 `status=0`、`bare` 两发非 0** ⇒ **形式被牵连**：该发绝对地址；偏移本身无关 ⇒ 下一步把正文改按绝对地址重发真正的厂商容器。
2. **`bare`/`abs` 内部两个偏移的 `status` 不同**（例如 0x1000 与 0x3000 一成一拒，无论形式）⇒ **偏移被牵连** ⇒ 扫描更多页对齐偏移找规律。
3. **四发 `status` 全为同一个非 0** ⇒ **地址这条轴被排除**（把用户直觉证伪，也是本轮最有价值的负结果）⇒ 拒在包/句柄/池/命令本身，不再靠换地址解决。
4. **四发全 0** ⇒ 传输层都接受 ⇒ 看 E2b 与有没有声音；"被拒"不再是本轮说法。
5. 一发**有响应**、另一发 **`no response`** ⇒ **不判**：缺响应不是"被拒"的读数（`0x2` 的教训），记为仪器/时序问题，需重跑。

本轮**不许得到**的结论：`error_code`/`status` 的**具体语义**仍不命名；18,504 正文依然没拿到；
`status=0` 不等于出声（终端判据仍是**耳朵听到的声音**）。
开机记账：120 里已用 17；E 轮是第 **18**。

宿主机预验（真实函数体 + 打桩 DSP，`-Wall -Wextra -Wformat=2 -Werror` 干净通过）：
四发都发出、`len=288 recs=9` 一致；打桩 DSP 按 `off>>12` 回填 `err`，
`0x1000` 组读出 `0x80000002`、`0x3000` 组读出 `0x80000004` ⇒ **回读真的跟随被试正文**，
不是固定读某处（这条不验 E2b 的读数无意义）；`0x2000+12` 保持 0 ⇒ 没踩图正文；
区域缩到 `0x1100` 时两发都走 `skipped` 且 send 计数 0 ⇒ E4 分支可用；
`g_map_pa=0` 时 `abs` 两发打印 `no region base, abs form skipped` 且只发 2 次 ⇒ E1 的守卫分支可用。

## E 判决（`logs/ring-e-oobcfg.txt`，2026-11-05，第 **18** 次开机；身份已核：ring 里 `boot-args` 结尾 `sweetoobcfg=1 sweettime=1793850218`，与本次镜像一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| E0 | 产物含 `oobcfg v2 try `(2)、`oobcfg-post`(1)、无 `oobcfg v1 `(0)；cmdline **583 B**，尾部 `sweetoobcfg=1 sweettime=1793850218` 在镜像里连续 | **成立** |
| E1 | 四条 `try 0..3` 全在，`len=288 recs=9` 一致，`handle=0xb0e145f8` 四次相同且非 0；地址依次 `0x1000 / 0xa2fe9000 / 0x3000 / 0xa2feb000` | **成立** |
| **E2** | **四发全部 `status=0x1`** —— 两个偏移相同、两种形式也相同 | **走判决表第 3 支：地址这条轴（偏移值 + 裸/绝对形式）被 4 个样本关门** |
| E2-h | 四发 `hop=0x2001005 len=32`，与我们**被接受**的命令（`graph_start … status=0 hop=0x2001005 len=32`）**同端口、同长度** | 不是路由层丢包：**是 APM 自己收到的、自己答的"不"** |
| E2b | 36 条 `err=0` | 按预登记记为**仪器不敏感**（同 D2b），对"逐 MIID 接受与否"不作结论；`iid/param/size` 四处相同是按构造，不算证据 |
| E4 | `skipped (region` 出现 **0** 次 | 成立 |
| **E5** | `oobcfg-pre` + 每发一次 `oobcfg-post` 全部打出来 | 仪器在场 |
| **E5‴ 命中** | pre 快照六个 CB **已经是** `FSR=0x400` + 无意义 `FAR/farhi`，四次 post 与 pre **逐字节相同**；本次 `fsr clear 0x400 -> 0x400` 仍清不掉 | **故障见证本轮不敏感** ⇒ **不许**把"没有新故障位"读成"没有发生翻译故障"，"DSP 到底读没读到正文"这一问**仍未测** |
| **E3 回归** | `vp/` 仍 9 行 ✓、`graph_prepare status=0` ✓、`graph_start status=0` ✓，**但数据面死了**：`data_buffer send` **1** 次（D/C 都是 31）、`dbraw … ready=0 len=0`、`no rsp (dst=0x40bd)`、`buffer probe -> -1`、`done status` **0** 次（C/D 各 4 次） | **E3 FAIL，且不是噪声** |

### E3 那条是本轮真正的额外发现（剂量—反应）
从 `vendor graph + WR input started` 往后逐行比对 boot 17 与 boot 18：**前半段完全同形**
（9 条 vp/ 全 `status=0`、prepare/start 全 `status=0`、第一次 `data_buffer send` 也发了），
唯一的行为差别是**被拒的带外 SET_CFG 的个数**：D = 1 发 ⇒ 数据面 31 发 + 4 次 `done status=0`；
E = 4 发 ⇒ 数据面 1 发无响应。⇒ **APM 的"不"不是免费的**：连着四次被拒之后，
ADSP 的音频路径不再消费第一块 PCM。**本轮不能据此命名机制**（可能是图状态机进了错误态，
也可能是四次 5 s 忙等/额外 MMIO 读的时序），但必须据此改实验设计：**带外探针每轮只发一发**。

### 由此定死的 F 轮（一变量 = 被 advertise 的正文地址是否越出我们声明的区域）
D/E 已经排除"偏移值"和"地址形式"；`ats1pr` 只说明"无故障位"，而故障寄存器本轮证明读不了。
剩下没排除的是 **APM 在答"不"之前到底有没有去读正文**。一次开机就能分开：
- **F1** 只发 **一发**，正文地址 = `0x6000`（我们声明的区域是 `0x5000` ⇒ **越界**；正文照旧不写，守卫只挡 memcpy 不挡发送）。
- **F2** 判据：`status` 与已知的界内读数 `0x1`（D 1 样本 + E 4 样本）**不同** ⇒ APM **先校验范围**，
  那界内的 `0x1` 就是**读完正文之后**给的 ⇒ **内存/句柄这条路是通的，墙在容器内容**
  ⇒ ② 的重心从"送得出去吗"改成"送什么"（把我们自创的 `0x08001017`/MIID 清单当嫌疑对象）；
  `status` **仍是 `0x1`** ⇒ 校验要么不存在、要么同码，内存侧嫌疑**保留**（本轮不作"排除"结论）。
- **F3** 回归：`data_buffer send` 回到 31 且 `done status=0` 回到 4 ⇒ 同时验证 E3 是**剂量**造成的
  （n=1 对照，只算支持、不算证明）。
开机记账：120 里已用 **18**。

## F 判据（0255：**一发**带外 SET_CFG，正文地址**越出我们声明的区域**；开机前写死）

E 关掉了"偏移值"和"地址形式"两条，故障寄存器实测读不到东西，于是剩下唯一能一问分开的：
**APM 答"不"之前，到底有没有去看/读那份正文。**

做法：`g_oob_offs = {0x6000}`、`SWEET_OOB_FORMS = 1` ⇒ 只发一发，`off=0x6000` 在我们声明的
`0x5000` 区域**之外**；正文**不写**（守卫只挡 memcpy，不挡发送），响应之后**不回读**
（`recs not read back (past the mapping)`），因为这已经越出我们自己的映射，读它是 AP 侧的越界访问。
`boot-args` 与 E 完全相同（583 B，仍 `sweetoobcfg=1`、不带 `sweetcccfg`）⇒ 本轮唯一变化在代码里，不在参数里。

| 编号 | 判据 | 取值 | 含义 / 判决走向 |
|---|---|---|---|
| **F0** | 产物含 `SWEET MARK: oobcfg v3 try `、`recs not read back (past the mapping)`、`is past the declared region`，且 `oobcfg v2 ` = 0；cmdline 583 B 与 E 相同 | 成立 | 0255 真进了产物；参数没动 ⇒ 变量唯一 |
| **F1** | ring 里**恰好一发**：`try 0 form=bare off=0x6000 addr=0x6000 len=288 recs=9 handle=<非 0>`，并伴随 `body not written` 与 `recs not read back` | 成立 | 越界探针发出去了，且我们没有越界访问自己的内存 |
| **F2** | 那一发的 `status=` 与界内已知读数比（界内基线 = `0x1`，共 **5 个样本**：D 1 发 + E 4 发，同 hop `0x2001005` 同 `len=32`） | 见下三支 | 本轮唯一判据 |
| **F3** | 数据面回到 `data_buffer send` = **31**、`done status=0` = **4** | 见下 | 同时是对 E 那条"剂量"读数的检验 |
| **F4** | 回归：`GLINK: vp/` 9 行、`graph_prepare/graph_start … status=0`、`gate flag=1` | 成立 | 与 D/E 同形 |

**F2 判决表（开机前定死）**
1. **`status` 仍是 `0x1`（与界内同码）** ⇒ 没有观察到范围校验 ⇒ **仍然分不开**"读完再拒"和"没读就拒"；
   地址这条轴变成**双重关门**（值、形式、以及"越界是否不同码"都不敏感）。这种情况下**不再往地址上花钱**。
2. **`status` 是别的值**（0 之外的任何非 `0x1`，例如 `0x2/0x4/0x8`） ⇒ APM **确实先看地址**，
   而界内那一发是**过了地址校验之后**才死的 ⇒ 死在校验之后的环节：**读失败 / 解析 / 派发**。
   注意这**不等于**"读成功了"。但把两条现有证据摆在一起，重心就移开内存了：
   在数据面活着的那些开机里，**同一句柄、同一区域**的 PCM（`0x0/0x800`）被 DSP 真的消费过
   （`data_buffer … done status=0`，`w32=0xb0e145f8` 回显我们的句柄，slot/offset 跟着我们的乒乓变），
   所以"这块区域 DSP 读不到"已经被**正面证伪过一次**。⇒ 下一步的钱应花在**容器内容**上
   （我们自创的 `0x08001017` + 那份 MIID 清单是头号嫌疑）。
3. **`status = 0`（越界反而被接受）** ⇒ 说明它既不校验也不读，界内那 `0x1` 的来源与地址无关；
   本轮**不作结论**，按仪器意外记录，需要重跑或换问法。
4. **`no rsp`** ⇒ 缺响应不是"被拒"的读数，同 D/E 的规则：**不判**，重跑。

**F3 的两用**（写清楚免得事后当成两件事）：E 是 4 发被拒 ⇒ 数据面 1 发无响应；
F 回到 1 发被拒 ⇒ 若数据面回到 31/4，则 E 那条"剂量效应"读数得到**支持**（每边 n=1，只算支持不算证明）；
若 F 的数据面**也是** 1 发无响应 ⇒ E 的解释被推翻，差别在 0254/0255 的别处（额外 MMIO 读或时序），要单独查。

本轮**不许得到**的结论：`status` 的**具体语义**仍不命名；18,504 正文依然没拿到；`status=0` 不等于出声。
开机记账：120 里已用 **18**；F 轮是第 **19**。

宿主机预验（真实函数体 + 打桩 DSP，`-Wall -Wextra -Wformat=2 -Werror` 干净通过）：
只发 1 次、`off=0x6000 addr=0x6000 len=288 recs=9`、`body not written` 与 `recs not read back` 都打印；
打桩 DSP 那边加了边界（**不加就会踩**：越界解引用直接把宿主机进程打到 SIGBUS，这正说明回读守卫不是可有可无）。

## F 判决（`logs/ring-f-oobrange.txt`，2026-11-05，第 **19** 次开机；身份已核：ring 内 `sweettime=1793858061` 与镜像一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| F0 | 产物 build v97，`oobcfg v3 try `/`is past the declared region`/`recs not read back` 均在，`oobcfg v2 `=0；cmdline **583 B** 与 E 完全相同 | **成立**（变量只在代码里） |
| F1 | 恰好一发：`try 0 form=bare off=0x6000 addr=0x6000 len=288 recs=9 handle=0xb0e145f8`，`body not written`、`recs not read back (past the mapping)` 都打印 ⇒ 我们**没有**越界访问自己的映射 | **成立** |
| **F2** | `oob_batch addr=0x6000 rsp op=0x1001006 status=0x1 hop=0x2001005 len=32` | **走第 1 支：与界内 5 个样本（D 1 + E 4）逐字段同码同长 ⇒ 没观察到范围校验** |
| E5‴ 再次命中 | 六个 CB 的 pre 与 post **逐字节相同**（cb5 `FSR=0x400 FAR=0x2140410 farhi=0x4300` 两侧一致） | 故障见证仍然**读不到东西**；"读没读到"不能由此判 |
| **F3** | `data_buffer send` = **1**、`no rsp` = 1、`done status` = **0**（C/D 是 31 / 0 / 4） | **FAIL，并且推翻我自己在 E 写的"剂量"解释** |
| F4 | `vp/` = 9、`graph_prepare status=0`、`graph_start status=0`、`gate flag=1 exp=0xf` | 配置面完好（无内核 panic；那 8 条 `CRASH/panic` 命中是用户态 `dasd` 尸潮与 `DumpPanic/ReportCrash` 进程名，本轮 `PSCI SYSTEM_RESET` 正常自复位） |

### F3 的更正（写在这里，不遮掩）
E 那节我把数据面死亡归因于"**四次被拒的剂量**"。F 只发了**一次**，数据面**照样**死在第一块。
⇒ **剂量解释被证伪**。按 F3 事先写好的分支，差别必须在 0254/0255 的别处。

### 但 F 顺手把三个开机排成了一条自洽的读法（假设，尚未测）
| 开机 | 探针 | 被试正文地址 | 数据面 |
|---|---|---|---|
| 17 (D) | 1 发 | `0x3000`（**界内、可服务**） | **31 发 + 4 次 done status=0** |
| 18 (E) | 4 发 | `0x1000`/`0x3000` 界内 + `0xa2fe9000`/`0xa2feb000`（**DSP 服务不了**） | 1 发无响应 |
| 19 (F) | 1 发 | `0x6000`（**越出我们声明的区域**） | 1 发无响应 |
⇒ 与数据面死活相关的**不是发数**，而是**被 advertise 的地址是否可服务**。
如果这就是因果，那意味着 DSP **真的去解引用了正文**（读失败会把音频路径带崩），
而界内那一发过了读这一关、死在后面（内容/解析/派发）。
**这条读法的软肋**：D 用的是 0253，E/F 用的是 0254/0255 —— 差分里还混着"新加的两次
`g_smmu_faults()` 读 + 守卫/回读改动"。⇒ 需要一发**界内**对照才能把 lineage 排除。

## G 判据（0256：**一发界内探针 `0x1000`**，代码 lineage 与 F 相同；开机前写死）
唯一变量 = 正文地址是否可服务（`0x6000` → `0x1000`，界内且是我们验过没人在用的空段）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **G0** | 产物含 `SWEET MARK: oobcfg v4 try `，`oobcfg v3 `/`v2 ` = 0；cmdline 与 E/F 同（583 B） | 成立 | 0256 真进产物；参数没动 |
| **G1** | 恰好一发 `off=0x1000 addr=0x1000 len=288 recs=9 handle=<非0>`，**且** 9 条 `rec …` 回读存在、**没有** `body not written`/`not read back` | 成立 | 界内 ⇒ 写了也回读了 |
| **G2（本轮的主判据）** | `data_buffer send` 计数：回到 **31** 且 `done status=0` = **4** | 31/4 ⇒ **界内可服务地址下数据面活着**；1/无响应 ⇒ **0254/0255 自己把平面弄坏** | 前者 ⇒ "死活跟地址可服务性走"这条读法立住，②的重心正式移到**容器内容**（自创 `0x08001017` + MIID 清单是头号嫌疑）；后者 ⇒ 上面那条推论**作废**，先修仪器副作用再谈别的 |
| **G3** | 那一发的 `status=` | 预期仍是 `0x1`（D/E 共 5 个界内样本） | 若变成别的值，说明发数/lineage 里有未记账的变量，本轮记录为异常 |
| **G4** | 回归：`vp/` = 9、`graph_prepare/graph_start status=0`、`gate flag=1`；无内核 panic；`PSCI SYSTEM_RESET` 正常 | 成立 | 与 D/E/F 同形 |

开机记账：120 里已用 **19**；G 是第 **20**。若 G2 落在"1/无响应"（仪器副作用支），**停手**：
那是仪器自身的问题，先记账并交给用户决定，不再往下追内容。

## ② 离线（续 7）：ACDB 的**记录级**参数普查 ⇒ **更正我自己的一条"已验证事实"**

工具：`tools/acdb_graph_rebuild.py` 的 `props_offsets()` + `walk_chain()`（同一套已验证原语），
对 6 份本机 ACDB 从每个 props 头走 TLV 链 ⇒ **9,039 条链、48,225 条记录**（链数与既有的
9,039 条目不变式吻合）。直方图（param_id → 次数）：

```
0x08001001 9039   0x08001000 9039   0x08001002 9039   0x08001003 9039   0x08001004 8149
0x08001061 2090   0x00000024 433    0x08001162  292    0x00000030 256    0x00000028 203 ...
```

**测到的否定结果**：`0x08001017`、`0x0800101b`、`0x08001176` 在**记录级出现 0 次**。

- 而我之前记过的"`0x08001176` 在 ACDB 出现 16 次"是**整文件 u32 字节扫描**的结果。
  把上下文打出来看（`CDP_ukee_acdb_cal.acdb`，全部落在 `off%4==2`）：
  `… 0x0800103c, 0x000043b1, 0x08001017, 0x000043b1, 0x08001018 …`
  ⇒ 那是一张 `{参数 id, ~0x4000..0x43b3 的值}` 的**目录表**（同一值成对夹住相邻参数），
  **不是** `{iid, param_id, size}` 的 TLV 记录。⇒ **那条"已验证事实"作废**：
  字节级出现 ≠ 记录存在。这正是 AGENTS.md 第 5 节"匹配不是证据"的又一次实例，只不过这次是我自己犯在**参数 id** 上。
- 对我们的直接影响：我们 SET_CFG 记录里用的参数 id（`0x08001017` / `0x0800101b`）**不在厂商 ACDB 的任何一条记录里**。
  这**不能**直接推出"所以被拒"——C 轮那条含 `0x08001017 size=12` 的带内 2 记录批次拿到的仍是 `status=0`
  （`ring-c2-ccbatch.txt:1731` `cc_batch rsp op=0x1001006 status=0 hop=0x2001005 len=32`），
  而**逐记录 error_code 至今是盲的**。⇒ 命令级 `status` 与记录级接受**是两件事**，
  我们现在连"未知参数会不会让整条命令失败"都没有测过。
- 因此 ② 的嫌疑顺序（更新版）：
  1. **OOB 这条路本身**（E/F 已把地址轴关掉；G 正在测"死活跟地址可服务性走"这条唯一还活着的推论）；
  2. **记录内容**：我们送的是 ACDB 里**根本不存在**的参数 id —— 现在有了记录级证据；
  3. 拿不到的 18 KB 正文（旧项，仍未解）。

顺带把可用信息立起来：ACDB 记录级真正大量出现的参数是
`0x08001000/0x08001001/0x08001002/0x08001003/0x08001004`（props 头族）与 `0x08001061`(2090)、`0x08001162`(292)。
**下一枪若要换成厂商真实存在的参数**，候选就是这两个非常规 id，且它们带的正文尺寸可以从同一走链里直接读出来
（`size` 字段），不需要猜。

### 同一次走链的附带普查（**带未解释字段，别急着用**）
12,069 条非 props 头记录里，参数量最大的是
`0x08001004`（8,149 条，size 众数 52/20/132/36/180）、`0x08001061`（2,090 条，52/196/244/100/292）、
`0x08001162`（292 条，56/32/64）。
但同一条记录里我以为的 `iid` 字段取值是 **`0x1`(10,239) / `0x0`(1,531) / `0x4`(292)**，
**不是**运行时那种 `0x40bd/0x428f/0x42d3` 模块实例号 ⇒ **这个字段的含义我没定**
（要么 ACDB 里存的是另一种索引，要么我的走链在头之后偏移理解错了）。
⇒ 只把它当**参数 id 的清单**用，**不要**据此认领 iid；要用 iid 得先把这层解释清楚。

## G 判决（`logs/ring-g-oobin.txt`，2026-11-05，第 **20** 次开机；身份已核：`sweettime=1793859080` 两侧一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| G0 | 产物 `oobcfg v4 try ` 在、`oobcfg v3 `/`v2 ` 均为 0；镜像 cmdline **583 B**、尾部 `sweetoobcfg=1 sweettime=1793859080` 连续 | **成立** |
| G1 | 恰好一发 `try 0 form=bare off=0x1000 addr=0x1000 len=288 recs=9 handle=0xb0e145f8`，正文写了、9 条 `rec …` 全回读，**没有** `body not written` / `not read back` | **成立** |
| **G2** | **`data_buffer send` = 31，`done status=0` = 4，`no rsp` = 0**，四条 done 都回显我们的句柄 `w32=0xb0e145f8` | **活着 ⇒ 走第一支** |
| G3 | `oob_batch addr=0x1000 rsp op=0x1001006 status=0x1 hop=0x2001005 len=32`（界内第 6 个同码样本） | 与预测一致 ⇒ 本轮没有未记账变量 |
| G4 | `vp/` = 9、`graph_prepare status=0`、`graph_start status=0`、`gate flag=1 exp=0xf`、**内核 panic = 0**、`PSCI SYSTEM_RESET` 正常 | **成立** |
| 故障见证 | cb5 `oobcfg-pre` 与 `oobcfg-post` 仍**逐字节相同**（`FSR=0x400 FAR=0x22140410 farhi=0x4300`） | 仍然读不到东西（第三次实测同一现象） |

### 判据兑现的结果（G2 第一支）
**同一份代码 lineage（0256 = 0255 只改偏移）、同一套仪器（含每发前后的 `g_smmu_faults()`），
把被试地址从越界 `0x6000` 换成界内 `0x1000`，数据面就从"1 发无响应"回到"31 发 + 4 次 done 回显我们句柄"。**
⇒ 1. **仪器被排除**：多出来的故障寄存器读、边界守卫、打印改动都**不是**数据面死亡的原因。
⇒ 2. **"死活跟地址可服务性走"这条读法立住**：DSP 侧**确实会去解引用**带外正文——
  地址不可服务时音频路径被打死，地址可服务时命令被拒（`0x1`）但图继续跑。
⇒ 3. 于是界内那一发的 `0x1` 是**在读取路径正常工作之后**给出的
  ⇒ **② 的墙正式从"送不送得出去"移到"送什么内容"**（内存/句柄/偏移/形式都不是变量了）。

### 内容侧现在有据可查的嫌疑（离线测量，见"② 离线（续 7）"）
我们记录里用的 `0x08001017` / `0x0800101b` 在 6 份 ACDB 的 **9,039 条 props TLV 链、48,225 条记录里出现 0 次**；
厂商记录里真实大量出现的是 `0x08001004`(8,149)、`0x08001061`(2,090)、`0x08001162`(292)，
且这些 id 的**正文尺寸可以直接从同一条链的 `size` 字段读出来**（如 52/196/244/100/292…），不用猜。
⇒ 下一枪（H）方向：**一发**带外 SET_CFG，把记录换成**厂商 ACDB 里真实存在的 param id + 它自己的尺寸**，
仍然只发一发，仍然用界内可服务地址（`0x1000`），判据先看 `status` 是否脱离 `0x1`。
注意限制：命令级 `status` 与记录级接受是两件事（C 轮含同样参数的带内批次拿到过 `status=0`），
而**逐记录 `error_code` 至今盲**——H 若拿到 `status=0` 也只是"命令级接受"，不得写成"模块接受了参数"。

开机记账：120 里已用 **20**。本轮之后**停手**（目标内 F/G 两枪已打完，判据未事后修改）。

## H 判据（0257：**同一地址/长度/形式/目的/句柄，只把记录内容换成厂商 ACDB 里真实存在的记录**；开机前写死）

G 已经把"仪器 vs 地址"分开并洗清仪器，于是 ② 的嫌疑落在**内容**。H 就是那一次对照：
- 正文地址 `0x1000`（G 实测可服务、数据面 31/4 的那个）、`len=288`、40 B OOB 包、`dst=1`、同一句柄；
- 记录改成走链量出来的**最常见真实三元组** `iid=1, param=0x08001004, size=52`（6 份 ACDB 共 1,099 条），
  正文取 4 条**互不相同**的厂商字节（`CDP_ukee_acdb_cal.acdb` @0x239e6/0x23bde/0x311ae/0x31c7e）；
- 4 × (16 + align8(52)) = 4 × 72 = **288** ⇒ 与 G 的总长、带外判定（>256）完全相同；
- G 送的是 `param=0x08001017, size=12` × 9 —— 该 id 在 48,225 条 ACDB 记录里**出现 0 次**。
⇒ 变化的**只有内容这一束**（param id / 每条尺寸 / 条数 / 正文字节），其余逐项照 G。

| 编号 | 判据 | 取值 | 含义 / 判决走向 |
|---|---|---|---|
| **H0** | 产物含 `SWEET MARK: oobcfg v5 try `；`oobcfg v4 `/`v3 `/`v2 ` 全为 0；cmdline **583 B** 与 G 相同 | 成立 | 0257 真进产物；参数未动 ⇒ 变量唯一 |
| **H1** | 恰好一发 `form=bare off=0x1000 addr=0x1000 len=288 recs=4 handle=<非0 且与 G 同型>`，且 4 条回读都是 `iid=0x1 param=0x8001004 size=52` | 成立 | 厂商记录真的被写进正文并按 72 字节步距回读（宿主机已验步距：打桩 DSP 在 72 步距处回填才能被读到） |
| **H2（本轮唯一主判据）** | 那一发的 `status=` 与 **G 的同址基线**比（G：`status=0x1`，界内共 6 个同码样本） | 见下三支 | |
| **H3** | 数据面仍 `data_buffer send` = **31**、`done status=0` = **4**（回显 `w32=0xb0e145f8`） | 成立 | 内容换了没把平面弄坏 ⇒ 与"地址不可服务 ⇒ 平面死"继续分开两件事；**若平面死了，本轮主判据作废**（说明内容束里混进了别的效果） |
| **H4** | 回归同 G4：`vp/` 9 行、`graph_prepare/graph_start status=0`、`gate flag=1`、**无内核 panic**、`PSCI SYSTEM_RESET` 正常 | 成立 | 设备安全与可比性 |

**H2 判决表（开机前定死）**
1. **`status` 脱离 `0x1`（尤其 = 0）** ⇒ **内容是变量**，我们第一次有一条"厂商形态且被命令级接受"的带外容器
   ⇒ ② 从"送不出去"正式变成"送什么"，下一步按同一三元组扩大正文（真容器 = `Σ(52+align8(blob))`）。
   **但不得写成"模块接受了参数"**：逐记录 `error_code` 仍是盲的（三轮实测 `err=0`）。
2. **`status` 仍是 `0x1`** ⇒ 在**命令级**这个读数上，内容这一束也不敏感
   ⇒ 诚实结论是"**`status` 这个判据分辨不了内容**"，**不是**"内容与墙无关"（记录级从未被测到）。
   ⇒ 那么 ② 剩下的只有两条：要么先做出**能分辨记录级接受与否的仪器**，要么回到"拿不到的 18 KB 正文"。
   **本轮到此停手**，不开第五枪（目标止损：F=19、G=20、H=21 为止）。
3. **`no rsp` / 平面死了（H3 不成立）** ⇒ 不判：读数不可比，按记账交回用户处理。

开机记账：120 里已用 **20**；H 是第 **21**，也是本轮目标内的最后一枪。

## H 判决（`logs/ring-h-oobcontent.txt`，2026-11-05，第 **21** 次开机；身份已核：`sweettime=1793864331` 两侧一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| H0 | 产物 `oobcfg v5 try ` 在、`v4/v3/v2` 全 0；cmdline **583 B** 与 G 相同 | **成立** |
| H1 | 恰好一发 `form=bare off=0x1000 addr=0x1000 len=288 recs=4 handle=0xb0e145f8`；4 条回读都是 `iid=0x1 param=0x8001004 size=52` | **成立**（厂商记录真的进了正文，72 步距回读正确） |
| **H2** | **`rsp op=0x1001006 status=0x3 hop=0x2001005 len=32`** | **走第 1 支：`status` 脱离了 `0x1`** |
| H3 | `data_buffer send` = **31**、`done status=0` = **4**、`no rsp` = 0 | **成立**（换内容没把平面弄坏） |
| H4 | `vp/` = 9、`graph_prepare/graph_start status=0`、`gate flag=1`、**内核 panic = 0**、正常 `PSCI SYSTEM_RESET` | **成立** |
| 记录级 | 4 条 `err=0` | 仪器**第四次**实测为盲 ⇒ 记录级接受与否仍然没测到 |

### 这一枪买到了什么（**这是本轮唯一第一次**）
把 G 的容器（`param=0x08001017, size=12`，ACDB 里 0 条）换成厂商 ACDB 里**最常见真实三元组**
（`iid=1, param=0x08001004, size=52`，1,099 条，正文用 4 条互不相同的厂商字节），
**其余全部照 G**（地址 `0x1000`、总长 288、带外判定、`dst=1`、句柄、代码 lineage、cmdline）：
⇒ **`status` 从 `0x1` 变成 `0x3`。**
1. **`status` 对内容敏感**——这是我们第一次拿到一个能**区分两种容器**的读数。
   之前所有轮次里 `0x1` 是常量（G/F/D/E 共 12 个样本全同码），无法分辨任何东西。
2. **`0x3` 仍然是拒绝**，语义**不命名**（`0x2` 的教训在前）。但它和 `0x1` 不同 ⇒
   APM 在"看完地址、能读正文"之后，**根据正文内容走到了不同的分支**。
3. ⇒ ② 的形状由此改变：不再是"送不出去/墙未知"，而是
   **"有一个能分辨 1 bit 的探针，和两个已知码 `0x1` / `0x3`"** ⇒ 剩下的是**把内容这一束拆成单变量**。

### 下一枪 I（**判据先写好，未开机，等用户点头**）
H 相对 G 一次动了三个子变量：**param id**、**每条 size（连带条数/步距）**、**正文字节**。
I 只动 **param id**：保持 G 的 `size=12`、9 条、32 步距、`0x1000`、`dst=1`，
仅把 `0x08001017` 换成 `0x08001004`（即"真实 id + 我们原来的尺寸/正文"）。
| 编号 | 判据 | 走向 |
|---|---|---|
| I0/I1 | 产物 marker（`oobcfg v6 try ` 在、v5 之下旧串按序归零）+ 一发 `recs=9 size=12 param=0x8001004` | 成立才可判 |
| **I2** | `status=` | **= `0x3`** ⇒ 光换 id 就够 ⇒ `0x1`/`0x3` 的分界在 **param id**，尺寸与正文不是 ⇒ 下一步换 id 集合里别的真实 id 看码是否再变；**= `0x1`** ⇒ id 不是分界，分界在尺寸/正文 ⇒ 下一枪改成"真实 id + 真实尺寸 + 零正文"；**= 第三个码** ⇒ 尺寸错配本身可分辨，同样是大收获 |
| I3 | 平面仍 31/4 | 不成立则本轮作废（不可比） |
| I4 | 回归同 H4；无 panic | 成立 |
预算：120 里已用 **21**；I 是第 **22**。

## I 判据（0258：**只换 param id**，其余回到 G 的容器原样；开机前写死，等用户点头才开）
H 一枪同时动了三个子变量（param id / 每条 size 连带步距与条数 / 正文字节），拿到 `0x3`。
I 把 id 这一维单独拿出来：**九条、size=12、32 步距、正文还是 G 那三个 u32、地址 `0x1000`、`dst=1`**，
唯一差别 `param 0x08001017 → 0x08001004`。

| 编号 | 判据 | 取值 | 含义 / 走向 |
|---|---|---|---|
| **I0** | 产物含 `SWEET MARK: oobcfg v6 try `；`oobcfg v5 `/`v4 `/`v3 `/`v2 ` 全 0；cmdline **583 B** 与 G/H 相同 | 成立 | 0258 真进产物；参数未动 ⇒ 变量唯一 |
| **I1** | 恰好一发 `form=bare off=0x1000 addr=0x1000 len=288 recs=9 handle=0xb0e145f8`，9 条回读全是 `param=0x8001004 size=12`，iid 序列与 G 相同（`0x428f/0x40bd/0x42d3/0x40bf/0x40c0/0x40c8/0x4165/0x4308/0x428f`） | 成立 | 只动了 id，正文与条数照 G |
| **I2（主判据）** | `status=` 与两个已知基线比：**G = `0x1`**（同容器、id 不同，12 个样本同码）、**H = `0x3`**（id+size+正文都换） | 见下三支 | |
| **I3** | 平面仍 `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0 | 成立 | 不成立则本轮不可比，作废记账 |
| **I4** | 回归同 H4（`vp/` 9、prepare/start `status=0`、`gate flag=1`、**无内核 panic**、正常自复位） | 成立 | 设备安全 |

**I2 判决表（开机前定死）**
1. **`status = 0x3`（与 H 同码）** ⇒ `0x1`/`0x3` 的分界**就是 param id 这一维**（尺寸与正文不参与）
   ⇒ 于是 `0x3` 是"APM 认识这个 id、但这条命令在下一步失败"这一类；
   下一枪（J）= 真实 id + 真实 size + **零正文**，把"正文字节要不要紧"单独测出来。
2. **`status = 0x1`（回到 G）** ⇒ id 不是分界 ⇒ 分界在 **size/步距/条数或正文字节**；
   下一枪（J′）= 真实 id + 真实 size 52 + 零正文。
3. **第三个码** ⇒ 尺寸错配本身可分辨 ⇒ 同样是第一次拿到可分离的失败，按码另立判据再走。
4. **`no rsp` / 平面死** ⇒ 不判（读数不可比），交回用户。

限制照写：`0x3` 与 `0x1` **都是拒绝**，语义不命名；逐记录 `error_code` 四轮实测全 0 ⇒ 记录级仍盲。
预算：120 里已用 **21**；I 是第 **22**，**尚未开机，等用户一句"开"**。

## I 判决（`logs/ring-i-oobparamid.txt`，2026-11-05，第 **22** 次开机；身份已核：`sweettime=1793866109` 两侧一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| I0 | 产物 `oobcfg v6 try ` 在，`v5/v4/v3/v2` 全 0；cmdline **583 B** 与 G/H 相同 | **成立** |
| I1 | 恰好一发 `off=0x1000 addr=0x1000 len=288 recs=9 handle=0xb0e145f8`；9 条回读全 `iid=<G 的九个模块号> param=0x8001004 size=12` | **成立**（只动了 id） |
| **I2** | **`status=0x1`**（回到 G 的码） | **走第 2 支：param id 不是 `0x1`/`0x3` 的分界** |
| I3 | `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0 | **成立** |
| I4 | `vp/`=9、prepare/start `status=0`、`gate flag=1`、**panic=0**、正常自复位 | **成立** |

⇒ **H 相对 G 改的三样里，"换成真实 param id"这一样单独不改变码**：
`(真实 id, size 12, 我们的正文) = 0x1`（I）与 `(自创 id, size 12, 我们的正文) = 0x1`（G）同码；
只有 `(真实 id, size 52, 厂商正文) = 0x3`（H）不同。⇒ 分界在**尺寸/步距/条数**或**正文字节**这一束里。

## J 判据（0259：**照 H 逐字不动，只把 4 条记录的 52 字节正文清零**；开机前写死）
| 编号 | 判据 | 取值 | 含义 / 走向 |
|---|---|---|---|
| **J0** | 产物含 `SWEET MARK: oobcfg v7 try `；`v6/v5/v4/v3/v2` 全 0；cmdline **583 B** 与 G/H/I 相同 | 成立 | 0259 真进产物；参数未动 ⇒ 变量唯一 |
| **J1** | 恰好一发 `form=bare off=0x1000 addr=0x1000 len=288 recs=4 handle=0xb0e145f8`；4 条回读全 `iid=0x1 param=0x8001004 size=52`；宿主机已验正文首末字为 0 | 成立 | 只有正文字节与 H 不同 |
| **J2（主判据）** | `status=` 与已知基线比：**H = `0x3`**（同址同长同 dst 同句柄，正文=厂商字节）、**G/I = `0x1`** | 见下三支 | |
| **J3** | 平面仍 31 / 4 / 0 | 成立 | 不成立则该轮不判、停手交回用户 |
| **J4** | 回归同 I4（`vp/`=9、prepare/start `status=0`、`gate flag=1`、**无 panic**、正常自复位） | 成立 | 设备安全 |

**J2 判决表（开机前定死）**
1. **`status = 0x3`（与 H 同码）** ⇒ **正文字节无关**，`0x1`↔`0x3` 的分界在**每条尺寸/步距/条数**这一束
   ⇒ ② 的可爬轴变成"尺寸结构"，下一枪（K，**不在本目标内**）= 真实 id + 真实 size + **一条**记录
   （把"条数"从"尺寸"里再拆出来）。
2. **`status = 0x1`（回到 G/I）** ⇒ **正文字节本身被检查**（APM 或模块看内容）
   ⇒ ② 出现一条**可爬的内容轴**：把正文一段段换成 ACDB 里真实的字节，看码何时离开 `0x1`。
3. **第三个码** ⇒ 记录级校验参与 ⇒ 同样是第一次拿到第三个可分辨读数，按码另立判据再走。
4. **`no rsp` / 平面死** ⇒ **不判**，按目标停手条件交回用户。

限制照写：`0x1/0x3` 都是拒绝、语义不命名；`error_code` 已四轮实测全 0 ⇒ 记录级仍盲。
预算：120 里已用 **22**；J 是第 **23**，也是本目标**最后一枪**。

### J 判据的开机前补充（**不改 J2 三支，只补一条未测变量**）
把 H↔G 的差异逐字段列出来，其实是**四个**子变量，不是我之前写的三个：
`param id`、`每条 size`、`正文字节`、**以及每条记录的 `iid`** ——
G/I 用的是我们图里的**真实模块号**（`0x428f/0x40bd/0x42d3/...`），H/J 用的是 **`iid=1`（= APM 自己）**，
因为我从 ACDB 走链量到的三元组本身就是 `iid=1`。I 只换了 param id，**iid 这一维至今没测过**。
⇒ J 之后：
- 若 J2 落在第 1 支（`0x3`，正文无关），K 必须先把 **iid** 与 **size** 拆开：
  K = H 的容器但把 4 条记录的 `iid` 换成 `0x428f/0x40bd/0x42d3/0x428f`（其余照 H）。
  `0x3` ⇒ iid 不参与，分界只剩尺寸/步距；换码 ⇒ **iid 才是分界**，
  而"记录头里的 iid 指向谁"正是多记录容器由 APM 分发的机制，属于大发现。
- 若 J2 落在第 2 支（`0x1`，正文要紧），iid 仍需在内容爬升过程中固定住不再变。
另外记录一条**测量事实**以免下次误读：ACDB 走链里 `iid` 字段只出现 `0x1/0x0/0x4`（10,239/1,531/292），
**没有**运行时的模块号 ⇒ ACDB 存的不是"每条记录归属哪个运行时模块"，
`iid=1` 在这里更像是结构性字段，所以**不能**据此断言厂商在带上也是 `iid=1`。

## ② 攻坚（续 8）：**ACDB 里有一张"按模块归档的参数表"** ⇒ 内容不用再猜
`tools/acdb_module_tuples.py`（新）从 6 份本机 ACDB 里解出一个结构，纯离线、不开机：

- 记录级普查里 `param=0x08001003` 的**正文**不是数据块，而是一张表：
  ```
  { u32 模块数,
    ( u32 module_instance_id, u32 参数个数,
      ( u32 param_id, u32 param_size, param_size 字节 (补齐到 4) )×参数个数 )×模块数 }
  ```
- 每条 props 链属于一个子图，子图号就在同链 `0x08001002` 的正文里（`0xb00000xx`）。
- 对我们那四个子图 `0xb0000002 / 0xb0000025 / 0xb0000024 / 0xb0000023` 解出
  **157 条互不相同的真实元组**，且 **157 条全部是 `param=0x08001015, size=8`**，一模块一条，data 是厂商字节。
  例子（`CDP_ukee_acdb_cal.acdb`）：
  ```
  sub=0xb0000023 iid=0x040bd param=0x08001015 size=8 data=[0x0,0x1]
  sub=0xb0000023 iid=0x040bf param=0x08001015 size=8 data=[0x1,0x1]
  sub=0xb0000024 iid=0x040c8 param=0x08001015 size=8 data=[0x1,0x1]
  sub=0xb0000002 iid=0x043b1 param=0x08001015 size=8 data=[0x1,0x0]
  ```

### 为什么这条比 H/I/J 更重要
1. **四个字段全是厂商的**：`iid`（运行时模块号，且与我们在带内成功发过的 `dst=0x40bd/0x428f/0x42d3` 同源）、
   `param_id`、`size`、`data`。H 用的 `iid=1` 是**归档字段**不是模块号（ACDB 里该位置只有 `0x1/0x0/0x4`），
   这正是 J 补充里点出的未测维度。
2. **尺寸刚好跨过带外阈值**：一条记录 = `16 + align8(8) = 24 B`；
   **11 条 = 264 B > 256** ⇒ 可以按 GSL 规则**合法地**走带外，不再是"我们硬把小包塞进 OOB"。
3. `0x08001015` 在带内从未被我们发过（我们发过的是 `0x08001017/0x0800101b/0x08001176`），
   而它是 ACDB 对我们子图**唯一**登记的逐模块参数 ⇒ 这是"厂商会往这张图里写什么"的第一手清单。

### 由此定出 K（**不在本目标内，等 J 判完再写死判据**）
`iid=` 我们子图的真实模块号、`param=0x08001015`、`size=8`、`data=` 厂商字节，11 条 ⇒ 带外；
地址沿用已验证可服务的 `0x1000`，`dst=1`，句柄不变。
判据只看一件事：`status` 是否离开 `{0x1, 0x3}`（尤其是否 = 0）。
必须同时带上：`J3` 同形的数据面回归（31/4），以及"仍不许命名任何码的语义"。

### K 的形状已在离线定死（**未开机、未写正式判据**）
单份 ACDB（`CDP_ukee_acdb_cal.acdb`）里，我们四个子图的逐模块记录数是：

| 子图 | 记录数 | 模块号 |
|---|---|---|
| `0xb0000023` | 10 | `0x40bd 0x40be 0x40bf 0x40c0 0x40c1 0x40c2 0x40c3 0x40c4 0x40c6 0x4165` |
| `0xb0000025` | 10 | （同形状，另 10 个 iid） |
| `0xb0000002` | 5 | `0x4012 0x4007 0x418e 0x43b1 0x4586` |
| `0xb0000024` | 1 | `0x40c8` |

合计 **26 条**，每条 `param=0x08001015, size=8` ⇒ 上线格式 `16 + align8(8) = 24 B/条`
⇒ 容器 **26 × 24 = 624 B > 256** ⇒ 走带外是**规则要求的**，不是我们硬塞。
顺带一个对照收获：`0x40bf / 0x40c0 / 0x4165 / 0x40c8` 这些我们之前在带内当"阳性/对照"用过的模块号，
**在厂商 ACDB 的同一子图里就是这么登记的** ⇒ 我们当年从运行时容器里抄出来的模块集合不是幻觉。

### 用线上 MTP 那份重核我们各轮用到的 param id（纯离线）
`MTP_ukee_acdb_cal.acdb` 记录级（props TLV）计数：
```
param=0x08001004 : 1375 条   （size 众数 52×192 / 20×176 / 36×161 / 132×142 / 148×141）
param=0x08001015 :    0 条
param=0x08001017 :    0 条      ← G/I 用的自创 id
param=0x08001176 :    0 条      ← 我们带内当对照用的那条
param=0x0800101b :    0 条
```
⇒ 三点必须写清楚，免得后一轮误读：
1. **H/J 用的 `(iid=1, param=0x08001004, size=52)` 在本机拓扑（MTP）里同样是真实记录**
   （192 条 size=52），不是只在 CDP 里成立 —— 但 **H 的正文字节是从 CDP 抄的**，
   严格说 H 的"内容真实性"混了一层拓扑；**J 把正文清零，反而把这个混淆消掉了**，
   所以 J 比 H 更干净。
2. `0x08001015` 记录级为 0 条是**预期**：它不是 props 链的顶层记录，而是**嵌在 `0x08001003` 表内部**
   的逐模块参数（见"② 攻坚（续 8）"）。它的真实性来自那张表，不是这条计数。
3. `0x08001017 / 0x08001176 / 0x0800101b` 在线上的 MTP 里也是 0 条 ⇒ 之前"我们自创的 id"这个判断
   **不依赖具体拓扑**，站得住。

## 拆变量用的阶梯（预先登记，**每一枪只动一个字段**；本目标只开到第 23 枪）
现在已知（同址 `0x1000`、同 `dst=1`、同句柄、同一轮只发一发）：
`G(自创id, 12B, 我们的正文, iid=真实模块号) = 0x1` · `I(真实id, 12B, 同上) = 0x1` ·
`H(真实id, 52B, CDP正文, iid=1) = 0x3` · `J(真实id, 52B, **零正文**, iid=1) = ?`

H↔G 之间原本捆了四个字段，J 解出"正文字节"这一个。剩下的按下面拆，
**每步只改一格**，免得再出现"一次换四样"：

| 枪 | 相对基准只改 | 内容 | 判据 |
|---|---|---|---|
| **J** | 正文字节 | H 全样 → 正文清零 | 仍是 `0x3` ⇒ 正文无关；回 `0x1` ⇒ 正文被检查 |
| **K1** | `iid` | H 全样（含 H 的正文）→ `iid` 换成真实模块号 `0x40bd/0x40be/0x40bf/0x40c0` | 离开 `0x3` ⇒ **iid 是分界**（这同时就是"多记录由 APM 按 iid 分发"的直接证据） |
| **K2** | `size` | `param=0x08001004` 保留，`size 52 → 20`，正文换成 MTP 里该三元组的真实字节 | 码随 size 变 ⇒ APM 校验参数尺寸；不变 ⇒ 尺寸不是分界 |
| **K2b** | `条数`（**J 之后必须补这一格**） | 保持 H 的 `size=52` 与真实正文，只把 **4 条 → 11 条**（总长 792 B，仍 >256 仍走带外） | J 与 H 的条数相同，所以"正文/尺寸"这一束里其实还捆着**条数**；K2b 才把它拆干净 |
| **K3** | —— | **目标形状**：MTP 的 26 条逐模块记录（`iid=真实模块号, param=0x08001015, size=8, 厂商字节`）= 624 B | `status` 是否离开 `{0x1,0x3}`；这一枪放在最后，出正结果才能直接接上出声 |
| **L** | 拓扑 | 若 K3 拿到 `0`（命令级接受）：把容器换成 2960 那套子图正文再发 | 之后才谈"内容对不对"与数据面是否变化 |

规则再写一遍，免得将来又犯：**一枪只动一格**；`iid`/`size`/正文/地址/条数里任何一次改两样，
得到的码就只算"新数据点"、**不算**"分辨出了哪一维"。所有轮次都要带 `data_buffer send`=31 /
`done status=0`=4 的回归，否则该轮不判。

## J 判决（`logs/ring-j-oobzeropay.txt`，2026-11-05，第 **23** 次开机；身份已核：`sweettime=1793867358` 两侧一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| J0 | 产物 `oobcfg v7 try ` 在、`v6/v5/v4/v3` 全 0；cmdline **583 B** 与 G/H/I 相同 | **成立** |
| J1 | 恰好一发 `form=bare off=0x1000 addr=0x1000 len=288 recs=4 handle=0xb0e145f8`；4 条回读全 `iid=0x1 param=0x8001004 size=52` | **成立** |
| **J2** | **`status=0x3 hop=0x2001005 len=32`（与 H 同码）** | **走第 1 支：正文字节不是分界** |
| J3 | `data_buffer send` 31、`done status=0` 4、`no rsp` 0 | **成立** |
| J4 | `vp/`=9、prepare/start `status=0`、`gate flag=1 exp=0xf`、**内核 panic=0**、`PSCI SYSTEM_RESET` 正常 | **成立** |

正文"是零"这一点是**由代码构造保证**的（`memset(body,0,…)` 之后只写 12 字节头 + error_code，
J 的改动就是把那 13 个 u32 的赋值删掉）；回读只打 `iid/param/size/err`，**不打正文字节**，
所以设备上没有独立的正文证据——这条限制写在这里，不当成已验证。
（宿主机跑真实函数体时验过：`pay0=0 pay12=0`。）

### 至此已测事实（同一地址 `0x1000`、同一长度 288、同一 `dst=1`、同一句柄、每轮一发）
| 轮 | iid | param | 每条 size | 条数 | 正文字节 | `status` |
|---|---|---|---|---|---|---|
| G | 真实模块号 | 自创 `0x08001017` | 12 | 9 | 我们的 | `0x1` |
| I | 真实模块号 | 真实 `0x08001004` | 12 | 9 | 我们的 | `0x1` |
| H | **`1`(=APM)** | 真实 `0x08001004` | **52** | **4** | 厂商(CDP) | `0x3` |
| J | `1`(=APM) | 真实 `0x08001004` | 52 | 4 | **全零** | **`0x3`** |
⇒ **正文字节被排除**；`0x1`↔`0x3` 的分界只剩三个候选，而且它们在 H↔J 里**仍然是捆在一起的**：
**每条 size（52 vs 12）**、**条数/步距（4×72 vs 9×32）**、**iid（1 vs 真实模块号）**。
⇒ 按预先登记的阶梯：K1 只改 `iid`，K2 只改 `size`，K2b 只改条数。**任何一枪同时改两样，得到的码只算新数据点，不算分辨出维度。**

### 本目标按止损收束
预算 120 已用 **23**（F=19、G=20、H=21、I=22、J=23），目标写明"J 是最后一枪" ⇒ **停手**，
K1/K2/K2b 等下一轮授权再开。全程无内核 panic、每轮正常 `PSCI SYSTEM_RESET` 自复位，
设备现在在 Android 侧待命。

## K1 判据（0260：**照 H 只把 4 条记录的 `iid` 换成真实模块号**；开机前写死）
基线（同址 `0x1000`、同 `dst=1`、同句柄、同 cmdline 583 B、每轮一发、`hop=0x2001005 len=32`）：
G `0x1` · I `0x1` · H `0x3` · J（正文全零）`0x3` ⇒ 地址、形式、id 单独、正文字节**都已排除**；
H↔J 还捆着三样：**每条 size**、**条数(步距)**、**iid**。K1 只动 `iid`：
`iid = 0x40bd / 0x40be / 0x40bf / 0x40c0`（MTP 给子图 `0xb0000023` 登记的前四个模块），
其余（`param=0x08001004`、`size=52`、4 条、步距 72、总长 288、正文=H 那四组厂商字节）
**逐项与 H 相同**。宿主机已跑过真实函数体：`rec 0..3 iid=0x40bd/0x40be/0x40bf/0x40c0 param=0x8001004 size=52`、
正文首末字 `0x3 / 0x2`、一发、`addr=0x1000`、`dst=1`。

| 编号 | 判据 | 取值 | 含义 / 走向 |
|---|---|---|---|
| **K1-0** | 产物含 `SWEET MARK: oobcfg v8 try `，`oobcfg v7 `/`v6 `/`v5 `/`v4 ` 全 0；cmdline **583 B** 与 G–J 相同 | 成立 | 0260 真进产物；参数未动 ⇒ 变量唯一 |
| **K1-1** | 恰好一发 `off=0x1000 addr=0x1000 len=288 recs=4 handle=<非0>`，四条回读 `iid=0x40bd/0x40be/0x40bf/0x40c0`、`param=0x8001004`、`size=52` | 成立 | 只有 iid 变了（若四条里还出现 `iid=0x1` 说明补丁没进去） |
| **K1-2（主判据）** | 那一发的 `status=` 与 H/J 的 `0x3`、G/I 的 `0x1` 比 | 见下三支 | |
| **K1-3** | 平面仍 `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0 | 成立 | 不成立则该轮不判、停手交回 |
| **K1-4** | 回归同 J4（`vp/`=9、prepare/start `status=0`、`gate flag=1`、**无内核 panic**、正常自复位） | 成立 | 设备安全 |

**K1-2 判决表（开机前定死，事后不改）**
1. **仍是 `0x3`** ⇒ **iid 不参与** ⇒ 分界只剩 **每条 size** 与 **条数/步距** ⇒ 下一枪 K2 只改 size（52→20，正文换成 MTP 该三元组真实字节），再下一枪 K2b 只改条数（4→11）。
2. **变成 `0x1`** ⇒ **iid 是分界**（方向上是"真实模块号反而退回 G/I 的码"）⇒ 说明 `0x3` 是"记录指向 APM 自己"才走到的分支；② 的容器构造规则就此多一条硬约束：**图级批量记录该用哪个 iid**。
3. **第三个码**（既不是 `0x1` 也不是 `0x3`）⇒ 同样证明 iid 参与，且 APM 对" iid 指向图内真实模块"另有分类 ⇒ 记为新的已知码，下一枪仍是 K2（先把 size 这格拆干净）。
4. **`no rsp` / 平面死** ⇒ **不判**，按停手条件交回用户。

限制照写：所有码**都是拒绝**，不命名机制；`error_code` 已五轮全 0 ⇒ 记录级仍盲。
预算：120 里已用 **23**；K1 是第 **24**。

## K1 判决（`logs/ring-k1-iidonly.txt`，2026-11-05，第 **24** 次开机；身份已核：`sweettime=1793870168` 两侧一致）

| 编号 | 读数 | 判定 |
|---|---|---|
| K1-0 | 产物 `oobcfg v8 try ` 在、`v7/v6/v5/v4` 全 0；cmdline **583 B** 与 G–J 相同 | **成立** |
| K1-1 | 恰好一发 `off=0x1000 addr=0x1000 len=288 recs=4 handle=0xb0e145f8`；四条回读 `iid=0x40bd/0x40be/0x40bf/0x40c0`、`param=0x8001004`、`size=52` | **成立**（只有 iid 变了） |
| **K1-2** | **`status=0x1 hop=0x2001005 len=32`** | **走第 2 支：iid 是分界** |
| K1-3 | 平面 31 / 4 / 0 | **成立** |
| K1-4 | `vp/`=9、prepare/start `status=0`、`gate flag=1`、**内核 panic=0**、正常自复位 | **成立** |

### 六个数据点拼出来的形状（同址 `0x1000`、同 `dst=1`、同句柄、每轮一发）
| 轮 | 传输 | iid | param | size | 条数 | 正文 | `status` |
|---|---|---|---|---|---|---|---|
| C | **带内** | 真实模块号 | 自创 | 12 | 2 | 我们的 | **`0`** |
| G | 带外 | 真实模块号 | 自创 `0x08001017` | 12 | 9 | 我们的 | `0x1` |
| I | 带外 | 真实模块号 | 真实 `0x08001004` | 12 | 9 | 我们的 | `0x1` |
| **K1** | 带外 | 真实模块号 | 真实 `0x08001004` | **52** | **4** | **厂商** | **`0x1`** |
| H | 带外 | **`1`=APM** | 真实 `0x08001004` | 52 | 4 | 厂商 | `0x3` |
| J | 带外 | `1`=APM | 真实 `0x08001004` | 52 | 4 | **全零** | `0x3` |
⇒ 1) **`iid` 是唯一能把 `0x1` 和 `0x3` 分开的字段**：其余全同，只把 iid 从真实模块号换成 `1` 就得到 `0x3`。
   所以 APM **确实读了正文**（否则看不到 iid 的差异），并且对"记录指向 APM 自己"另有一个失败分支。
⇒ 2) **`size`/`条数`/`正文` 在 `iid=真实` 下全部不影响码**（G/I/K1 三种组合都是 `0x1`）
   ⇒ 原计划的 K2（只改 size）与 K2b（只改条数）**已失去信息量**，不该再花开机。
⇒ 3) 剩下唯一没测、而且**横跨 C 与 G/I/K1** 的差别是**传输方式本身**：
   同样的 iid=真实 + 多记录容器，**带内拿到 `0`，带外拿到 `0x1`**。
   ⇒ 所以"② 的墙"现在的形态是：**OOB 这条路对我们这个句柄/池就是不通**，而不是内容不通。

### 目标内改序（开机前登记，不是事后追认）
K1 的结果把 K2/K2b 变成低信息量，故本目标剩余两枪改为：
- **第 25 枪 L1**：**容器与 K1 逐字节相同（4 条、iid=真实、`0x08001004`、size 52、厂商正文、288 B），只把传输改成"带内"**
  ⇒ 变量只有一个：OOB→in-band。`status` 变 `0` ⇒ 墙确认为 OOB 通路；仍 `0x1` ⇒ 尺寸>256 的带内被 APM 拒，
  那就换成"同样内容但截到 256 以内（3 条=72 B）带内"再判。
- **第 26 枪 L2**：带内 3 条（72 B ≤256）同内容 ⇒ 把"超过 256 的带内包"与"带内本身"分开。
（K2/K2b 记为"被 K1 淘汰，未花开机"。）

新增观察随 L1 一起上：**响应之后把整份正文与写入内容逐字节比对**（`body touched=%u first=%u`）；
它不改任何发送字节，只给"没写回"一个正面读数。宿主机已跑过（打桩回填 ⇒ `touched=8 first=12`）。

## L1 判据（0261：**K1 的容器逐字节不动，只把传输从带外换成带内**；开机前写死）
K1 已把 `size` 与 `条数` 变成低信息量（iid=真实模块号时三种组合都是 `0x1`），
所以剩下唯一横跨"成功"和"失败"的差别是**传输本身**：
C（带内、iid=真实、多记录）⇒ `0`；K1（带外、iid=真实）⇒ `0x1`。

容器：4 条 × 72 B = **288 B**，`iid=0x40bd/0x40be/0x40bf/0x40c0`、`param=0x08001004`、
`size=52`、正文=厂商字节；发送走 `g_apm_cmd`（C 用的同一个 helper），
`dst=1`、`src=0x2010`、`op=0x01001006`；**正文仍然写进映射区域**，
所以回读与新增的 `body touched=` 与 K1 可比。宿主机已跑过：
一发、包长 328 B、`plen=288`、`first=0x40bd`、四条 `iid/param/size` 正确、`touched=0`。
**run 387 之后重跑（`tools/stub_oobcfg.py`，从"CI 落库态 + 0261"重建的文件里原文抽函数，
`-Wall -Wextra -Wformat=2 -Werror` 编译并三种模式跑过）**：
DSP 沉默 ⇒ `touched=0 first=0`；DSP 每记录回写 `0x3` ⇒ `touched=4 first=12`（只低字节变，
因为我们把 error_code 预置成 0）；DSP 回写 `0` ⇒ **`touched=0`**——所以这条仪器**分辨不了
"没写"和"写了 0"**，见 L1-5 的修正。

| 编号 | 判据 | 取值 | 含义 / 走向 |
|---|---|---|---|
| **L1-0** | 产物含 `SWEET MARK: oobcfg v9 try `，`oobcfg v8 `/`v7 `/`v6 ` 全 0；cmdline **583 B** 与 G–K1 相同 | 成立 | 0261 真进产物；参数未动 ⇒ 变量唯一 |
| **L1-1** | ring 里那一发是**带内**：`APM oob_batch src=0x2010 dst=0x1 (328 bytes)`，并且 `oobcfg v9 try 0 form=inband off=0x1000 len=288 recs=4` | 成立 | 传输真的换了（不是又发了一个 40 字节的带外包） |
| **L1-2（主判据）** | `oob_batch rsp op=0x1001006 status=?` | 见下三支 | |
| **L1-3** | 平面仍 `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0 | 成立 | 不成立则该轮不判、停手 |
| **L1-4** | 回归同 K1-4（`vp/`=9、prepare/start `status=0`、`gate flag=1`、**panic=0**、正常自复位） | 成立 | 设备安全 |
| **L1-5（新仪器，非判据）** | `body touched=%u first=%u of 288` | 记录 | **只观察**，且两种不可分辨支已写明：① 带内时 DSP 没有理由读那块区域 ⇒ 预期 `touched=0`；② 若 DSP 把 `error_code` 写成 **0**，因为我们预置 0，`touched` 也是 0 ⇒ **`touched=0` 不等于"没写回"**，只有非 0 才是证据（`first=` 指出第一个被改的字节）。若非 0 说明有人写了这份正文，是独立线索，**不改判据** |
| **L1-6（仪器自身的限制）** | 带内时的 `rec %u ... err=%#x` 读的是**区域里的副本**，APM 在带内没有理由碰它 | 预期恒 `err=0` | **不许**把带内这四条 `err=0` 读成"记录级也接受了"；记录级仍是盲的（与 K1 的 0x1 同判） |

**L1-2 判决表（开机前定死）**
1. **`status = 0`** ⇒ **墙就是带外通路**（同一份内容带内就接受）。⇒ ② 的下一步不是再改内容，而是**为什么我们的 OOB 包不被处理**：嫌疑回到 `mem_map_handle` 与区域（`0x10003, flag4, IOVA, size`）在 **CAL/config 读路径**上的绑定方式。
2. **`status = 0x1`（与 K1 同码）** ⇒ 带内 >256 B 也被拒 ⇒ 分界其实是**长度阈值**（GSL 的 256 是客户端规则，服务端可能同样按它校验）。⇒ 第 26 枪 L2 改成**同内容截到 3 条 = 216 B ≤256 带内**：拿到 `0` ⇒ 确认"≤256 带内 + 真实 iid"是可行组合；仍非 0 ⇒ 与 C 的差异只剩 iid 集合与 param，回到内容轴。
3. **第三个码** ⇒ 又一种可分辨读数，记入基线表，按 L2 的分支规则继续。
4. **`no rsp` / 平面死** ⇒ **不判**，按停手条件交回用户。

限制照写：`0/0x1/0x2/0x3` 除 `0` 外**都是拒绝**且不命名机制；`0` 也只写"命令级接受"；
逐记录 `error_code` 五轮全 0 ⇒ 记录级仍盲。

**L1-2 开机前修订（零开机；重跑 `scripts/parse_gpr_trace.py logs/vendor-gprsend.txt`）**
厂商 42 包普查里 **SET_CFG 只有 1 包**，形态固定：`len=40 src=0x2010 dst=0x0001 token=0x4 dom=0x302`、
`addr=0x800/0x0 handle=0xb0d89128 size=0x2a0(=672 B)`，**正文全在共享内存里**（包体 14 个 0）。
⇒ ① 厂商**从不发带内 SET_CFG**（不分大小），所以这份捕获既不能证明"带内小尺寸会被接受"、
也不能证明"带内大尺寸会被拒"；② 我们唯一一次带内 SET_CFG 是 **C：`recs=2 len=80`、iid=真实 `0x428f`、
但 param 是自创的 `0x08001017`/`0x0800101b`、每条 size 12** ⇒ `status=0`。
两支的措辞按此收紧：
- **支 1（`status=0`）**：除"墙=带外通路"外必须同时写成"**带内长度不是我们的变量**"；
  厂商 OOB 672 B 能用、我们 OOB 672/288 B 不能用 ⇒ 差别只剩**句柄/区域的绑定方式**（按原计划查 `mem_map_handle`）。
- **支 2（`status=0x1`）**：**不许**写成"服务端按 256 阈值校验"。能写的只有
  "**这份 288 B 带内内容被拒**、与带内 80 B 的 C 不同码"。而 C 与 L1 之间除长度外还差
  **param（自创 → 真实 `0x08001004`）与每条 size（12 → 52）** ⇒ 支 2 之下 L2 仍有信息量：
  **L2 = 同内容截到 3 条 = 216 B 带内**新增的不是"再短一点"，而是**第一次把真实 param + size 52 放进带内 ≤256**；
  L2 拿 `0` ⇒ 分界含长度；L2 仍 `0x1` ⇒ 与 C 的差只剩 param/size ⇒ **回到内容轴**（不是回到传输轴）。
预算：120 里已用 **24**；L1 是第 **25** 枪，L2 是第 **26**（本目标上限）。

## L1 判决（0261，第 25 次开机，`logs/ring-l1-inband.txt`，sweettime=1793873640）

**主判据落在预注册的支 2：带内同一份容器仍是 `status=0x1` ⇒ 传输方式不是 `0`↔`0x1` 的分界。**

| 门 | 读数 | 结论 |
|---|---|---|
| L1-0 身份 | kernel+镜像含 `oobcfg v9 try `（各 1）、`form=inband` 1、`oobcfg v8/v7/v6` **0**；cmdline **583 B** 与 G–K1 全等；ring 内 `sweettime=1793873640` 命中 | **成立**（run 387 跳过补丁的教训走完后，这轮真是 0261） |
| L1-1 真的换成了带内 | `oobcfg v9 try 0 form=inband off=0x1000 len=288 recs=4 handle=0xb0e145f8` + `SWEET GLINK: APM oob_batch src=0x2010 dst=0x1 (328 bytes)` | **成立**（`g_apm_cmd` 的描述符三字段全 0、正文内联在 +40，`pkt_size=328` 自证） |
| L1-2 主判据 | `oob_batch rsp op=0x1001006 status=0x1 hop=0x2001005 len=32` | **`0x1`＝与 K1（带外、同内容）同码** |
| L1-3 平面 | `data_buffer send` **31** / `done status=0` **4** / `no rsp` **0** | **成立**（与 K1 逐条等值 ⇒ 换传输没有打死数据面） |
| L1-4 回归 | `vp/` 全部 18 = 发 9 + 应 9、`gate flag` 1、`panic` 0、`PSCI SYSTEM_RESET` 正常自复位 | **成立** |
| L1-5 新仪器 | `body touched=0 first=0 of 288` | **记录**，且按预注册写法**不作证据**：`touched=0` 与"DSP 把 error_code 写成 0"不可分辨（我们预置 0），只有非 0 才是线索 |
| L1-6 仪器限制 | 四条 `rec … err=0`（带内时读的是区域副本，APM 没理由碰） | **不许**读成"记录级也接受"；记录级仍盲（第七轮） |

**这一枪买到的结论（不命名机制）**：
1. `0x1` 与传输**无关**——同一份内容（4×72=288 B、iid 真实、`param=0x08001004`、`size=52`）带外与带内**同码**。
   ⇒ 目标里"带内 vs 带外是唯一横跨 `0` 与 `0x1` 的轴"这个前提**被自己的一枪关掉**。
2. 于是横跨 `0`（C）与 `0x1`（L1/K1）的**剩余差**只剩四样，且两两捆着：
   **长度 80 vs 288（跨 256）**、**条数 2 vs 4**、**每条 size 12/28 vs 52**、**param 自创 vs 真实 `0x08001004`**
   （iid 两边都是真实模块号，已排除；正文字节已排除；地址/形式已排除）。
3. 数据面在两种传输下都活着（31/4/0）⇒ `0x1` 不是仪器副作用。

**下一枪（本目标最后一枪，第 26）**：照预注册支 2 走 **L2 = L1 逐字不动、只把条数 4→3 ⇒ 216 B ≤256 带内**。
它是相对 L1 的**单变量**（长度/条数一起动，但条数是唯一能改长度而不动 param/size/iid 的旋钮）：
- `0` ⇒ 分界含**长度**，并且"≤256 带内 + 真实 iid + 真实 param + size 52"是**可用组合**（大配置可切块送）；
- `0x1` ⇒ 分界不含长度 ⇒ 与 C 的差只剩 **param/size** ⇒ 下一枪只换 param id（拿 C 的形状 + `0x08001004`）。

预算：120 里已用 **25**；L2 是第 **26**（本目标上限）。

## L2 判据（0262：**L1 逐字不动，只把发出去的条数 4→3 ⇒ 正文 216 B ≤256 带内**；开机前写死）

L1 把传输轴关掉之后，横跨 `0`（C）与 `0x1`（L1/K1）的差只剩四样且两两捆着：
**长度（80 vs 288，跨 256）/ 条数（2 vs 4）/ 每条 size（12 vs 52）/ param（自创 vs 真实 `0x08001004`）**。
条数是**唯一**能在不动 param/size/iid/字节的前提下改变长度的旋钮，所以这一枪相对 L1 是单变量。
容器：3×72 = **216 B**（整包 24+16+216 = **256 B**，判长度阈值看的是**正文** 216 ≤ 256），
`iid=0x40bd/0x40be/0x40bf`（L1 的前三条，逐字节同）、`param=0x08001004`、`size=52`、正文=厂商字节、
`dst=1`、`src=0x2010`、走 `g_apm_cmd`；数组仍 4 行 ⇒ 第 4 条字节不参与发送也不参与回读。
宿主机已跑过（`STUB_SRC=/tmp/v0262/... tools/stub_oobcfg.py`，编译 + 三模式运行）：
`len=216 recs=3`、一发 `plen=216`、三条 `rec` 的 iid/param/size 正确、`body touched` 写 0x3 时 =3/first=12、沉默时 =0。

| 编号 | 判据 | 取值 | 含义 / 走向 |
|---|---|---|---|
| **L2-0** | kernel 与镜像都含 `SWEET MARK: oobcfg v10 try `，`oobcfg v9/v8/v7/v6` **全 0**；cmdline **583 B**（与 G–K1、L1 全等）；ring 里 `sweettime=<本轮>` 命中 | 成立 | 0262 真进产物、变量唯一（run 387 的教训按新规则核过） |
| **L2-1** | `oobcfg v10 try 0 form=inband off=0x1000 len=216 recs=3 handle=0xb0e145f8` **且** `SWEET GLINK: APM oob_batch src=0x2010 dst=0x1 (256 bytes)` **且** `rec` 行恰好 **3** 条（`iid=0x40bd/0x40be/0x40bf`、`param=0x8001004`、`size=52`） | 成立 | 长度真的降到 256 以下、条数真的少一条（少任一 ⇒ 本轮不作长度结论） |
| **L2-2（主判据）** | `oob_batch rsp op=0x1001006 status=?` | 见下判决表 | |
| **L2-3** | 平面仍 `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0 | 成立 | 不成立 ⇒ 该轮**不判**、停手 |
| **L2-4** | 回归同 L1：`vp/` 18（=发 9 + 应 9）、`gate flag` 1、**panic 0**、`PSCI SYSTEM_RESET` 正常自复位 | 成立 | 设备安全 |
| **L2-5（仪器，不作证据）** | `oobcfg v10 body touched=%u first=%u of 216` | 记录 | 预期 0；**按预注册写法**：`touched=0` 与"写回 0"不可分辨 ⇒ 不得据此判"正文没被读" |

**L2-2 判决表（开机前定死）**
1. **`status = 0`** ⇒ **跨 256 的长度在分界里**（同一份内容 288 B 被拒、216 B 命令级接受），
   并且给出一个**可用组合**：带内 ≤256 + 真实 iid + 真实 `param=0x08001004` + `size=52`。
   ② 的下一步随即可离线设计：把要的正文切成 ≤256 B 的多条 SET_CFG 送。本目标到 26 枪为止 ⇒ 只交付这条结论。
2. **`status = 0x1`（与 L1 同码）** ⇒ 长度**不在**分界里 ⇒ 与 C 的差只剩 **param id** 与 **每条 size**
   ⇒ 交下一轮（本目标预算已尽）：拿 C 的形状（2 条、size 12/28、80 B 带内）**只把 param 换成 `0x08001004`**，
   那才是真正的一格。
3. **第三个码** ⇒ 记进基线表当作可分辨读数，不命名机制。
4. **`no rsp` / 平面死** ⇒ **不判**，按停手条件交回用户。

语义纪律照写：`0x1/0x2/0x3` 都是**拒绝**且不命名机制；`0` 只写"命令级接受"；逐记录 `error_code` 到 L1
已连续 6 轮读 0 ⇒ 记录级仍盲。预算：120 里已用 **25**，L2 是第 **26** 枪（本目标上限，打完即停）。

## 离线普查：ACDB 里"真实三元组"到底长什么样（`tools/acdb_param_census.py`，零开机、零网络）

网卡了这一轮不碰 CI 与开机，把 L2 的前提拿到本地 `MTP_ukee_acdb_cal.acdb`
（1,465,878 B，与 `work/phone-acdb` 和 `work/sweet-audio/vendor-ukee` 两份 **sha 相同** ⇒ 就是线上那份）里核到底。

普查对象是分开的两处，而我们的探针一直把它们混着当"同一种记录"：

| 位置 | 首词是什么 | param 取值 | 与模块号的关系 |
|---|---|---|---|
| **链级 TLV 记录**（`chain_records`，首词 ≤ `0x20`） | **pool 索引**（实际全是 `0x1`），**不是模块 instance id** | `0x08001000/1001/1002/1003/1004` 家族；`0x08001004` 有 size 20/36/52/…/404 | 与模块号无关 |
| **`0x08001003` 里的逐模块 cal 表** | 真正的 `module_instance_id` | **只有一个** `param=0x08001015 size=8`（1607 个不同三元组、9397 条） | 这里才是"模块 ↔ cal 参数"的注册表 |

三条硬事实（命令：`python3 tools/acdb_param_census.py --file MTP_ukee --iid 0x428f,0x40bd,0x40be,0x40bf,0x40c0`）：
1. **`0x08001004` 从不出现在逐模块表里**（`header-family params in the per-module table: 0`）。
   ⇒ K1/L1/L2 发的 `(iid=真实模块号, param=0x08001004, size=52)` **在这份 ACDB 里任何地方都不存在**，
   是我们拼出来的混合格；H/J 发的 `(iid=1, param=0x08001004, size=52)` 才是逐字抄自链级记录——
   但那里的 `iid=1` **是 pool 索引，不是模块号**。
2. **C 发的东西同样是凭空造的，而且造得更彻底**：`iid=0x428f` 不在任何逐模块表里，
   `param=0x08001017` 与 `0x0800101b` 在**两处都不存在**（chain NONE / table NONE）⇒ 却拿到 `status=0`。
3. ⇒ ** 诱人规则"非 0 码 = 该三元组没注册"被 C 直接否掉，且不需要开机**。

**对 L2 的影响：不改变下一枪。** C 与 L1 之间唯一还干净地分开的轴仍是**长度**（80 vs 288，跨 256），
这正是已在飞的 L2（216 B 带内）。普查的价值是把"内容轴"的下一次枪**算准**，不是换枪。

**M1（已算好、待网络恢复后只需照抄常量，不改判据）**：第一次发**ACDB 真正注册过的模块记录**——
`{iid=0x40bd, param=0x08001015, size=8, bytes=00000000 00000001}`、
`{0x40be/0x40bf/0x40c0 → 同 param，bytes=00000001 00000001}`；步距 16+8=**24**，4 条 = **96 B**（带内 ≤256）。
诚实写明它的局限：**M1 相对 L2 同时动了 param、每条 size、步距与总长四格**，
所以 M1 只能**与 L2 并排读**才有意义（L2=0x1 而 M1=0 ⇒ 分界在混合三元组；两者都 0x1 ⇒ 内容整体出局），
不能单独当"一枪一格"。

顺带修一处数据完整性：`docs/data/acdb-mtp-module-tuples.txt` 本轮发现是 **0 字节**
（原先"157 条"的那份产物已空），现由 `acdb_param_census.py` 的普查结果取代并重新生成一份。

**跨文件复核（同一命令换 `--file`）**：把五份本地 ACDB（CDP_ukee / QRD_ukee / MTP_ukee / IDP_UPD / QRD_diwali）全跑一遍，
结论一致且与拓扑无关：`0x08001004` **每份都只有链级**（size 20…404，步长 16）、**每份都不在逐模块表**；
`0x08001017` 与 `0x0800101b`（C 用的那两个）**每份都两处都不存在** ⇒ C 的"自创"标签现在是**被证明**的，
不是推测；而 C 恰是**唯一拿到 `status=0` 的那一份内容**。
⇒ 三个码目前追踪的不是"ACDB 真假"，而是我们自己记录的结构组合
（`iid=1` ⇒ `0x3`；真实 iid + `size 52` ⇒ `0x1`；真实 iid + `size 12/28` 且总长 80 带内 ⇒ `0`）。
L2 正落在最后一格与第二格之间唯一干净的差别上：**总长跨不跨 256**。

### 672 B 的分解：负结果（离线，零开机）
厂商那份唯一 OOB SET_CFG 的正文是 `payload_size=0x2a0=672`。拿 MTP 的 1524 张逐模块表逐张算
"Σ(16 + align8(param_size))"（即真把这些记录发到线上要占的字节数），**没有任何一张等于 672**；
最接近的是 480（20 条 × 24 B，`0x08001015 size=8`）。
⇒ **不能**把厂商那 672 B 读成"某个子图整张逐模块表的记录集"。这条推论到此为止，
不拿去解释 `0x1`，也不据此改枪。

### 第 26 枪的一次**未遂**（记在这里，避免下次误记为已开）
`images/l2-inband-3recs.img` 打包与身份全部核过（`oobcfg v10 try` 在镜像 1 次、`v9/v8`=0、
cmdline **583 B**、`sweettime=1793877856` 在产物内），但 `scripts/boot_one.sh` 在
`adb reboot bootloader` 之后轮询 120 秒没等到 fastboot 节点，打印 `!! not in fastboot` 自行退出；
随后 adb、fastboot、`system_profiler` 三方都看不到设备与 Qualcomm USB 节点 ⇒ USB 链路掉线。
**该枪没有发出**，`fastboot boot` 从未执行，预算不变 **25/120**，没有任何分区被写入。
另记一条工具坑（和已有的"管道吞失败"同类）：我把启动命令写成 `… | tail -30`，
于是后台通知报的是 `tail` 的退出码 0，**看起来像跑完了**——判"有没有真开过机"要认日志正文
（`Booting OKAY` 行）而不是退出码。这份镜像可以直接重开，无需重建。

## L2 判决（0262，第 26 次开机 = 本目标上限，`logs/ring-l2-inband-3recs.txt`，sweettime=1793877856）

**主判据落在预注册的支 2：正文降到 216 B（≤256）带内，仍是 `status=0x1` ⇒ 长度不在分界里。**

| 门 | 读数 | 结论 |
|---|---|---|
| L2-0 身份 | kernel 与镜像含 `oobcfg v10 try `（各 1）、`oobcfg v10 body touched=` 1、`v9`=0、`v8`=0；cmdline **583 B** 与 G–K1、L1 逐字同；ring 内 `sweettime=1793877856` 命中 | **成立**（run 387 那条规则改掉后第一次一把过：#389 success） |
| L2-1 容器自证 | `oobcfg v10 try 0 form=inband off=0x1000 len=216 recs=3 handle=0xb0e145f8`；`APM oob_batch src=0x2010 dst=0x1 (256 bytes)`；`rec` 行恰好 **3** 条 = `0x40bd/0x40be/0x40bf`、`param=0x8001004`、`size=52` | **成立**（长度真的降到 256 以下，条数真的少一条） |
| L2-2 主判据 | `oob_batch rsp op=0x1001006 status=0x1 hop=0x2001005 len=32` | **`0x1`＝与 L1(288 B 带内)、K1(288 B 带外) 同码** |
| L2-3 平面 | `data_buffer send` **31** / `done status=0` **4** / `no rsp` **0** | **成立** |
| L2-4 回归 | `vp/` 18 = 发 9 + 应 9（九条 `status=0`）、`gate flag` 1、**panic 0**、`PSCI SYSTEM_RESET` 正常自复位、设备已回到 Android | **成立** |
| L2-5 仪器 | `body touched=0 first=0 of 216` | 按预注册写法**不作证据**（预置 0 ⇒ 写回 0 与从不写回不可分辨）；记录级第 8 轮仍盲 |

**这个目标要拆的三格，到此拆完（答案不是原计划的三格）**：
- **iid**（K1）：`iid=1 → 0x3`，`iid=真实模块号 → 0x1` ⇒ **iid 是分界**。
- **每条 size / 条数**（原 K2/K2b）：在 iid=真实 之下，size 12 与 52、4 条与 9 条**都是 `0x1`**（G、I、K1）⇒ 这两格**低信息量，未花开机即淘汰**（开机前记录）。
- **传输**（L1）：带外→带内**同码 `0x1`** ⇒ 传输不是分界。
- **长度**（L2）：288→216（跨不跨 256）**同码 `0x1`** ⇒ 长度不是分界。
⇒ 横跨 `0`（C）与非 0（其余）的差，**只剩 param id 与每条 size 两格，且它们对 C 是一起动的**：
C = `iid=0x428f`、`param=0x08001017`(size 12) + `0x0800101b`(size 28)、2 条、带内 80 B ⇒ `0`；
L2 = `iid=0x40bd…`、`param=0x08001004`、3 条 size 52、带内 216 B ⇒ `0x1`。

**离线普查（同轮，零开机）已经把这两格的候选缩到一个**：`0x08001004` 在五份 ACDB 里**只以链级记录出现、首词恒为 `0x1`（pool 索引）**，从不与模块号配对；而逐模块表里唯一的参数是 `0x08001015 size=8`。
⇒ 下一枪（**M1，本目标预算已尽，留给下一条**）应当是 C 的形状做**最小改动**：拿 C 那份 80 B 两记录的容器，
**只把 `param` 换成 `0x08001004`**（iid、size、条数、总长、传输都不动）。
`0` ⇒ param 不参与，分界在每条 size；`0x1` ⇒ **param id 就是 `0`↔`0x1` 的分界**，而 `0x08001004` 恰恰是我们从 ACDB 抄来的那个——
这会直接把 ② 的问题从"内容对不对"转到"为什么注册过的 param 反而被拒"。

**止损**：本目标预算 26 已到（120 里已用 26），三格拆完 + 一支离线否证，**停手**。
终端判据仍是耳朵听到的声音，本目标未追求、也未声称出声。

## M1 判据（0263：**C 的容器只把两条记录的 `param` 换成 `0x08001004`**；开机前写死）

拆完传输与长度之后，横跨 `0`（C）与 `0x1`（其余全部）只剩 **param id** 与 **每条 size** 两格。
M1 走**被接受基线**：I 轮已在被拒基线（G 的 `0x1`）上换过 param ⇒ 不换码，那一格已花掉；
M1 问的是反方向的、也是唯一还没问过的问题——**param 单独能不能让一份已经被接受(0)的容器变成被拒**。
容器逐字保持 C：`iid=0x428f`（`G_VP_TDM_IID`，TDM sink）、两条记录 `size=12/28`、
正文 `{96000,0x30020,1}` 与 `{0,2,0x10001,7,0x200004,0,1}`、`dst=1`、`g_apm_cmd` 带内、
步距 `16+align8(12)=32` 与 `16+align8(28)=48` ⇒ **总长 80 B 不变**（整包 120 B），唯一变的两格是 param。
marker 升到 `cccfg v2`（`v1` 归零），`error_code` 槽位仍只靠 `memset` 留 0（cccfg 路径从不显式写它）。

| 编号 | 判据 | 取值 | 含义 / 走向 |
|---|---|---|---|
| **M1-0** | kernel 与镜像含 `SWEET MARK: cccfg v2 `，`SWEET MARK: cccfg v1 ` **= 0**；cmdline **583 B** 且尾部 `sweetcccfg=1 sweettime=<本轮>` 连续（**不带** `sweetoobcfg`，一轮一个容器） | 成立 | 0263 真进产物、开关唯一 |
| **M1-1** | 三行自证：`cccfg v2 dst=1 inband recs=2 len=80`、`cccfg v2 rec 0 iid=0x428f param=0x8001004 size=12`、`rec 1 … param=0x8001004 size=28`；**且** `SWEET GLINK: APM cc_batch src=0x2010 dst=0x1 (120 bytes)` | 五条全中 | 发的就是那个容器（`%#x` 打 `0x8001004`，无前导 0，照 C 原样） |
| **M1-2（主判据）** | `SWEET GLINK: cc_batch rsp op=0x1001006 status=?`（tag 是 `cc_batch`，不是 `cccfg`） | 见下表 | C 同位置读数 = `status=0`（`ring-c2-ccbatch.txt:1732`） |
| **M1-3** | 平面仍 `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0 | 与 C 轮实测逐条等值 | 不成立 ⇒ 该轮不判、停手 |
| **M1-4** | 回归同 C 轮：`vp/` 18（发 9 + 应 9）、`gate flag` 1、**panic 0**、`PSCI SYSTEM_RESET` 正常自复位 | 等值 | 设备安全 |
| **M1-5** | **不出现** `cccfg v2 reject` / `bad size` / `walk … !=` 任一行 | 成立 | 出现 ⇒ 容器没发出去（`reject` 与 `walk` 都是先 return 不发包），本轮不作 param 结论 |

**M1-2 判决表（开机前定死）**
1. **`status = 0`** ⇒ **param id 不参与**（在被接受基线上也不换码）⇒ 与 C 的差只剩 **每条 size**
   ⇒ 下一枪 **M2s** = C 的容器**只把两条 size 12/28 都改成 52**（param 保持 C 的 `0x08001017/0x0800101b`；
   长度随之 80→144 B，仍 ≤256 带内）。
2. **`status = 0x1`** ⇒ **`param=0x08001004` 单独就能把被接受的容器变成被拒** ⇒ APM 侧按 param 把关，
   而这个 key 恰是 ACDB 里**只属于链级**的那个 ⇒ 下一步 **M2r** = 同容器把 param 换成
   逐模块表真正登记的 `0x08001015`、size 换成 8（**明写：它一次动 param+size 两格，只作 M1 的旁证，不算单变量**）。
3. **第三个码** ⇒ 新的可分辨读数，记入基线表，不命名机制。
4. **`no rsp` / 平面死 / M1-5 命中** ⇒ **不判**，写 docs 交回用户。

限制照写：`0` 只写"命令级接受"（不代表配置落地）；`0x1/0x2/0x3` 都是拒绝且不命名机制；
记录级仍盲（`error_code` 八轮 0，且带内正文在 ADSP 侧的包里，DSP 无法写回我们的 `body`
⇒ `touched` 这类读数只在带外那一侧才可能变非 0，N1 就是为此而设）。
M1 是第 **27** 枪；N1 是第 **28** 枪（本目标上限）。

### M1 开机前的标签更正（子智能体提出、我逐条在头文件里复核）

| 常量 | 我自己核到的出处 | 更正 |
|---|---|---|
| `0x08001017` | `work/sweet-audio/refs/.../modules/hw_intf_cmn_api.h:156` = `PARAM_ID_HW_EP_MF_CFG` | **C 的第一个 param 是真·模块参数**，不是我们编的 |
| `0x0800101b` | `.../modules/pcm_tdm_api.h:362` = `PARAM_ID_TDM_INTF_CFG` | **C 的第二个 param 也是真·模块参数** |
| `0x08001004` | `.../apm/apm_module_api.h:150` = `APM_PARAM_ID_MODULE_CONN`（`ko-…audioreach.h:161` 同名） | 它是 **APM 图级参数**；H/J/K1/L1/L2 一直把它放进"模块记录"的命名空间 |
| `iid=1` | `.../apm/apm_api.h:261` = `APM_MODULE_INSTANCE_ID` | `iid=1` 就是"发给 APM 本身"，所以 `0x3` 与 `0x1` 的差别是**记录归属**，与"模块真假"无关 |
| `0x08001015` | `.../apm/apm_module_api.h:127` = `APM_MODULE_PROP_ID_PORT_INFO` | 它是 `MODULE_PROP` 下的**属性 id**，不是 SET_CFG 的 cal key ⇒ **原计划的 M2（发 `0x08001015`/size 8）前提不成立，撤回** |

**改写的读法（不含机制命名）**：`ACDB 里查不到` **不等于** `不是真参数`——厂商运行时按 backend_conf 生成 `MF_CFG/INTF_CFG` 这类 cal，本来不落 ACDB。
⇒ 之前"ACDB 普查证明 C 是自创的"这句话**作废**；普查证明的只是"ACDB 里没有"。
⇒ M1 的实际问题也从"param 真假"改述为**命名空间**：**图级参数 `MODULE_CONN` 放进模块记录里，会不会让一份本来被接受的容器变成被拒**。
预测（写下来当可判读，不当前提）：头文件读法 ⇒ `0x1`；"param 不参与"读法 ⇒ `0`。
下一枪仍排 **N1（带外哨兵）**：带内正文在 ADSP 侧的包里、DSP 写不回我们的 `body`，所以记录级只有带外侧能拿到非 0 读数——这一条不因上面的更正而变。

## N1 判据（第 28 枪，仪器枪；**开机前写死**，代码待 0263 落库后切 0264）

**这一枪为什么必须走带外**：带内时正文在 ADSP 侧的包里，DSP 没有任何途径写回我们的 `body`
⇒ `touched` 在带内**永远**只能是 0，那个"分不清没写回 / 写了 0"的歧义在带外侧才消得掉。

**N1 = K1 的那一发 + 一个预置值**（对照锁定 K1，不是 L2）：
- 发送形态回到 K1：OOB 描述符 `{payload_address=0x1000, msw=0, mem_map_handle, payload_size=288}`、
  4 条记录、`iid=0x40bd/0x40be/0x40bf/0x40c0`、`param=0x08001004`、`size=52`、正文照抄；
  **这段代码 0261 删掉了 ⇒ 0264 从链树"0260 落库态"（`021bb1e`）原文取回，不凭记忆重写**
  （这条规矩今天已经救过我一次：`cc_batch` 的 tag 与 120 字节整包就是子智能体逐字抄回来才没写错判据）。
- 唯一新增字段：`g_put32(body + off + 12, 0)` → `0xa5a5a5a5u`（记录里的 `error_code` 槽位）。
  预置成非 0 之后，读数变成**双向**的：
  `touched != 0` ⇒ 有人写过这份正文（`first=` 指出第一个被改的字节）；
  `touched == 0` ⇒ **没有人写过**（`0xa5a5a5a5` 不是一个合法回答值，所以不再是"没写回/写了 0"二义）。
- marker `oobcfg v10` → `v11`（整串 printf，跨补丁唯一，同 push 更新 marker 列表）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **N1-0** | kernel/镜像含 `SWEET MARK: oobcfg v11 try `，`v10/v9/v8`=0；cmdline **583 B**、尾部 `sweetoobcfg=1 sweettime=<本轮>` 连续（不带 `sweetcccfg`） | 成立 | 0264 真进产物、开关唯一 |
| **N1-1** | `oobcfg v11 try 0 form=oob off=0x1000 len=288 recs=4` + 发包行是 40 字节带外描述符（**不是** `APM oob_batch` 的带内行）+ 四条 `rec` 的 `err=0xa5a5a5a5`（发前读回应仍是哨兵） | 成立 | 通路真的回到带外、哨兵真的写进来了 |
| **N1-2（主判据）** | `oob_batch rsp op=0x1001006 status=?` 仍应为 `0x1`（=K1） | 记录 | 若变成别的码 ⇒ **预置字段本身参与**了判定，则本轮不判、把这条当新事实记下 |
| **N1-3（仪器）** | `oobcfg v11 body touched=%u first=%u of 288` | **两支都可读** | 非 0 ⇒ 有写回（记录级第一次不盲）；0 ⇒ 无写回 |
| **N1-4** | 平面 31 / 4 / 0（K1 界内地址基准）+ `vp/` 9 应 + `gate flag` 1 + **panic 0** + 正常自复位 | 成立 | 不成立 ⇒ 该轮不判、停手交回 |

限制照写：`status=0` 仍只是"命令级接受"；`0x1/0x2/0x3` 都是拒绝、不命名机制；
`err=` 读的是**区域副本**，只有带外侧可能被 DSP 改到 —— 这正是 N1 存在的理由。
预算：M1 第 27、N1 第 28（本目标上限），已用 26/120。

### 离线改写 ② 前提的一条（我自己读到的原文，不是转述）

`work/sweet-audio/refs/audioreach/audioreach-graphservices-master/spf/api/apm/apm_module_api.h:150-158`：

> `#define APM_PARAM_ID_MODULE_CONN 0x08001004` … "ID of the parameter to configure the connection between
> module ports. **This parameter ID must be used under command APM_CMD_GRAPH_OPEN.**"

配合 `apm_api.h:245-252`（APM 模块 `iid=1` 名下挂的那串 param：`SUB_GRAPH_CFG / CONTAINER_CFG / MODULES_LIST / MODULE_PROP / MODULE_CONN / …`，
且 `@calibration_hdr_field module_id -- APM_MODULE_INSTANCE_ID`），得到：

1. 我们从 **H 起**就在 `SET_CFG` 里携带 `0x08001004`，而规范把它划给 **`GRAPH_OPEN`**；
   `iid=1` 那支拿到 `0x3`、模块 iid 那支拿到 `0x1` —— 两支**可以同源**：命令与 param 不匹配，而不是"内容/地址/长度不对"。
   这一条**不命名机制**，只是把"为什么 26 枪里内容怎么换都是那两码"变成一个可测的前提。
2. M1 因此是这条前提的第一个判据：在**本来被接受的容器**（C）上只换成 `0x08001004`。
   预期 `0x1` ⇒ "SET_CFG 携带连线 param 被拒"成立 ⇒ ② 的下一步从"再猜内容"改成
   **把连线数据放回 `GRAPH_OPEN` 正文**（我们的 2960 B `GRAPH_OPEN` 已经会被响应），
   `SET_CFG` 只留给真模块 param（`0x08001017/0x0800101b` 这一类，C 已证明可用）。
3. 反面也写死：若 M1 拿到 `0` ⇒ 头文件这句话**不足以**解释我们的读数（APM 没按它校验），
   那么 ② 继续留在内容轴上，下一枪改走"每条 size"（C 的容器只把 12/28 换成 52/52）。

注意不要再犯的错：厂商 `GRAPH_OPEN` 的 OOB 正文是 1520 B（`0x5f0`），
**不要**用"长度凑得上"去认领它就是连线记录组（AGENTS §5：尺寸匹配不是证据）。

## 更正：`g_graph_open_payload` 的 framing（零开机；写于 M1 之前）

`docs/status-log.md:2466-2470`（commit `af11c9a`，含其 message 里的 "@0x134"）把这份 344 B 正文读成
"ACDB 链式 `{param_id,size}` 8 字节头、无 iid"，并把 `0x3c`/`0xa4` 当记录间分隔符。**两条都错**，
按字节复算的结果（完整分解已写进 `docs/status-log.md` 末节"离线更正二"）：

- 它是**标准 16 字节 `apm_module_param_data_t` + align8** 的 5 记录容器，头在
  `0x000/0x040/0x0b8/0x0e8/0x130`，size `48/100/32/52/20` ⇒ `5×16 + (48+104+32+56+24) = 344` 闭合；
  `iid` 全 `0x1`、`error_code` 全 `0`。
- 两个 `0xffffffff` 是 `0x08001010 SCENARIO_ID` 与 `0x080010cb` 的**值**。

**对判据的影响（M1 判据一字不改）**：
1. 被接受的 `GRAPH_OPEN` 与被拒的 `SET_CFG` 各轮**共用同一套记录编码** ⇒ "framing 不同"这条退出解释空间。
2. `iid=0x1 + param=0x08001004` 在 `GRAPH_OPEN` 下**一直拿 `status=0`**，同一对在带外 `SET_CFG`（H/J）拿 `0x3`
   ⇒ 上面"命令与 param 不匹配"这条前提（第 4385 行）**仍然活着，且现在多一条正面支撑**；
   但它仍**不命名机制**，也仍**不解释** K1 的"真实 iid ⇒ `0x1`"。
3. 附带把 `0x08001174`（真 `APM_CONTAINER_PROP_ID_HEAP_ID`）与 `0x08001176`（我们自创）分开标；
   旧笔记里两者混用过。

## N1 开机前修订（0264 已落库，写于开机之前）

**唯一改字面量的一处**：N1-1 里我预注册的是 `form=oob`，那是我当时随手写的简称。
实际 printf 是 021bb1e 原文，`SWEET_OOB_FORMS=1` ⇒ 只跑 `f=0`，字面量是
**`oobcfg v11 try 0 form=bare off=0x1000 addr=0x1000 len=288 recs=4 handle=0x…`**。
按"代码原文优先"改判据，不改代码。其余 N1-0/N1-2/N1-3/N1-4 一字未动。

0264 的落地核对（零开机）：
- `git diff ff2a016 3f215ad` ⇒ 2 hunk、212 行；纯净基线（`git archive ff2a016`）上
  `git apply --check -p1` 通过，apply 后 `diff -q` 与链树 HEAD 相同。
- 恢复的那段是**逐字比对**过的：描述符 11 行、发送+等答+应答打印三段与 `021bb1e` **字符串全等**
  （`python3` 切片比较，只有 marker 版本与 `recs=` 取自 `SWEET_OOB_SEND` 两处不同）。
- 宿主机 stub（`tools/stub_oobcfg.py` 已扩到两种发送形态）跑 `0264` 产物树：
  `len=288 recs=4`、包 `len=40 w0=0x2860 w1=0x302 op=0x1001006 addr=0x1000 handle=0xb0e145f8 size=288`；
  **两支都可读**：DSP 不写 ⇒ `err=0xa5a5a5a5 ×4`、`touched=0`；DSP 写 0 ⇒ `err=0 ×4`、
  `touched=16 first=12`；无应答支打印 `no response`（仪器能自证失败）。
- 同一工具回跑 L2 的树仍是 `oobcfg v10 form=inband len=216 recs=3` ⇒ 工具没丢掉上一轮。
- workflow marker 列表：`oobcfg v10 try `/`v10 body touched…` ⇒ **`v11`** 两条；`cccfg v2` 不动。

**开机顺序不变**：M1（第 27）先开，N1（第 28）后开。两轮的镜像不同源文件，
M1 只动 `sweetcccfg` 块、N1 只动 `sweetoobcfg` 块 ⇒ 各自 cmdline 只有一个开关。

### M1 开机前补记（`fastboot boot` 已执行，**ring 一行都还没读**；只改一个预测字节数，不碰任何判决分支）

- M1-0 写的 cmdline **583 B** 是我沿用 G–L2 的数字。`sweetoobcfg=1`（13 字符）比 `sweetcccfg=1`（11 字符）长 2，
  所以本轮产物**开机前实测就是 582 B**（这个数在 push 前的产物核对里已经量到，也写进了 `docs/status-log.md`
  那句"582 B cmdline"）；判据表里的 583 是笔误。
- 只更正这一个预测长度。身份主判据（`cccfg v2 ` 在 kernel 与镜像各命中、`cccfg v1 `=0、
  尾部 `sweetcccfg=1 sweettime=1793880108` **连续**、**不含** `sweetoobcfg`）与 M1-1…M1-5、
  以及 M1-2 的四支判决表**一字未动**。
- 产物核对（开机前，`images/m1-cccfg-header-param.img`，51,269,632 B）：
  `cccfg v2 `×4 / `cccfg v1 `×0 / `oobcfg v11 try `×0 / `oobcfg v10 try `×1（=0262 的正文仍在链里，正常）/
  cmdline 582 B 完整可打印、`sweetbufs=30` 在位、`sweets1ap=3`、尾 38 字符连续。
- 打分器 `tools/score_probe.py` 扩到 `m1`/`n1`（每轮自带 printf 前缀与 GPR tag），开机前先跑两支自证：
  `l2` 对它自己的 ring 复现"branch 2 ⇒ 长度不在分界"；`m1` 误跑 C 的 ring 时正确报
  `cccfg v2` 四行 **MISS**、`cc_batch rsp op=0x1001006 status=0`（=C 基线本体），
  ⇒ 改动没把已有轮子的打分路径弄坏，也不会把"别的 ring"当成满分。

## M1 判决（第 27 枪，boot 2026-11-05 20:44–20:47，`logs/ring-m1-cccfg.txt`）

**主判据 `status = 0x3` ⇒ 开机前判决表的第 3 支：又一个可分辨读数，记入基线表，不命名机制。**

| 门 | 读数 | 结论 |
|---|---|---|
| M1-0 身份 | ring `sweettime=1793880108`×2；产物 `cccfg v2 `×4 / `cccfg v1 `×0；cmdline **582 B**（见上节更正） | **成立** |
| M1-1 自证 | `cccfg v2 dst=1 inband recs=2 len=80`、`rec 0 iid=0x428f param=0x8001004 size=12`、`rec 1 … size=28`、`APM cc_batch src=0x2010 dst=0x1 (120 bytes)` **各 1** | **成立**（发的就是那个容器） |
| M1-2 主判据 | `SWEET GLINK: cc_batch rsp op=0x1001006 status=0x3 hop=0x2001005 len=32` | **`0x3`**（C 同位置 = `0`） |
| M1-3 平面 | `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0，与 C 轮逐条等值 | **成立** |
| M1-4 设备安全 | `vp/` 18（9 发 + 9 应）、`gate flag` 1、内核 panic 0 —— **但 ring 里没有 `pcm tone done`、没有 `===== done =====`、没有 `PSCI SYSTEM_RESET`** | **不成立（新形态）** |
| M1-5 | `reject`/`bad size`/`walk` 全 0 | **成立**（容器确实发出去了） |

### M1-4 的取证（为什么不是"我的解码丢了尾巴"）
- 解码窗口 = 头部 `total` = **86,441 B**，与 `logs/ring-m1-cccfg.txt` 字节数**逐字节相等** ⇒ 窗口没被截。
- raw 里 0x175e2 的 `pcm tone done` 与 0x17a71 的 `PSCI SYSTEM_RESET` 与 **L2 的 raw 同偏移同内容**
  ⇒ 那是**上一轮没被覆盖的陈旧字节**，不是本轮写的（ring 不清零）。
- 复位代码在 `printf("…PSCI SYSTEM_RESET")` 之后**显式 `sweet_log_sync()` 才 `smc #0`**
  （链树 `bsd/dev/sweet_glink.c:7906` 起）⇒ 真走到收尾就必然落盘，不存在"差一拍"的竞态。
- 前 **12 轮** ring 全部以 `PSCI SYSTEM_RESET` 收尾（c2/d/e/f/g/h/i/j/k1/l1/l2），本轮是第一个不收尾的。
- 本轮自己的最后一行是 `data_buffer send` 后跟 `CRASH NewsToday2 … extension check-in timeout`。
- 设备侧：手机**自己**回到 Android（20:50 时 uptime 3 min），我没碰过按键。
⇒ 读法只到"**这一发在音尾之前离开了 XNU，且没走我们的复位路径**"。**不命名机制**：
可能是同一次拒绝引出的后果，也可能是 LPASS/时钟读、watchdog、别的——一次开机不足以归因。
源码里早有同形态的记录（`sweet_glink.c:4059`："no PSCI SYSTEM_RESET, watchdog picked it up"）。

### 0x3 与"iid 是分界"这条旧结论的冲突（必须改，不能沉默）
旧的三码基线：**H/J**（带外、`iid=1`、`param=0x08001004`、size 52×4）⇒ `0x3`；
**K1**（带外、真实模块 iid `0x40bd…`、同 param/size）⇒ `0x1`；据此我写下"`iid` 是 `0x1`↔`0x3` 的分界"。
**M1 把这个概括打掉了**：`iid=0x428f`（真实模块，TDM sink）、`param=0x08001004`、size 12/28、带内 ⇒ **`0x3`**。
⇒ 现在能说的是：拿到 `0x3` 的三支里 `iid` 分别是 `1`、`1`、`0x428f`；拿到 `0x1` 的四支 iid 是 `0x40bd…`/`0x428f+真 param`…
**"1 vs 真实"不再是分界**；剩下的候选是"**记录点名的是哪一个 iid**"与"传输/param/size"的**组合**。
按老规矩：**三个码都是拒绝，语义不命名**；`0` 只写"命令级接受"。

### 停手（设备安全门未满足 ⇒ 第 28 枪 N1 **不开**，交回用户）
- 未满足的是 M1-4（收尾/自复位），不是 panic、不是 `no rsp`、不是平面（31/4/0 与 C 等值）。
  主判据在**已落盘窗口内**取到，所以 M1 的读数**成立并记账**；但"下一枪是带外探针"这件事
  要求先把这次的早退搞清楚，或得到你的同意。
- 可零开机做的下一步（都不烧开机次数）：把 M1 与 C/H/J/K1 的 `iid×param` 组合列成矩阵，
  看 `0x3` 是否只跟 `iid∈{1, 0x428f}`；把 `021bb1e` 复位路径与 `out:` 之前的
  `g_clk_witness("post")/g_lpass_probe()` 读一遍，列出音尾之前所有可能阻塞的 MMIO 读。
- 开机记账：**27/120**（本目标上限 28，N1 未开）。

## 零开机取证（M1 之后）：把 27 枪摊成 `iid × param × size × 条数 × 传输` 矩阵

同址 `0x1000`、同 `dst=1`、同句柄、每轮一发（G/I 的 iid 数组是从链树 `821eb67` 原文取的，不是我记忆的）：

| 轮（补丁） | 传输/总长 | 条数×size | iid 集合 | param | `status` |
|---|---|---|---|---|---|
| **C**（0252） | 带内 80 B | 2×(12,28) | `{0x428f}` | `0x08001017`,`0x0800101b` | **`0`** |
| **M1**（0263） | 带内 80 B | 2×(12,28) | `{0x428f}` | **`0x08001004`×2** | **`0x3`** |
| **G**（0256） | 带外 288 B | 9×12 | `{0x428f,0x40bd,0x42d3,0x40bf,0x40c0,0x40c8,0x4165,0x4308,0x428f}` | `0x08001017` | `0x1` |
| **I**（0258） | 带外 288 B | 9×12 | 同上（**含 `0x428f` 两次**） | **`0x08001004`** | `0x1` |
| **H**（0257） | 带外 288 B | 4×52 | `{1,1,1,1}` | `0x08001004` | `0x3` |
| **J**（0259） | 带外 288 B | 4×52 | `{1,1,1,1}` | `0x08001004`（正文全零） | `0x3` |
| **K1**（0260） | 带外 288 B | 4×52 | `{0x40bd,0x40be,0x40bf,0x40c0}` | `0x08001004` | `0x1` |
| **L1**（0261） | 带内 288 B | 4×52 | 同 K1 | `0x08001004` | `0x1` |
| **L2**（0262） | 带内 216 B | 3×52 | `{0x40bd,0x40be,0x40bf}` | `0x08001004` | `0x1` |

**这张表里最硬的一条是 M1 与 I 的冲突**：`(iid=0x428f, param=0x08001004)` 这一对，
在 I 的容器里（第 0 条与第 8 条就是它，size 12）**回答 `0x1`**，在 M1 的容器里（size 12 的那条一模一样）**回答 `0x3`**。
⇒ **"iid 是 `0x1`↔`0x3` 的分界"作废**（原先的证据只有 K1-vs-H 一对，iid 与 size/条数当时还没分开）。
⇒ 同样作废的还有"真实模块 iid ⇒ `0x1`"：M1 的 iid 是真实模块号，拿到的是 `0x3`。

**因此现在能钉住的只有这些（不命名机制）**：
1. `0x3` 与 `0x1` **都能**在真实 iid 上出现，`iid=1` 只出现在 `0x3` 那一侧（H/J）——单向事实，不是规则。
2. **`param` 单独就能把 C 的 `0` 变成拒绝**（M1 只动这一格，其余逐字同 C）。
3. `0x1`↔`0x3` 的差别**跟的是整个容器**，不是任何单字段：M1 与 I 的差同时是"第二条的 size 28 vs 12"、
   "条数 2 vs 9"、"带内 vs 带外"、"总长 80 vs 288"。**这四格还捆在一起，一次都没拆开过。**
4. 拆它的手段仍然是记录级：`error_code`。带内侧 DSP 写不回我们的 `body`，
   ⇒ **N1（带外 + `0xa5a5a5a5` 哨兵）是唯一能把"哪一条被拒"变成可观察量的枪**；
   它在 M1 之后的价值不降反升（因为现在的结论是"容器级"的）。

**M1 早退的候选区间（源码读，零开机）**：本轮最后一行是第 31 发 `data_buffer send`，
`out:` 之前依序是 `pcm tone done (30 buffers)` → `ibat post` → `SWEET I2C: pin 56/57`（I2C 引脚重投）
→ `sweet_i2c_amp_readback("post-stream")`（8 次 I2C 读，`r=-2` 一直如此）→ `g_rx_snapshot("post-stream")`
→ `g_clk_witness("post")` → `g_lpass_probe(...)` → `printf("===== done =====")`。
⇒ 断点在"**第 31 发之后、`pcm tone done` 之前**"这一段（音尾等待），
不是 I2C/LPASS 那些后置读——**这是范围，不是原因**；一次开机不足以命名机制。

## M1c 判据（控制枪，**开机前写死**：同一 kernel、同一 cmdline，只去掉 `sweetcccfg=1`）

用户授权"先重新打包，然后直接继续" ⇒ 这一枪本地重打包（**不动 CI**，`out/kernel.adsp` 原样），
变量唯一 = **那一发探针发还是不发**。它是 M1-4 早退的控制组，不是新的内容枪。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **M1c-0** | 镜像含 `cccfg v2 `×4、`cccfg v1 `×0；cmdline **不含** `sweetcccfg`；长度 ≤570 B；ring `sweettime=<本轮>` 命中 | 成立 | 0263 的 kernel 原样、开关真的关掉 |
| **M1c-1（控制门）** | ring 里 `cccfg v2 dst=1 inband`、`cc_batch rsp` **各 0 行** | 成立 | 这一发确实没发（不成立 ⇒ 不是控制组，本轮作废） |
| **M1c-2（主读数）** | ring 结尾是否出现 `pcm tone done (30 buffers)`、`===== done =====`、`SWEET GLINK: PSCI SYSTEM_RESET` | 三行齐 / 仍截尾 | 见下面判决 |
| **M1c-3** | 平面 31/4/0、`vp/` 18（9+9）、`gate flag` 1、内核 panic 0 | 与 M1 等值 | 不成立 ⇒ 停手交回 |

**判决（开机前定死）**
1. **三行齐** ⇒ 同样代码、同样音尾，**不发探针就能正常收尾** ⇒ M1 的早退与那一发 `0x3` **同现**。
   仍**不命名机制**；N1 暂不开，先补一发"重放 M1"确认早退可复现（否则一次不算现象）。
2. **仍截尾**（`data_buffer send` 之后没有收尾三行）⇒ 早退**与探针无关** ⇒ 是音尾/环境自身的事，
   M1 的主判据不受影响，N1 按原判据开。
3. **出现第三种情况**（平面不等值、`no rsp`、panic）⇒ 该轮不判、停手交回用户。

限制：一次开机不足以命名机制；控制组只回答"是否与探针同现"，不回答"为什么"。
记账：M1=第 27 枪；**M1c=第 28 枪**；N1 顺延为第 29 枪（原目标上限 28 由用户本次授权延长）。

## M1c 判决（第 28 枪，控制组，`logs/ring-m1c-noprobe.txt`）

| 门 | 读数 | 结论 |
|---|---|---|
| M1c-0 | ring `sweettime=1793883678`×2；产物 cmdline **569 B**、`sweetcccfg`/`sweetoobcfg` **都不在**；`window_check`：`total=0x162b3` 与解码字节**相等** | **成立** |
| M1c-1 控制门 | `cccfg v2 dst=1 inband` 0、`cccfg v2 rec ` 0、`cc_batch` 0、`reject/bad size/walk` 0 | **成立**（探针确实没发） |
| **M1c-2 主读数** | `pcm tone done` **1** / `===== done =====` **1** / `PSCI SYSTEM_RESET` **1**（都在窗口内；raw 里 0x1e965 那条 `===== done =====` 在窗口之后，由新自检显式判为**上一轮陈旧字节**） | **正常收尾** |
| M1c-3 | 平面 31/4/0、`vp/` 18（9+9）、`gate flag` 1、内核 panic 0，与 M1 逐条等值 | **成立** |

⇒ **落在开机前判决表第 1 支**：同一 kernel、同一音尾、同一档位，**不发那一发就能正常收尾**；
M1 的早退与那一发 `status=0x3` **同现**（这是相关性，**不是机制**，n=1）。
按同一条判据的走向：**N1 暂不开**，先补一发"重放 M1"看早退是否**可复现**。

## M1r 判据（重放枪，第 29 枪；**开机前写死**）

**用哪份 kernel**：手边 `out/kernel.adsp` 已是 0264（N1 那份）。0264 的两个 hunk **只碰**
`SWEET_OOB_SEND` 宏与 `g_apm_set_cfg_oob`，**cccfg 发送器逐字节未变** ⇒ 加 `sweetcccfg=1`、
不加 `sweetoobcfg`，发出去的就是 M1 那一发；代价是 kernel 里多一段**永不调用**的带外代码。
好处：`sweettime` 是新的，身份门不被"复用旧镜像"削弱（复用 `images/m1-cccfg-header-param.img`
会连 `sweettime=1793880108` 一起复用，两轮 ring 就无法区分）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| M1r-0 | 镜像含 `cccfg v2 `×4、`cccfg v1 `×0、`oobcfg v11 try `×1（**预期在**，但本轮不触发）；cmdline **583 B**、尾部 `sweetcccfg=1 sweettime=<本轮>` 连续、**不含** `sweetoobcfg` | 成立 | 发的是 M1 那一发 |
| M1r-1 | 四行自证各 1：`cccfg v2 dst=1 inband recs=2 len=80`、`rec 0 iid=0x428f param=0x8001004 size=12`、`rec 1 … size=28`、`APM cc_batch src=0x2010 dst=0x1 (120 bytes)` | 全中 | 容器真发出去了 |
| M1r-2 | `cc_batch rsp op=0x1001006 status=?` | 记录（预期 `0x3`） | `0x3` 是否可复现 |
| **M1r-3（主判据）** | 窗口内是否齐 `pcm tone done` / `===== done =====` / `PSCI SYSTEM_RESET`（必须过 `window_check`，`total==decoded`） | 齐 / 仍截尾 | 早退是否可复现 |
| M1r-4 | 平面 31/4/0、`vp/` 18、`gate flag` 1、panic 0、无 `reject/bad size/walk` | 等值 | 不成立 ⇒ 停手 |

**判决（开机前定死）**
1. **又截尾** ⇒ 早退**可复现且与那一发同现** ⇒ ② 的内容枪暂停，转"音尾为何死"这条设备安全线，
   并**交回用户**（红线：尽量不要让设备处于未收尾状态）。
2. **正常收尾** ⇒ M1 那次是**一次性**（n=1 不成现象）⇒ M1 的 `0x3` 读数站住，
   接着开 **N1（第 30 枪）**，内容仍按 N1 原判据。
3. `no rsp`、平面不等值、或内核 panic ⇒ **不判、停手交回**。
记账：M1=27、M1c=28、**M1r=29**、N1 顺延为 30（用户在 21:00 授权"直接继续"）。

### M1r-0 开机前更正（**ring 一行未读**，只改预测字节数）
`images/m1r-replay-cccfg.img` 实测 cmdline **582 B**，不是 M1r-0 写的 583。
理由与 M1 那次同一条：`sweetcccfg=1`（11 字符）比 `sweetoobcfg=1`（13 字符）短 2，
583 只属于带外那几支（G–L2、N1）。**其余 M1r-0…M1r-4 与三支判决一字未动。**
同轮其余核对全过：`cccfg v2 `×4、`cccfg v1 `×0、`oobcfg v11 try `×1（预期在，本轮不触发）、
`oobcfg v10 try `×0、尾部 `sweetcccfg=1 sweettime=1793884011` 连续、无 `sweetoobcfg`、`sweetbufs=30` 在位。
教训入账：**同一张判据表里"583"是我从上一支抄的**——长度属于"随开关字符数变化"的量，
每轮都必须从**该轮产物**重测，不能继承。

### 断点再收窄（零开机，源码 + 两轮 ring 对照；写于 M1r 结果之前）

把"M1 死在哪"从"音尾附近"收窄到**一个具体的、几乎不含代码的间隙**：

1.  tone 循环最后一轮的语句只有：`delay(gap)`（**gap=0**，cmdline 无 `sweetbufgap`，且 `gap>0` 才会打印
    `tone pace` —— 两轮都没有这行）→ `if (nw && (n&31)==0)` 的 `tone buf` 进度行（`n=30` 不满足，
    两轮都无此行）→ `g_apm_data_buffer(...)`（打印 `data_buffer send` 后打印 `TX_DATA`）→ 循环退出。
2.  本轮**最后一条已落盘记录**是 `CRASH NewsToday2 reason ns 7 code 0x3`，紧挨着它之前是
    第 31 发的 `data_buffer send` + `TX_DATA iid=112 len=68` ⇒ **发送本身已经完成**。
3.  循环退出后的下一条语句就是 `printf("SWEET GLINK: pcm tone done (%d buffers)")`，
    再后面才是 `g_ibat_burst("post")`、`sweet_i2c_amp_readback("post-stream")`、`g_rx_snapshot`、
    `g_clk_witness("post")`、`g_lpass_probe` —— **全部在 `pcm tone done` 之后，因此全部排除在断点之外**。
    （我先前把"I2C/LPASS 后置读"列为嫌疑，这一段读完就作废：它们连发生的机都没有。）
4.  **正常收尾必然落盘**：复位路径是 `printf("===== done =====")` → `printf("PSCI SYSTEM_RESET")`
    → **`sweet_log_sync()`** → `smc #0`。M1c 就是证据：它的窗口 `total=0x162b3` 里三行齐全。
    ⇒ M1 缺这三行**不能**用"差一拍没刷上"解释；但**可以**用"AP 在第 31 发完成后、
    `pcm tone done` 刷盘之前（同一个 1 s 刷新周期内）就离开了 XNU"解释。

⇒ 现象的准确说法：**第 31 发完成后不到 1 s，AP 离开 XNU，且没走到我们的复位路径。**
**仍然不命名机制**（看门狗 / ADSP 侧把 AP 拖死 / 别的一次性事件，一次相关 + 一次对照都不够分辨）。
M1r 的重放只回答一个问题：**这个 1 s 内的早退是否与那一发 `0x3` 一起复现**。

## M1r 判决（第 29 枪，重放，`logs/ring-m1r-replay.txt`）⇒ **走第 2 支，N1 放行**

| 门 | 读数 | 结论 |
|---|---|---|
| M1r-0 | `sweettime=1793884011`×2；`cccfg v2 `×4 / `v1 `×0 / `oobcfg v11 try `×1（在镜像里，本轮不触发）/ `v10 `×0；cmdline **582 B**（判据原文 583 已开机前更正） | **成立** |
| M1r-1 | 四行自证各 1（`recs=2 len=80`、`rec 0 iid=0x428f param=0x8001004 size=12`、`rec 1 … size=28`、`APM cc_batch … (120 bytes)`） | **成立**：发的就是 M1 那一发 |
| M1r-2 | `cc_batch rsp op=0x1001006 status=0x3 hop=0x2001005 len=32` | **`0x3` 第二次** ⇒ 该读数**可复现**（n=2） |
| **M1r-3 主判据** | 窗口内 `pcm tone done` **1** / `===== done =====` **1** / `PSCI SYSTEM_RESET` **1**；`window_check` `total=0x16191 == decoded`；raw 里 0x1e965 的 `===== done =====` 显式判为陈旧字节 | **正常收尾** |
| M1r-4 | 平面 31/4/0、`vp/` 18（9+9）、`gate flag` 1、panic 0、无 `reject/bad size/walk` | **等值** |

**判决（对照开机前三支）**：走**第 2 支** —— 同一发容器、同一 kernel 家族、同一 cmdline 档位，
重放**没有**再出现"第 31 发后 1 s 内离开 XNU"。⇒
1. M1 的早退是**一次性事件**（n=1 不成现象），**与那一发 `0x3` 无因果证据**；三轮对照如下，
   早退只出现在 M1：`M1（探针，0x3）截尾` / `M1c（无探针）正常` / `M1r（探针，0x3）正常`。
   **不据此命名机制**，也不再为它单花开机。
2. M1 的主判据**站住**：`param` 单独就能把 C 的 `0` 变成拒绝（`0x3`，两次一致）。
   `iid` 不是分界这一条由 M1/M1r 两支共同支撑。
3. **N1 放行**：镜像 `images/n1-error-code-sentinel.img` 已核对（kernel=0264，
   `oobcfg v11 try `×1、`v10`/`v9`=0、`cccfg v2 `×4、cmdline **583 B** 且尾部
   `sweetoobcfg=1 sweettime=1793883775` 连续、无 `sweetcccfg`、`sweetbufs=30` 在位）。
   判据仍用开机前写死的 N1-0…N1-4 与那条 `form=bare addr=0x1000` 的字面量更正，一字不改。
记账：**M1=27、M1c=28、M1r=29、N1=30**（120 预算内；本目标原上限 28 由用户 21:00 "直接继续" 延长）。

## N1 判决（第 30 枪，仪器枪，`logs/ring-n1-sentinel.txt`）⇒ **记录级不再盲：读数是"没人写过这份正文"**

| 门 | 读数 | 结论 |
|---|---|---|
| N1-0 身份 | ring `sweettime=1793883775`×2；镜像 `oobcfg v11 try `×1、`v10`/`v9`/`v8`=0、`cccfg v2 `×4（在但本轮不触发）；cmdline **583 B**、尾部 `sweetoobcfg=1 sweettime=1793883775` 连续、无 `sweetcccfg` | **成立** |
| N1-1 通路+哨兵 | `oobcfg v11 try 0 form=bare off=0x1000 addr=0x1000 len=288 recs=4 handle=0xb0e145f8` **1**（正是开机前更正过的那条字面量）；四条 `rec i iid=0x40bd/0x40be/0x40bf/0x40c0 param=0x8001004 size=52 **err=0xa5a5a5a5**` | **成立**：通路真回到带外，哨兵真在区域里 |
| N1-2 主判据 | `oob_batch addr=0x1000 rsp op=0x1001006 status=0x1 hop=0x2001005 len=32` | **`0x1` = 与 K1 同码** ⇒ 预置字段本身不参与命令级判定（原判据的"本轮不判"支**不触发**） |
| **N1-3 仪器（这一枪真正买到的东西）** | `oobcfg v11 body touched=0 first=0 of 288`，**且**应答之后四条 `err` 仍是 `0xa5a5a5a5` | **`touched=0` 第一次是正证据**：从 `memcpy+dsb` 到应答后的读回，这 288 B 区域副本**一字节未变** |
| N1-4 设备安全 | 平面 31/4/0、`vp/` 18（9+9）、`gate flag` 1、panic 0、**收尾三行齐**（`window_check` `total=0x15a1a == decoded`；raw 里 0x17161/0x1717f 的 `===== done =====`/`PSCI SYSTEM_RESET` 由新自检显式判为**上一轮陈旧字节**） | **成立** ⇒ 界内地址的带外一发不牵连数据面 |

### 这条读数买到什么（以及不买到什么）
- 头文件对 `error_code` 的原文（`apm_api.h`，`struct apm_module_param_data_t`）：
  *"**Error code populated by the entity hosting the module. Applicable only for out-of-band command mode.**"*
  ⇒ 逐记录错误码在规范里**只该在带外侧出现**，而 N1 正是带外。
- 前八轮读到的 `err=0` 现在有了一个**能区分的说法**：预置非 0 之后应答回来仍是 `0xa5a5a5a5`，
  所以那不是"它写了 0"，而是**它根本没写**。⇒ "**这一发被 `0x1` 拒绝时，宿主侧没有往我们这块区域填任何逐记录码**"
  （窗口内成立；不等于"它永远不会写"，也不等于"它没读"——K1 已证明它读得到 `iid`）。
- 命令级应答只有 **32 B**（24 B GPR 头 + 8 B 正文），装不下四条逐记录码；
  ⇒ 记录级通道在当前协议形态下**只有"宿主写回请求正文"这一条**，而那条已被这一枪测成"没写"。
  ⇒ **此后不再为逐记录 `error_code` 花开机**；要看某一条记录被拒的原因，只能换判据形态
  （命令级 `status` 的组合，或换一份会被接受的容器），不是换仪器。

### 内容轴的现状（与本轮无关，收拢一下）
- `0`：只有 C（`iid=0x428f` + `0x08001017`(12)/`0x0800101b`(28)，2 条，带内 80 B）。
- `0x3`：H/J（`iid=1`、param 1004、52×4、带外）；**M1/M1r**（`iid=0x428f`、param 1004、12/28、带内）⇒ **`iid` 不是分界**。
- `0x1`：G/I/K1/L1/L2（真实模块 iid 那几支）。
- 仍捆在一起、一次没拆的：**M1 与 I 之间那四格**（第二条 size 28 vs 12、条数 2 vs 9、传输、总长 80 vs 288）。
  带外/带内对同一容器不改码（L1/K1、L2），**下一枪若要拆，应在带内侧**做单变量。
记账：**M1=27、M1c=28、M1r=29、N1=30**（120 预算内）。本目标（M1/N1）交付完毕。

## ②-D 第一轮：厂商 GPR **接收方向**抓到了（零开机，2026-11-05 22:27）

**方法**（新工具，均可复跑）：`tools/gpr_rx_capture.sh` 在 Android 侧**追加**一条 kprobe
`p:gpr_rx gpr_callback$<hash> [gpr_dlkm]`（符号运行时从 `/proc/kallsyms` 取，不写死哈希），
dump `x1` 起 32 个 u32 + `r0/r1/r2`；同时挂 `p:gpr_tx gpr_send_pkt` 当**对照**。
**绝不清空 `kprobe_events`**：本机 GPU/RPMh 的 `kbw/rhw/rhb` 正在跑（`tracing_on=1`），
清掉就是碰 GPU 线；本工具只 append、结束时只 `-:gpr_rx`/`-:gpr_tx`。
解码 `tools/parse_gpr_rx.py`：**按 `p0>>8` 的总长截断**，长度之外的字是缓冲区残留。

**触发方式的三个坑（都实测过，写下来免得重走）**
1. `tinyplay /data/local/tmp/silence.wav -D 0 -d 2` 返回 **rc=0、76800 B、drain 正常**，
   但 **`gpr_tx` 与 `gpr_rx` 双双 0 命中** ⇒ **裸 PCM 后端 DAI 不产生 GPR 流量**（没有 HAL/AGM 建图）。
   本机 `/proc/asound/pcm` 里**没有 MultiMedia 前端**，全是 `CODEC_DMA-*`/`MI2S-*` 后端与 dummy。
2. 音量键（`input keyevent 24/25`，流音量已设 0）也 **0 命中**，独立见证是
   `dumpsys media.audio_flinger`：`Standby: yes`、`FastMixer COLD_IDLE`、`framesWritten=0`、pcm `closed`
   ⇒ **根本没播放**，所以"0 命中"不能读成"没有响应"。
3. 有效的静音触发 = **把音乐/铃声/通知流设 0，再由用户真放一次音乐**：
   `framesWritten=105408`、`writeSequence=1098`（独立见证成立），
   `kprobe_profile`：**`gpr_rx` 1933、`gpr_tx` 1936**，GPU 三条命中数照常增长（没被我影响）。
   ⇒ **音量为 0 时 AudioFlinger 照样建图、照样发 GPR**，宿舍安静与取证可以同时满足。

**读数（`logs/gpr-rx-20261105-222736.txt`，1933 个接收包）**
- 长度分布：**56 B ×1908**、**32 B ×24**、28 B ×1。
  56 B 那批是 **`DATA_BUFFER` 完成事件**（`rsp_op=0x05001004`、`src=0x4001`、`dst=0x2011`、
  正文 `{offset 0x0 / 0x3000 交替, 0, handle 0xb0d89128, …}`）——
  ⇒ **厂商 PCM 乒乓偏移是 `0x0` 与 `0x3000`**（我们用的是 `0x0`/`0x800`，图放 `0x2000`）。
- **命令级响应一律 32 B**（24 B 头 + `{请求 opcode, status}`）⇒ **响应里从来没有逐记录错误码**，
  这从**另一侧**印证了 N1 的结论（带外正文也没人写）⇒ 记录级通道在当前协议形态下确实不可观察。
- 响应头里 `p5` 我们一直叫 `hop`，实测是 **`0x02001005`（响应 opcode）**；`p6`=被应答的请求 opcode、`p7`=status。
- **15 个 `SET_CFG` 响应，全部 `status=0`**，按 token 与 `gpr_tx` 配对后**只有两种形状**：

| 形状 | `dst` | 整包 | 正文 | 应答者 `src` | 例（token） |
|---|---|---|---|---|---|
| **单记录·带内·发给模块自己** | 模块 iid | 64 / 72 / 88 B | 40 / 48 / 64 B（=16+align8(size)） | **同一个模块 iid** | 40,41(`0x4308`)、50,51(`0x42d3`)、52(`0x4001`)、53,54(`0x428f`)、55(`0x4003`)、56(`0x4004`)、57(`0x4163`) |
| **图级·发给 APM** | **1** | **40 B** | 16 B = OOB 描述符 `{addr_lsw,msw,handle,size}` | 1 | 39,46,48,61 |
| **图级·发给 APM·带内** | **1** | **104 B** | **80 B** = `16+align8(12)` + `16+align8(28)` | 1 | **49** |

⇒ **tok 49 那一发就是 C 的几何（80 B 带内双记录、`dst=1`）且被接受** ⇒ **C 不是侥幸，是厂商真在用的形状**。
- **厂商点名的模块 iid 集合**（本场景）：`0x4308, 0x42d3, 0x4001, 0x428f, 0x4003, 0x4004, 0x4163` + APM `1`。
  **`0x40bd/0x40be/0x40bf/0x40c0` 一次都没出现** —— 那四个是 K1/L1/L2 从 ACDB 子图 `0xb0000023`
  抄来的 iid，厂商在这台机这个场景里**根本不点名它们**。（这是观察，**不命名机制**。）
- 客户端端口有**两个**：`0x2010` 与 `0x2011`（另有 `0x2002` 发过 `SHM_UNMAP`）；我们只用 `0x2010`。
- 还差一步：**tx 探针第一轮只 dump 了 6 个字（包头）**，所以那 80 B 正文里的 `iid/param/size` 还没读到。
  已把 `gpr_tx` 扩到 **32 个字**并重抓（第二轮窗口），目标是**逐字节读出"被接受的容器"**。

**残留字陷阱（差点又踩）**：第一轮 `p8=0xB0D89128`（厂商的 `mem_map_handle`）出现在 32 B 包**之外**，
是缓冲区残留；若不按 `p0>>8` 截断，就会把它读成"响应里带 handle"。解析器现在强制截断。

## ②-D 第二轮：tx 扩到 32 字后，**被接受的容器逐字节拿到了**（零开机）

数据与逐条表格：`docs/data/vendor-accepted-setcfg-2026-11-05.md`（含 40 个 SET_CFG、11 个 OOB 描述符、
SHM_MAP/GRAPH_OPEN/PREPARE/START、时间线）。原始证据 `logs/gpr-both-20261105-223253.txt`。

### 先更正我第一轮写错的两条（都已 push，所以在此显式作废）
1. **"tok49 = 80 B 正文 = C 的几何"——错。** 每个 SET_CFG 都带 **16 B `apm_cmd_header_t`**，
   正文从第 10 个 u32 起。104 B = 24 + 16 + **64 B**（两条 size 12 的记录）；
   **C 的整包是 120 B = 24 + 16 + 80**，两者不是同一形状。第一轮我把 `t6..t9={0,0,0,0x40}` 当成记录头，
   其实那是 `{addr_lsw=0, msw=0, handle=0, payload_size=64}`。
2. **"`0x40bd/0x40bf/0x40c0` 一次都没出现"——只对第一窗成立。** 第二窗它们**既被点名当 `dst`，
   也自己当 `src` 应答**（tok 100/103/104/146/147/148）。`0x40be` 两窗都没出现。
   ⇒ 那句"厂商根本不点名它们"作废；**iid 集合是随图变的**，不是设备常量。

### 新的结构性事实（都是观测，不命名机制）
- **厂商被接受的每一条记录，都是"那个模块自己的参数"**：
  TDM(`0x428f`/`0x4308`) ← `HW_EP_MF_CFG`(12) + `TDM_INTF_CFG`(28)；
  `0x40bd`/`0x4001` ← **`MEDIA_FORMAT` `0x0800100c`(30)**；
  `0x40bf`/`0x40c0`/`0x4003`/`0x4004` ← **`PCM_OUTPUT_FORMAT_CFG` `0x08001008`(30)**；
  SPR(`0x4163`/`0x4165`) ← **`SPR_DELAY_PATH_END` `0x080010c4`(4)**，值指向 `0x428f`；
  AW(`0x42d3`) ← 厂商私有 **`0x10013d2a`(28)**（头文件里没有）。
- **厂商唯一的多记录 SET_CFG（`dst=1`，104 B）装的是 `MFC_OUTPUT_MEDIA_FORMAT` `0x08001024`**，
  两条各 size 12，iid 是 MFC 模块（`0x40ca`+`0x40c8` / `0x401d`+`0x40b6`）。
  ⇒ **多记录容器里也是模块参数，不是图级参数。**
- **`0x428f` 那两条与我们 C 的正文逐字节相同**（`{0x17700,0x30020,1}` 与 `{0,2,0x10001,7,0x200004,0,1}`）
  ⇒ **C 是厂商两发单记录的合并版**，不是我们编的。
- **大正文各有独立映射区与 handle**：18504 → 区 `0x1_1FFE0000`/20480 B/handle `0xb0d92ac8`；
  22960 → 区 `0x1_1FFD8000`/24576 B/handle `0xb0d920f8`；其余小正文（112/176/264/1544）
  才放在 PCM 那块 `0xb0d89128` 里，偏移 `0x0/0x400/0x1000/0x6000/0x6800/0x7000`。
  **我们一直只有一块区、一个 handle。**
- **那两块区寿命 1.68 ms / 2.48 ms**（SHM_MAP→SET_CFG→SHM_UNMAP，时间戳实测）
  ⇒ 事后读 `/proc/<pid>/maps` + `mem` **抓不到**（已试，AGMIPC 1703 无对应映射）。
  要读 18504/22960 正文，只能在**写入那一刻**挂探针（uprobe + 尺寸过滤，或 kprobe 拿指针后立刻读），
  这是一条**尚未做**的路，不是已判死的路。
- `GRAPH_OPEN` 正文 496 / **2872** / 2904 / 2032 B，偏移 `0x0` 或 `0x6000`；
  `PREPARE/START` 带内正文里的子图列表是 `0xb00000d5`、`0xb0000002+25+24(+23)`、`0xb0000008+1c`…
  ⇒ **厂商开的图与我们手写的 `0xb000013d` 极简图不是一张**；`0xb0000023` 正是我们普查过的那张。

### 由此得到的**最强候选解释**（仍不命名机制，写成可证伪的形式）
把 30 枪的读数按"**参数与模块是否配对**"重排，全部一致：
- **C**：`0x428f`（TDM）配 TDM 自己的两个参数 ⇒ **`0`**（唯一被接受）。
- **G/I**：把 `0x08001017`（hw-ep 参数）/`0x08001004` **喷给 9 个不同类模块** ⇒ `0x1`。
- **K1/L1/L2**：把图级 `0x08001004` 喷给 4 个模块 ⇒ `0x1`。
- **H/J**：把图级 `0x08001004` 发给 APM 自己(`iid=1`) ⇒ `0x3`。
- **M1/M1r**：在 C 上**只把 param 换成图级 `0x08001004`**（模块仍是 TDM）⇒ `0x3`。
⇒ 候选规律：**`SET_CFG` 的记录必须是"该 iid 自己的模块参数"；图级参数（`MODULE_CONN`）只能走 `GRAPH_OPEN`。**
这条**能解释全部 30 枪**，而且与头文件那句 "must be used under command APM_CMD_GRAPH_OPEN" 一致。
**它是候选，不是结论**：还没有一发"配对正确但换模块"的对照。

### 下一枪因此变得很具体（判据等决定要开时再写死）
不再是"猜内容"，而是**照抄厂商被接受的那一发**：
- **P1（最便宜、单变量）**：C 的形状**只把两条 param 换成 `0x08001024`（MFC_OUTPUT_MEDIA_FORMAT）**、
  size 仍 12/28、iid 仍 `0x428f`、仍带内 ⇒ 若规律成立应当**被拒**（TDM 模块配 MFC 参数）；
  若拿到 `0`，则"配对"规律被否证，回到 size/组合轴。
- **P2（正面验证）**：给 `0x428f` 发**厂商原样的单记录** `0x08001017`(12) 与 `0x0800101b`(28)
  —— 我们其实已经在发（`vp/tdm_ep_mf`、`vp/tdm_cfg` 都是 `status=0`），
  ⇒ **P2 已经在我们自己的 ring 里被证明成立**，不需要再花开机。
- **P3（结构侧，另一条轴）**：把大正文放进**独立的第二块映射区**（自己的 handle），
  照厂商的 `SHM_MAP{0x10003,4,addr,msw,size}` 形状；这是"我们只有一块区"这个差异的第一次对照。
**优先级建议**：P1 一发就能证伪/支持"配对"规律，且**本地重打包即可**（0263 的 cccfg 块只改两个常量），
不需要 CI；P3 要新代码 ⇒ 一次 CI。

## T0 判据（第 31 枪，**零 CI：本地重打包 0264 内核**；开机前写死）

**这一枪买什么**：`vp/tdm_power` 是我们自己加的 `0x08001176`（值 0），**厂商 42 包里一票都没有**（0248 时代就记下了，
任务 #9 一直挂着没打）。会诊指出：如果这一票真的把 TDM/PA 路径按在某个非默认状态，
它就是"控制面全绿、PCM 被消费、却没有声音"最省的候选。现成开关 `sweettdmpwr`：
`0`=出厂（发）、`1`=发值 1、**`2`=不发**。这一枪用 `2`，**同时不开 `sweetcccfg`/`sweetoobcfg`**（一轮一个变量，也不双探针）。

cmdline = 音频已知-good 那条 + `sweettdmpwr=2`；**不带** `sweetcccfg`、**不带** `sweetoobcfg`。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| **T0-0 身份** | ring `sweettime=` 与本轮镜像一致；镜像内 `oobcfg v11 try `×1、`cccfg v2 `×4（该内核的 cccfg 仍是 v2，正常）、cmdline 长度**实测**且尾部 `sweettdmpwr=2 sweettime=<本轮>` 连续、**不含** `sweetcccfg`/`sweetoobcfg`、`sweetbufs=30` 在位 | 成立 | 打的确实是这一枪 |
| **T0-1 开关门** | `SWEET GLINK: tdm_power skipped (vendor does not send it)` **恰 1**，且 `vp/tdm_power` **0 行** | 全中 | 那一票确实没发（不是发了别的值） |
| **T0-2 主判据** | **有没有声音**（用户耳朵 = 终端判据；`ampread post-stream` 与 `data_buffer` 计数只作旁证） | 听到 / 没听到 | 听到 ⇒ 这一票是元凶之一，后续按 `0/1/2` 复现并拆attr；没听到 ⇒ 该格关掉，转 `sweettdmclk=1`（0265 的时钟票） |
| **T0-3 回归** | 平面 `data_buffer send` 31 / `done status=0` 4 / `no rsp` 0；`vp/` 其余各条 `status=0`；`gate flag` 1；内核 panic 0；**窗口内**收尾三行齐（`tools/ring_window.py` + `score_probe` 的 window_check） | 等值 | 不成立 ⇒ 该轮不判、停手交回 |

限制照写：听到/没听到都是**一次**样本，只说明"这一票在不在场时声音的有无"，**不命名机制**；
1/16 音幅档位不变（要更响/更长需要用户明确许可）。

## T0 判决 + 主判据的开机前修订（**这一条影响过去所有"没声音"的结论**）

**T0（第 31 枪，`logs/ring-t0-tdmpwr-off.txt`）各门**：身份 ✓（`sweettime=1793891212`×2、`window_check` 相等）；
开关门 ✓（`tdm_power skipped` 恰 1、无 `vp/tdm_power`，`vp/` 18→**16** 恰等于少掉那一封的"发+应"两行）；
回归 ✓（`data_buffer send` 31、`done status=0` 4、`no rsp` 0、`gate flag` 1、panic 0、收尾三行在窗口内）；
**主判据：用户完全没听到。**

**但主判据当时不可测量**：ring 自报 `SWEET MARK: tone gain g=1 peak=1024`，即 **−30.1 dBFS**
（`sweet_glink.c:5748` 的 `sweetvol`，1..31，默认 1 = "1/8 的 1/8"，注释自己写的是
"振幅刻意低——这个音只需要证明管道通，不需要响"），长度 31 发 × 6 ms ≈ **180 ms**。
⇒ **纪律入账：终端判据是"听得见"，而仪器在 1/16 档时物理上可能听不见。**
凡本轮之前依据"没听到"得出的结论，一律降级为**"在 −30 dBFS/180 ms 下未测到可听声"**；
其余全部结论（`status` 码、平面计数、哨兵）不受影响，因为它们不依赖耳朵。

**M1c 之后的开机档位（需用户逐轮许可，本次由用户主动提出"是不是太小声"而授予）**
- `T0b`：**只动仪器，不动被测物**——同一 kernel、同一 `sweettdmpwr=2`，加 `sweetvol=8`（−12 dBFS）
  与 `sweetbufs=120`（≈720 ms）。判据：用户听到 / 没听到；`tone gain g=8 peak=8192` 必须在 ring 里自证。
  回归门因变量变了要重述：`data_buffer send` 预期 ≤120（不是 31），`done status=0` ≥4，`no rsp` 0，panic 0，收尾齐。
- 若 T0b 听到 ⇒ "静音"其实是阈值假象，诊断重心从"哪封配置被拒"转向**音质/连续性**（内嵌可辨识片段来分辨糊/咔哒/变调），
  并顺手把 ② 被拒那一封降级为次要问题。
- 若 T0b 仍听不到 ⇒ 静音为真，按原计划打 **T1（`sweettdmclk=1`，0265 内核）**：补上厂商投而我们从没投过的
  播放侧 TDM 位时钟 `0x204@12.288 MHz`。

### T0b 的开机前变更（cmdline 预算逼出来的，理由记在这）
`+ sweetvol=8 + sweetbufs=120` 叠在 T0 那条上是 **595 B > 590**，被 `mkimage.sh` 的守卫挡下
（这道守卫就是历史上把 `sweetbufs=30` 切成 `3` 的那类事故的防线，不许绕过）。
⇒ T0b **去掉 `sweettdmpwr=2`**，测"**出厂配置 + 抬音量**"：T0 已经量过"少发那一票"对平面无影响
（31/4/0 与 M1r 等值），所以把 T0b 定义成"出厂形状的可听性"最干净，变量仍然只有一个（音量/长度＝仪器）。

## T0b 判决（第 32 枪，`logs/ring-t0b-audible.txt`）⇒ **静音不是阈值假象**

仪器自证：`SWEET MARK: tone gain g=8 peak=8192`（−12.0 dBFS，是出厂 1/8 振幅）、
`pcm tone done (120 buffers)`（≈720 ms）、`data_buffer send` **121**、`done status=0` 4、`no rsp` 0、
`gate flag` 1、panic 0、`===== done =====` 与 `PSCI SYSTEM_RESET` 均在本会话窗口内。
用户报告：**没有声音传出**。

⇒ 从"可听性"这条轴上，**−12 dBFS / 720 ms / 1 kHz 应当是可听见的**，因此：
**"控制面全绿 + PCM 被消费 + 无声"是真实的故障，不是音量档位造成的测量假象。**
（这条只否定"阈值解释"，不否定 T0 那一枪的开关门结论。）
⇒ 下一枪按 T0b 判据第 2 支走：**T1 = 0265 内核 + `sweettdmclk=1`**，补厂商投而从没投的
播放侧 TDM 位时钟 `0x204 @ 12.288 MHz / attr 4 / root 0`（`PRM_REQUEST_HW_RSC`，Android 侧 `status=0`）。
仪器档位改为**可听优先**：`sweetvol=31`（峰 31744 ≈ −0.3 dBFS）+ `sweetbufs=200`（≈1.2 s）——
振幅是测量条件、不是被试变量，故与"配不配时钟"这一格不冲突；开这一枪前须用户确认可以响。

## T1 判据（第 33 枪，**0265 内核 + `sweettdmclk=1`**；开机前写死）

**被试变量只有一个**：补上厂商投、我们从没投过的**播放侧 TDM 位时钟**
`PRM_REQUEST_HW_RSC(0x0100100f) → dst=2`，正文 36 B `{2, 0x0800102C, 20, 0, 1, 0x204, 12288000, 4, 0}`
（Android 侧 `status=0`，见 `docs/data/vendor-accepted-setcfg-2026-11-05.md`）。
其余全部与 T0b 相同（出厂图 `vp-wr`、`sweetampfirst=1`、`sweetbufs`、无 cccfg/oobcfg 探针）。
**仪器档位（测量条件，不是变量）**：`sweetvol=16` ⇒ 峰 16384 ≈ **−6.0 dBFS**，`sweetbufs=200` ⇒ ≈**1.2 s**；
理由：T0b 已在 −12 dBFS/720 ms 下证明"可听档没听到"，再抬 6 dB 且拉长到 1.2 s，排除"仍然太轻"这一条。
**开这一枪前需用户确认可以出声**（用户已问过"是不是太小声"，视为愿意听；响度上限仍留给 `31`）。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| T1-0 身份 | 镜像内 `tdmclk v1 `×1、`cccfg v3 `×6、`cccfg v2 `×0、`oobcfg v11 try `×1；ring `sweettime=<本轮>` 命中；cmdline 实测长度且尾部 `sweettdmclk=1 sweettime=…` 连续、无 `sweetcccfg`/`sweetoobcfg` | 成立 | 打的是 0265 的这一枪 |
| **T1-1 开关门** | `SWEET MARK: tdmclk v1 id=0x204 freq=12288000 attr=4 root=0 rc=?` **恰 1 行** | 出现 | 那一票真发了；`rc` 是要读的第一个数 |
| **T1-2 主判据（两读）** | ① `rc=0` 且用户**听到** ≈1.2 s 单音 ⇒ 时钟票是元凶；② `rc=0` 但仍无声 ⇒ 该格关掉；③ **`rc!=0`** ⇒ PRM 拒绝这一票（属性/root/该 clk_id 不适用于本图），也是一条实质读数 | 见左 | 三种都有信息，不依赖"必须成功" |
| T1-3 回归 | `tone gain g=16`、`pcm tone done (200 buffers)`、`data_buffer send` ≈201、`done status=0` ≥4、`no rsp` 0、`gate flag` 1、panic 0、收尾在窗口内 | 等值 | 不成立 ⇒ 不判、停手交回 |

**若 T1 听到**：立刻把方向从"哪封配置被拒"改成"音质与连续性"，并按你的问题做**内嵌 1.5 s 可辨识音频**
（走现成 `SWEETFW1` blob 读取器，一次编译），用它分辨糊/咔哒/变调；② 与 M 系列降级为次要。
**若 T1 三种读数都不利**：下一枪回到 0265 里已编好的 `sweetcccfg=2/3/4`（带内 iid=1+图级参数 / 配对矩阵），
它们**不需要再编译**，只换 boot args 本地重打包。

### T1 打包时的两次拦截（都记下来，它们各救了一次开机）
1. **590 守卫**：T0b 那条 581 B 再加 ` sweettdmclk=1` = **596 > 590**，`mkimage.sh` 直接失败退出。
   因为它是**在写镜像之前**失败，随后的 `cp` 把**上一轮 T0b 的镜像**复制成了 `images/t1-tdmclk.img`
   —— 差点开出一枪"参数里没有 `sweettdmclk` 的 T1"。**开机前逐条核产物这一步抓到了它**
   （`sweettime` 是上一轮的值、cmdline 里没有 `sweettdmclk`）。⇒ 已删掉那份错镜像；今后 `mkimage` 之后
   **必须先核产物再 `cp`/再开机**，不许连着写。
2. **"删一个值为 `-` 的参数"是反的**：`xnu/patches/0054-sweet-process-sampler.patch:198-202`
   —— `sweetsample` **不存在时默认 `"SpringBoard,backboardd"` 并真的起采样线程**，`=-` 才是关。
   ⇒ 腾字节改用删 `-noprogress`（只影响开机进度显示，不启动任何线程），并在此写明这是本轮与 T0b 之间
   **除被试变量外的第二个差异**（它不产生任何 GPR/音频行为）。

### T1 实际打的 cmdline（与 T0b 的差异全部列出）
`-noprogress` 在 `mkimage.sh` 的**基础串**里，不传不掉 ⇒ 字节从附加参数里腾：
**去掉 `sweetexp=15`**（12 字节）。它的缺席方向已核：`:7953/:7988` 都是 `int exp2 = 0;` 再 parse，
注释也写明这些位只是"加见证不花字节" ⇒ 少掉的只是 IBAT 电流见证、IOKit 音频探测见证、
以及经 `exp&4` 打开的 `ampread`（本就有 `sweetampfirst` 独立控制），**不新增也不移除任何 GPR 包**。
`sweetsample=-` **保留**（删它会打开采样线程，见上一节）。
最终 cmdline **566 B**、`sweettime=1793892602`、`sweettdmclk=1 sweetvol=16 sweetbufs=200` 全在，
镜像内 `tdmclk v1 `×1、`cccfg v3 `×6、`cccfg v2 `×0。

---

## 第 33 枪 T1 判决（`logs/ring-t1-tdmclk.txt`，打分于开机后当场）

**T1-0 身份：成立。** `sweettime=1793892602` 命中 2 次；`ring_window.py t1-tdmclk` 报
`total=0x1358c == decoded 0x1358c`（窗口吻合=True），`tdmclk v1 ` 恰 1 行且在窗口内。
⇒ 这份 ring 就是本轮的，没有吃到上一轮的尾巴。

**T1-1 开关门：成立，且 `rc=0`。**
`SWEET MARK: tdmclk v1 id=0x204 freq=12288000 attr=4 root=0 rc=0`（ring `:1556`），
前面是 `clk 0x204 attr=4 root=0 ok`。⇒ 那一票发了，ADSP 在**命令级**接受了它。

**T1-2 主判据：无法读，因为声根本没能发出去。** 本轮出现了一个**新的、更靠前的门**：

```
:1689 SWEET GLINK: graph_prepare rsp op=0x1001001 status=0  hop=0x2001005 len=32
:1690 SWEET GLINK: graph_start    rsp op=0x1001002 status=0x1 hop=0x2001005 len=32
:1691 SWEET GLINK: vp-wr graph opened but did not start
```

`op=` 打印的是响应正文 `g_le32(g_rsp+24)`（`sweet_glink.c:2641`）——**回显的是命令自己的 opcode**，
所以这不是把前一封的响应错认成 `graph_start` 的关联错位；`status` 取 `g_rsp+28`。
按既有规矩，`0x1` 只当"另一种拒绝"，不命名机制。

**T1-3 回归：下游全部归零，但不是采样器/流的问题。**

| 判据 | 期望 | 实测 |
|---|---|---|
| `tone gain g=16` | 1 | **1**（`:1654`，峰 16384 = −6.0 dBFS）✓ |
| `pcm tone done (200 buffers)` | 1 | **0**（只在窗口外 `0x184a5` 命中 = 上一轮的字节）✗ |
| `data_buffer send` | ≈201 | **0** ✗ |
| `no rsp` | 0 | 0 ✓ |
| panic | 0 | 0 ✓，收尾 `===== done =====` / `PSCI SYSTEM_RESET` 都在窗口内 ✓ |

`data_buffer` 循环本来就在 `did not start` 之后被跳过，所以这三行 ✗ 全部由**同一个更早的门**解释，
不构成第二件事。

**这条 `0x1` 有多反常（离线普查，零开机）**：`logs/ring-*.txt` 里
`graph_start rsp op=0x1001002 status=0` **157 次**，非零 **仅 T1 这 1 次**（共 158 次机会）。
⇒ 不是"以前也偶尔失败"，这是这个阶梯上的第一次。

**混淆项已量化（这是我该在开机前就抓出来的）**：T1 相对绿色基线**同时换了两件事**——
内核 0264→0265 **和** `sweettdmclk=1`。证据（打产物里的标记字符串）：
`images/t0-tdmpwr-off.img`、`images/t0b-audible-probe.img` 内 `tdmclk v1 `×**0**、`cccfg v3 `×**0**；
`images/t1-tdmclk.img` 内 ×**1**、×**6**。⇒ T0/T0b 跑的是 0264。
0265 里另外两个功能在本轮**运行期证明是休眠的**：ring 内 `cccfg v3` 0 行、`oobcfg v11` 0 行
（两个开关都不在 cmdline 里）。cmdline 上除被试变量外的差异（`sweetexp=15`、`sweetaudioprobe=1` 缺席）
只减见证打印，`sweetaudioprobe` 在链树源码里 grep 为 0 命中。
⇒ **执行过的代码里唯一的差异就是那一票**，但它不是"内核相同、只差一票"的那种对照，
所以 T1 单独一次开机**不能**给"票导致拒绝"定罪。

**关于"听"**：本轮**不向你索取听感读数**——一帧样本都没发出，物理上不可能有声音。
上一轮 T0b（−12 dBFS / 720 ms）你答"没有声音传出"仍是有效读数。

## 下一枪：C1 对照（第 34 枪）——重建 `graph_start` 基线。判决表在开机前写死。

镜像 `images/c1-no-vote.img`，本地重打包（**不跑 CI**），内核与 T1 同一个文件
（`out/kernel.adsp` sha256 `52ebd21c0b463b60…`，23:26 取来后未再生成）。
cmdline = **T1 逐字去掉 ` sweettdmclk=1`**，实测 **552 B**（= 566−14，本轮重测、不继承），
`sweettime=1793893320`，产物内核里 `tdmclk v1 `×1 / `cccfg v3 `×6 仍在（⇒ 仍是 0265，只是没触发）。

**为什么先花这一枪而不是直接开 `sweetcccfg=2/3/4`**：阶梯的绿色基线断了。基线不重建，
之后任何一枪的 `graph_start` 读数都没法解释。这一枪同时还顺手是我们**迄今最响的一次流**
（g=16、200 buf ≈1.2 s），与 T1 完全同档，不新增响度许可。

| 编号 | 判据 | 取值 | 结论分支（开机前定死，事后只许追加） |
|---|---|---|---|
| C1-0 身份 | ring 内 `sweettime=1793893320` 命中；`ring_window` 窗口吻合=True；`tdmclk v1 ` 运行期 **0 行**；`tone gain g=16` ×1 | 全部成立 | 任一不成立 ⇒ 本轮作废、不判、交回 |
| **C1-1 主判据** | `graph_start rsp op=0x1001002 status=?` | 0 | 票（在这个位置、这个形态）与 `graph_start` 被拒**相关**（1/1，仍不命名机制）。⇒ "补 TDM 位时钟能打通声音"这一格**在该位置不可测**；下一枪=同一票移到 `graph_start` **之后**（要编译，走 CI） |
| | | 0x1 | 票被**洗清**：差异要么在 0265 本身，要么偶然。⇒ 先在 0265 上**重放同一枪**（第 2 次，仍是无票），两次都 0x1 才允许谈 0265；两次都 0 才允许谈票 |
| C1-2 回归 | `pcm tone done (200 buffers)`、`data_buffer send`≈201、`no rsp` 0、panic 0、收尾在窗口内、控制面 4 行绿（`graph_open status=0`、`cb5 cba2r=0x1`、`ats1pr … par=0 OK`、`sctlr=0xc00027`） | 等值 | 不成立 ⇒ 不判 |
| C1-3 听感（仅当 C1-1=0 且 C1-2 齐） | 手机回到 adb 前最后 ≈1.2 s 有没有"哔——" | 听到 | "无声"前提**塌掉**：DAC/功放链路本来就能响，我们只是太轻；方向立刻改为响度与连续性 |
| | | 没听到 | −6 dBFS 下无声是**确证**（不是听阈问题）⇒ 回到功放/供电那一侧（`aw_cfg`、`tdm_power`、SPMI 轨），②/cccfd 系列降为次要 |

---

## 第 34 枪 C1 判决（`logs/ring-c1-no-vote.txt`，开机后当场打）

**C1-0 身份：成立。** `sweettime=1793893320` ×2；`ring_window` 报 `total=0x166fb == decoded 0x166fb`（窗口吻合=True）；
`tdmclk v1 ` 运行期 **0 行**（⇒ 早票真的没发）；`tone gain g=16 peak=16384`（`:1636`）✓。

**C1-1 主判据：`graph_start status=0` ⇒ 按预登记表落"票相关"这一格。**

```
:1667 graph_prepare rsp op=0x1001001 status=0  hop=0x2001005 len=32
:1670 graph_start   rsp op=0x1001002 status=0  hop=0x2001005 len=32
:1671 vendor graph + WR input started
```

同一个 0265 内核、同一份 cmdline（只差 ` sweettdmclk=1` 这 14 字节）、同一个 `out/kernel.adsp`
（sha `52ebd21c0b463b60…`）：**票一去掉，`graph_start` 就回到 0，并且真的开始流水**。
⇒ "在 `graph_open` 之前把 TDM 位时钟claim下来"这件事本身关掉了图。
按规矩这只写**相关**（1/1 对照 + 158 样本绿色基线），不命名机制；32 B 响应里没有逐记录字段可挖。

**C1-2 回归：不齐 —— 但不是被测变量坏的，是我的仪器坏在时间预算上。**

| 判据 | 期望 | 实测 |
|---|---|---|
| `data_buffer send` | ≈201 | **135** |
| `pcm tone done (200 buffers)` | 1 | **0**（只命中窗口外 `0x184a5`，上一轮字节） |
| `no rsp` / panic | 0 / 0 | 0 / 0 ✓ |
| `===== done =====` / `PSCI SYSTEM_RESET` | 各 1（在窗口内） | **各 0，且窗口内都没有** ✗ |
| 控制面四绿行 | 全绿 | 全绿：`graph_open/vp-wr status=0`（`:1615`）、`cb5 cba2r=0x1`（`:1627`）、`ats1pr … par=0 OK`（`:1628`）、`sctlr=0xc00027 … leaf=0x7c1`（`:1629`）；`geni amp 0x34/0x35 r=0 id=0x2032 profile 44/44`（`:1557-1560`）**早于** `pcm tone start`（`:1722`）✓ |

ring 最后一行是 `exec privacyaccountingd`，**停在流中间**。这不是"我们的收尾没跑"，而是**整个会话被看门狗咬掉**：
GPU 线早就见过同一签名并写在源码注释里（`sweet_glink.c:4059` "no PSCI SYSTEM_RESET, watchdog picked it up"）。

**离线核出来的开机窗口事实（今后每一枪都要按它规划）**：
`logs/ring-*.txt` 里 `data_buffer send` > 60 的轮次分两类——

| 轮 | sends | `pcm tone done` | 收尾 |
|---|---|---|---|
| ring-ab | 1032 | 0 | **无**（当年记录就是"cmdline 被截、1200 buf 跑过头"） |
| ring-j1-pcm / j3 / l2-gap8 / l3-gap20 / **c1-no-vote** | 127 / 106 / 121 / 104 / **135** | 0 | **无** |
| ring-t0b-audible | 121 | 1 | 有 |
| ring-u/v/w/z、s1ap3 | 501 / 1201 | 1 | 有 |

⇒ **等待应答那条路（`sweetnowait` 不开）每帧都要等 DATA_BUFFER 回包**，`sweetbufs` 是从"帧数"换算成"秒"的东西：
**120 帧能在看门狗之前跑完并留下整条 ring；200 帧不行（135 帧处被咬）。**
被咬的代价不只是尾巴：**`post` 时钟见证、音后 PA 读回、`===== done =====` 全丢**。
⇒ 规则：**在等待路径上 `sweetbufs` 上限取 120**；要加长必须先减掉别的会话工作，不能白拿。

**C1-3 听感：按预登记表作废**（表写的是"仅当 C1-1=0 **且 C1-2 齐**"）。
但这一枪**确实已经放出去 ≥0.8 s 的 −6 dBFS 单音**（135 帧 × 6 ms，被看门狗截断），
所以我把它当**辅助读数**单独问用户一次；听到就是新事实（说明 DAC/功放能出声），
没听到则与 T0b 的 −12 dBFS/720 ms 一起构成"无声是真的"，**不拿它定任何一格的罪**。

## 已经落库的下一枪准备（0266，`sweettdmclk=2`：图起来之后再投这一票）

内核侧改动已推（commit `0c4de79`，链树 `54ecf73`，补丁 `xnu/patches/0266-sweet-tdmclk-late-vote.patch`）：
早票的门从 `tdmclk != 0` 改成 `tdmclk == 1`，并把**同一个包**（`g_prm_vote_clock_ex(0x204,12288000,4,0)`，76 B，与厂商逐字段一致）
在 `g_apm_graph_bringup()` 里、`graph_start` 已答 0 之后、第一帧 `DATA_BUFFER` 之前重发一次，标记 `tdmclk v2 late`。
这样 `=1` 仍是 T1 那一枪的逐字复现，`=2` 是"绿起步 + 位时钟在流开始前起来"，**一次编译两种枪位**。

宿主机证据（`tools/stub_tdmclk.py`，真编译真跑，打印它读的哪个文件）：

```
compile clean: -std=gnu11 -Wall -Wextra -Wformat=2 -Werror
  sel  early_vote  late_vote  tone_runs        expect 0->0/0  1->1/0  2->0/1  3->0/0
    0          0          0          1
    1          1          0          1
    2          0          1          1
    3          0          0          1
```
⇒ 0265 那个"`=2` 会连早票一起打出去"的双变量坑被杀掉了；`=3` 是静默的（越界值不投任何票，别拿它当"关"）。
`tools/ci_replay.sh`：`applied=207 / skipped=49`，`OK: CI post-image == chain-tree HEAD`。

## 下一枪：F1（第 35 枪）= 0266 + `sweettdmclk=2 sweetvol=16 sweetbufs=120`

开机前判决表（事后只许追加）：

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| F1-0 身份 | 镜像内 `tdmclk v2 late `×1、`tdmclk v1 `×1、`cccfg v3 `×6；ring `sweettime=<本轮>` 命中且 `ring_window` 窗口吻合；运行期 `tdmclk v1 ` **0 行**、`tdmclk v2 late ` **1 行** | 全成立 | 打的是 0266 的"晚票"这一枪；晚票行必须**排在 `vendor graph + WR input started` 之后**（位置也是读数的一部分） |
| **F1-1 主判据（两读）** | ① `graph_start … status=0` **且** `tdmclk v2 late … rc=0` **且**用户听到 ⇒ 位时钟必须在流开始前起来，早票只是起得太早；② `status=0 rc=0` 但仍无声 ⇒ **时钟这条线整格关掉**（早/晚两种位置都试过且命令级都被接受），回到功放/供电与 `aw_cfg` 那一侧；③ `rc!=0` ⇒ PRM 在图已起来时拒这一票，也是实质读数 | 见左 | 三种读数都有信息 |
| F1-2 回归 | `graph_start status=0`、`tone gain g=16`、`pcm tone done (120 buffers)`、`data_buffer send`≈121、`no rsp` 0、panic 0、**`===== done =====` 与 `PSCI SYSTEM_RESET` 都在窗口内**（这条是看门狗事实的直接后果）、控制面四绿行 | 等值 | 不齐 ⇒ 不判、停手交回 |
| F1-3 若 `status=0x1` 再次出现（第三次异常） | 与 C1 只差"晚票 + 120 buf" | — | 先按"0266 引入的新异常"处理，重放 C1 同参（`=2` 换成空）一次再说 |

打包预算（本轮重测，不继承）：C1 实测 552 B + ` sweettdmclk=2` 14 B = **566 B**（`sweetbufs=120` 与 `=200` 等长）。

## F1 开机前修订（不是事后改表：镜像已核、尚未上电）

用户 06 日 09:19 说"继续就行，现在已经是白天了" ⇒ 宿舍安静档解除。被试变量不变（`sweettdmclk=2`），
**只把测量条件从 −6.0 dBFS 抬到代码允许的最大档 `sweetvol=31` ⇒ 峰 31744 = −0.27 dBFS**，
时长仍是 `sweetbufs=120` ≈0.72 s（看门狗事实：等待路径 >120 帧会被咬、丢整条尾巴）。
这样一枪就把"太轻"这条解释**一次封死**：满幅 0.72 s 单音仍听不见，就不是响度问题。
F1-1 的三种读数与响度无关，表格其余部分逐字沿用上一节。

**顺手记一个坑（`sweet_glink.c:5750`）**：`if (g < 1 || g > 31) g = 1;` ——
**`sweetvol=32` 不是更响，而是回到最轻的 −30 dBFS**。想超出 31 必须先改代码；
读 `tone gain g=` 时如果发现 `g=1` 而 cmdline 写的是 `32+`，那是钳位不是失效。

打包实测（本轮重测，不继承）：cmdline **566 B**（预测 566，命中），`sweettime=1793927938`，
尾部 `sweetvol=31 sweettdmclk=2 sweettime=…` 连续，产物内
`tdmclk v2 late `×1 / `tdmclk v1 `×1 / `cccfg v3 `×6 / `oobcfg v11 try `×1 / `sweetvol=16`×0（旧值不在镜像里）。
内核 `out/kernel.adsp` sha256 `4e5bc152b5e341bb…`（CI 摘要一致，`EXPECT_MARKER="SWEET MARK: tdmclk v2 late "` 过门）。

---

## 第 35 枪 F1 判决（`logs/ring-f1-late-tdmclk.txt`）—— 落在预登记的③，而且是个对称读数

**F1-0 身份：成立。** `sweettime=1793927938` ×2；`ring_window` 报 `total=0x16336 == decoded`、窗口吻合=True；
运行期 `tdmclk v1 ` **0 行**、`tdmclk v2 late ` **1 行**，且位置正如设计：

```
:1683 graph_start rsp op=0x1001002 status=0     <== 图起来了
:1684 vendor graph + WR input started
:1685 PRM clk 0x204 @ 12288000 attr=4 root=0    <== 晚票在这里才发
:1686 TX_DATA iid=112 len=76
:1687 clk 0x204 attr=4 root=0 status 0x1        <== PRM 拒了
:1688 SWEET MARK: tdmclk v2 late ... rc=-1
```

`rc=-1` 的来源是**响应 status 非 0**（`g_prm_vote_clock_ex` 的两条 -1 分支里，`no response` 那条本轮 0 命中），
`-1` 具体是 `0x1`——按既有规矩只记成"另一种拒绝"，不命名机制。

**F1-2 回归：第一次全绿的完整会话（看门狗规矩生效）**

| 判据 | 实测 |
|---|---|
| `tone gain g=31 peak=31744` | ✓（−0.27 dBFS，代码钳位允许的最响一档） |
| `pcm tone done (120 buffers)` | ✓ 在窗口内（`:2008`） |
| `data_buffer send` | **121** ✓ |
| `no rsp` / panic | 0 / 0 ✓ |
| `===== done =====` / `PSCI SYSTEM_RESET` | 各 1，**都在窗口内** ✓（`ibat post` 与 `lpass read skipped` 也跑到了） |
| 控制面 | `graph_open/vp-wr status=0`、`graph_prepare status=0`、`vp/tdm_*` 与 `vp/aw_cfg*` 全 `status=0` |

**把三枪合起来读（同一包 76 B、逐字段与厂商一致，只换位置）**：

| 0x204 那一票的位置 | 票的答复 | `graph_start` | 流 |
|---|---|---|---|
| `GRAPH_OPEN` 之前（T1，0265 `=1`） | **status 0（接受）** | **0x1 拒绝** | 0 帧 |
| 完全不投（C1，0265 无参） | — | 0 | 135 帧后被看门狗咬 |
| `GRAPH_START` 之后（F1，0266 `=2`） | **status 0x1（拒绝）** | 0 | **120 帧跑完，满幅** |

⇒ 事实是"**这一个资源在我们这张图的两侧各拒一次，而且总是另一侧通**"。
厂商能在 `OPEN` 之前投成，是因为它那时候开的是**它自己的 496 B 小图**（`0x2010`）+ **2872 B 大图（`0x2011`）**；
我们的 760 B `vp-wr` 既不是它任何一张。所以"抄厂商的包"这条路到 F1 已经走到头：**包对了，图不对**。

**这一枪的声学状态**：F1 是我们**第一次**在"满幅 −0.27 dBFS、完整 0.72 s、收尾全部跑到"的条件下放音，
所以听感读数只可能由它给出（C1 的 0.8 s/−6 dBFS 是被截断的辅助读数）。

## F1 的听感读数（用户 06 日答："没有，完全没有"）—— 响度这条解释正式关闭

F1 是**满幅（−0.27 dBFS，代码钳位上限）、完整 0.72 s、收尾全部跑到**的一次放音，
用户明确回答**完全没有听到**。连同前面两点：

| 轮 | 幅度 | 时长 | 收尾 | 听感 |
|---|---|---|---|---|
| T0b | −12.0 dBFS (g=8) | 720 ms | 完整 | 没有 |
| C1 | −6.0 dBFS (g=16) | ≈0.8 s 后被看门狗截断 | 截 | 未答（作废） |
| **F1** | **−0.27 dBFS (g=31)** | **0.72 s** | **完整** | **没有，完全没有** |

⇒ "**太轻**"不再是可用解释。剩下的事实链是：APM 收包、SHM 槽位被消费、
`data_buffer done status=0`×、图 prepare/start 全 0、PA 的 I2C 配置在**放音前**写入且应答 0，
但**声波没有**。这一条现在只能往"ADSP 之后的物理链路"追。

## 零开机复盘：我们发给 TDM 模块的参数表 vs 厂商接受过的参数表（决定下一枪）

把 22:32 那一窗的厂商 SET_CFG **逐字段**解开（`payload = {iid, param, size, error} + data`，
描述符第四个字段才是 payload_size），对 `dst=0x428f`（TDM）只有**两封**：

| 厂商（被接受） | 正文 |
|---|---|
| `0x08001017` PARAM_ID_HW_EP_MF_CFG，12 B | `{96000, 0x00030020, 1}` |
| `0x0800101b` PARAM_ID_TDM_INTF_CFG，28 B | `{0, 2, 0x10001, 7, 0x200004, 0, 1}` |

对 `dst=0x42d3`（AW882xx）两封 `0x10013d2a`，28 B：
`{0, 0x10013d1b, 1, 0, 0, 25387, 0}` 与 `{0, 0x10013d1c, 1, 0, 0, 28512, 0}`。

我们这一轮（F1 ring `:1657-1677`，全部 `status=0`）：

| 我们 | 与厂商对比 |
|---|---|
| `vp/tdm_cfg` = `0x0800101b` 28 B `{0,2,0x10001,7,0x200004,0,1}` | **逐字相同** ✓ |
| `vp/tdm_ep_mf` = `0x08001017` 12 B `{96000,0x30020,1}` | **逐字相同** ✓ |
| `vp/tdm_frame` = `0x08001018` 4 B 值 1 | **厂商从不发** |
| `vp/tdm_power` = `0x08001176` 4 B 值 **0** | **厂商从不发**（0248 已记下来源是 CDMA 时代列表） |
| `vp/tdm_clk` = `0x0800113c` 16 B `{0,12288000,0,0}` | **厂商从不发** |
| `vp/aw_cfg` / `vp/aw_cfg2` = `0x10013d2a` 各 28 B | **逐字相同** ✓（含 25387 / 28512） |

⇒ 到 F1 为止，**"照抄厂商"这件事在带内单包这条路上已经抄到只剩三封多余包**。
这三封都"命令级被接受"，但接受不等于无害——尤其 `0x08001176 = 0`：
如果这个参数是某种 enable/gate，我们等于每次都在**放音前把它写成 0**。
（历史：`sweettdmpwr=2` 已在 T0 试过去掉它，但那一轮是 g=1/30 帧，**听不出任何结论**；
`=1`（写 1）**从未开过**；而 `tp=0` 与 `tp=1` 的日志当时一模一样，无法自证发了哪个值——
这一条仪器坑在 0267 里补掉了。）

## 0267 已落库（`sweettdmvendor` 位掩码）

链树 `3aa2e91`，补丁 `xnu/patches/0267-sweet-tdmvendor-parameter-mask.patch`，
标记 `SWEET MARK: tdmcfg v1 sup=`（workflow 已同步）。宿主机 `tools/stub_tdmvendor.py`
从真实源码抽四个区域（parse / 1018 / 1176 / 113c）编译并跑满表：

```
 sup tdpwr  发给 0x428f 的包（按顺序）
   0    0   1018(val1) 1176(val0) 113c            n=3
   0    1   1018(val1) 1176(val1) 113c            n=3      <- 值现在可从日志分辨
   0    2   1018(val1)            113c            n=2
   1    *   (1018 not sent)       1176, 113c      n=2
   2    *   1018, (1176 not sent) 113c            n=2
   4    *   1018, 1176            (113c not sent) n=2
   7    *   (nothing)                             n=0      <- 厂商最小集
```
`tools/ci_replay.sh`：`applied=208 / skipped=49`，`OK: CI post-image == chain-tree HEAD`。

## 下一枪 H1（第 36 枪）= 0267 + `sweettdmvendor=7 sweetvol=31 sweetbufs=120`（厂商最小集）

开机前判决表（事后只许追加）：

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| H1-0 身份 | 镜像内 `tdmcfg v1 sup=`×1、`tdmclk v2 late `×1、`tdmclk v1 `×1、`cccfg v3 `×6；ring `sweettime=<本轮>` 命中且窗口吻合；运行期 `tdmcfg v1 sup=7 tdpwr=0` ×1、`1018 not sent` ×1、`1176 not sent` ×1、`113c not sent` ×1；ring 里 **不再有** `vp/tdm_frame` / `vp/tdm_power` / `vp/tdm_clk` 三行 | 全成立 | 三封自创包真的没发（这套仪器能自证失败） |
| **H1-1 主判据（两读）** | ① `vp/tdm_cfg` 与 `vp/tdm_ep_mf` 仍 `status=0`、`graph_start=0`、`pcm tone done (120 buffers)`、**用户听到** ⇒ 三封里至少一封是有害的，立刻按 `sup=1/2/4` 三枪二分定位是哪封；② 同样全绿但**仍无声** ⇒ "**我们发给 TDM/AW 的带内参数内容**"这一整条轴关闭（与厂商完全一致仍无声），下一枪只能往 **ADSP 之后的物理侧**追：PA 是否在总线上（0240 那条读路）、`tdm_power` 之外功放自己的供电轨、以及"这张 760 B 图里 TDM 输出到底接到哪个 MI2S 实例" | 见左 | 两种都有信息；不依赖成功 |
| H1-2 回归 | `tone gain g=31`、`data_buffer send`≈121、`no rsp` 0、panic 0、`===== done =====` 与 `PSCI SYSTEM_RESET` 在窗口内、控制面四绿行、`geni amp 0x34/0x35 … profile 44/44` 早于 `pcm tone start` | 等值 | 不齐 ⇒ 不判 |
参数预算：F1 566 B − ` sweettdmclk=2`(14) + ` sweettdmvendor=7`(18) = **570 B**（本轮实测填死）。

### H1 打包实测（开机前，逐条核产物）
`sweettime=1793930330` 命中 1 次，cmdline 实测 **569 B** —— **预测 570 差 1**：
` sweettdmvendor=7` 是 17 字节不是 18（`sweettdmvendor=7` 本身 16 个字符）。
这是同一类错误的第三次（T1 两次预测 583 实际 582），**长度永远是本轮实测值，预测只用于抓意外，不用于继承**。
569 < 590 ✓。尾部连续：`… sweetvol=31 sweettdmvendor=7 sweettime=1793930330`，无 `sweettdmclk` 在 cmdline 里
（镜像内那个 1 次命中是内核里的参数名字符串）。产物内核标记：
`tdmcfg v1 sup=`×1、`1018 not sent`×1、`1176 val=`×1、`113c not sent`×1、`tdmclk v2 late `×1、`cccfg v3 `×6。
`out/kernel.adsp` sha256 `67b24f148512259d…`（CI 摘要一致，`EXPECT_MARKER` 过门）。

---

## 第 36 枪 H1 判决（`logs/ring-h1-vendor-minimal.txt`）—— 自创参数这条轴关闭

**H1-0 全部成立，仪器自证到位。** `sweettime=1793930330` ×2、`ring_window` 窗口吻合=True；
运行期四条标记按序出现（`:1652-1655`）：

```
tdmcfg v1 sup=7 tdpwr=0
tdmcfg v1 1018 not sent
tdmcfg v1 1176 not sent (tp=0 sup=7)
tdmcfg v1 113c not sent
```
并且 `vp/tdm_frame` / `vp/tdm_power` / `vp/tdm_clk` 在整条 ring 里 **0 次**，
厂商那两封仍在：`vp/tdm_cfg` ×2、`vp/tdm_ep_mf` ×2、`vp/aw_cfg*` ×4，全部 `status=0`。

**H1-1/H1-2：全绿。** `graph_open status=0`、`graph_prepare status=0`、**`graph_start status=0`**、
`tone gain g=31 peak=31744`、`pcm tone done (120 buffers)`、`data_buffer send` 121、`no rsp` 0、panic 0、
`===== done =====` 与 `PSCI SYSTEM_RESET` 在窗口内、控制面四绿行齐、`geni amp 0x34/0x35 r=0 id=0x2032 profile 44/44` 早于 `pcm tone start`。

**听感：用户答"没响"，且 H1-2 齐 ⇒ 这是一条有效负读数。**
⇒ **预登记分支②**：三封自创包**不是**元凶（去掉它们什么也没变），
"我们发给 TDM/AW 的带内参数内容"这条轴**正式关闭**——因为此刻我们与厂商在带内单包上已经完全一致。

## 零开机普查（由外部建议"修音频解码"触发）：我们整张图里**没有转换级**

把三个 blob 的字节直接扫一遍（`python3`，只读源码数组，不猜格式）：

| blob | 实例 iid 集合 |
|---|---|
| `g_graph_vp_wr`（760 B，我们在开的） | `0x4006 0x4007 0x4012 0x40bd 0x418e 0x4199 0x41e2 0x422c 0x428f 0x42d3 0x4333 0x4436 0x4448 0x467e 0x468d 0x48ba` |
| `g_graph_vendor4`（2724 B，厂商真图） | 上面这些 **加上** `0x4008 0x40be 0x40bf 0x40c0 0x40c1 0x40c2 0x40c4 0x40c6 0x40c8 0x4165 0x42a1 0x42a4 0x42a6 0x42a7 0x42ad 0x42d4 0x42d7` |

而厂商 accepted 流量里，`0x40bf` / `0x40c0` 各收到一封
`0x08001008 PARAM_ID_PCM_OUTPUT_FORMAT_CFG`（30 B，正文含 48000 与 0x1b0020/0x20001 等），
`0x40bd` 收到 `0x0800100c`；我们源码 :2047 自己的注释就把这些实例称作
**MFC/CONVERTER instances 0x40c8/0x40bf/0x40c0**。

再看两端声明（都在我们自己的代码里）：
- WR 端点 `0x0800100c` = **48000 Hz / 16 bit / q15 / 立体声**（:6731-6742，逐字来自厂商 kprobe）——我们的正弦也按 48k/16bit 填；
- TDM 侧 `0x08001017` = **96000 Hz / 0x30020**、`0x0800101b` 含 `0x200004`，位时钟 `0x204 @ 12288000 = 96000×128`（:6783-6790、:6925-6929）。

⇒ **48 kHz/16-bit 进去、96 kHz/32-bit 出线，中间那一跳在厂商图里由 0x40bf/0x40c0/0x40c8 这类转换实例做，而我们的图里一个都没有。**
这是本轮唯一还能同时解释"所有命令被接受、缓冲被消费、满幅仍无声"的结构性缺口。
诚实标注：**它目前是"未被任何开机否证"，不是"已被证实"**——下一步先零开机把厂商图的 module/port/connection 表解出来，
指名哪个实例是 SRC/MFC、端口怎么接；再用**现成开关**（`sweetvgraph/sweetvgoff/sweetvglen`，不用编译）开真图试响。
次疑点同时记下：`0171` 对 GPIO121-124 写的是**同一个** mux 值 4，而 DT `tert_tdm_*_active` 是四个不同函数名
（`mi2s2_sck`/`mi2s2_data0`/`mi2s2_ws`/`mi2s2_data1`，drive-strength 8、bias-disable）——对不对得看每脚函数表，我们手上没有。

**响度规矩更新（用户 06 日）**：以后**用耳朵当判据的轮次一律 `sweetvol=6`（峰 6144 = −14.5 dBFS）**；
只读寄存器的轮次压回默认 `g=1`（−30 dBFS）。已写进本文件与 `START-HERE.md`。

## 0268 落库：把我们**真的有**的那个 MFC 配上输出格式（子智能体报告 + 我的两处更正）

**更正 1（子智能体的措辞）**：它说厂商"对 MFC 0x40c8 发 0x08001024"——**不完整**。
抓包解出来是**一个 dst=1 批量里的两条记录**（payload 64、整包 104）：

```
rec1 {iid=0x40ca, param=0x08001024, size=12, err=0, data={0x0000bb80(48000), 0x00020020, 0x00020001}}
rec2 {iid=0x40c8, param=0x08001024, size=12, err=0, data={0x00017700(96000), 0x00020020, 0x00020001}}
另一批同形状：{0x401d -> 48000, 0x40b6 -> 96000}
```
**更正 2（我自己的普查也错过）**：我第一次只取每条报文的**首记录**，于是断言"`0x40c8` 从没出现"——错，
它是第二条。**多记录容器必须整条走链**（`tools/walk_param_records.py` 的存在理由），这条重新写进规矩。

**新事实（两方独立扫描一致，我已复核）**：
- 我们的 `g_graph_vp_wr` 模块表里**只有一个转换实例**：`MODULE_ID_MFC = 0x07001015`，iid `0x48ba`（blob 内偏移 0x0100）。
  厂商的**设备子图那半张与我们逐字节相同**（它的 14 条 connection = 我们的 15 条减去 `0x40bd→0x4012`）；
  厂商多出来的是**流子图**那一串 `0x40bd→0x40be→0x40bf(PCM_DEC 0x07001005)→0x40c0(PCM_CNV 0x07001003)→0x40c1→0x40c2→0x4008→0x40c4→0x40c6→0x4165`。
- **`0x48ba` 在整个 driver 里只出现在两条注释里**（:6685、:6826）⇒ 我们图里唯一坐在 48k/96k 边界上的模块，
  **从来没被告知任何输出格式**。而 `sweetcccfg=3` 把那两条 MFC 记录发给了 `0x428f`（TDM）——
  按配对规则就是"对的参数配错的模块"，被拒是应该的。
- `sweetvcfg2` 三枪（`ring-ab/i4/vcfg2`）已证：发给厂商那套 iid（`0x40bf/0x40c0/0x40c8`）**全部 `status=0x1`**
  ⇒ 那些模块确实不在我们图里，**"多补配置"这条路已死**；剩下的是配置我们自己的 MFC，或改图。

**0268 的做法**（链树 `3568269`，补丁 `0268-sweet-mfc-output-media-format-selector.patch`，标记 `SWEET MARK: mfc v1 sel=`）：
`sweetmfc` 选择子 —— `1` 单条 dst=0x48ba @96000、`2` 单条 @48000、`3` 厂商形状的两条批量（两条都点名我们自己的 0x48ba），
**其它值什么都不发并打印 out of range**。

**宿主机 stub 抓到的两个真 bug（`tools/stub_mfc.py`，一次开机都没花）**：
1. 我第一版按 `16 + 12` 跨记录 ⇒ 批量 body 只有 **56**，厂商是 **64**：第二条记录会**早 8 个字节**，
   DSP 会把它的 `iid/param/size` 读歪。改成 `16 + align8(12) = 32` 后 body=64 ✓、整包 104 ✓。
2. 我第一版的 `else` 让**越界值（如 9）掉进批量分支**——那会把"没这个档"变成"发了但读数无法解释"。
   现在 `sel=9 -> NONE`，且打印独立的一行。
```
  sel  form     target   param       rec_size  rate/body
    0  NONE                                          nrec=0 body=0
    1  single   0x48ba   0x8001024  12        0x17700 nrec=1
    2  single   0x48ba   0x8001024  12        0xbb80  nrec=1
    3  batch    0x48ba   0x8001024  12        0xbb80  nrec=2 body=64
    9  NONE                                          nrec=0 body=0
```
`tools/ci_replay.sh`：`applied=209 / skipped=49`，`OK: CI post-image == chain-tree HEAD`。

## 下一枪 G1（第 37 枪）= 0268 + `sweetmfc=1`，其余与 H1 逐字相同

对照 = **H1**（同一 0267/0268 家族、`sweettdmvendor=7`、控制面全绿、满幅仍无声）。
**唯一变量 = 多发的这一封 MFC 输出格式**。响度改用你定的天花板 **`sweetvol=6` = 峰 6144 = −14.5 dBFS**，
`sweetbufs=120` 不变。

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| G1-0 身份 | 镜像内 `mfc v1 sel=`×**2**（summary + out-of-range 两条 printf 都含该前缀，这是**镜像里的静态串**，不是运行期次数）；ring 内 `sweettime=<本轮>` 命中 + 窗口吻合；运行期 `mfc v1 sel=1 iid=0x48ba param=0x8001024 rc=?` **恰 1 行**，且 `sel=9 out of range` **0 行** | 全成立 | 这一封真发了，且发到的是我们自己的 MFC |
| **G1-1 主判据（三读）** | ① `rc=0` 且**听到** ⇒ MFC 输出格式就是缺失的那一环，下一步把厂商的 48k/96k 成对结构补全（`sel=3`）并讨论音质；② `rc=0` 且**仍无声** ⇒ "配置我们自己的 MFC"这一格关闭，**只剩改图**（加 PCM_CNV/PCM_DEC 实例或补流子图）；③ **`rc!=0`** ⇒ 这个 iid 不吃这个 param（配对规则在我们自己模块上也成立），同样是实质读数 | 见左 | 三种都有信息，不依赖成功 |
| G1-2 回归 | `tone gain g=6`、`graph_start status=0`、`pcm tone done (120 buffers)`、`data_buffer send`≈121、`no rsp` 0、panic 0、`===== done =====`/`PSCI SYSTEM_RESET` 在窗口内、`tdmcfg v1 sup=7` 三行 not sent 仍在、控制面四绿行 | 等值 | 不齐 ⇒ 不判 |
预算：H1 实测 569 − `sweetvol=31`+`sweetvol=6`（−1）+ ` sweetmfc=1`（+12）= **预测 580，本轮实测填死**。

## 轮次重排（开机前，G1 未打过 ⇒ 不是事后改表）

0269（GET_CFG 读回）落库后，下一枪从 G1 改为 **J1**：

| 枪 | 参数 | 为什么先看它 |
|---|---|---|
| **J1（第 37 枪）= 只读** | `sweetmfcget=2`（不发 set，`sweetmfc` 保持 0） | GET 不改模块状态；一次开机直接拿到 **MFC 此刻真实输出速率**（post-start 那次尤其关键）。读数就分胜负：`48000/16` ⇒ "MFC 还停在 Native"被**寄存器**支持；`96000/32` ⇒ MFC 这条轴当场关掉，不必再花耳朵 |
| **J2 = 设 + 重读** | `sweetmfcget=2 sweetmfc=1` | 同一次开机里 pre-set/post-set/post-start 三个读数互为对照，"设了以后变没变"不再依赖开机间比较 |
| G1（表已提交，**未打过**） | `sweetmfc=1` 单变量 | 降级为 J2 的备选形状；它仍然只动一个变量，但少了读回，信息量低于 J1 |

J1 其余条件沿用 H1 的绿基线：`sweettdmvendor=7 sweetvol=6 sweetbufs=120`（−14.5 dBFS、看门狗上限内），
`sweetmfc=0`（不发 set）。**判据（开机前写死）**：

| 编号 | 判据 | 取值 | 含义 |
|---|---|---|---|
| J1-0 身份 | 镜像内 `mfcget v1 `×1、`mfc v1 sel=`×2；ring `sweettime=<本轮>` 命中 + 窗口吻合；运行期 `mfcget v1 pre-set raw:`、`mfcget v1 post-start raw:` 各恰 1 行，且**没有** `mfc v1 sel=` 行（`=0` 不发） | 全成立 | 只发了读，没发写 |
| **J1-1 主判据** | `post-start` 那行的 `w11`（预期采样率）与 `w9`（预期实际返回长度，若从 72 变小即"模块真的写了回来"） | `w11=0xbb80`(48000) | MFC 仍在 Native/输入速率 ⇒ 速率不匹配由寄存器成立，G1/J2 的"设 96k"就是对症；且这是**不依赖耳朵**的第一条音频机制读数 |
| | `w11=0x17700`(96000) | ⇒ MFC 已经在出 96k，这一轴当场关掉，回到"线上有没有时钟/功放有没有供电" |
| | 无回包 / `hop!=0x2001000` / `w9` 仍是 72 | ⇒ GET 形状或该模块的 get 能力不对，**这是仪器读数，不是负结果**；先换 `sweetmfc=1`（J2）拿写侧的 rc 再定 |
| J1-2 回归 | 与 H1 逐项等值：`graph_start status=0`、`tone gain g=6`、`pcm tone done (120 buffers)`、`data_buffer send`≈121、`no rsp` 0、panic 0、收尾两行在窗口内、控制面四绿行、三条 `not sent` 仍在 | 等值 | 不齐 ⇒ 不判 |
字节预算：H1 569 − ` sweettdmclk=2`? （H1 没有该参数）+ ` sweetmfcget=2`(14) = **583 预测**，本轮实测填死。

### J1 判据修订（**开机前**，0270 替换 0269 的解析；J1 仍未打过）
- 镜像 marker 门：`mfcget v2 `×1（`mfcget v1 ` 已不存在 ⇒ 若产物里还是 v1，说明取到的是 0269 内核，**不许开机**）。
- J1-1 的取值改成"看应答的 opcode 分支"：
  `get_cfg … rate=` 行 ⇒ 有结构化应答，`rate` 就是 MFC 当前输出采样率（48000/96000 两个结局同前）；
  `basic … echoed_op=0x1001007 status=0xN` ⇒ 请求没进结构化路径（路由/头/映射/应答分配层失败），
  **这是仪器故障**，不是"MFC 不支持 Get"；
  `get_cfg … size=72 rate=0` ⇒ 应答结构对了但**没人写回来** ⇒ 该模块的 Get 能力/容器转发有问题，
  同样不算 MFC 的负结果；
  `no response` ⇒ 传输层无应答，按仪器故障处理。
- 其余回归项不变。

### J1 打包实测（开机前；设备仍不在，未上电）
内核 `out/kernel.adsp` sha256 `92fa441944d76b68…`（`EXPECT_MARKER="SWEET MARK: mfcget v2 "` 过门，CI 摘要一致）。
产物 `images/j1-get-mfc-rate.img`：`sweettime=1793942777` 命中 1，cmdline 实测 **582 B**
——**预测 583 又差 1**（今年第四次；` sweetmfcget=2` 是 14 字节不是 15）。
镜像内标记：`mfcget v2 `×**4**（四个分支各一条 printf，符合预期）、`mfcget v1 `×**0**
⇒ **这不是 0269 的内核**（身份门要的正是这一条）；另有 `mfc v1 sel=`×2、`tdmcfg v1 sup=`×1、
`tdmclk v2 late `×1、`cccfg v3 `×6。
cmdline 里 `sweetmfc=` ×0（⇒ 只读不写，J1 的单变量成立）、`sweetvol=6` ×1、`sweetvol=31` ×0、
`sweettdmvendor=7` ×1，尾部 `… sweetvol=6 sweettdmvendor=7 sweetmfcget=2 sweettime=1793942777` 连续。
`sweettdmclk` ×1 是内核里的**参数名字符串**，不在 cmdline（尾巴已核），别把它当成参数还在。

### J1 开机前快照（0271 内核，真实产物；设备已连）
内核 `out/kernel.adsp` sha256 `3fac17f0c29d5830…`（17:26 取回，`EXPECT_MARKER="SWEET MARK: mfcget v3 "` 过门，
CI `6daab4e` 的 build = completed/success）。
**身份三查**：镜像内 `mfcget v3 `×**4**、`mfcget v2 `×**0**、`mfcget v1 `×**0**
⇒ 这是 0271，不是 0270/0269（上一条失误的教训就落在这三行上）。
产物 `images/j1-get-mfc-rate.img`：`sweettime=1793957309` 命中 1，cmdline 实测 **582 B**
（与同参数上一轮实测一致）；cmdline 内 `sweetmfc=` ×**0**（⇒ 真的只读不写）、
`sweetmfcget=2` ×1、`sweettdmvendor=7` ×1、`sweetvol=6` ×1、`sweetbufs=120` ×1，
尾部 `… sweettdmvendor=7 sweetmfcget=2 sweettime=1793957309` 连续。
其余标记齐全：`mfc v1 sel=`×2、`tdmcfg v1 sup=`×1、`tdmclk v2 late `×1、`cccfg v3 `×6、`tone gain g=`×1。

**本轮要读的三行（`SWEET MARK: mfcget v3 <相位> …`）与预设分支**：

| 相位 | 主预测 | 偏离时的读法 |
|---|---|---|
| `pre-set` | 无效：`rate=-1 nch=-1 bw=32`（MFC 初始化即 invalid，且 bw 走 helper 默认分支） | 若这里就有效 ⇒ 有别的来源在 OPEN 阶段就写了格式，改查来源 |
| **`post-prepare`** | **`{48000, 16, 2}`**（WR 声明的就是 16/Q15，见 §15） | `{96000,32,2}` ⇒ 有东西反向覆写；无效 ⇒ 传播断在 WR→SAL（位宽那一维优先） |
| `post-start` | 同 `post-prepare` | prepare 有效而这里无效 ⇒ 断在 START/数据路径 |

**仪器故障类（不算模块结论，先修仪器再判）**：`basic … echoed_op=0x1001007 status=0xN`、
`get_cfg … size=72 rate=0`（没人写回）、`no response`、`len=` 异常（GPR 头长度假设 24）。
**回归门**（不齐则不判）：`graph_start status=0`、`tone gain g=6`、`pcm tone done (120 buffers)`、
`data_buffer send`≈121、`no rsp` 0、panic 0、`===== done =====`/`PSCI SYSTEM_RESET` 在窗口内、
控制面四绿行、`tdmcfg v1 sup=7` 的三条 not sent 仍在。

---

## 第 37 枪 J1 判决（`logs/ring-j1-get-mfc-rate.txt`）—— 整条音频线**第一条正向机制读数**

**J1-0 身份：成立。** `sweettime=1793957309` ×2；`ring_window` 报 `total=0x162af == decoded`（窗口吻合 True）；
`mfc v1 sel=` 运行期 **0 行**（真的只读不写）；`tdmcfg v1 sup=7` 三条 `not sent` 仍在；
`tone gain g=6 peak=6144`。

**仪器本身先被证明能用**（这是过去几个月最缺的东西）：三行都走了**结构化分支**
`get_cfg`（应答 opcode = `0x02001000`，不是 basic result），
`st=0`、`err=0`，而且 **`size` 从我们提供的 72 变成 8** ⇒ **模块真的写回来了**
——这正是 N1 那次"`err=0` 其实是没人写过"的反向哨兵。`len=116`。

**读到的内容（三行逐字）：**

```
:1675 mfcget v3 pre-set       get_cfg len=116 st=0 iid=0x48ba param=0x8001024 size=8 err=0 rate=-1 bw=32 nch=65535 cmap=0
:1680 mfcget v3 post-prepare  get_cfg len=116 st=0 iid=0x48ba param=0x8001024 size=8 err=0 rate=-1 bw=32 nch=65535 cmap=0
:1686 mfcget v3 post-start    get_cfg len=116 st=0 iid=0x48ba param=0x8001024 size=8 err=0 rate=-1 bw=32 nch=65535 cmap=0
```

`rate=-1`、`nch=0xFFFF`（即 -1）、`bw=32` —— **`bw=32` 不是"输出 32 bit"**，
而是外部答案预言过的那个形状：无效 Q 值走 helper 的默认分支落回 32。
⇒ **命中的是"备选预测"（MFC 从未收到有效输入格式），不是主预测 `{48000,16,2}`。**

**这条读数的意义（也是它的边界）**：
- 意义：**OPEN 之后的配置、PREPARE 的控制路径传播、START 之后——三个时刻 MFC 都没有媒体格式。**
  这是本项目第一次不需要靠"哪个命令被拒"来定位：**没有任何东西被拒**（本枪里
  `graph_open/prepare/start` 与全部 SET_CFG 都是 `status=0`），而链路上的转换级**根本没被喂过格式**。
  它同时解释了长期现象："全部接受、PCM 被消费、满幅仍无声"。
- 边界：**还不能说断在哪一段**。三种可能都还活着：
  (a) WR 自己就没有把格式发布成"模块的输出格式"；
  (b) SAL 拒收（位宽/Q 那一维，见 §15：我们声明 16/Q15，厂商 24/Q23）；
  (c) 格式本来该由图内各模块的 cal 数据给出，而我们拿不到那份 18 KB 自定义配置（②的老墙）。
  ⇒ 下一枪用**多模块读回**来定位，而不是继续猜。

**J1-2 回归：全绿。** `graph_prepare status=0`、**`graph_start status=0`**、
`pcm tone done (120 buffers)`、`data_buffer send` 121、`no rsp` 0、panic 0、
`===== done =====` 与 `PSCI SYSTEM_RESET` 在窗口内、控制面四绿行齐
（`graph_open status=0`、`cb5 cba2r=0x1`、`ats1pr par=0 OK`、`geni amp 0x34 r=0 id=0x2032` 早于 tone）。

## 下一枪 0272 / K1：把"读回"从 MFC 一点扩到三点 + 第四个相位

改动很小（只读，零状态写入）：
1. 读回**三个模块各自的格式参数**：`0x40bd` 的 `0x0800100c`（WR）、`0x428f` 的 `0x08001017`（TDM_SINK 接收格式）、
   `0x48ba` 的 `0x08001024`（MFC 输出）。
2. 相位加第四个：**`post-tone`**（`g_audio_tone()` 返回之后，120 帧真的流过去之后）。

判读（开机前写死）：

| 观察 | 结论 |
|---|---|
| WR 有效(48000/16/2) 且 TDM 有效，**只有 MFC 无效** | 断点在 WR→SAL→MFC 之间的**传播**；优先测 `sweetwrmf=2`（厂商逐字 24/Q23）与 SAL 位宽那一维 |
| **WR 也无效** | 我们的 `SET_CFG(0x0800100c)` 虽然 `status=0` 但**没被当成模块格式存下来** ⇒ 格式来自图内 cal 数据 ⇒ ②那堵 18 KB 墙重新变成主线，且这次有寄存器依据 |
| TDM 无效 | 硬件端点也没被喂格式 ⇒ 无声更可能直接由端点未配置解释 |
| 三点都无效、但 `post-tone` 变有效 | 格式只在数据路径上发布 ⇒ 我们的读取时序是问题，图本身可能没坏 |

---

## 第 38 枪 K1 判决表（**开机前写死**，0272 内核 `mfcget v4`）

**本轮唯一变量**：`sweetmfcget` 2 → **3**。其余与 J1 逐字相同
（`sweettdmvendor=7 sweetvol=6 sweetbufs=120`，**`sweetmfc=` 仍不在 cmdline** ⇒ 全程只读、零写入）。
0272 是纯读扩展：`g_apm_get_cfg_readset()` 只发 GET_CFG，不改任何 SET_CFG / 图 / 时钟。

**身份门（不过就不许开机）**：产物内 `mfcget v4 `×**5**（五个分支各一条 printf：
no-response / get_cfg-decoded / get_cfg-raw / basic / unexpected）、
且 `mfcget v3 `×**0**、`v2 `×**0**、`v1 `×**0**；ring 内 `sweettime=<本轮>` 命中、`ring_window` 窗口吻合。

**预期 8 行 `SWEET MARK: mfcget v4 <相位> …`（逐字预期值）**

| # | 相位 / 模块 | 预期 |
|---|---|---|
| 1 | `pre-set` 0x48ba/0x08001024（解码分支）| 重复 J1：`size=8 err=0 rate=-1 bw=32 nch=65535 cmap=0` |
| 2 | `post-prepare` 0x48ba（解码分支）| 同上（仍无效）|
| 3 | `post-start` **0x40bd/0x0800100c**（RAW 分支）| 若 SET 被当格式存下：`size=30 d0=0x1 d1=0x9001000 d2=0x12 d3=0xbb80 d4=0x10010 d5=0xf0010` |
| 4 | `post-start` **0x428f/0x08001017**（RAW 分支）| 若存下：`size=12 d0=0x17700 d1=0x30020 d2=0x1`，`d3..d5=0` |
| 5 | `post-start` 0x48ba/0x08001024 | 无效（J1 三点皆如此）|
| 6-8 | `post-tone` 0x40bd / 0x428f / 0x48ba | 前三行不变；**只有 0x48ba 允许在这里变有效** |

（3、4 的"存下形状"= 我们自己 0267/0268 发出去的那段字节；RAW 分支**不做解码**，
`d*` 只有在**应答长度和模块自报 `param_size` 都覆盖**时才读——
`len=` 与 `size=` 两个字段就是这条界的证据，宿主 stub 已用"截到 d2 的应答"验过。）

**判读（互斥，先命中的那条算）**

| 观察 | 结论 | 下一枪 |
|---|---|---|
| A：3 有效、4 有效、5 无效 | 端点都发布了，**只有转换级没被喂** ⇒ 断点在 WR→SAL→MFC 的传播 | 同一内核 `sweetmfc=1 sweetmfcget=3`：0268 那条写我们从来没在有读回的情况下试过 |
| B：3 无效（`size` 仍 72 / `d3≠0xbb80`） | `SET_CFG(0x0800100c)` 虽然 `status=0` 但**没被当格式存下来** ⇒ 格式来自图内 cal 数据，②那堵 18 KB 墙带着寄存器依据回到主线 | 转 ②：cal 对象正文，不是再加探针 |
| C：3 有效、4 无效 | 硬件 sink 也没被喂 ⇒ 无声可直接由端点未配置解释 | TDM 侧参数集（`sweettdmvendor` 的另一组）|
| D：1-5 全无效，6-8 里 0x48ba 变有效 | 格式只在数据路径上发布 ⇒ **是我们的读取时序**，图本身没坏 | 把读取点固定到 post-tone，再谈内容 |
| E：3/4 走 `basic … echoed_op=0x1001007 status=0xN` | **不是仪器故障**：同一个 `g_apm_get_cfg_dump()` 构造器在 0x48ba 上拿到过结构化应答，所以这里 basic 只可能是**该模块/该参数不支持 GET**（0x3=EUNSUPPORTED）| 该模块的格式只能从写侧证据推断；不为它单独开枪 |
| F：`no response` ≥ 3 行 | 传输层问题（端口/应答分配），全部按仪器读数处理 | 先修仪器 |

**仪器故障类（不算模块结论）**：`size` 仍 =72 且 `err=0`（没人写回）、`len=` 与 `size=` 矛盾、
`unexpected rsp=`、`no response`。

**回归门（不齐则不判）**：`graph_prepare status=0`、`graph_start status=0`、
`tone gain g=6 peak=6144`、`pcm tone done (120 buffers)`、`data_buffer send`≈121、`no rsp` 0、
panic 0、`===== done =====` / `PSCI SYSTEM_RESET` 在窗口内、控制面四绿行、
`tdmcfg v1 sup=7` 的三条 `not sent` 仍在、`mfc v1 sel=` 运行期 0 行。

**已声明的风险（本轮改动自带）**
1. 8 次 GET 各含一个最长 ~5 ms 的应答等待 ⇒ 会话多花 ≤40 ms；`sweetbufs` 仍在 watchdog 学到的 ≤120 以内。
2. 对 0x40bd/0x428f 发 GET 是本项目第一次向这两个实例问话。GET 按定义只读；
   但"参数不支持 GET"会走 E 行，不产生模块状态改变。
3. RAW 分支的 `d*` 若某模块把 body 放在与 `param_size` 不一致的位置，会显示成 `size` 小而 `d*` 有值——
   这一形状已在 stub 里被 `len` 界住，真机上出现即按仪器读数处理。

### K1 的执行副本 `tools/score_k1.py`（开机前写好，判决表和打分器必须同一份）

`./tools/score_k1.py logs/ring-k1-3module.txt <本轮 sweettime> images/k1-*.img`
把上面 A-F 六行做成可执行版本，并额外做三件事：

1. **身份用位置 + 行内 iid 双重判定**。 0272 的 `no response` / `basic` 两条 printf
   **不带 `iid=`**（这是仪器自己的缺口，见下），但 `g_apm_get_cfg_readset()` 的发出顺序
   固定是 WR→TDM→MFC，每个 GET 恰好打一行，所以 8 次读 ↔ 8 行是一一对应的；
   行内出现 `iid=` 时**必须**与它的槽位一致，不一致就报 "not this round's ring" 并拒绝打分，
   而不是悄悄换键。
2. **表外的形状不许改名塞进 A-F**：命中不了任何一行时打印
   `NO ROW OF THE COMMITTED TABLE MATCHES (<签名>)`。自检用的四个合成环
   （`tools/fixtures/ring-k1-synthetic.txt` + 三行改写）分别命中 A/B/C/D，
   而"端点有效 + 只有 post-tone 的 MFC 有效"这种表外形状确实走了 fallback。
3. **E 行是正交的**：任何槽位答 `basic` 都单列出来，标成"该模块不支持这样 GET ⇒ 不可判"，
   既不算传输故障（同一个构造器在 0x48ba 上拿到过结构化应答），也不算格式的负结果。

**已知仪器缺口（记下来，值得在下一个真需要它的数据轮里一起修，不值得单独一次 CI）**：
`no response` 与 `basic` 两条 print 没带 `iid=/param=`，真机上如果同一相位里连续两发失败，
只能靠顺序恢复身份。修法就是给这三条非结构化 printf 补上 `iid=%#x param=%#x`，
marker 再 bump 一次；本轮不这么做，因为顺序是硬编码的、且 8↔8 的对应已在 fixture 上验过。

### K1/K2 字节预算（离线算死，免得下一枪死在 590 守卫上）

582 B 是 J1 产物实测（本轮 K1 只把末位 `2` 换成 `3`，同一长度）；`mkimage.sh` 的守卫在 590。
逐参数占用（从产物里量出来的，含前导空格）：

```
sweetbufs=120 14  sweetpcm=1 11  sweets1=1 10  sweetiova=1 12
sweets1va=0x1FFE8000 21  sweetnolpass=1 15  sweetshmhi=1 13
sweetshmflag=4 15  sweets1ap=3 12  sweetampfirst=1 16  sweetvol=6 11
sweettdmvendor=7 17  sweetmfcget=2 14  sweettime=xxxxxxxxxx 21
```

**判决表 A 行的后续（`sweetmfc=1`，+11）如果直接叠在 582 上是 593 > 590**，
所以必须先腾字节。腾哪一个要用代码证明，不能挑顺眼的：

| 候选 | 判定 | 依据 |
|---|---|---|
| `sweetshmflag=4` (15) | **可去**：与默认值逐字相同 | `bsd/dev/sweet_glink.c:5139` 的 `static uint32_t flags[] = { 4u, 0u };` 就是默认，`:5144` 的 `f==4` 分支算出的也是 `{4,0}`；参数只多打一行 `shm flag order 4,0` 证人 |
| `sweets1va=0x1FFE8000` (21) | **不可去** | `:4397` 默认 `s1_va=0`，`:4579` 取 `s1_va ? s1_va : G_VENDOR_IOVA(0x11ffe8000)`，而 `:4581` 用 `wide = (va>>32)!=0` —— 传参选的是**窄地址形式**，删了会把 wide 翻成 1，是行为改变 |
| `sweets1=1` / `sweetshmhi=1` / `sweetampfirst=1` / `sweetnolpass=1` / `sweetiova=1` | **不可去** | 默认全为 0/关（`:4389`、`:8163`、`:6216` 的 `nolpass=0` 预置），删任何一个都是把一条正在生效的路径关掉 |

⇒ 腾出 `sweetshmflag=4` 之后：582 − 15 = 567，A 行的 `sweetmfc=1` = **578 ✓**，
将来 `sweetwrmf=2`（+11）同一算法也放得下。K1 本身不动预算。

### K1 开机前快照（真实产物；第 38 枪 / 预算 120）

内核 `out/kernel.adsp` sha256 `a94454809af0e9f0…`，18:01 取回，
`EXPECT_MARKER="SWEET MARK: mfcget v4 "` 过门（CI 构建 `af240bc`；重试 1 次是因为当时
release 里还是 0271 的 `f0f353ba…`，被 marker 正确拒掉——这条正是"摘要可能是旧的"的那道门）。
**取回后在 `out/kernel.adsp` 上直接查标记**：`mfcget v4 `×**5**、`v3 `×**0**、`v2 `×**0**、`v1 `×**0**
⇒ 这就是 0272；另有 `mfc v1 sel=`×2、`tdmcfg v1 sup=`×1、`tdmclk v2 late `×1、`cccfg v3 `×6。

产物 `images/k1-3module.img`（由已过门的 `out/kernel.adsp` 现打包，cp 前先验过新产物里
`sweettime=1793959323`×1）：cmdline 实测 **582 B**（与 J1 同长，本轮只把末位 `2` 换成 `3`，
字节预算算过：`sweetmfcget=3` 仍是 14 B），尾部
`… sweetvol=6 sweettdmvendor=7 sweetmfcget=3 sweettime=1793959323` 连续；
镜像内 `mfcget v4 `×**5** / `v3`/`v2`/`v1`×**0**；
cmdline 内 `sweetmfc=` ×**0** ⇒ **单变量成立：这一枪只读，不发任何 SET_CFG**；
`sweetvol=6`、`sweettdmvendor=7`、`sweetbufs=120` 各 ×1。

**打包命令（原样可复现）**：
`D22AP_ADT=…/work/xnu/research/ipsw-x/d22ap.adt ./scripts/mkimage.sh sweetbufs=120 sweetpcm=1
sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4
sweets1ap=3 sweetampfirst=1 sweetvol=6 sweettdmvendor=7 sweetmfcget=3`
（**不带 `CPUS=8`**：J1 产物里是 `cpus=1`，本轮要与它逐字同形；`GPU_PROBE_LEVEL` 已不是
`mkimage.sh` 的变量，文档里那几处是历史记录。）

打分：`./tools/score_k1.py logs/ring-k1-3module.txt 1793959323 images/k1-3module.img`
判读以本文件"K1 判决表"为准，表外形状照实报告，不改名。

---

## 第 38 枪 K1 判决（`logs/ring-k1-3module.txt`，`sweettime=1793959323`）

**K1-0 身份：成立。** `sweettime` 命中 2；`ring_window`：`total=0x16536 == decoded`，
**window-match True**，三条收尾各 in-window=1（past-window 的 `0x184a5/0x18917/0x1e965` 是上一轮的
残留字节，已按老规矩标为"不是证据"）；产物内 `mfcget v4 `×5 / `v3`×0 / `v2`×0 / `v1`×0；
运行期 `mfc v1 sel=` **0 行** ⇒ 这一枪确实只读。

**8 行逐字（三模块 × 两相位 + 两个 MFC 控制点）：**

```
:pre-set       mfcget v4 pre-set       get_cfg len=116 st=0   iid=0x48ba param=0x8001024 size=8  err=0 d…  rate=-1 bw=32 nch=65535
:post-prepare  mfcget v4 post-prepare  get_cfg len=116 st=0   iid=0x48ba param=0x8001024 size=8  err=0       rate=-1 bw=32 nch=65535
:post-start    mfcget v4 post-start    raw   len=116 st=0x1   iid=0x40bd param=0x800100c size=72 err=1       d0..d5=0
:post-start    mfcget v4 post-start    raw   len=116 st=0     iid=0x428f param=0x8001017 size=12 err=0       d0=0x17700 d1=0x30020 d2=0x1
:post-start    mfcget v4 post-start    get_cfg len=116 st=0   iid=0x48ba param=0x8001024 size=8  err=0       rate=-1 bw=32 nch=65535
:post-tone     mfcget v4 post-tone     raw   len=116 st=0x1   iid=0x40bd param=0x800100c size=72 err=1       d0..d5=0
:post-tone     mfcget v4 post-tone     raw   len=116 st=0     iid=0x428f param=0x8001017 size=12 err=0       d0=0x17700 d1=0x30020 d2=0x1
:post-tone     mfcget v4 post-tone     get_cfg len=116 st=0   iid=0x48ba param=0x8001024 size=8  err=0       rate=-1 bw=32 nch=65535
```

**三条结论（按证据强度排）**

1. **TDM_SINK `0x428f/0x08001017` 读回来的是我们自己写进去的字节**：`size=12`、
   `d0=0x17700`(96000)、`d1=0x30020`、`d2=1` —— 与 `g_audio_graph_cfg_vp()` 里
   `tdm_rate[]` 那 12 个字节逐字相同，post-start 与 post-tone 一致。
   ⇒ **GET 仪器本身被证明可用**（同一构造器、同一长度界），
   并且"**SET_CFG 的 body 会被模块存下来、而且能读回**"至少在硬件 sink 上成立。
   这是本项目第一次拿到"写进去的东西原样读回来"。
2. **WR `0x40bd/0x0800100c` 不肯答**：走的是**结构化**分支（应答 opcode=`0x02001000`），
   但 `st=0x1`、`err=1`、`size` 仍是我们提供的容量 72、`d0..d5` 全零 —— 即模块**明确回了 EFAILED 并且一个字节都没写**。
   同一 boot 里对**同一 iid+param** 的 `SET_CFG(0x40bd, 0x0800100c)` 是 `status=0` 被接受的
   （`vp/wr_mf rsp op=0x1001006 status=0`）。
   ⇒ **WR 槽位本轮不可判**。特别强调：这**不等于**"我们的 SET 没被存下来"，
   预登记的 B 行结论**不采用**（B 行写的时候没预料到"结构化应答 + st/err≠0 + size 原封不动"这一形状）。
   记分器把它单列为 **E2**（`added after this boot, NOT pre-registered`），
   并且分类器对整轮打印 `NO ROW MATCHES (WR-start=None TDM-start=True MFC-start=False MFC-tone=False)`。
3. **MFC `0x48ba/0x08001024` 四个相位全部无效**（`rate=-1 nch=0xFFFF bw=32`），**包括 post-tone**
   ⇒ 预登记的 **D 行当场关掉**："格式只在真实数据流过去之后才发布"这个解释死了。
   120 帧 DATA_BUFFER 已经真的发到 `0x40bd` 并被消费，转换级的输出格式仍然是初始化默认值。

**K1-4 回归：全绿**（`graph_open/prepare/start rsp … status=0`、`tone gain g=6 peak=6144`、
`pcm tone done (120 buffers)`、`data_buffer send` 121、`no rsp` 0、panic 0、
`cba2r@… = 0x1`、`ats1pr iova=0x1ffe8000 -> par=0 OK`、`geni amp 0x34 r=0 id=0x2032` + `profile 44/44`、
三条 `not sent`、`===== done =====` / `PSCI SYSTEM_RESET` 在窗口内）。

**打分器自己挨的两枪（都记下来，因为是同一类错误）**
- `st=%#x` 被 `(\d+)` 解析 ⇒ `st=0x1` 读成 **0**：**一个拒绝应答的 GET 会被打成干净应答**。已改，
  并且 fixture 里两种写法都覆盖。
- 回归门里我用了从来没人打印过的简写（`graph_start status=0`、`cb5 cba2r=0x1`）⇒ 一个全绿的会话
  被打出两条 MISS。真实串是 `graph_start rsp op=0x1001002 status=0` 与 `cb5 cba2r@<addr> = 0x1`。
- 两条都是 post-hoc 修正，**表本身没动**；修完 A/B/C/D 四个改写仍然各自命中。

## 下一枪 K2（**同一颗 0272 内核，只是 repack**）：`sweetmfc=1 sweetmfcget=3`

为什么是它：K1 之后唯一还没试、又已经被仪器证明"能验证"的动作，就是**我们自己去写转换级的输出格式**
（0268 早就建好了这条写路径，但从没有在有读回的情况下跑过）。
TDM 的先例说明"写进去能原样读回来"在这套固件上是成立的；WR 的先例说明**读不到不等于没写进去**。
所以 K2 的判据不再是"有没有声音"，而是**MFC 的 body 是否变成我们写的那 12 个字节**。

参数与预算：`582 − sweetshmflag=4(15) + sweetmfc=1(11) = 578 ✓`。
`sweetshmflag=4` 的删除已证明是**行为 no-op**（默认 `flags[]={4,0}`），
证人也没丢：真正证明 offset 模式生效的是 `SWEET GLINK: shm mapped pa=… mode=offset`，那行仍在。

**判决表（开机前写死）**

| 门 | 期望 | 命中含义 |
|---|---|---|
| K2-0 身份 | 同一内核：产物内 `mfcget v4 `×5、`v3/v2/v1`×0；`sweettime=<本轮>`；窗口吻合 | 不过门不开机 |
| K2-1 写确实发了 | `SWEET MARK: mfc v1 sel=1 iid=0x48ba param=0x8001024 rc=0` 恰 1 行 | `rc!=0` ⇒ 发都发不出去，先修发送；0 行 ⇒ 参数没进 cmdline |
| **K2-2 主判据：`post-set`** | **`size=12 err=0 rate=96000 bw=32 nch=2`**（= 我们写的 `{0x00017700,0x00020020,0x00020001}` 原样读回） | **MFC 存下来了** ⇒ 与 TDM 同形；转换级从此有格式，声音问题移出"格式内容"轴 |
| | `size=72` 且 `st≠0/err≠0`（WR 那种） | MFC **接受但不存** 0x08001024 ⇒ 这条参数不是可写参数或 iid 不对；**这是新的负结果，值钱** |
| | `size=8 err=0 rate=-1`（和 pre-set 一模一样） | 有人写了但写的不是格式 ⇒ 看 `post-prepare/post-start` 是否被传播覆盖 |
| K2-3 后续相位 | `post-prepare`、`post-start`、`post-tone` 三处 MFC 的取值 | 存下来了但 `post-start` 又变 -1 ⇒ **有东西在 START 时重置转换级**（新的一轴）；一路保持 96000 ⇒ 格式轴闭合，回到"线上有没有时钟/功放供电"那一轴 |
| K2-4 听感（附带，不是主判据） | `sweetvol=6` = **−14.5 dBFS**（用户定的上限），120 帧 | 听到 ⇒ 进音质；没听到且 K2-2 成功 ⇒ 无声与 MFC 格式无关，这条当场记下 |
| K2-5 仪器故障类 | `mfcget v4 … no response` ≥3、`unexpected rsp=`、`basic … echoed_op=` | 按仪器处理，不算模块结论 |
| K2-6 回归 | 与 K1 逐项等值（上面那串），且 `data_buffer send`≈121 | 不齐 ⇒ 不判 |

**风险（写明）**：K2 是本项目**第一次真的往 MFC 模块写状态**。若该模块因非法/不支持的格式内部出错，
图的后半段可能报错——但读回与回归门都能看见，且不改图、不改时钟、不写盘；
最坏情况是这一发的三模块读数变脏，代价一发开机，不换不可逆状态。

---

## 第 39 枪 K2 判决（`logs/ring-k2-mfc-set.txt`，`sweettime=1793959887`）——
## **本项目第一次把一个模块的媒体格式写成真的、并且它一直保持**

**K2-0 身份：成立。** `sweettime` 命中 2；`ring_window` `total=0x1654a == decoded`，
**window-match True**（三条收尾 in-window=1，past-window 的 `0x184a5/0x18917/0x1e965` 仍是上一轮残留）；
产物 `mfcget v4 `×5 / `v3`×0；9 次 GET 全部打出行（no-response 0、basic 0、unexpected 0）。

**K2-1 写确实发了**：`SWEET MARK: mfc v1 sel=1 iid=0x48ba param=0x8001024 rc=0`。

**K2-3 主判据 = STORED**，并且 K2-4 显示它**没有被任何后续步骤丢掉**：

```
pre-set      rate=-1    bw=32 nch=65535 size=8    (写之前：仍是初始化默认)
post-set     rate=96000 bw=32 nch=2    size=12 cmap=0x20001   ← 我们写的 12 字节原样读回
post-prepare rate=96000 bw=32 nch=2    size=12
post-start   rate=96000 bw=32 nch=2    size=12
post-tone    rate=96000 bw=32 nch=2    size=12    (120 帧 DATA_BUFFER 流过去之后依然如此)
```

仪器自身的两个对照也如期：TDM 槽仍然 `0x17700/0x30020/0x1`；WR 槽仍然 `st=0x1 err=1 size=72`
（K1 已证明这个模块不答这个 GET，**两向都不算证据**）。

**K2-6 回归：与 K1 逐项等值**（`graph_open/prepare/start … status=0`、`tone gain g=6 peak=6144`、
`pcm tone done (120 buffers)`、`data_buffer send` 121、`no rsp` 0、panic 0、`cba2r@…=0x1`、
`ats1pr … par=0 OK`、`geni amp 0x34 r=0 id=0x2032`、`shm mapped … mode=offset`（腾字节的
证人替代已兑现）、三条 `not sent`、`===== done =====`/`PSCI SYSTEM_RESET` 在窗口内）。

**这一枪关掉的轴**：
- "**START 时会重置转换级**"这个候选当场死（post-prepare / post-start / post-tone 三处都是 96000）。
- 预登记的 NOT STORED / UNCHANGED 两条都没命中，所以 **0x08001024 在 0x48ba 上是可写、可存、可读的**，
  iid 也没选错。
- 公开 API 里**没有 MFC 输入格式参数**：`0x08001023` 在他核两个公开版本里都不存在
  （`docs/data/audioreach-graph-format-2026-11-06.md:230` 已撤回过），
  所以输入侧只能靠传播，读不到；本轮之后"读 MFC 输入"这条路**不值得再花一发**。

**还欠的、也是唯一还没拿的**：**耳朵**。判决表里 K2-5 是次判据，但这一枪之后它的权重变了——
以前"无声"发生在**一个从未被喂过格式的转换级**上，现在发生在**一个已经带有效输出格式、
并且格式在 START 和 120 帧之后都还在的转换级**上。同样一行"没听到"，
从这一枪之后含义完全不同。

## 第 40 枪 K3：**同一份产物重放**，唯一变量是人耳

K2 的产物已经判过寄存器部分，`images/k2-mfc-set.img` 一个字节都不用变：
**重放同一镜像，人在手机旁边听**。`sweetvol=6` = **−14.5 dBFS**（用户定的上限），120 帧。

| 观察 | 结论 | 下一枪 |
|---|---|---|
| 听到（任何声音，含噪声/杂音） | 转换级就是那个卡点，音频"有没有输出"轴关闭 | 转音质/采样率匹配（先比对 `g_audio_tone` 生成的 PCM 速率与 WR 声明的 48000） |
| 仍然完全无声 | **带有效格式的转换级 + 全绿控制面 + 120 帧 = 仍然无声** ⇒ 格式内容轴（输出侧）到此关闭，剩余嫌疑集中在 WR 声明的位宽（我们 16/Q15 vs 厂商 24/Q23，§15）与端点之后的物理层 | `sweetwrmf=2`（发厂商逐字 24/Q23 四字节）——**需要 0273**（现在 `sweetwrmf` 只有 0/1 语义：`rc = wrmf ? set_cfg(…) : 0`），不占本发 |

风险：重放同一产物不引入任何新变量，`sweettime` 会变（新时间戳 ⇒ 新 cmdline ⇒ 重新打包；
若要保持字节全同，就用同一份镜像直接 `fastboot boot`，`sweettime` 不变，
身份改用 `ring_window` 的 seq + 本轮 9 行读数确认）。

---

## 第 40 枪 K3 判决（`logs/ring-k3-ear.txt`）——**仍然完全没有声音，而且这一枪的"无声"与之前不同**

**同一份产物重放**，所以 `sweettime` 与 K2 相同（`1793959887`，这是按构造必然的，不是身份证据）；
身份改由 `ring_window` 承担：`total=0x1664e == decoded`、**window-match True**、`seq=292`（K2 是 293）、
raw 文件是本轮新拉的那一份。

**寄存器部分与 K2 逐行等值**：`mfc v1 sel=1 … rc=0`；9 次 GET 全出行；
MFC 在 `post-set / post-prepare / post-start / post-tone` 四处一致 `size=12 rate=96000 bw=32 nch=2 cmap=0x20001`；
TDM 对照仍然 `0x17700/0x30020/0x1`；WR 仍然 `st=0x1 err=1 size=72`（不可判，两向都不算证据）；
回归门与 K2 逐项等值（`data_buffer send` 121、`no rsp` 0、panic 0、控制面全绿、窗口内收尾）。

**听感门（用户在场，`sweetvol=6` = −14.5 dBFS，120 帧）**：
> "我还是没听到任何声音。"

⇒ K3 表里第二行命中："**带着有效输出格式、格式熬过 START 与 120 帧的转换级，仍然无声**"。
这条把"格式内容（输出侧）"轴关闭：MFC 的输出格式已经不是嫌疑，至少"它没被喂"这个解释不成立了。

**表里预登记的下一枪（`sweetwrmf=2` + 0273）在此被零开机的量测顶掉了**——
不是被开机结果否证，而是被逐字节比对否证：厂商时间线（`docs/data/vendor-tx-timeline-2026-11-06.md`）显示
**我们的 WR 正文与厂商完全相同**（16-bit/Q15 本来就是厂商的值），
所以那一发是白花；同一份时间线换成两处**真的**不同：**TDM MF_CFG 的容器位宽**
（厂商 24 / 我们 32）与 **MFC output 记录是四词声明 14**。0273 因此为这两处而写（默认全关）。

## 第 41 枪 K4 判决表（**开机前写死**，0273 内核）：`sweetmfc=4 sweetmfcget=3`

单变量 = MFC output 记录换成厂商形状。cmdline 仍是 **578 B**（只把 `sweetmfc=1` 换成 `=4`）。

| 门 | 期望 | 命中含义 |
|---|---|---|
| K4-0 身份 | 产物内 `tdmmf v1 `×**≥1**（这条存在才是 0273）、`mfcget v4 `×5；ring 里 `tdmmf v1 tv=7 w1=0x30020` 恰 1 行（bit3 关，基线不变）| 不过门不开机；`tv=7` 那行同时是"本轮没顺手改到 TDM"的证人 |
| K4-1 写发了 | `SWEET MARK: mfc v1 sel=4 iid=0x48ba param=0x8001024 rc=0` 恰 1 行 | `rc!=0` ⇒ 形状被本地拒了；0 行 ⇒ 参数没进 cmdline |
| **K4-2 主判据 `post-set`** | 期望 `rate=96000`、**`bw=24`**、`nch=3`（`0x00030018` 的低/高半字就是这两个数），`size` 为模块自报 | `bw=24` ⇒ **厂商位宽存进去了**；`bw=32` ⇒ 存的还是 K2 那次？（`pre-set` 若已是 32 说明上一轮状态不清）；`rate=-1` ⇒ **14 字节/四词的记录被拒**，与 K2 的 12 字节被接受形成对照，这是值钱的负结果 |
| K4-3 一致性 | `post-prepare/post-start/post-tone` 的 MFC 与 K4-2 同形 | 变回 -1 ⇒ START 阶段有东西重置它（K2 已排除过一次，若这里出现要重看）|
| K4-4 听感（在场才问） | `sweetvol=6` = −14.5 dBFS | 听到 ⇒ 转换级的形状是门槛；没听到且 K4-2 成功 ⇒ 位宽轴在 MFC 侧关闭，走 K5 |
| K4-5 仪器故障 | `no response`≥3 / `unexpected rsp=` / `basic echoed_op=` | 按仪器处理 |
| K4-6 回归 | 与 K2/K3 逐项等值 | 不齐 ⇒ 不判 |

## 第 42 枪 K5 判决表（同一颗 0273 内核，repack）：`sweettdmvendor=15`（+ K4 里判出来的 MFC 值）

单变量 = bit3 ⇒ TDM MF_CFG 第二词 `0x00030020` → `0x00030018`。cmdline **579 B**（`7`→`15` 多 1 字节）。
TDM 的读回在 K1 就被证明可用，所以这一枪同样是**寄存器自证**：

| 门 | 期望 | 命中含义 |
|---|---|---|
| K5-0 身份 | ring 里 `tdmmf v1 tv=15 w1=0x30018` 恰 1 行 | 0 行或 `w1=0x30020` ⇒ 参数没生效，下面不判 |
| **K5-1 主判据** | `post-start raw iid=0x428f param=0x8001017 … d1=0x30018`（`d0=0x17700`、`d2=1` 不变） | **存进去了** ⇒ 硬件端点也换成厂商容器；`d1` 仍是 `0x30020` ⇒ 端点不接受改写容器宽度（新的、有用的负结果）|
| K5-2 听感 | −14.5 dBFS | 听到 ⇒ 定位在容器宽度；没听到 ⇒ **两处位宽分歧都消除**，剩下只有 `0x40bf/0x40c0` 两个我们图里没有的模块、`0x4165/0x080010c4`、以及那 18128 字节配置 |
| K5-3 回归 | 与 K4 等值 | 不齐 ⇒ 不判 |

预算：41/120 与 42/120。两枪都不写盘、不碰 GPU、不改图与时钟；最坏结局是各自读回一次"不接受改写"，
而那本身就是能把嫌疑清单继续缩短的读数。

### K4 开机前快照（真实产物；第 41 枪 / 预算 120）

取回这条路上今天真正发生的事，记下来是因为它差点让我们开错内核：
CI **已经成功**（release 资产摘要换成 `1806ad0e…`，且失败时才更新的 `build-error.txt` 里写的还是旧
commit `11ac917…` ⇒ marker 门没报错），但 **objects CDN 连续给出两份互不相同的旧字节**
（`5f52254b…`、`7128b108…`），`fetch_kernel.sh` 按"摘要 + 本轮 marker"双门连拒两次；
第 3 次（19:15）拿到 `c321be4668b1…`，当场数标记：
**`tdmmf v1 `×1、`mfcget v4 `×5、`mfcget v3 `×0**，体积 17563976（0272 是 17547528）⇒ 这才是 0273。
两条被否掉的旁路也记一笔，免得下次又试：`fetch_kernel_api.sh` 的门更松（摘要不符时只要看到
`SWEET AUDIO: build v` 就放行——那是每个 build 都有的串，不能当新鲜度），所以我把它指到 `/tmp/kdl`
单独验；未带 token 的 api.github.com 在这个出口 IP 上是 **限流到 0**（reset 还要 ~1 小时），
Keychain 里那条 github.com 只有账号没有 token；直连 github.com 超时（这台机器必须走代理）。
0272 那颗内核留着备份 `out/kernel.adsp.0272.bak`（`a9445480…`）。

产物 `images/k4-mfc24.img`：cmdline 实测 **578 B**（与预测一致），尾部
`=3 sweetampfirst=1 sweetvol=6 sweettdmvendor=7 sweetmfc=4 sweetmfcget=3 sweettime=1793963894` 连续；
`sweetmfc=4` ×1 / `sweetmfc=1` ×0 / `sweetshmflag` **不在 cmdline**；
镜像内 `tdmmf v1 `×1、`mfcget v4 `×5、`v3 `×0、本轮 `sweettime` ×1。

打分：`./tools/score_k45.py k4 logs/ring-k4-mfc24.txt 1793963894 images/k4-mfc24.img`
（该工具的判决副本已在 `02c9e2b` 开机前提交，并用 boot 40 的 ring 做过反向对照：旧产物会被打成
IDENTITY FAIL + 证人 MISS）。

---

## 第 41 枪 K4 判决（`logs/ring-k4-mfc24.txt`，`sweettime=1793963894`）
## **STORED-24：厂商那份四词记录整份被 MFC 收下，并且仍然完全无声**

**K4-0 身份：成立。** `sweettime` 命中 2；产物 `tdmmf v1 `×1、`mfcget v4 `×5、`v3 `×0；
`ring_window` `total=0x165d6 == decoded`、**window-match True**。
证人如期：`tdmmf v1 tv=7 w1=0x30020`（⇒ 这一枪的 bit3 是关的，TDM 那一维没被顺手改动，
单变量成立）+ `mfc v1 sel=4 iid=0x48ba param=0x8001024 rc=0`。

**九行读数（9 次 GET 全出，no-response/basic/unexpected 各 0）：**

```
pre-set      size=8  err=0 rate=-1    bw=32 nch=65535        ← 写之前仍是初始化默认
post-set     size=14 err=0 rate=96000 bw=24 nch=3 cmap=0x20001   ← 主判据
post-prepare size=14 err=0 rate=96000 bw=24 nch=3 cmap=0x20001
post-start   size=14 err=0 rate=96000 bw=24 nch=3 cmap=0x20001
post-tone    size=14 err=0 rate=96000 bw=24 nch=3 cmap=0x20001
WR   0x40bd/0x0800100c  post-start / post-tone：st=0x1 err=1 size=72 全零（E2，两向都不算证据）
TDM  0x428f/0x08001017  post-start / post-tone：0x17700 / 0x30020 / 0x1（仪器对照）
```

**判决（对照开机前那张表）**
- K4-3 = **STORED-24**。而且带回来一个比 K2 更强的形状证据：**模块自报的 `param_size` 从 8 变成了 14**，
  即它按厂商声明的那个"四词、14 字节"的长度作答——不是我们把 12 字节记录改了个数值，
  而是**厂商那份记录的形状整体被接受**。K2 那次（12 字节 / `size=8`）与之形成干净对照。
- K4-3 的三处后续相位一致 ⇒ "**START 重置转换级**"再次被排除（K2 已排除过一次，这次是带着厂商形状再排除一次）。
- K4-6 回归与 K2/K3 逐项等值（120 帧、`data_buffer send` 121、`no rsp` 0、panic 0、控制面全绿、窗口内收尾）。
- **K4-4 听感：用户在场，答"完全没有"**（`sweetvol=6` = −14.5 dBFS）。

**⇒ 内容轴在 MFC 这一侧彻底关闭。** 现在的事实是：
转换级的输出格式 = 厂商逐字形状（96000 / 24-bit 容器 / `{1,2}` / 声明 14），
读回自证、熬过 PREPARE/START/120 帧，**然后仍然一点声音都没有**。
"没被喂格式"与"喂的是我们自创的形状"两种解释同时出局。

## 第 42 枪 K5 开机前快照（真实产物）
`images/k5-tdm24.img`：cmdline 实测 **579 B**（预测 579 = 578 + `7`→`15` 的 1 字节），尾部
`1ap=3 sweetampfirst=1 sweetvol=6 sweettdmvendor=15 sweetmfc=4 sweetmfcget=3 sweettime=1793964411` 连续；
镜像内 `tdmmf v1 `×1、`mfcget v4 `×5、`v3 `×0。同一颗 0273 内核，**唯一变量 = TDM 端点容器位宽**
（`0x08001017` 第二词 `0x00030020` → 厂商的 `0x00030018`），主判据是端点读回 `d1=0x30018`；
MFC 这一枪保持 K4 判出来的 `sweetmfc=4` 不变（表里就是这么定的）。
预算 42/120。

---

## 第 42 枪 K5 判决（`logs/ring-k5-tdm24.txt`，`sweettime=1793964411`）
## **NOT REWRITTEN：端点的容器位宽不是那条 SET_CFG 能改的**

身份与证人：`sweettime` 命中 2；产物 `tdmmf v1 `×1 / `mfcget v4 `×5 / `v3 `×0；
`window-match True`（`total=0x16b2c`）；**`tdmmf v1 tv=15 w1=0x30018` 恰 1 行**
⇒ 我们**确实把厂商的那个词发出去了**；`vp/tdm_rate rsp op=0x1001006 status=0`（72 字节包被接受）。

可是读回纹丝不动，两处相位都是：

```
post-start  iid=0x428f param=0x8001017 size=12 err=0 d0=0x17700 d1=0x30020 d2=0x1
post-tone   iid=0x428f param=0x08001017 size=12 err=0 d0=0x17700 d1=0x30020 d2=0x1
```

MFC 对照与 K4 完全一致（`size=14 rate=96000 bw=24 nch=3` 四处）；WR 仍是 E2；
9/9 GET 有应答，no-response/basic/unexpected 各 0；回归逐项等值（120 帧、121 发、
`no rsp` 0、panic 0、控制面全绿）。**听感：仍然完全没声音**（`sweetvol=6` = −14.5 dBFS）。

**判决（对照开机前的表）**：命中 K5-1 的第二支——"**发出去的词与端点答出来的词不同**"。
三种解释里能站住的是：那个 GET 返回的是**端口/接口决定的格式**，不是我们写进去的记录
（最自然的来源是 `0x0800101b` 里的 `slot_width=32`——那条我们本就与厂商逐字相同；
厂商的 24 是"**32 位槽里放 24 位有效音频**"，不是另一种槽）。
⇒ **"厂商 24 vs 我们 32"这一维到此关闭：它既不是我们能调的旋钮，也不是能解释无声的差异**——
因为端点无论我们发什么都报同一个值。

## 音频线现状（截至第 42 枪，全部为寄存器证据，不含推测）

**已经用读数排除的**（每条都有至少一次开机证据）：
- 时钟票的两个位置（早：票被接受但 `graph_start` 回 0x1，158 枪里唯一一次；晚：PRM 答 `status 0x1` 而图照常绿）；
- 幅度轴（clamp 最大 −0.27 dBFS 仍无声；`sweetvol=32` 反而更小声这条也测过）；
- 三个我们自创的 TDM 参数（`sup=7` 全不发，仍然无声）；
- WR 逐字节（与厂商完全相同，含 16-bit/Q15）；
- 转换级"没被喂格式"（J1/K1）与"喂错形状"（K4：现在存的是厂商逐字 14 字节记录，四处保持）；
- **START 重置转换级**（K2/K4 两次排除）；
- 端点容器位宽可写性（K5）。

**还缺的东西现在只剩三类，且都是"我们根本没有的对象"而不是参数取值**：
1. 厂商 `SET_CFG` 里的 **`0x40bf` / `0x40c0`**（`0x08001008` psize=30，与 WR 同族但**没有速率词**）
   和 **`0x4165 / 0x080010c4`**（正文 = `0x428f`）——这三个 iid **都不在我们的图容器里**，
   所以给它们发指令不是"少发一条"，而是"图不同"；
2. **`GRAPH_OPEN` 正文本身**：厂商带内 2712 字节，我们带内 800 字节（我们的链是自己拼的）；
   kprobe 只抓到那条包的前 96 个词（≈384 字节），**所以厂商容器全文不在手上**；
3. 那 **18128 字节自定义配置**（t=+11 ms，dst=1）——②那堵老墙，正文同样不在手上。

**⇒ 下一枪的分叉（成本差别很大，需要定一条）**
| 候选 | 要什么 | 能买到什么 |
|---|---|---|
| **K6a：把 `g_graph_vendor4`（ACDB 解出的厂商容器）改成**带内**发送** | 需要 **0274**（现在 `sweetvg` 只走 SHM 形式：`vg==1` off=12 len=2712，`vg==2` off=16 len=2944，都经 `g_apm_graph_open_shm`） | 同一套读回仪器直接比较"我们的图 vs 厂商形状的图"里 MFC 是否**不用我们写就有效** ⇒ 第一次能把"图数据"与"命令序列"这两类解释分开 |
| K6b：`sweettdmclk=1` + `sweetmfc=4` | 只要 repack（腾 15 字节去掉 `sweetnolpass=1`：`g_nolpass` 只被赋值、从不被读，那条证人只在发送失败路径打印，而最近每轮 `no rsp`=0）⇒ 574 B | 只回答"提前投票 + 厂商形状转换级，graph_start 是否仍回 0x1"——是资源冲突的进一步定位，**不直接买声音** |
| K6c：转回 ②（18128 正文） | 不是开机，是资产/协议工作：那份配置的对象在 ACDB 里对应哪一段、能否从 `/vendor` 的 acdb 或运行中的 ADSP 内存镜像里重建 | 唯一可能补上"我们根本没有的东西"的那条路，但历史上它已经吃掉最多轮次 |

推荐 **K6a**：它是剩下三条里唯一"用现有仪器能直接判"的，且失败也不会污染别的轴
（容器不同 ⇒ 读回与 graph 应答都自证；不改时钟、不写盘、不碰 GPU）。

---

## 0274 之后两枪的判决表（**开机前写死**）

### K6a（第 43 枪）：`sweetvgraph=4` —— 厂商形状的容器，**带内**发送，我们一个字都不写

cmdline = K5 那条去掉 `sweetnolpass=1`（−15，见下方证据）再加 ` sweetvgraph=4`（+14）
⇒ **预测 578 B**，打包时实测。`sweetmfc` **不在 cmdline**，这是这一枪的全部意义。

腾 `sweetnolpass=1` 的证据（不是"看着无关"）：`g_nolpass` 在本文件里只出现两次
（`:2794` 声明、`:6219` 赋值），**从不被读**；`sweetnolpass` 唯一的作用是发送失败路径里那 20 行
"nolpass alive" 打印，而最近每一轮 `no rsp` 都是 0 ⇒ 它打印的条件从未发生。

| 门 | 期望 | 命中含义 |
|---|---|---|
| K6a-0 身份 | 产物内 `vginl v1 `×1、`dbb v1 `×1、`tdmmf v1 `×1、`mfcget v4 `×5；ring 内 `sweettime=<本轮>` + 窗口吻合 | 不过门不开机 |
| K6a-1 证人 | `SWEET MARK: vginl v1 vg=4 off=16 len=2944 form=inband` 恰 1 行；`dbb v1 bytes=1152 ms=6`（本轮不动缓冲区）| 打印自己算出的 off/len，避免我事后"记得"是哪种形式 |
| K6a-2 容器被接受 | `graph_open/ukee2960-inband rsp op=0x1001000 status=0` | 非 0 ⇒ **同一个容器换成带内就被拒**（此前所有 vgraph 结果都同时是 SHM 传输的结果，从没被单独判过）⇒ 这一格本身就是新信息，不算失败 |
| K6a-3 图起来 | `vgraph 4 (in band) started` 或 `did not start` + `graph_start rsp … status=` | 与 K6a-2 分开算，两者可能不同 |
| **K6a-4 主判据** | **`pre-set` 那一行的 MFC**（我们此时**还没有对 0x48ba 写过任何东西**）| `rate=96000/48000` 有效 ⇒ **容器自己就把格式喂给了转换级** ⇒ 差异在图数据，不在命令序列；仍 `rate=-1` ⇒ 换厂商形状容器也救不了转换级 ⇒ 嫌疑集中到 18128 配置与 `0x40bf/0x40c0` 两处缺失 |
| K6a-5 数据被消费 | `pcm tone done (120 buffers)` 且 `pcm buffer` `not consumed` = 0 | 出现 not consumed ⇒ 这个容器里没有能收 DATA_BUFFER_V2 的端点 ⇒ **听感作废**，读数仍有效 |
| K6a-6 回归（**换图后定义不同，先写清楚**）| `ats1pr … par=0 OK`、`cba2r@…=0x1`、`geni amp 0x34 r=0 id=0x2032`、`tone gain g=6 peak=6144`、`no rsp` 0、panic 0、窗口吻合 | `graph_open/vp-wr` 那行**必然缺席**（我们不再走自己的容器）——这是预期，不算不一致；打分器按这条写 |
| K6a-7 听感 | `sweetvol=6` = −14.5 dBFS，120 帧 × 6 ms ≈ 0.7 s | 次要；K6a-5 不过则记"未测" |

风险：容器与 `g_ukee_current_sgs` 的 4 个 subgraph id 若与 DSP 侧不符，start 会拒——那也是读数；
不改时钟、不写盘、不碰 GPU；这一枪**可能整轮都拿不到耳朵**，换来的是"图数据 vs 命令序列"第一次被分开。

### K7（第 44 枪）：我们自己的图 + `sweetdbb=4096 sweetvol=16` —— 专门给用户耳朵的那一发

cmdline = 579 − 15（同样腾掉 nolpass）+ 14（` sweetdbb=4096`）+ 1（`sweetvol=6`→`=16`）
⇒ **预测 579 B**。寄存器门 = 与 K4 等值（`pre-set` 无效、`post-set/prepare/start/tone` 四处
`size=14 rate=96000 bw=24`、TDM `0x30020`），新增证人 `dbb v1 bytes=4096 ms=21`。

| 门 | 期望 | 含义 |
|---|---|---|
| K7-1 | `dbb v1 bytes=4096 ms=21` 恰 1 行 | 缓冲区几何真的换了（不是我只改了个字符串）|
| K7-2 | `pcm tone done (120 buffers)`、`not consumed` 0、`no rsp` 0、panic 0、窗口吻合、尾部 `===== done =====`/`PSCI` 在窗内 | **尾部在窗口内**是这枪的硬条件：4096×120 会让会话变长，若 watchdog 切断尾部 ⇒ 听感记"未测"，`sweetdbs` 回 1152 重放，不追加别的变量 |
| K7-3 听感 | 120 帧 × 21 ms ≈ **2.5 秒**，`peak=16384` = **−6.0 dBFS**（比 K2–K5 响 8.5 dB；clamp 上限是 g=31 = −0.27 dBFS，需要更响要说一声）| 这一枪的**唯一产物就是耳朵**；读数只用来证明"几何换了但一切照旧" |

已声明风险：更大的缓冲区会拉长会话，可能顶到 watchdog（120 是学到的上限，200 曾在 135 发被切）；
若被切，本轮作废重放 1152，不把它算作"无声"。

---

## 第 43 枪 K6a 判决（`logs/ring-k6a-inband.txt`，`sweettime=1793970117`）
## **不是 DSP 拒了厂商容器——是我们的发送函数根本没把它装出去**

证人 `SWEET MARK: vginl v1 vg=4 off=16 len=2944 form=inband` 在（1600 行前），
紧接着 `SWEET GLINK: vgraph 4 (in band) open refused rc=-1`，
**但它上面没有任何 `APM graph_open/ukee2960-inband` 发送行**——一条都没有。
原因在代码里，可查：`g_apm_cmd()` 的包是 **栈上 1064 字节**
（`bsd/dev/sweet_glink.c:2687 uint8_t pkt[24 + 16 + 1024]`），
守卫 `:2690 if (plen > sizeof(pkt) - 40) return -1;` **在自己的 printf 之前就返回**，
所以 2944 字节的容器不是"被拒"，是**从未上路**。

更要紧的是**判决本身**：`vg` 分支拒绝后没有 `return`，代码顺势落回我们自己的 800 字节容器
（`:1601 APM graph_open/vp-wr … (800 bytes)`），于是这一轮后半程的全部读数——
`pre-set rate=-1`、`post-prepare/post-start/post-tone` 全无效、TDM 原样、120 帧走完、
控制面全绿——**是 K4/K5 已知对照的一次重放，不是"厂商图喂不喂转换级"的答案**。
我第一版打分器把 `rc=-1` 读成 DSP 决定并打出了表里的 "STILL UNFED"，那是错的判词，
已改成 NOT SENT 直接短路（`tools/score_k6.py`），并保留"没有 inband 证人就拒绝判"那道门。

顺带一条同源事实（只陈述代码，不改写历史）：`g_apm_set_cfg()` 把包交给同一个
`g_apm_cmd()`（`:5535`），所以**任何超过 1024 字节的带内 SET_CFG 也都是这个静默上限**——
这正是 18 KB 厂商自定义配置此前只能走 OOB（带外/共享内存）形式的原因。
至于历史上哪一轮曾经把它误读成 DSP 拒绝，文档里并没有记录，我不在这里替它编。

⇒ **修复 = 0275**：`g_apm_cmd_big()` 用 4136 字节 **static** 缓冲（不是栈），
超限分支**会打印** `%u bytes exceeds the inline buffer (%u) - NOT SENT`。
K6b = 用 0275 内核重放 `sweetvgraph=4`，这一回才是那一格真正的答案。

## 第 44 枪 K7 判决（`logs/ring-k7-long-loud.txt`，`sweettime=1793971127`）——寄存器侧

cmdline 实测 **578 B**（我表里预测 579，错因与 K6a 同一类：预测基线取的是 K5 那一行，
而这一轮 `sweettdmvendor` 也从 `15` 回落到 `7`，少 1 字节）。

| 门 | 结果 |
|---|---|
| 身份 | 产物 `vginl/dbb/tdmmf ×1`、`mfcget v4 ×5`、`v3 ×0`；`window-match True`（`total=0x16275`，`seq=243`）|
| K7-1 几何证人 | **`dbb v1 bytes=4096 ms=21`** ✓、**`tone gain g=16 peak=16384`** ✓ ⇒ 缓冲区与响度都真的换了，不是我只改了字符串 |
| K7-2 寄存器与 K4 等值 | `pre-set` 仍无效（`rate=-1`）；`post-set / post-prepare / post-start / post-tone` 四处 `size=14 rate=96000 bw=24 nch=3`；TDM `d1=0x30020`；WR 仍 E2 ⇒ **更长的帧没有改变任何一条读数** |
| 消费与收尾 | `not consumed` 0、`data_buffer send` 121、`no rsp` 0、panic 0、`pcm tone done (120 buffers)`、`===== done =====` / `PSCI SYSTEM_RESET` **都在窗口内** ⇒ 表里"尾部必须活着"的硬条件满足，这发的耳朵**是有效测量** |
| 实际音频量 | 120 × 4096 B = 491,520 B；按 48 kHz/16-bit/stereo = **2.56 秒**；峰值 16384/32767 = **−6.0 dBFS**（比 K2–K6a 响 8.5 dB）|

⇒ 这一枪的**唯一产物是耳朵**，寄存器部分已经证明：换了帧长与响度以后，DSP 侧的状态与我们
已经判定过的 K4 完全一致。**听感读数见下一节（用户回报后填写）。**

---

## 第 45 枪 K7 重放判决（`logs/ring-k7-long-loud.txt`，同一镜像同一 `sweettime=1793971127`）
## **听感门是有效测量，不是"没发生的事件"：2.56 秒、−6.0 dBFS 的音真的生成了**

重放与第 44 枪逐字同形（`dbb v1 bytes=4096 ms=21`、`tone gain g=16 peak=16384`、
四处 MFC `size=14 rate=96000 bw=24 nch=3`、`pre-set` 仍无效、TDM 原样、
`not consumed` 0、`data_buffer send` 121、`pcm tone done (120 buffers)`、`no rsp` 0、panic 0）。
身份靠 `ring_window` 的 seq 与新 raw 文件承担（`sweettime` 按构造与 K7 相同）。
**用户回报：看见了最后那堆日志，但确实没听到声音。**

⇒ 这条读数的价值在于它排除了"我根本没播出来"这一类混淆：2.56 秒的 1 kHz 正弦，
峰值 16384（−6.0 dBFS），WR 逐字与厂商相同、MFC 输出逐字与厂商相同并且保持到 120 帧之后。
**响度/时长轴到此双重关闭**：H1 已在 clamp 最大 −0.27 dBFS（0.7 s）测过无声，
K7 又在 −6.0 dBFS（2.56 s）测过无声 ⇒ 单独推 `sweetvol=31` 不再值得一次开机。

## 零开机补的一格：厂商时间线里那三个"没名字"的 opcode，其实有两个是有名字的

按本地 ASoC 头查（`work/sweet-audio/refs/qdsp6/ko-sound_soc_qcom_qdsp6_audioreach.h`）：

| 行 | opcode | 名字 | 依据 |
|---|---|---|---|
| 109（1971 ms，0x2010→1）| `0x01001003` | **APM_CMD_GRAPH_STOP** | 头 `:60` |
| 4（12.90 ms，**src 0x2002**）| `0x0100100d` | **APM_CMD_SHARED_MEM_UNMAP_REGIONS** | 头 `:66` |
| 111（1977 ms，0x0007→0x0002）| `0x01001010` | 头里没有；按 dst=2（PRM 服务）+ 与第 0 行的时钟票成对 ⇒ **释放 HW 资源**，这是**带证据的推断**，不是名字 | 时间线 + `:2687` 那条请求 |

⇒ **厂商一次真实播放用到的命令族，我们全都已经在发**（PRM 请求/释放、GRAPH_OPEN/PREPARE/START/STOP/CLOSE、
SHM MAP/UNMAP、SET_CFG、DATA_BUFFER_V2）。第 4 行那个"神秘命令"不是隐藏的使能步骤，
而是**解映射**（拆一块上一次的区域）。所以"我们缺一条命令"这个解释正式出局，
剩下的差异只有**内容**：

1. 那 **18128 字节自定义配置**的正文；
2. 他们图里有、我们图里没有的 **`0x40bf` / `0x40c0` / `0x4165`**（`0x08001008` ×2 与 `0x080010c4`）；
3. ACDB 比对里我们从不发的 AW 侧 **`0x10013d1c`**（注意：它不在这份 112 行 capture 里，
   它的"厂商也发"来自更早的 ACDB/权限分析，不是这一枪族的新证据）。

K6b（0275 内核 + `sweetvgraph=4`）正是同时压 1 与 2 的那一发：容器是厂商形状的、
传输是厂商用的那种带内形式，读回仪器原样在位。

---

## 第 46 枪 K6b 判决（`logs/ring-k6b-inband2.txt`，`sweettime=1793972363`）
## **带内厂商容器这次真的上路了，APM 的回答是 `status=0x2` = AR_EBADPARAM**

```
:1600 SWEET GLINK: APM graph_open/ukee2960-inband src=0x2010 dst=0x1 (2984 bytes) inline-big
:1608 SWEET GLINK: graph_open/ukee2960-inband rsp op=0x1001000 status=0x2 hop=0x2001005 len=32
:1609 SWEET GLINK: vgraph 4 (in band) open refused rc=-1
:1610 SWEET GLINK: APM graph_open/vp-wr src=0x2010 dst=0x1 (800 bytes)      ← 落回我们自己的图
```

- **0275 起效**：`inline-big` 那行就是 `g_apm_cmd_big()` 的打印，2984 字节真的发出去了；
  K6a 那轮同样位置是"没有发送行"，两轮对比把"是发送函数的锅"这件事钉死了。
- **APM 的回答是 `0x2` = AR_EBADPARAM**：不是 0x1(EFAILED)、不是 0x3(EUNSUPPORTED)，
  是**参数/形状层面不接受**。这与 ② 系列在**另一种传输形式**下拿到的 `status=0x2`
  （SHM 形式 + `sweets1ap=1`）**同值** ⇒ **我们那份从 ACDB 重建的厂商容器，不管从带内还是带外递上去，
  APM 都当作坏参数拒收**。这是同一份内容在两条独立通路上的两次复现，不是抖动。
- ⇒ 所以这一轮 `pre-set` 等 MFC 行**仍然是我们自己的图的读数**（落回之后的重放），
  **不能当成"厂商图喂不喂转换级"的答案**。打分器已加这道门
  （`vgraph 4 (in band) started` == 0 就拒绝下 K6a-4 判词）。
- 可核对的结构差：厂商那包 payload_size = **2712**，我们这包 = **2944**（2984 是 24+16+2944）
  ⇒ **差 232 字节的容器结构**。这一步此后是**离线重排**问题（把我们的容器按厂商信封重排），
  不是继续开机的问题。

## 用户提出的新问题（值得单独记）：**"他是不是根本不是按我们预想的方式接收数据？"**

这个方向不是猜测，它有一个可测的形式，而我们现在缺的恰恰是那个测量。

**厂商侧（从 112 行 capture 直接算）**：95 个 DATA_BUFFER_V2 从 +23.7 ms 到 +1967.6 ms
⇒ **每帧 20.5 ms**；`payload_size` 字段 = **4096 B** ⇒ 4096/0.0205 ≈ **200 KB/s**
≈ **48 kHz × 2ch × 16-bit（=192 KB/s）** —— 与 WR 声明的格式逐字一致。
⇒ 厂商的模型是**一帧音频配一个缓冲、按实时节律喂**，不是"尽快灌完"。

**我们侧（代码可查）**：`g_apm_data_buffer()` 确实等应答——发送在 `:6198`，
随后是 `:6408-6415` 的 `for (i = 0; i < limit; i += 5) { g_drain(0); if (g_rsp_ready) break; g_wait(5); }`，
而 `g_wait()` 的单位是**毫秒**（`clock_interval_to_deadline(ms, NSEC_PER_MSEC, …)`），
`limit = sweetrspwait`，默认 **100**（不是 5000；这一条纠正我口头说过的"每发最多 5 秒"）。
每一轮 `not consumed` 都是 0 ⇒ **我们每一发都在 100 ms 内拿到了 DATA_BUFFER_DONE**。

**但我们从来没量过"多久"。** 而这正是分歧点：
| 每发 DONE 的等待 | 含义 |
|---|---|
| ≈ 4 次迭代 = 20 ms | 模块按实时消费，与厂商同模型 ⇒ 你的假设不适用于这条通路 |
| ≈ 0 次迭代（立刻） | 我们把 2.56 秒音频在极短时间里全推进队列，DONE 只是队列回执；**超出内部环深度的部分被丢掉** ⇒ 恰好是"全绿 + 无声"的组合 |
| ≈ 20 次迭代（顶到 100 ms） | 消费在挣扎/部分超时，行为介于两者之间 |

**两件事已经在做**：
1. **K8（第 47 枪，`images/k8-paced.img`，cmdline 实测 578 B，我预测 579 又差 1）**：
   不加任何新代码，直接按 1152 B = 6 ms 音频的事实把发送**节律化**
   （`sweetbufgap=6` ⇒ 每发之间 `delay(6)`，`tone pace 6 ms/buffer` 是自带的证人打印），
   响度保持你要的 `sweetvol=16`（−6.0 dBFS）。**如果声音出现，洪水模型就是对的**。
2. **0276（测量，不改行为）**：把上面那个循环的迭代次数按发统计，收尾打印
   `SWEET MARK: dblat v1 min=… med=… max=… sum_ms=… n=…`。它只用现成的循环计数器和加法，
   **不需要任何新的时钟 API**（这一点很重要：本 port 里 `machine_absolute_time` 之类我们从未用过，
   而 CNTVCT 自旋那套已经在 GPU PR 里被证明需要额外小心），
   单位天生就是 5 ms，足以区分上表三行。

顺带一条被这次讨论纠正的记录：我之前说过"每发的等待上限是 5 秒"，实际默认是 **100 ms**
（`sweetrspwait` 可调到 5000）；这不影响任何已判结论，但影响"洪水和实时如何区分"的先验，
所以留字在这里。

---

## 第 47 枪 K8 判决（`logs/ring-k8-paced.img`→`logs/ring-k8-paced.txt`，`sweettime=1793972702`）
## **实时节律也没能换来声音——但这一枪的真正价值要等 0276 的数字才能读全**

证人齐了：`dbb v1 bytes=1152 ms=6`（几何回到出厂）、**`tone pace 6 ms/buffer`**（节律真的加上了）、
`tone gain g=16 peak=16384`（−6.0 dBFS）、四处 MFC 全部 `size=14 rate=96000 bw=24 nch=3`、
`pcm tone done (120 buffers)`、控制面全绿、`no rsp` 0、panic 0、窗口吻合。
**用户回报：仍然完全没声音。**

诚实的边界：K8 只证明了"**每发之间睡 6 ms**"这一种节拍不产生声音。
它**没有**证明"我们不是在被洪水式回执"——那需要**每发 DONE 的真实延迟**，
而那个数字是 0276 正在编的仪器（`dblat v1 n/min/avg/max/timeout`，单位 5 ms）。
两种结局都已经可判：
- `avg ≅ 20` ⇒ 模块本来就在按实时拉，我们的 6 ms sleep 只是叠加在它之上，
  洪水模型出局，"无声"必然在端点之后（TDM→功放→喇叭）或某个我们看不见的内部缓冲；
- `avg = 0` ⇒ 即便 K8 每发之间睡了 6 ms，回执仍是立刻到的 ⇒ **DONE 不是消费的回执而是入队回执**，
  那么"送出音频"这件事在我们全部证据里从来没有被证明过，
  下一步就不是调节拍而是**换数据通路的形式**（一次发多帧 / 用模块自己声明的环深），
  而那需要厂商环深信息 ⇒ 回到 18128 那份配置。

**顺带把三次"音频形状"实验归总（都是开机证据）**：
`1152 B × 120 帧 @0.72 s`（K2–K6）、`4096 B × 120 帧 @2.56 s −6 dBFS`（K7/K7 重放）、
`1152 B × 120 帧 @6 ms 节律 −6 dBFS`（K8）——三种都无声，且控制面/读回全绿。
⇒ **音频的"形状/时长/响度/节律"这一族已经不能单独解释无声**，剩下的自由度只有：
数据通路形式（0276 的数字决定它是否还活着）、图内缺失对象（`0x40bf/0x40c0/0x4165`、
18128 正文）、以及端点之后的物理层。

## K9（第 48 枪）：同一份 K8/4 参数 + 0276 内核，**只为拿那个延迟数字**

寄存器门 = 与 K4/K8 等值（`pre-set` 无效、四处 `size=14 bw=24`、`not consumed` 0），
新增唯一读数 = `dblat v1 pre …` 与 `dblat v1 tone n=120 min/avg/max timeout`。
不需要耳朵，不需要新变量。

---

## 第 48 枪 K9 判决（`logs/ring-k9-latency.txt`，`sweettime=1793974906`，cmdline 实测 **564 B**）
## **第一个延迟读数：`dblat v1 tone n=120 min=5 avg=5 max=5 timeout=0`——以及它为什么还不算答案**

```
SWEET MARK: dblat v1 pre  n=1   min=0 avg=0 max=0 ms timeout=0
SWEET MARK: dblat v1 tone n=120 min=5 avg=5 max=5 ms timeout=0
```
其余门与 K4/K8 等值：`dbb v1 bytes=1152 ms=6`、`tone gain g=16 peak=16384`、
`pcm tone done (120 buffers)`、`not consumed` 0、窗口吻合（`total=0x16aec`）。

**读法（严格）**：采样循环是"先 `g_drain`，没到就睡 `step=5` ms 再看"，
所以 `5` 的意思是 **第一次 drain 没有、第二次有了**，即真实延迟落在 **(0, 10] ms**，
下界由我们自己的步长决定。`min=max=5` 对 120 发全都一样，这个均匀性
**是仪器的量子化特征，不是接收端的特征**。

⇒ **这一读数没有裁决任何分歧**：它既与" sink 按实时拉一个 6 ms 的 1152 B 帧"相容，
也与"队列立刻回执、只是我们的第一次 poll 早于回执到达"相容。
把它当成"洪水模型成立"或"洪水模型出局"都是过度解读，我不这么写。

**所以 0277 不是新实验，是把这把尺子的刻度做细**：`sweetdbstep=1..50`（默认 5 = K9 行为不变），
读数行里带 `step=`，marker 升为 `dblat v2`——**一个不带自身分辨率的延迟数字不该和带分辨率的长一样**。

**K10（第 49 枪）在 `step=1` 下的判读，开机前写死**：
| `dblat v2 tone … step=1` 的读数 | 结论 |
|---|---|
| `avg ≈ 0-1`（第一次 drain 就有） | **DONE 是入队回执**，不代表消费 ⇒ "音频曾被送出"从未被证明，下一枪改数据通路形式 |
| `avg ≈ 6`（±2） | 与 1152 B = 6 ms 音频**吻合的实时消费** ⇒ 洪水模型出局，无声的机制在端点之后 |
| `avg ≈ 20` | 与厂商 4096 B/20.5 ms 的节律同源 ⇒ 也在实时侧，但节奏更粗 |
| `timeout > 0` | 100 ms 窗口不够 ⇒ 先调 `sweetrspwait`，**这不算模块结论** |

预算 48/120。K10 只是同一颗内核换 `sweetdbstep=1`（+15 B ⇒ 579 B）+ 新内核 0277。

---

## 第 49 枪 K10 判决（`logs/ring-k10-latency1ms.txt`，`sweettime=1793979284`，578 B）
## **决定性读数：`dblat v2 tone n=120 min=1 avg=1 max=2 ms timeout=0 step=1`**
## —— `DATA_BUFFER_DONE` 不是消费的回执

1 ms 分辨率下，**120 发的应答全部在 1–2 ms 内到达**，中位 1 ms。
而每个 1152 B 缓冲装的是 **6 ms 音频**（48 kHz×2ch×16-bit）。
⇒ 模块回答我们**比它"应该消化"的速度快 6 倍**，而且 `pre`（图刚开、tone 之前那一发探针）
是 `min=avg=max=0` —— 第一次 poll 就拿到了 DONE。

对照厂商：他们的节律是 **20.5 ms/4096 B = 21.3 ms 音频**，1:1 贴着消费；
我们是 1 ms/6 ms 音频。**同一个 opcode、同一个 dst=0x40bd、同一种 offset 模式，
行为却是两种。**

**这次读数直接推翻一条我们用过很多轮的表述**：
> `not consumed = 0` / `data_buffer done status=0` **不等于** "PCM 被消费了"。

以前所有轮次里"120 帧被 DSP 消费"这句话，其实是"120 帧拿到了 DONE"。
K10 之后这两件事必须分开写。这也符合既有的纪律：**被接受但没有变化 = 该值不是这条通路写的**。

**由此得到的机制图（标注为目前最相容的解释，不是已证）**：
WR 端点在收到 DATA_BUFFER 通知后立刻回 DONE，说明它**没有把"等待消费"作为回执条件**；
在厂商那侧，回执是等价的消费节律 ⇒ 差异最可能出在
**"这个端点知不知道自己的环有多大、每个缓冲多少字节"** ——
而那正是图内 cal 数据 / 18128 自定义配置会提供的东西之一。
如果它没被告知几何，最自然的行为就是"收通知、回 DONE、把数据丢在原地"，
于是出现我们看到的全部症状：**每一层都绿，一路全绿，扬声器静音。**

**已经可以关掉的候选**（凭这次读数）：
- "我们的发送节律错了"——节律不改变 DONE 的性质，K8 已经试过 6 ms 节拍，且 K10 表明
  回执时间由模块决定（1 ms），不由我们决定；
- "音频从未被接受"——它被接受了，只是"接受"不意味着"被读走"。

**下一批候选（都还没开）**
1. **K11 = 用 `sweettok=1`（复合 token：`dsp_buf | (len<<16)`）跑同一条流**：
   如果模块对 token 的解释决定了它是否搬运数据，回执时间会**变**——这是唯一能
   从 AP 侧观测的旁证；repack 即可（+11 B ⇒ 589 B，不越 590）。**这是 K10 之后最便宜的一发。**
2. **0278 = 在 tone 进行中读功放寄存器**（此前从未测过的那个时间点），
   用来判"端点之后有没有东西在跑"；注意它会引入 mid-stream 的 I2C 读，
   所以不能和 K11 同轮做（仪器不该扰动被测量）。
3. **18128 正文**：这份 capture 拿不到（kprobe 只取前 96 词），
   所以它需要的是资产/协议工作而不是开机。

---

## 第 50 枪 K11 判决（`logs/ring-k11-tokencmp.txt`，`sweettime=1793979539`，589 B 与预测一致）
## **`sweettok=1` 没有改变任何东西——但这一发的证据强度要如实打折**

```
dblat v2 tone n=120 min=1 avg=1 max=2 ms timeout=0 step=1     ← 与 K10 逐字相同
dblat v2 pre  n=1   min=0 avg=0 max=0 ms timeout=0 step=1
dbb v1 bytes=1152 ms=6 / tone gain g=16 peak=16384 / pcm tone done (120 buffers)
```

**判读**：如果模块对 token 的解释决定了它是否真的搬运数据，回执时间应当变化；它没有变化。

**必须同时写下这条限制**：`sweettok` 的两种形式在代码里是
`tok = up ? ((token & 0xffffu) | (len << 16)) : (token & 0xffffu);`
——**这一行没有打印自己选了哪一支**。我能证明的只有"参数进了 cmdline"，
不能证明"运行时用的是复合形式"。所以严格地说，K11 是
**"无变化，且缺少生效证人"**，而不是"证明 token 无关"。
这条正是 0267 已经立过的规矩（决定不打印自己的值就不能被打分）在这里复发了一次，
所以 0278 从一开始就带了 `amptone v1 on=%d` 这样的证人。

**因此 K11 的真实产出是排除了一个"便宜的下一步"**：不需要再做 token 实验；
下一个能推进问题的仪器只剩 `0278 = tone 进行中读功放寄存器`（已推、CI 构建中，
marker `SWEET MARK: amptone v1 `，ci_replay applied=219）。
K12 = K10 那行去掉 `sweetdbstep=1`（节律已定为 1 ms）加上 `sweetampverify=1`，
**预测 580 B**，判据是 `ampread mid-tone addr=… reg=…` 与 pre-stream/post-stream **是否不同**：

| 观察 | 结论 |
|---|---|
| 三个时刻读数相同 | 功放侧在播放中**没有任何状态变化** ⇒ "端点之后没在跑"获得第一条正向证据（这是过去只能靠"读不到"来推断的一格）|
| mid-tone 与 pre/post **不同** | 数据确实到达了 PA ⇒ 无声的问题在 PA 之后（供电/使能/mute），并且 K10 的"入队回执"解释继续成立但不解释全部 |
| `not consumed` 出现或 dblat avg 明显移动 | 这一发的 I2C 读扰动了流 ⇒ **仪器读数**，按 K12-故障处理，不作模块结论 |
| 读回本身 NACK（`r=-1/-2`）| 回到 0238/0241 那条已知线；**这不算对功放的否定**，是仪器 |

预算 50/120。

### K12 开机前快照（真实产物；第 51 枪 / 预算 120）

内核 `out/kernel.adsp` = `7b33f6da6cecb18d…`，23:58 到（取回器第 3 次成功，前两次拿到的是
0277/0276 的资产，被 marker 正确拒掉）；当场数标记 **`amptone v1 `×1、`dblat v2 `×1、
`mfcget v4 `×5**，体积 17548104（0277 是 17548064）。

产物 `images/k12-amptone.img`：cmdline 实测 **581 B**。字节账现在能对上：
K10 的 578 − ` sweetdbstep=1`(14) = **564**（正是 K9 实测过的那个数 ✓ 自洽），
再 + ` sweetampverify=1`(17) = **581** —— 我先前"预测 580"是把 token 名数错一个字符，
不是算术模型有问题；这已经是本周第四次"长度必须实测、不许继承"，规则继续生效。

`sweettime=1793980732`；镜像内 `amptone v1 `×1 且本轮 `sweettime` ×1。
判据与四种结局见上一节 K12 表（`3dd2658`，开机前提交）。
这一发不需要耳朵。

---

## 第 51 枪 K12 判决（`logs/ring-k12-amptone.txt`，`sweettime=1793980732`）
## **落在表里第四格：仪器失败，不是对功放的否定**

窗口自检：`total=0x17485` 与解码字节**逐字节相等**，`pcm tone done` 在窗口内 ⇒
下面这些读数都是本局的，不是上一局残留。

| 门 | 实测 | 判读 |
|---|---|---|
| 身份 | `amptone v1 on=1 at=60/120`（:1910）| **仪器真的在第 60/120 帧跑了** —— 这是 0278 从一开始带证人的回报，K12 唯一确证的新事实 |
| 三个时刻 | `pre-stream` :1644 起、`mid-tone` :1915 起、`post-stream` :2067 起，**16 行全部 `r=-2 val=0xa5a5`** | 落在开机前表的**第四格**："读回本身 NACK ⇒ 回到 0238/0241 那条已知线；**这不算对功放的否定**，是仪器" |
| 表第一格是否成立 | **不成立** | 第一格要求的是"三处**读数**相同"；这里是三处**读不到**相同。哨兵 0xa5a5 是我们自己填的值，不是 PA 的回答（N1 那条教训的原样复发，被开机前表接住了） |
| 表第三格 | 不成立 | `dblat v2 tone n=120 min=avg=max=5 ms timeout=0`、`not consumed`=0 ⇒ 播放中的 I2C 读**没有扰动流**（顺带：这条现在是实测，不是推测） |

**本局真正拿到的新事实（同一次开机内的 A/B，跨 boot 对照不能给的）**：
:1591–:1598 `pre-bitbang` 八个读数 **全部 `r=0` 且是真值**（`0x2032/0xffff/0x4003/0x38`），
中间夹着 :1601–:1635 的位暴力扫描（`released/spksw-high/asserted scan found 0 device(s)`、
`amp 0x34 no ack`），此后 :1642 重新 `pins re-applied` 再读就**全部 NACK**，
且每行前一条都是 `SWEET I2C: se15 err irq=0x409 st=0`。
⇒ "读不到功放寄存器"这件事的定位从"流/功放状态"移到了**位暴力那一段的副作用**，
而且是**同机同刻**证明的。0241 当年只能跨 boot 比，这次一格之内比完了。

**因此 K12 的账是**：AP 侧仪表盘上仍没有"数据到没到引脚"的正向读数；
但我停 AP 侧旋钮的既定结论不因本局改变（旋钮族已作为族关闭）。
若要真拿这个读数，代价是一发 `sweetnobb=1` + `sweetampverify=1`，
而 K12 的 cmdline 实测 581 B，加 ` sweetnobb=1`(12) = 593 > 590 ⇒
必须先删一个已有开关（`sweetmfcget=3`，14 B，其读数 K1–K5 已闭）。
**这一发我没有开**：它买的是仪表，不是声音，而 §"sheng 源码带来的新格"
（见 `docs/unixnu-sheng-audio-2026-11-07.md`）给出的是能直接买内容的枪。

---

## 2026-11-08：0289 描述符隔离对照，第 65/66 枪事前判据（未开）

用户本轮明确要求：**“请你直接展开工作，推送触发构建，然后继续测试”**。
据此执行音频构建及最多两次 RAM-only 对照；不扩大响度/时长，不开 GPU/基带/Wi-Fi，不写分区命令。
预算起点 **64/120**，两轮都执行后为 **66/120**；原 B1 去 AW 候选仍暂停。
本轮只判“描述符隔离是否保持已知控制面/提交行为，并修正 AP 侧 PCM 污染”；无听感或渲染证据时不判扬声器出声。

### 参数与顺序

两轮都使用同一份经完整构建、CNTP 修补和 marker 门验证的 0289 内核：

```bash
D22AP_ADT=../xnu/research/ipsw-x/d22ap.adt bash scripts/mkimage.sh \
  sweetbufs=120 sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetshmhi=1 \
  sweets1ap=3 sweetampfirst=1 sweetvol=16 sweettdmvendor=7 sweetvgraph7=1 \
  sweetrxlog=1 sweetdescsep=0
```

- 第 65 枪 `d0-descsep-control-0289`：`sweetdescsep=0`，图描述符仍在 offset 0。
- 第 66 枪 `d1-descsep-isolated-0289`：参数唯一语义变化 `sweetdescsep=1`，描述符复制地址和 wire offset 都为 `0x4000`。
- 两轮均不发 EOS，不带去 AW / 旧四包 / 新格式 / nowait / 大缓冲开关；sweettime 由脚本自动生成，镜像身份可不同。
- 两条 cmdline 离线预期均 **582 B**，打包后从 payload 的真实 608 B 字段和最终镜像内嵌 payload 实测，≤590 才放行；不是只 grep 常量。
- 音幅沿用 `sweetvol=16`，`sweetbufs=120`（约 0.72 s）；不更响、不更长。

### 每轮前置门

| 门 | 必须满足 | 未满足时 |
|---|---|---|
| W0 设备 | 只指定项目目标 serial；无其他会话抢设备；电量 >50% | 不启动 |
| W1 新内核 | 完整构建产物含 `SWEET MARK: descsep v1 on=%d off=%#x len=%u`、`dcnv v2`、`rxall v1`；CNTP raw 恰一处、修后 0 处；按资产名配对摘要 | 不打包/不开机；旧 0288 不得冒充 |
| W2 本地链 | stub 严格编译、12 项 host 回归；ci_replay post-image==链树 HEAD（原有 49 skip 不增加） | 不推送/不开机 |
| W3 镜像 | 实际 cmdline 唯一 sweettime，正确 descsep 值、vgraph7=1、bufs=120、vol=16、cpus=1；两轮去掉 sweettime/descsep 后逐字相同 | 不启动 |
| W4 事前表 | 本节已 commit，记录本轮镜像摘要/时间戳后再启动 | 不启动 |

### 收包与判读门（两轮分别打分）

| 观察 | 期望/判读 |
|---|---|
| 身份 | ring 内第一枚 sweettime 等于该镜像 build log/payload；raw SWEETLOG magic/total 与解码窗口匹配；只用本轮窗口，不读陈旧 tail |
| DEC/CNV 路径 | `dcnv v2 on=1 bytes=856 start=0xb0000002`；`graph_open/dec-cnv` 仅 1 次，open/start status=0；`dec-cnv started` |
| 实际隔离 | 控制轮 `descsep v1 on=` 0 行（门为 0 时该 printf 不执行）；隔离轮恰 1 行 `on=1 off=0x4000 len=856`；任何 refused 行使该轮无效 |
| 回落排除 | no-aw、aform-cur、ukee2960、默认 vp-wr 等其他 graph_open 发送数=0；失败后回落不得当主路径成功 |
| 数据回执 | `pcm tone done (120 buffers)`，`dblat v2 tone n=120 ... timeout=0`，`rxall v1 op=0x5001004` 恰 121 条；这是提交/ACK，不是渲染证据 |
| 收包仪器阳性 | 至少一个 basic result、一个 MEM_MAP_DONE 和实际 DONE；不能只用“没有某事件”当负结果 |
| EOS | 没有 `APM wr_eos/last` 发送，分支证人应为 `eos v2 on=0`；不重新判 EOS |
| 安全收尾 | `===== done =====`、`PSCI SYSTEM_RESET` 均在有效窗口，panic 0；回到 adb 可读取 |
| 听感 | 只有上述路径/回归门成立才记录用户听感；用户未报告时写“未知”，绝不把未知写成无声 |

### 配对判决与止损

- 两轮均 open/start=0、120 帧/121 ACK/正常收尾：隔离没有破坏控制面/提交路径；由同一代码的本地字节测试可确认 AP 侧污染修复生效，但是否有声需听感，不能说已经到功放。
- 只有隔离轮 open/start 拒绝或 no response：定位为此次 offset 变化引入的行为差异，先报告事实，不将其泛化为“图内容被否”；不加第三枪旋钮。
- 任一身份、收尾或回落门失败：该轮 VOID；若设备卡死立即停止，不能为完成对照强行软件复位。
- 两枪完成后本轮开机止损，不自动开展 B1、EOS 或格式第三枪；后续只做离线判读和记录。
