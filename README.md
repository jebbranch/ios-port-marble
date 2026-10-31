# marble XNU —— iOS 16 内核跑在 Redmi Note 12 Turbo 上

> **音频/SMMU 接手必读：** [`docs/smmu-dead-ends.md`](docs/smmu-dead-ends.md) ——
> 已经证伪的 SMMU 做法、正确的寄存器地图、以及“别再拿 DATA_BUFFER 当探针”的纪律。
>
> **GPU 接手必读：** [`docs/gpu-probe.md`](docs/gpu-probe.md)（v1–v27 全部真机读数与
> 厂商 kprobe trace）、[`docs/gpu-gpt-question.md`](docs/gpu-gpt-question.md)（当前卡点的
> 完整问答稿）、[`docs/gpu-feasibility.md`](docs/gpu-feasibility.md)（可行性盘点）。

把 Apple 开源的 **XNU 8796.141.3**（iOS 16 同期内核）跑到
**Redmi Note 12 Turbo（marble / SM7475 / Snapdragon 7+ Gen 2）** 上。
启动方式是 **shim + `fastboot boot`，纯 RAM 运行**：不刷 boot、不写
userdata、不改 GPT。测试结束内核自己复位，手机回到 TWRP/Android。

> 研究项目，仅供学习与实验。不分发 Apple 固件/用户态镜像；
> 不要把这套东西当作日常可用的手机系统。

## 当前状态

| 子系统 | 状态 |
|---|---|
| XNU early boot（16K 页 / GICv3 / vtimer / framebuffer） | ✅ |
| UFS 读写、HFS+ root（userdata 尾部） | ✅ |
| 触摸 / 按键 / RTC | ✅（触摸有已知限制） |
| USB（枚举 / 部分传输） | 🟡 |
| ADSP（PAS + GLINK + AudioReach APM） | ✅ GLINK 通、图可 open/prepare/start、TDM 参数全 status 0 |
| 音频（TDM → 功放） | 🟡 控制面全通；`SHM_MAP` 高字修正后不再挂总线，卡在 **DSP 不消费 DATA_BUFFER**（详见下文） |
| 电池（pmic_glink） | 🟡 协议通、字段待校准 |
| 震动（PM8350B） | 🟡 电源键触发已验证；UIKit/CoreHaptics 的请求被 backboardd 用户态 gate 拦下，见 [docs/haptics-abi-2026-10-30.md](docs/haptics-abi-2026-10-30.md) |
| GPU（kgsl-smmu / GPU_CC / GMU） | 🔴 **进行中**：AP 能读写全部 GPU 寄存器、GMU 固件能装进 TCM；卡在 CX 域电源无法真正打通（详见“GPU 硬件启动”） |
| Wi-Fi / BT | ⬜ 未开始 |

## 音频当前卡点（一句话）

**控制面全通，卡在数据面**：厂商 `SHM_MAP` 的地址高 32 位不是地址，而是
**stream index（`msw=1`）**。我们此前一直发 0，才导致"DSP 一读主存整机冻结"；
新增 `sweetshmhi=1` 之后同一套 S1 配置不再挂总线，所有控制命令 status 0。
现在剩下的是 **`DATA_BUFFER` 发出去、DSP 不回 DONE**（`gfsr=0`，无 fault），
以及厂商 4 子图 blob 的 `GRAPH_OPEN` 无响应——属于配置完整性，不是硬件权限。

完整证据链、失败记录与下一轮假设见
**[docs/audio-closeout.md](docs/audio-closeout.md)**（最新结论）、
**[docs/audio-status.md](docs/audio-status.md)**（快照）、
**[docs/status-log.md](docs/status-log.md)**（逐轮时间线）、
**[docs/handoff.md](docs/handoff.md)**（上手流程）。
`docs/smmu-dead-ends.md` 里的寄存器数据仍有效，但"停止 apps-smmu 路线"的
旧结论已被 `audio-closeout.md` 推翻。

厂商 GPR 报文格式见 **[docs/vendor-gpr-packets.md](docs/vendor-gpr-packets.md)**，
抓包原始数据在 `docs/traces/`，解码用 `scripts/parse_gpr_trace.py`。

## GPU 硬件启动（进行中）

目标：把 Adreno 725v1（a730 家族、gen7 GMU）从 XNU 里点亮 —— 先 GMU/CM3，
再 CP、HFI，最后才是 Metal。启动链是 ABL → 自写裸机 shim → XNU，
**没有 Android，也没有任何厂商 GPU 驱动参与**。

### 已确证（全部真机回读）

- **GPU 的 SMMU 是独立的一颗**（`kgsl-smmu @0x3da0000`），不受 apps-smmu 那套
  hypervisor 限制：`IDR0=0x4c017e18`（24 stream）、`IDR1=0x40000016` →
  **CB0 = 0x3dc0000**（不是 apps-smmu 的 0x80000，早期结论已纠正）。
