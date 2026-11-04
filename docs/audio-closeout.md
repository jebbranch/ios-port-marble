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
