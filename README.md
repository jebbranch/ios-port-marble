# marble XNU —— iOS 16 内核跑在 Redmi Note 12 Turbo 上

把 Apple 开源的 **XNU 8796.141.3**（iOS 16 同期内核）和 **iPhone X 的 iOS 16.7.16 用户态**
跑到 **Redmi Note 12 Turbo（marble / SM7475 / Snapdragon 7+ Gen 2）**上。
启动方式是 **shim + `fastboot boot`，纯 RAM 运行**：不刷 boot、不写 userdata、不改 GPT。
测试结束内核自己复位，手机回到 TWRP/Android。

> 研究项目，仅供学习与实验。不分发 Apple 固件/用户态镜像；
> 不要把这套东西当作日常可用的手机系统。

**当前主线是 Wi-Fi（0299–0303，总开机 74/120）；音频暂停在 73/120；GPU 第二优先。**
按时间的记录看 [`docs/chronology.md`](docs/chronology.md)，当前状态看
[`docs/STATUS.md`](docs/STATUS.md)，可复现配方看 [`docs/START-HERE.md`](docs/START-HERE.md)。

## 当前状态

| 子系统 | 状态 |
|---|---|
| XNU early boot（16K 页 / GICv3 / vtimer / framebuffer） | ✅ |
| 启动到 SpringBoard / 设置 | ✅ CPU 软件渲染 |
| 触摸 / 按键 | ✅（触摸有已知限制） |
| UFS 读写、HFS+ root（userdata 尾部） | ✅ |
| 8 核 SMP | ✅（发热偏大） |
| 电池 | ✅ 基本实现（电流为负 = 充电、为正 = 放电） |
| 关机 | ✅ userland 关机会真正断电 |
| USB（枚举 / 部分传输） | 🟡 |
| ADSP（PAS + GLINK + AudioReach APM） | ✅ GLINK 通、图可 open/prepare/start、TDM 参数全 status 0 |
| 音频（HAL / TDM / 功放） | ⏸ 暂停在 73/120；0298 离线修好 PRM 回包绑定，尚无新内核/镜像/开机 |
| Wi-Fi / 蓝牙 | 🚧 当前主线：第 74 轮只读 handoff 通过，确认 QCA6490/PCIe0 交接后保持关闭；端点识别尚未实机验证。0302/0303 尚未整机启动 |
| GPU（kgsl-smmu / GPU_CC / GMU） | 🚧 第二优先：旧 XNU 8796 线冻结，FoXLinne 上游 iOS 18 实现可参考，iOS 16 适配未开始 |
| 基带 / modem | ⬜ 未开实验；先决门是 306 MB `mpss_region` |

## 安全规则（请先读）

1. **只允许 `fastboot boot`**。禁止 `flash` / `erase` / `format` /
   `set_active` / `oem` 写命令；不要碰 GPT / persist / modemst。
2. 知道怎么进 fastboot（Power + Vol Down）和恢复路径。
3. 只对**你自己的研究机**使用。本项目默认机器序列号 `2aa26095`。
4. 仓库不包含 Apple 的任何二进制；XNU 源码由 CI 从公开源构建。

### 已知存储风险

root 镜像写在 userdata 尾部，但 **f2fs 没有被缩小，GPT 也没有改**。
研究机日常还要开 Android，因此 f2fs 写满会覆盖尾部 root。当前没有机制阻止它，
只能开机前重读 HFS+ 卷头签名与 free 块数，发现被覆盖就重新分块写尾部。
详见项目根 `docs/08-dual-boot-plan.md` §2。

## 仓库不包含什么

本仓库是公开的**纯代码、补丁与文档**仓库，不包含任何 Apple、Qualcomm 或小米的二进制文件。
完整跑通需要你自备：

| 文件 | 来源 | 说明 |
|---|---|---|
| **D22AP 设备树** `d22ap.adt` | 你自己的 iPhone X（D22AP）iOS 16.7.16 IPSW | Apple 版权内容，不可再分发 |
| **ADSP 固件** `adsp2.mdt` + `adsp2.bNN` | 你自己的手机 `/vendor/firmware/` | Qualcomm / 小米专有固件 |
| **HFS+ iOS 用户态镜像** | 你自己的 iPhone X IPSW / 设备 | Apple 版权；仓库不提供，也不提供获取渠道 |
| Android NDK / Xcode+KDK / adb | Google / Apple 公开下载 | 工具链，公开可获取 |

来源、提取命令、放置位置与许可说明详见 [`docs/required-assets.md`](docs/required-assets.md)。
**请不要把上述专有文件上传到本仓库或任何公开渠道。**

## 仓库结构

