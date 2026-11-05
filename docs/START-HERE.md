# 从这里开始（marble / Redmi Note 12 Turbo，XNU 8796 + iOS 16）

> **工作守则 / 踩坑清单 / 已验证事实的单一入口 = 仓库根的 `AGENTS.md`**（`work/xnu-gh/AGENTS.md`）。
> 本文件保留"已知-good 配置与复现命令"，其余纪律以 `AGENTS.md` 为准。

> 目标：**只做 `fastboot boot`（纯 RAM 运行，不刷分区、不动 GPT）**，把
> Redmi Note 12 Turbo（marble，SM7475）跑到 iOS 16 用户态，最终能当一台
> 能用的机器（显示/触摸/USB/电池/RTC），并让扬声器出声。
>
> 本文是入口；细节都在 `docs/`，按下面的索引查。

## 一、现在能做什么（表格基线 2026-10-27 / 内核 v90；音频行更新到 2026-11-02）

| 能力 | 状态 | 说明 |
|---|---|---|
| 启动到 SpringBoard | ✅ | shim + 自建 ADT + XNU 8796，RAM-only |
| 显示 / 触摸 / USB | ✅ | 自建驱动；触摸是 GPIO 软件 SPI |
| UFS root | ✅ | userdata 尾部的 HFS+ 镜像 |
| 8 核 SMP | ✅ | GICR stride 0x40000 修复后 8/8 起核，调度器分配工作 |
| ADSP / GLINK / GPR / APM 控制面 | ✅ | GRAPH_OPEN/PREPARE/START、TDM SET_CFG 全 status 0 |
| **音频数据面（DSP 消费我们的 PCM）** | **✅ 2026-11-02** | `sweets1ap=3`（S1 叶子 AP=11）后 `DATA_BUFFER done status=0`，槽位归还与我们的 ping-pong 一致，1200 buffer 跑完正常复位，cb5 无新故障位 |
| GPU SMMU（kgsl-smmu） | 🟡 探测完成 | 全局寄存器可读，SMR/S2CR=TRANS 可写，BYPASS 被禁，CB 页访问挂死 |
| 音频出声（内核侧） | 🔴 未证（且 E/P 上次开机作废） | ① PA 寄存器三时刻逐位相同（J3 未命中）；② 厂商四包重放全被拒 `vcfg2 rc bf=-1 c0=-1 c8=-1 p65=-1`，跨开机可复现（`ring-i4.txt:1647`）；③ 时钟见证因拿不到厂商已知值而关闭 ⇒ 静态快照这条路判死。**2026-11-03 重判**：I4 那次的 `sweetbufs=30` 被 607 字节顶切成 `sweetbufs=3`（`ring-i4.txt:1732` 只有 3 buffer ≈ 12 ms 音），所以那组"三时刻相同"的具体读数不作数（结论方向不变）；`ring-ab` 的 E1–E5/P1–P4 三个参数全没进内核 ⇒ 整次作废，且因 `sweetbufs` 丢失实际跑了 1032 个 buffer——宿舍安静的保护当时是失效的，只是那轮提前崩在 `CRASH ExtragalacticPos`。剩能量侧（0243）与 iOS 侧的桥 **能量侧也在结构上关门（2026-11-03）**：`logs/fdt/marble-live.dts` 的 SPMI 上没有燃油计兼容节点（`pmk8350@0/pm8350@1/pm8350c@2/pm8350b@3/pmr735a@4/pm8450@7`），`qcom,battery_charger` 挂在 `compatible = "qcom,pmic-glink"`（:9007,9013,9019），而 glink 属性缓存 5 s 一采样（`0181:3604-3607`，打印 30 s），对 130 ms 的音原理性看不见；0243 v3 的六 SID 扫描把这条推断变成同一次开机内的测量。见 `docs/audio-hal-bridge.md` §11.4 |
| iOS 应用出声（音频桥） | 🟢 2026-11-03 已证：mediaserverd 会来 open | `ring-ep.txt:954/957` —— pid 28（同 ring `:1681 p=28[mediaserverd]` 核验）打开了我们发的 `AppleAOPAudioUserClient` 并调了 **selector 0（method_Open）**，`aop sel=0 ran` 说明抄自真机的方法表在跑；随后在 `map refused type=0` 一行之后 closed/died（顺序是事实、因果未证，ep2 用"给缓冲区"来否证它）。`IOAudio2DeviceUserClient` 也被 open（`:1367`）。0049 埋点第一次开启即 396 行、mediaserverd 288 行、对照 backboardd 108 行 ⇒ 三条候选阻塞点：`IODeviceTree:/product/audio -> none`（×3，`/product` 本身 ×271 成功）、`AppleEFINVRAM`/`IODTNVRAM -> none`（我们内核没有 NVRAM）、以及一条被 240 字符截断的 `IOUserAudioDriverUserClientProperties` 匹配（0245 已放到 600）。E 组能量侧按 `marble-live.dts` 证据关闭中：燃油计不挂在 SPMI，`qcom,battery_charger` 属于 `qcom,pmic-glink`。详见 `docs/audio-hal-bridge.md` §11 |
| 电池 | 🟡 通道已开 | pmic_glink 能 open，属性请求的响应格式不对 |
| RTC | 🟡 读到了 | PMK8350 RTC 由 EE4（ADSP）拥有，AP 写入被拒 |

