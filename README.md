# marble XNU —— iOS 16 内核跑在 Redmi Note 12 Turbo 上

> **音频/SMMU 接手必读：** [`docs/smmu-dead-ends.md`](docs/smmu-dead-ends.md) ——
> 已经证伪的 SMMU 做法、正确的寄存器地图、以及“别再拿 DATA_BUFFER 当探针”的纪律。

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
| 音频（TDM → 功放） | 🔴 **卡在"DSP 读主机内存"**：只要 DSP 真的去读共享内存，整机在几毫秒内冻结（详见下文） |
| 电池（pmic_glink） | 🟡 协议通、字段待校准 |
| 震动（PM8350B） | ✅ 电源键触发，真机验证 |
| Wi-Fi / BT / GPU | ⬜ 未开始 |

## 音频当前卡点（一句话）

控制面全通，**所有成功的命令都有一个共同点：DSP 不需要读内存**（参数内联在
GPR 包里）。一旦让 DSP 去读主机上的共享内存——DATA_BUFFER，或把 GRAPH_OPEN
改成厂商那种"图放共享内存"的形态——整机就在几毫秒内冻结，且不是一次可记录的
fault。

已确认**不影响**结果的四个维度：apps-smmu 配置有无、S1 页表正确与否、
`CFCFG` 的 stall/terminate、buffer 物理地址。完整证据、失败记录和下一步见
**[docs/audio-status.md](docs/audio-status.md)**（结论快照）、
**[docs/status-log.md](docs/status-log.md)**（逐轮时间线）、
**[docs/handoff.md](docs/handoff.md)**（上手流程）。

厂商 GPR 报文格式见 **[docs/vendor-gpr-packets.md](docs/vendor-gpr-packets.md)**，
抓包原始数据在 `docs/traces/`，解码用 `scripts/parse_gpr_trace.py`。

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
xnu/patches/               XNU 的全部移植补丁（含音频、UFS、触摸、USB…）
shim/                      RAM-only 引导 shim（AArch64 裸机 + boot.img v4 打包）
scripts/                   取内核 / 打包 / 启动 / 拉日志 / 抽固件
docs/                      音频状态、TDM、厂商图、SWEETLOG 等技术文档
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

## 音频调试现状（详细版见 docs/audio-status.md）

- 扬声器走 **TDM-LPAIF-RX-TERTIARY**（96 kHz / 32 bit），不是 CODEC_DMA。
- 厂商播放图 `0xb0000002` 已被提取并按 wire 格式重建；内联 GRAPH_OPEN、
  全部 TDM/AW 配置、PREPARE、**START 均返回 0**。
- 卡点：AP 侧 `SHM_MAP` 返回 status=1（连 ADSP 自己的 carveout 也失败），
  因此 PCM 数据无法喂给 DSP；同时 AW882xx 的 I2C（i2c@880000 / 0x34、0x35）
  在 XNU 下无 ACK（Android 下正常，chipid 2032）。
- 下一步与待验证假设都记录在 `docs/audio-status.md`。

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
