# 必须自备的文件与来源

本仓库（public）**只包含代码、补丁和文档**，不包含任何 Apple、Qualcomm
或小米的二进制文件。下面列出完整跑通所需的自备内容、来源、提取方法和
许可说明。**请不要把这些文件上传到本仓库或任何公开渠道。**

| # | 需要的东西 | 来源 | 仓库为何不包含 | 获取方式 |
|---|---|---|---|---|
| 1 | **XNU 内核** `kernel.development.sweet` | Apple 开源的 **xnu-8796.141.3**（APSL 2.0），经 `blacktop/darwin-xnu-build` 构建 | 源码本身是公开的，但仓库不重复存源码；内核由 GitHub Actions 现场编译 | 见下文 1 |
| 2 | **D22AP 设备树** `d22ap.adt` | 你自己的 **iPhone X（D22AP）iOS 16.7.16 IPSW** 里的 `DeviceTree.d22ap.im4p` | Apple 版权内容，不可再分发 | 见下文 2 |
| 3 | **ADSP 固件** `adsp2.mdt` + `adsp2.bNN` | 你手机的 `/vendor/firmware/`（Qualcomm / 小米专有） | 专有固件，禁止再分发 | 见下文 3 |
| 4 | **HFS+ iOS 用户态镜像** | 你自己的 iPhone X iOS 16 IPSW / 设备上的系统分区 | Apple 版权内容；且写入 userdata 是独立的高风险步骤 | 见下文 4 |
| 5 | **Android NDK**（编译 shim） | Google 官方：<https://developer.android.com/ndk> | 公开可下载，仓库不重复存 | 安装后设 `ANDROID_NDK_HOME` |
| 6 | **Xcode 15.x + KDK 13.5**（仅本地编译 XNU） | Apple 开发者工具 | 需 Apple 许可；CI 已用 `macos-15` 自带环境 | 仅在你本地重编内核时需要 |
| 7 | **adb / fastboot** | Android Platform Tools（Google 公开下载） | 公开工具 | `brew install android-platform-tools` |
| 8 | **测试设备** | 你自己的 Redmi Note 12 Turbo（marble），bootloader 已解锁 | 设备不在仓库里 :) | 默认 `SERIAL=2aa26095` 只是作者的测试机，可改 |

## 1. XNU 内核（公开源码构建）

- 源码：Apple 开源的 **xnu-8796.141.3**（对应 macOS 13.5 / iOS 16 时代），
  许可证 **APSL 2.0**。
- 构建框架：[`blacktop/darwin-xnu-build`](https://github.com/blacktop/darwin-xnu-build)，
  workflow 里固定了 `DXB_SHA`。
- 本仓库的 `.github/workflows/xnu.yml` 在 GitHub 的 `macos-15` runner 上编译，
  产物发布到 rolling release **`kernel-latest`**。
- 本地取回并打本机需要的 CNTP_CTL 补丁：

  ```bash
  GH_REPO=<你的用户名>/ios-port-marble ./scripts/fetch_kernel.sh
  ```

  这会下载并校验 sha256，输出 `out/kernel.adsp`。

## 2. D22AP 设备树（来自你自己的 IPSW）

XNU 需要一个 Apple Device Tree 来提供设备身份（`/product`、机型、UI 形态
等元数据）。marble 的 ADT 以 **D22AP（iPhone X）** 的真实设备树为蓝本，
改写 GIC、内存、UART 等地址（见 `scripts/mkadt.py`）。

**来源**：你自己的 iPhone X（D22AP）iOS 16.7.16 IPSW 中的
`Firmware/all_flash/DeviceTree.d22ap.im4p`。该文件属于 Apple 版权内容，
**本仓库不提供、也不代为分发**。

参考提取方法（工具自备）：

```bash
# 方式 A：ipsw 工具（https://github.com/blacktop/ipsw）
ipsw extract --dtree <你拥有的 IPSW> -o ipsw-x
# 取出的设备树按工具输出命名，重命名为 d22ap.adt 即可

# 方式 B：项目早期的 remotezip 流程
python3 tools/remotezip.py <IPSW_URL> DeviceTree.d22ap.im4p ipsw-x
ipsw img4 im4p extract ipsw-x/Firmware/all_flash/DeviceTree.d22ap.im4p \
  -o research/ipsw-x/d22ap.adt
```

放置位置二选一：

- `research/ipsw-x/d22ap.adt`（`scripts/mkadt.py` 的默认位置），或
- 用环境变量指定：`D22AP_ADT=/path/to/d22ap.adt ./scripts/mkimage.sh`

没有这个文件时，`mkimage.sh` 会直接报错退出（宁可失败，也不生成缺
`/product` 的 ADT）。

## 3. ADSP 固件（来自你自己的手机）

音频 DSP（ADSP）需要手机自带的 `adsp2.mdt` + `adsp2.bNN` 分段固件。
这些是 **Qualcomm / 小米的专有二进制**，只允许在你自己的设备上使用，
**不得再分发**。

```bash
SERIAL=<你的序列号> ./scripts/extract_adsp_fw.sh
# 默认写到 ./adsp-fw/
```

脚本通过 adb 从 `/vendor/firmware/` 复制；没有 root 时通常也能读，
但建议用 root（本机是 SukiSU Ultra）。

## 4. HFS+ iOS 用户态镜像（来自你自己的 IPSW/设备）

内核只负责**挂载** userdata 尾部的 HFS+ 分区；分区里的 iOS 用户态文件
来自 Apple，**本仓库不提供镜像，也不提供获取渠道**。

- 位置：`userdata` 分区尾部；`scripts/mkimage.sh` 里的
  `ufsroot_lba=0x3603600`、`ufsroot_blocks=5243387` 是本测试机的几何值，
  换机器/换布局时必须改成你的值。
- **制作或写入这个镜像需要修改 userdata，属于独立的高风险步骤**，
  不在本仓库"RAM-only、不写分区"的流程内。请先做完整备份，并自行承担风险。
- 公开仓库里不要上传该镜像的任何部分。
- root 位于 userdata 尾部，而 f2fs 没有被缩小；研究机开 Android 后写满会覆盖 root。
  详见项目根 `docs/08-dual-boot-plan.md` §3。

## 5. 许可速览

| 内容 | 许可 / 状态 |
|---|---|
| XNU 源码（Apple） | Apple Public Source License 2.0 |
| `blacktop/darwin-xnu-build` | MIT |
| 本仓库的补丁、shim、脚本 | Apache License 2.0（见 `LICENSE`） |
| Apple IPSW 内容（D22AP ADT、iOS 文件系统） | Apple 版权，**不可再分发** |
| Qualcomm / 小米 ADSP 固件 | 专有，**不可再分发** |

本项目不包含任何绕过签名 / DRM 的工具；所有实验都应在你**自己拥有**
且 bootloader 已解锁的设备上进行。因使用本仓库导致的设备损坏或数据
丢失，作者不承担责任。