## 二、几个必须知道的结论

### 1. 音频：挂死的真正原因不是 hypervisor，数据面断在**我们自己 S1 叶子的 AP**
厂商 `SHM_MAP` 的地址是 `{addr_lsw=0x1FFD0000, addr_msw=1}`。高字不是地址
（33 位 IOVA 会让 DSP 翻译 fault），而是 **stream index**。我们一直发
`msw=0`。加上 `sweetshmhi=1` 后**不再挂总线**，控制面全绿。
**2026-11-02 数据面打通**：真正的门槛是 cb5 阶段 1 页表叶子 `0x701` 的
`AP=[7:6]=00`（特权访问专用）——DSP 非特权读 `0x1ffe8000` 被 SMMU 终止，
于是 `DATA_BUFFER` 一声不响；把它放到 `0x7c1`（`sweets1ap=3`）后
`done status=0` 回来、槽位归还跟我们的 ping-pong 一致、1200 个 buffer 跑完并正常复位，
全程没有新故障位。当初怀疑的"厂商 18 KB APM 级 SET_CFG"**不是主因**
（大小随开机变 18128/18504，一个厂商字节都没多给就通了）。
未证：是否真出声（图里仍缺 `0x40bf/0x40c0/0x40c8/0x4165/0x4308` 这些配置对象）。
详见 `docs/audio-closeout.md`（2026-11-02 16:54 一节）。

### 2. 多核：GICR stride
marble 的 redistributor 间距是 **0x40000**，XNU 默认 **0x20000**。
`cpus=1` 永远只用 offset 0，所以一直没暴露。修法：`gicrstride=0x40000`
（`mkimage.sh` 默认已带）。AMP / E-P 调度需要 CLPC/Edge policy，暂时
没跑起来（`docs/status-log.md`）。

### 3. GPU：CB 页不可达
`kgsl-smmu` 的 `0x0000..0x1FFF` 可读写（SMR、S2CR=TRANS 都能保持），
但 `0x80000+` 的 context bank 页一碰就整机挂死；BYPASS 被上层改写为
FAULT。没有 CB 就建不了页表。详见 `docs/gpu-probe.md`。

### 4. RTC：AP 没有写权限
PMK8350 RTC（SID 0，ctrl `0x6146`、write `0x6140..0x6143`、read `0x6148`）
读出来是 1972（未设置）。AP 侧写入被 SPMI 拒绝——SweetBattery 的探测日志
直接写着 `ppid 0x61 ... owner=4` 和 `write refused ppid=0x61 owner=4`，
即 **RTC 外设归 EE4（PMIC 固件）所有**；Vendor DT 的 `rtc@6100` 也没有
`allow-set-time`，TWRP 的 `hwclock -w` 因此同样 EPERM。
**iOS 的时间实际来自 `sweettime`**（构建镜像时写入 cmdline），不依赖 PMIC
RTC；要真正持久化 RTC 需走 ADSP/PMIC 固件的服务，不建议抢 APID 所有权。