- `S2CR=BYPASS` 会被改写成 FAULT，但 **stage-1 + CB0 可写可回读**；SMR 可写；
  AP 侧确实能配置这套 SMMU。
- GPU_CC 基址 `0x3d90000`：`CX_GDSCR@0x9108`（bit31 PWR_ON / bit1 HW_CONTROL /
  bit0 SW_COLLAPSE）、`ahb@0x911c`、`cx_gmu@0x913c`、`hub_cx_int@0x9148`、
  `smmu vote@0x7000`、gmu RCG `0x9318/0x931c`、hub RCG `0x93ec/0x93f0`。
- **GMU 寄存器是 kgsl regmap 的“字偏移”**，实际字节：GMU base `0x3d68000`，
  ITCM `0x3d6d000`、DTCM `0x3d79000`、`CM3_SYSRESET 0x3d7e000`、
  `FW_INIT_RESULT 0x3d7e070`、`HFI_CTRL_INIT 0x3d7e618`。早期按字节偏移访问
  是错的（读到 `0xdeafbead` 或挂总线）。
- GMU 固件块格式 `{u32 addr; u32 size; 0xdeadbeef; 0xdeadbeef}`（magic 大端），
  `gmu_gen70000.bin` 可正确解析并写入 ITCM/DTCM，**逐字节回读一致**。
- **CM3 真的会执行**：shim 版实验里放复位 200 ms 再拉回，XNU 早期启动必挂；
  瞬时放/拉（CM3 来不及执行）则正常 ⇒ 这 200 ms 里 CM3 在跑，并留下持久副作用。
  放复位后 AP 对 GMU 寄存器的访问会挂（AHB 交给 GMU），所以轮询 `FW_INIT` 不能
  在放复位的同一个核上做。
- **厂商完整上电序列已抓到**（kprobe，8.47–8.53 s）：`lcx.lvl=7`、`lmx.lvl=5`、
  `CX_GDSCR=0x228800`（EN_REST_WAIT=2/EN_FEW_WAIT=2/CLK_DIS_WAIT=8/RETAIN_FF）、
  gmu RCG → cx_gmu → hub RCG → hub_cx_int → smmu vote → GCC `0x81010/0x81018`
  → `gpu_cc_ahb=1` → `mxc.lvl`。**全程没有 collapse→restore 周期。**

### 当前卡点

1. **CX 域电源无法真正打通**。ABL 交接时 `PWR_ON=1`；我们做一次
   “清 HW_CONTROL → 置 SW_COLLAPSE → 再清”之后永远回不来（`0x228801`）。
   厂商 trace 里根本没有这一步，所以 **collapse 已从默认流程移除**
   （`GP_PWR` 现在是显式实验开关，不再是 `gpu_pwr.sh` 的默认参数）。
2. **RPMh ARC 投票“完成”但不生效**。`apps_rsc` drv-2 @`0x17a20000`、
   `tcs-offset=0xd00`、`CFG=0x800c8104`（TCS=8/NCPT=16，非 solver 模式）；
   ACTIVE TCS + AMC 投 `lcx/lmx/mxc` 后命令状态 `ISSUED|COMPLETE`、TCS 回 idle，
   但 CX 依旧回不来。命令的 `msgid=0x10108`（LEN=8|RESP_REQ|WRITE）已与
   `rpmh-rsc.c` 逐位核对，**格式没有问题**，剩下的是资源语义/仲裁/因果关系。
3. 下游现象（`gpu_cc_ahb` 不 latch、GX 不开、CM3 无 AO 活动、HFI 无消息、
   SMMU 无 fault）统一归因到上面这一条根：**平台电源没有真正就绪**。

### 正在做的实验（工具已就位）

| 开关 | 作用 |
|---|---|
| `sweetgsnap=1` | 在任何 GPU 写入前只读快照：GDSCR/CFG_GDSCR、GX、各 branch/RCG、GMU BCR、RBBM、SMMU ID/S2CR、CM3/FW_INIT/HFI 与 TCM |
| `sweetrail=1` | SPMI 扫 SID 0..3 × `0x1400..0x1f00` 的 SMPS 块（VOUT/MODE/EN），物理验证轨道 |
| `sweetgmuboot=1` / `sweetgmurel=1` | 内核侧 GMU 启动 / 委托 shim 的牺牲核放复位（带 DDR 标记：`0xdead0001/2` + 心跳，区分“写没完成”与“之后才卡”） |
| `GP_RAIL=1` | shim 侧只投 `lcx=7/lmx=5/mxc=6` 三个 ARC，不动 GDSCR（用于投票/不投票同机对照） |