```text
.github/workflows/xnu.yml  GitHub Actions：构建 XNU、marker 门、发布 kernel-latest
xnu/patches/               XNU 的全部移植补丁（当前到 0303）
shim/                      RAM-only 引导 shim（AArch64 裸机 + boot.img v4 打包）
scripts/                   取内核 / 打包 / 启动 / 拉日志 / Wi-Fi 构建
docs/                      编年史、当前状态、音频/GPU/触感/Wi-Fi 文档
docs/traces/               厂商 kprobe / GPR 抓包原始数据
```

## 从零构建与部署

### 0. 你需要

- 一台 bootloader 已解锁的 marble（Redmi Note 12 Turbo，SM7475）。
- macOS（Apple Silicon 或 Intel 均可）+ `adb` / `fastboot` / Python 3。
- 构建 shim 需要 **Android NDK**（`ANDROID_NDK_HOME` 指向 NDK 目录）。
- 构建 ADT 需要 Apple 的 **D22AP 设备树**：放到
  `research/ipsw-x/d22ap.adt`，或设置 `D22AP_ADT`。
- 手机端需要 root 才能抽取 ADSP 固件。
- 一个已写入 userdata 尾部的 HFS+ 用户态镜像。

### 1. 构建 XNU 内核（GitHub Actions）

Fork 本仓库后：

- 仓库 → **Actions** → `xnu` → **Run workflow**
  - `macos_version`: `15`
  - `kernel_config`: `DEVELOPMENT`
  - `machine_config`: `SWEET`
- 等 30–60 分钟。成功后 release `kernel-latest` 会滚动更新，
  产物 `kernel.development.sweet`。

> 一定要用 `fetch_kernel.sh` 处理过的内核：它会校验 sha256，并把
> EL1 不可用的 `CNTP_CTL_EL0` 改成 NOP。直接 boot 原始产物会触发
> `Undefined kernel instruction`。

### 2. 从手机抽取 ADSP 固件

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

内核跑完（若 cmdline 带 `sweetreboot=1`）会自己复位；手机通常落在 TWRP/recovery。

### 5. 取回日志

```bash
SERIAL=2aa26095 ./scripts/pull_logs.sh run1
# logs/raw/ring-run1.bin + logs/ring-run1.txt
```

原理见 [`docs/sweetlog.md`](docs/sweetlog.md)。

## 文档入口

| 文档 | 用途 |
|---|---|
| [`docs/chronology.md`](docs/chronology.md) | 唯一编年史：从 10-21 那句「不可能」到 11-28 |
| [`docs/STATUS.md`](docs/STATUS.md) | 当前状态、预算、下一道门 |
| [`docs/START-HERE.md`](docs/START-HERE.md) | 当前验证过的构建与启动配方 |
| [`docs/audio-closeout.md`](docs/audio-closeout.md) | 音频当前判决与证据索引 |
| [`docs/audio-hal-listen.md`](docs/audio-hal-listen.md) | 0293→0298 / R1 的合并记录 |
| [`docs/wifi-foxlinne-port.md`](docs/wifi-foxlinne-port.md) | 当前 Wi-Fi 移植 |
| [`docs/gpu-status.md`](docs/gpu-status.md) | GPU 当前判决与原始日志索引 |
| [`docs/haptics-abi-2026-10-30.md`](docs/haptics-abi-2026-10-30.md) | 触感 ABI 与当前 gate |
| [`docs/smmu-dead-ends.md`](docs/smmu-dead-ends.md) | 已证伪的 SMMU 路线 |
| [`docs/required-assets.md`](docs/required-assets.md) | 自备文件与许可 |

## 参与项目

这是一个仍在进行中的研究项目，一个人踩不完所有的坑。欢迎：

- 手里有同款机器的朋友：实机复现、抓日志、读寄存器、验证补丁；
- 做 XNU / IOKit / 驱动移植的同学：提交补丁和代码评审；
- 有 Qualcomm 平台或 PMIC/RPMh/SMMU 经验的人：指出文档里的错误。

开 Issue 请贴：现象、`logs/ring-*.txt`、启动参数、镜像标识 `sweettime`。
发 PR 请改 `xnu/patches/`、脚本或文档，一个 PR 尽量只做一件事；
失败的尝试也请写进 `docs/`，下一个人才不会重走。

Issues and pull requests in English are welcome too.

## 许可与声明

- 本仓库的补丁、shim 与脚本按仓库 LICENSE 发布。
- XNU 源码版权归 Apple，按其开源许可使用；构建过程从公开源拉取。
- Qualcomm/Xiaomi 固件、Apple 用户态镜像不在仓库内，需从你自己的设备提取。
- 仅供研究，不提供任何担保；因使用导致的设备损坏/数据丢失由使用者自负。