### 5. 电池：通道能开，协议还没对上
`PMIC_RTR_ADSP_APPS` 能 open（ack=1、remote=1、rcid=6），请求也发出去了，
但返回的 24 字节看起来是请求回显/ack，不是属性值（所有请求都解析成 1）。
需要对厂商的 pmic_glink 收发做一次抓包（和当年抓 GPR 一样的方法）。


### 2026-11-04 新增/更正的结论（细节在 `docs/audio-closeout.md` 的 H/I/J/K/L/M/N 七节）

- **震动输出侧已解决**：AP 侧 SPMI 直驱真的能震（用户 44/44 有震感、震感与设置一致）。
  `0x2` 那个位**每次脉冲后都出现、清除后为 0** ⇒ 是本次脉冲产生的，且**与"没动"无关**；
  `auto_res` 与 DT `lra-f0-*` 的比较是拿错字段，**永久停用**。
- **4 个同名 bhap 实例是我们造出来的**：4 个不同 `self`、`provider` 全是同一个
  `IOResources` 节点 ⇒ 同一个 provider 被同一条 personality 匹配了 4 次，
  **不是**实例互相匹配。iOS（pid 57）attach 的是**最后注册的那个**（`prov` 命中第 4 个 `self`）。
- **UC 连接不调用 driver 侧 `open()`**（`bhap open` 0 行，预先声明为非判据）⇒ 以后要问
  "谁 attach 了哪个实例"，只能挂在**已证明会执行的 selector 路径**上。
- **`sel=4` 是音频 flag 驱动的**（L 轮），且 `GetControlValue`（就是 sel=4）的回答从 0 改成 1
  **不改变任何东西**（M 轮，调用形状逐项与 L 相同）。
- **mediaserverd 的现状**：open 4 台设备、map、notify 之后**一个 IOAudio2 方法都不调**；
  它落空的查询只有 `AppleEFINVRAM`/`IODTNVRAM`/`bluetooth`，且**在这之后还继续跑了 180+ 行**
  ⇒ "缺 NVRAM 所以停住"没有证据。`/product` 的 287 次路径查询发生在**成功 open 之后**。
- `crashed: mediaserverd` 只在 **24/181** 个 ring 出现，与"能 open 音频设备"的 **12** 个 ring
  **完全不重叠** ⇒ 崩溃风暴是另一批 boot，**不能**拿来解释"开壳不出流"。
- **两条我自己写错的笔记已更正**：`0x08001176` **不是**我们自创（MTP ACDB 里 16 次）；
  `p6=0x10003` 是 `{mem_pool_id=3, num_regions=1}` 被我读成一个字（我们发送端一直用 pool=3）。
  `0xb0000081` 属于我们 10 月的老容器，不在厂商现役子图里。
- **②的尺寸事实**：机内 6 份 ACDB **没有任何 blob 落在 18,000–19,000**；每份只有**一条**超限
  blob（17,452 或 17,620，校准记录 16,405/16,570），两者都对齐到厂商映射用的 **20,480**；
  QRD_ukee 是 35,244→36,864，**不匹配** ⇒ 厂商当时加载的不是这一族。
  而"每子图按 key 各取一份 808+756+256+944 + 前导/头 = 2,960"**反向证明了我们容器的构造方法是对的**。

## 三、构建与启动

```bash
# 1) 取内核（CI 的 kernel-latest）
./scripts/fetch_kernel_api.sh        # 或手动下载 release asset

# 2) 生成 RAM-only 启动镜像
D22AP_ADT=/path/to/d22ap.adt ./scripts/mkimage.sh

# 3) 启动 + 拉日志
./scripts/boot_one.sh images/shim-iosroot-adsp.img <label>

# 多核 + 调度采样（测试用；默认不带）
KEEPALIVE=1 CPUS=8 ./scripts/mkimage.sh \
  sweetsmp=1 sweetsmpn=20 sweetsmpms=1000 sweetglink=0
```