顺序：先跑**纯基线**（不投票、不 collapse、不进 GMU boot）拿 ABL 交接快照 +
PMIC 读数；再跑同一镜像序列的投票版，差分回答“投票是否落地”。

```bash
# 纯基线：快照 + 轨道读数，不开 GP_PWR、不进 GMU boot
EXTRA_ARGS="sweetgsnap=1 sweetrail=1" ./scripts/gpu_pwr.sh build "GP_SURVEY=0"
./scripts/gpu_pwr.sh run
```

### 还没开始的部分

RPMh 完整实现（solver/batch/ARC 语义）、互连/BCM 带宽投票、PMIC 轨道接管、
GPU PAS（ZAP：`pas_init_image/mem_setup/auth_and_reset`）、GMU HFI 协议与 CP
固件、a725 的 HW-init 表、gpucc 时钟树完整化、Metal 前端（metal2vulkan/Turnip）。
顺序与依赖见 [`docs/gpu-probe.md`](docs/gpu-probe.md) 末尾的“还差什么”盘点。

## 震动（触感）现状

内核侧链路（Callan → `AppleHapticsSupport` → PMIC HAP520）已打通，电源键触发
真机验证通过；**UIKit / CoreHaptics 的请求被 `backboardd` 的用户态 gate 拦下**，
内核各路径都看不到 IPC。这条线 2026-10-30 起暂停，恢复前先读
**[docs/haptics-handoff-2026-10-30.md](docs/haptics-handoff-2026-10-30.md)**
（交接状态、纪律与 P2 任务），ABI 细节见
**[docs/haptics-abi-2026-10-30.md](docs/haptics-abi-2026-10-30.md)**。

约束不变：只允许 `fastboot boot`；不得用坐标/selector/内核定时器模拟脉冲，
测试必须来自真实 UIKit、CoreHaptics、ButtonResolver 或系统 HID 事件。

## 仓库不包含什么（重要）

本仓库是公开的**纯代码、补丁与文档**仓库，**不包含任何 Apple、Qualcomm
或小米的二进制文件**。完整跑通需要你自备：

| 文件 | 来源 | 说明 |
|---|---|---|
| **D22AP 设备树** `d22ap.adt` | 你自己的 iPhone X（D22AP）iOS 16.7.16 IPSW | Apple 版权内容，**不可再分发** |
| **ADSP 固件** `adsp2.mdt` + `adsp2.bNN` | 你自己的手机 `/vendor/firmware/` | Qualcomm / 小米专有固件 |
| **HFS+ iOS 用户态镜像** | 你自己的 iPhone X IPSW / 设备 | Apple 版权；仓库不提供、也不提供获取渠道 |
| Android NDK / Xcode+KDK / adb | Google / Apple 公开下载 | 工具链，公开可获取 |

来源、提取命令、放置位置与许可说明详见
**[docs/required-assets.md](docs/required-assets.md)**。
**请不要把上述专有文件上传到本仓库或任何公开渠道。**

## 安全规则（请先读）

1. **只允许 `fastboot boot`**。禁止 `flash` / `erase` / `format` /
   `set_active` / `oem` 写命令；不要碰 GPT / persist / modemst。
2. 测试前确认电量 > 50%，并知道怎么进 fastboot（Power + Vol Down）和 EDL。
3. 只对**你自己的研究机**使用。本项目默认机器序列号 `2aa26095`。
4. 仓库不包含 Apple 的任何二进制；XNU 源码由 CI 从公开源构建。

## 仓库结构

```
.github/workflows/xnu.yml  GitHub Actions：构建 XNU 内核并发布 kernel-latest
xnu/patches/               XNU 的全部移植补丁（含音频、UFS、触摸、USB、GPU…）
shim/                      RAM-only 引导 shim（AArch64 裸机 + boot.img v4 打包）
scripts/                   取内核 / 打包 / 启动 / 拉日志 / 抽固件 / GPU 探针
docs/                      音频、SMMU、GPU、触感的状态与技术文档
docs/traces/               厂商 kprobe / GPR 抓包原始数据
```

## 从零构建与部署

### 0. 你需要

- 一台 bootloader 已解锁的 marble（本机：Redmi Note 12 Turbo，SM7475）。
- macOS（Apple Silicon 或 Intel 均可）+ `adb` / `fastboot` / Python 3。
- 构建 shim 需要 **Android NDK**（`ANDROID_NDK_HOME` 指向 NDK 目录）。
- 构建 ADT 需要 Apple 的 **D22AP 设备树**（从你自己的 iPhone X IPSW 提取，
  仓库不包含）：放到 `research/ipsw-x/d22ap.adt`，或设置 `D22AP_ADT`。
  提取命令见 `scripts/mkadt.py` 顶部注释。
