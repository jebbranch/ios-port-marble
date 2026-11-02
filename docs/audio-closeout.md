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