### 音频（数据面已知-good，2026-11-02 实机确认；参数预算修正 2026-11-03）

> **boot-args 只有 607 字节**：`scripts/mkpayload.py:36-38` 把 CMDLINE 放在 0x48、
> SEG_TABLE 在 0x2a8（`BOOT_LINE_LENGTH = 608`），`:186` 以前是**静默切片**。
> 内核侧同一常量 `pexpert/pexpert/arm64/boot.h:26`，`bsd/kern/kern_sysctl.c:2167-2170`
> 用 `strlcpy(…, BOOT_LINE_LENGTH)` 回显 ⇒ ring 里那行 `boot-args = ` 就是内核真正看到的，
> 长度顶到 607 就说明后面全没了。`mkimage.sh` 现在超 **590** 直接 build fail。
> 仪器开关合并成 `sweetexp`（bit0 = IBAT 突发，bit1 = 音频服务探针），新增见证不再吃字节。
> `sweetgpu=0` 已从示例里去掉——`g_gpu_probe()` 起于 `int en = 0`
> （`work/chain-tree/bsd/dev/sweet_glink.c:2689`），传 0 与不传等价，别为它浪费 11 字节。

```bash
# kernel 需要 >= 0232（S1 叶子的 AP 由 sweets1ap 控制）；0233 起可把 PA 提前配置
D22AP_ADT=~/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt CPUS=8 GPU_PROBE_LEVEL=0 \
  ./scripts/mkimage.sh sweetbufs=30 \
  sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 \
  sweets1ap=3 sweetampfirst=1
./scripts/boot_one.sh images/shim-iosroot-adsp.img audio
```

下一轮（ep）要开的是"E 能量格 + P 空壳 + Q 查询埋点"三件互不干扰的仪器，
用的是 0049 里那个**从没开过**的 IOKit 查询埋点（每 boot 上限 400 行；
`backboardd` 是对照，它今天确实在发 matching，所以 mediaserverd 那边为 0 才是真负结果）。
cmdline 实测 584 字节（硬顶 607）：

```bash
SWEETIOLOG=mediaserverd,backboardd \
  D22AP_ADT=~/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt \
  ./scripts/mkimage.sh sweetexp=3 sweetbufs=30 sweetnobb=1 \
  sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 \
  sweets1ap=3 sweetampfirst=1
./scripts/boot_one.sh images/shim-iosroot-adsp.img ep
```

必须同时看到的四条才算这一配置成立（少一条就别当基线用）：

```
SWEET MARK: s1ap leaf=0x7c1                  # 叶子只改了 AP[7:6]，[1:0] 仍是 block
SWEET GLINK: graph_start rsp … status=0
SWEET I2C: geni amp 0x34 r=0 id=0x2032 … profile 44/44   # 必须早于 pcm tone start
SWEET GLINK: data_buffer: dst=0x40bd done status=0        # 槽位字段跟着 ping-pong 走
```

已知坏仪器（0237→anchor3 两次开机定位，别重复推一遍）：`sweetampverify=1` 的 GENI **读回**
有两层坏，都实测过：
1. `sweet_i2c.c:873-874` 的 bit-bang 段把 SCL/SDA 两个 pad 的 mux 切回 GPIO，于是 pad 之后的
   每次传输连启动都没启动（`se15 timeout ... irq=0`）。0238 读回前重新 `se15_pins()` 修掉了这层。
2. 修完以后传输真在总线上了，**但芯片自己不应答**：pads 已经是 cfg=0x87（和读成功时同一个值），
   同一个 read 仍返回 `r=-2`＝SE15 的 NACK 分支。也就是说 bit-bang 段弄掉的不是我们的引脚，
   是**放大器本身**。所以历史上每一次开机，真正放声的那一刻 PA 早已不在总线上。