- 手机端需要 root 才能抽取 ADSP 固件（本机用 SukiSU Ultra）。
- 一个已写入 userdata 尾部的 HFS+ 用户态镜像（Apple 内容，本项目不分发；
  分区布局见 `docs/sweetlog.md`，来源与风险见 `docs/required-assets.md`）。

> 以上自备文件的完整清单、来源与法律说明见 **docs/required-assets.md**。

### 1. 构建 XNU 内核（GitHub Actions）

Fork 本仓库后：

- 仓库 → **Actions** → `xnu` → **Run workflow**
  - `macos_version`: `13.5`
  - `kernel_config`: `DEVELOPMENT`
  - `machine_config`: `SWEET`
- 等 30–60 分钟。成功后 release `kernel-latest` 会滚动更新，
  产物 `kernel.development.sweet`。

> 不提供本地编译脚本：xnu-8796 需要 Xcode 15.x 的 `iig` 和对应的 KDK，
> Actions 的 `macos-14` 环境已经配好；本地环境不一致会出现 IORPC ABI 错误。

本地取回并打上本机需要的 CNTP_CTL 补丁：

```bash
GH_REPO=<你的用户名>/ios-port-marble ./scripts/fetch_kernel.sh
# 输出 out/kernel.adsp（已校验 sha256，并把 MSR CNTP_CTL_EL0 改成 NOP）
```

> 一定要用 `fetch_kernel.sh` 处理过的内核。直接 boot 原始产物会因
> EL1 不可用的 `CNTP_CTL_EL0` 触发 “Undefined kernel instruction”。

### 2. 从手机抽取 ADSP 固件（不随仓库分发）

```bash
SERIAL=2aa26095 ./scripts/extract_adsp_fw.sh
# 默认写到 ./adsp-fw/（adsp2.mdt + adsp2.bNN）
```

### 3. 生成 RAM-only 启动镜像

```bash
D22AP_ADT=/path/to/d22ap.adt ADSP_FW_DIR=$PWD/adsp-fw ./scripts/mkimage.sh
# 输出 images/shim-iosroot-adsp.img
```

内部流程：`mkfwblob.py` 打包固件 → `mkpayload.py` 生成 XNU payload →
`shim/build_xnu.sh` 编译 shim 并封成 boot.img v4。

### 4. 启动（不写任何分区）

```bash
SERIAL=2aa26095 ./scripts/boot_ram.sh images/shim-iosroot-adsp.img
```

内核跑完（cmdline 里 `sweetreboot=1`）会自己复位；手机通常落在
TWRP/recovery。

### 5. 取回日志

```bash
SERIAL=2aa26095 ./scripts/pull_logs.sh run1
# logs/raw/ring-run1.bin + logs/ring-run1.txt
```

原理见 **[docs/sweetlog.md](docs/sweetlog.md)**。

## 音频调试现状（详细版见 docs/audio-closeout.md）

- 扬声器走 **TDM-LPAIF-RX-TERTIARY**（96 kHz / 32 bit），不是 CODEC_DMA。
- 厂商播放图 `0xb0000002` 已被提取并按 wire 格式重建；内联 GRAPH_OPEN、
  全部 TDM/AW 配置、PREPARE、**START 均返回 0**。
- 曾经的“DSP 一读主存就整机冻结”已定位：`SHM_MAP` 的地址高字是 stream
  index，厂商发 `msw=1`，我们发 0。加 `sweetshmhi=1` 后不再挂总线。
- 现在卡在 **DATA_BUFFER 发出后 DSP 不回 DONE**（`gfsr=0`、无 fault）；厂商
  4 子图 blob 的 `GRAPH_OPEN` 无响应。AW882xx 的 I2C（`i2c@880000` / `0x34`、`0x35`）
  在 XNU 下仍无 ACK（Android 下正常，chipid 2032）。
- 下一步与待验证假设都记录在 `docs/audio-closeout.md`。

## 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `Undefined kernel instruction ... d51be228` | 用了未打 CNTP_CTL 补丁的内核，重新跑 `fetch_kernel.sh` |
| 启动后没有输出 | 用 `scripts/pull_logs.sh` 从 userdata 尾部读 SWEETLOG |
| `fastboot boot` 后设备停在 TWRP | 正常回退；直接拉日志即可 |
| 内核 panic 花屏 | 拍照记录 pc/lr/far + 寄存器；重启进 TWRP 拉上一轮日志 |
| GitHub 下载很慢 | 设置 `MARBLE_PROXY=http://127.0.0.1:7890` |

## 许可与声明

- 本仓库的补丁、shim 与脚本按仓库 LICENSE 发布。
- XNU 源码版权归 Apple，按其开源许可使用；构建过程从公开源拉取。
- Qualcomm/Xiaomi 固件、Apple 用户态镜像**不在仓库内**，需从你自己的设备提取。
- 仅供研究，不提供任何担保；因使用导致的设备损坏/数据丢失由使用者自负。