0240（读回插到 bit-bang 之前）是唯一还活着的读路：`ampread pre-bitbang addr=0x34 reg=0 r=0
val=0x2032`——AP 第一次真读到 AW882xx。下一步是 0241 `sweetnobb=1`（整段 bit-bang 不跑）。
判据表在 `docs/audio-closeout.md` 最后两节。

三个开关的来历（不要凭印象改）：`sweetshmhi=1` 把 SHM_MAP 地址高字当 **stream index**
发（不是地址位）；`sweetshmflag=4` **不是** offset 模式：厂商代码里 `property_flag = 1 << (mem_pool_id-1)`，
pool=3 必然得 4（2026-11-04 由 `libar-gsl.so:0x186fc→0x18710` 反汇编确认）。当年"只有在
`sweets1ap=3` 下才会被 DSP 真正读通"是实测现象，保留，但与 flag 语义无关；`sweetampfirst=1` 把 PA 的配置挪到开图之前——历史上顺序是反的，
所以任何"没出声"的观测在 0233 之前都不成立。

**B1 判决（2026-11-03，b1+b2 两次开机）——iOS 会话常驻但不出流。**
两次都有 U0 全绿 + 真输入（b2：99 条 touch、21 次音量键），但音频域**零新增**：
控制器 selector 只有 `sel=0`/`sel=4`，`sel=2/3/5` 计数为 0；Actuator 的重复 map 仍 4 次没停；
`IOK mediaserverd` 埋点在跑（407 块）只见匹配我们的壳。
（`AppleAOPButtonHapticsServiceUserClient sel=1` 每 boot 都是 2 条，是基线，不是新增。）
另有一个**独立阻塞**：b1/b2 都没到 GUI，ring 结尾是 launchd 的
`Too many corpses being created`（dasd + 三个 TTS 宿主，sig 6），
没有 `PSCI SYSTEM_RESET`、没有 panic ⇒ "自己重启"不是我们的复位路径。
⇒ 主攻按预登记转 ②（不依赖 iOS 行为），判决表在 `docs/audio-closeout.md` 末尾。

② 现在按预登记**止损关闭**，写下的是那句话本身：

> **内核侧（数据面 / SHM / GPR / 图开-准备-启动的厂商帧形）与 iOS 侧（mediaserverd 常驻、
> 开我们的壳、`GetControlValue` 在问）都已经在场，"手机真的出声"仍未证。**

理由是逐条回源读出来的（细节与撤回记录在 `docs/audio-closeout.md` 末尾两节）：
厂商自己的 GRAPH_OPEN 包声明的长度就是 **2712**（10-27 抓包 `p9=0xa98`），所以
"把 12 字节图头剥掉是我们的 bug"这个说法**作废**；`sweetvgoff=0/len=2712` 也早跑过
（`ring-o3-vgraph0` 的 cmdline 里有）。五次 vendor4 开机里 **一次打印 `no response`、
三次在发出那一行之后日志就结束了**（没有 rsp）——那是 `sweet_glink.c:6260` 注释里的
run115 挂死形态，不是被拒。`images/e1-vgraph2724.img` 留着但**不启动**（它的先验来自
我已撤回的那条前提，失败模式又是要长按电源才恢复）。

还有一条把"再抓一次厂商正文"也堵上的实测：这台 ROM 的 root **装不上 tracefs**
（`kprobe_events` 读写全 EACCES，连 `setenforce 0` 都被拒，`CapEff` 是全能力位、
`u:r:su:s0`；`uname -r=5.10.226-android12-9`，`ro.soc.model=SM7475` 确认是 marble 这颗，
`ro.build.fingerprint=OnePlus/PKG110/…:15` 是改过的 props，别当型号判据）。
替代办法 `scripts/apm_harvest.py --heapscan`（不开探针、不放音，按
`{graph_size, num_subgraphs}` 头 + 体内 GRAPH_SIG 个数**验真**，先用已知-good 正文当正对照验过）
扫了 5 个音频域进程共 50 MiB 堆，**一个经验证的图头都没有**——包含刚跑完 agmplay
真会话之后。诚实边界：这个检法认的是 48 字节（`size=0x30`）那种子图属性块，
若当前图换了小节长度会漏，这一条没测，不许写成"堆里确实没有"。

**22:0x 更新（用户授权的那一次开机已用掉）**：`sweetvgraph=2` 发 2960 自洽容器
（`logs/ring-v2960.txt`，2640 行）。两条不依赖基线的读数：

- **X3 成立、X4 排除**——`poll beat` 走满 25 拍到 `i=4995`，随后 `no response`，
  环继续跑到 `===== done =====` 并自己 `PSCI SYSTEM_RESET` ⇒ **AP 活着、DSP 不回**，
  不是挂死。这是 ② 第一次能把这两种读法分开。
- `vgraph-post` 的 **cb5 由 `FSR=0x400` 变成 `FSR=0x408`、`FAR=0x1ffe8000`**
  （正是 `sweets1va=` 的区域基址）。**但这不能记成"正文内容问题"**：同样故障在盘上共 6 次，
  另 5 次（`audio-coldamp / audio-hi1 / audio-pace-hi1 / rtc-batt / s1ap11`）**只发了我们自己的
  `vp-wr`**，而 `tone done` 正常的好开机里 **0 次**。

⇒ ② 的未闭合点因此从"图正文/变体"移到：**同一片区域，`DATA_BUFFER` 读能成
（`done status=0`），`GRAPH_OPEN` 只发描述符去读就故障**。差别候选（句柄/池不同
`0xb0d7f408` vs 厂商 `0xb0d890e8`、区域偏移语义 `w6`、读发生在建表之前）**都还没测**。
按我自己 X5 那格的预定动作：**不再动正文**。

红线（实测，判据设计要用）：**SHM 形态的 GRAPH_OPEN 一发出，同一 boot 之后连
`status=0` 的 `vp-wr` 也不再被应答** ⇒ "同版必须带数据面基线四行"这类门与该形态互斥，
0246 的 X0 就是踩了它（记录为判据设计缺陷，不是仪器没生效）。

**注意**：`sweetsmp=1` 是测试开关，采样 20 秒后主动复位；**默认 cmdline
里没有它**，正常启动不会自己重启。


### 3.9 2026-11-04 追加：今天验证过的可复现命令

三条轮子，都是 `fastboot boot`（纯 RAM，不写任何分区）。共同前提：

```bash
export D22AP_ADT=$HOME/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt
export ADSP_FW_DIR=$HOME/Desktop/marble-ios/work/adsp-fw
cd $HOME/Desktop/marble-ios/work/xnu-gh
```

| 用途 | 构建 | 启动 | 取日志 |
|---|---|---|---|
| **震动/交互轮**（停在 iOS 让人按电源键） | `KEEPALIVE=1 bash scripts/mkimage.sh sweethap=1` | `SERIAL=2aa26095 bash scripts/boot_interactive.sh images/shim-iosroot-adsp.img` | 人重启到 TWRP 后 `SERIAL=2aa26095 ./scripts/pull_logs.sh <label>` |
| **音频已知-good 配置**（b1/L/M 同一条，569 字节） | `bash scripts/mkimage.sh sweetexp=15 sweetbufs=30 sweetpcm=1 sweets1=1 sweetiova=1 sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 sweets1ap=3 sweetampfirst=1 sweetaudioprobe=1` | `SERIAL=2aa26095 bash scripts/boot_one.sh images/<img> <label>`（自动复位+自动拉 ring） | 同左（boot_one 内置） |
| **零补丁仪器轮**（用户态 IOKit 查询追踪） | 上面任一 + 环境变量 `SWEETIOLOG=mediaserverd,backboardd`（**替换** base 里的 `sweetiolog=-`，不是追加；boot-args 预算 590） | 同上 | 同上 |
| **OOB SET_CFG 探针**（D/E/F/G 轮，2026-11-05，cmdline **583 B**） | 音频已知-good 那条**原样**，末尾追加 `sweetoobcfg=1`（**不带** `sweetcccfg`，一次一个容器） | 同上 | 同上；取完日志**先核 `sweettime` 与本次镜像一致**再打分。**探针每轮只发一发**；界内地址（如 `0x1000`）下数据面 = `data_buffer send` 31 + `done status=0` 4，越界/不可服务地址会把数据面打死 ⇒ 用它当"仪器 vs 地址"的分辨读数 |
| **取产物** | `bash scripts/fetch_kernel.sh`（**已修**：`kernel-latest` 旁边躺着一个陈旧的 `build-error.txt` 且排在前面，旧代码"页面上第一个 sha256"拿的是它的摘要，会一直误判"没出新内核"；现在按资产名配对取摘要） | — | — |

**boot-args 必须逐条在打包产物里核对**（不是只核 cmdline 打印）：

```bash
LC_ALL=C grep -ac "sweetbufs=30" images/<img>          # 必须为 1，且值没被 607 字节字段切半
LC_ALL=C grep -ac "SWEET MARK: dtprop " images/<img>   # 补丁是否真进了产物
```

注意两类假信号：对镜像做**裸字符串 grep 证明不了启动参数**（例如 `sweetaopval` 命中 1 次，
其实是 kernel 里 `PE_parse_boot_argn("sweetaopval")` 的常量串）；`%#x` 打印 0 时输出 `0` 而不是
`0x0`，数故障值要用 `=[0-9a-fx]*` 而不是 `0x[0-9a-f]*`。

本地分析产物（都不需要开机，已从 /tmp 挪出来防清理）：

```
work/phone-acdb/*.acdb        6 份本机 ACDB（MTP/CDP/QRD_ukee、IDP_UPD、QRD_diwali×2）
work/phone-libs/libgsl.so     /vendor/lib64/libgsl.so，用来反汇编 GDC/SET_CFG 组包
work/xnu-gh/tools/acdb_graph_rebuild.py / acdb_size_scan.py    ACDB 走链工具
```

## 四、文档索引

| 文件 | 内容 |
|---|---|
| `docs/status-log.md` | 按时间追加的完整实验日志（最新结论先看这里） |
| `docs/audio-closeout.md` | 音频收尾：SHM_MAP 高字、报文/模块对表、S1 叶子 AP 与 PA 排序，以及 ①②③ 三次开机各自的判决表（J1–J6 / I1–I6 / K1–K6） |
| `docs/audio-hal-bridge.md` | **iOS 侧出声还差什么**：已复核的十条事实、四条路线排序、最便宜的判别实验（空壳 Speaker 看有没有 IOServiceOpen）、可数差距清单；震动为何同线（§7） |
| `docs/smmu-dead-ends.md` | apps-smmu 的寄存器地图与已证伪/已修正的路线 |
| `docs/gpu-probe.md` | kgsl-smmu 探针、组合扫描结果、CB 页墙 |
| `docs/speaker-path.md` | 扬声器 = TERTIARY TDM + 2×AW882xx（无 WSA） |
| `docs/handoff.md` | 常用命令、boot args 速查 |
| `docs/required-assets.md` | 需要的 Apple 资产及来源/法律说明 |

## 五、下一步

1. **电池**：抓厂商 pmic_glink 的收发（`dumpsys battery` 时），把属性
   ID/响应偏移对上；接上后 `SweetBattery` 会自动用 `sweet_batt_get()`。
2. **RTC**：找 ADSP 的 RTC 服务（或 pmic_glink 里的 RTC client），在 ADSP
   侧设置时间；AP 直接写 SPMI 已被证明不通。
3. **启动时间**：kernel 到 launchd 约 39 秒（4 核/8 核一样），需要打点找
   最大的一段；E/P 调度以后再说（需要 AMP/CLPC）。
4. **音频（主线）**：那 18 KB **不在机内 ACDB 里**（已量完），所以现在做两件不花开机的事：
   (a) 反汇编 `work/phone-libs/libgsl.so` 求 GDC/自定义配置的记录头与 SET_CFG 组包规则；
   (b) 重跑 N 轮（0251 v3 已修好我那个导致 panic 的重入守卫）量 mediaserverd 在
   `IODeviceTree:/product` 上到底读哪些属性。
