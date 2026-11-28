# 从这里开始（marble / Redmi Note 12 Turbo，XNU 8796 + iOS 16）

> 当前状态看 [`STATUS.md`](STATUS.md)，编年史看 [`chronology.md`](chronology.md)，
> 操作守则看仓库根 [`AGENTS.md`](../AGENTS.md)。
> 本文件只保留**当前验证过**的构建、启动与验证配方；旧实验参数不在这里复述。

## 1. 目标与边界

把 Redmi Note 12 Turbo（marble / SM7475）跑到 iOS 16.7.16 用户态。
启动方式只有一种：**shim + `fastboot boot`，纯 RAM 运行**。

- 不刷 boot、不写 userdata、不改 GPT；
- 不主动 collapse GPU；
- 一次开机一个会话；
- 每次开机前完成本地验证，判据表先写死再开机。

## 2. 你需要

- 一台 bootloader 已解锁的 marble（Redmi Note 12 Turbo，SM7475）；
- macOS + `adb` / `fastboot` / Python 3；
- Android NDK（设置 `ANDROID_NDK_HOME`）；
- Apple 的 D22AP 设备树：`research/ipsw-x/d22ap.adt`，或设置 `D22AP_ADT`；
- 手机 root，用于抽取 ADSP 固件；
- 已写入 userdata 尾部的 HFS+ iOS 用户态镜像；
- 自备文件的来源与许可见 [`required-assets.md`](required-assets.md)。

## 3. 构建与启动

```bash
export D22AP_ADT=$HOME/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt
export ADSP_FW_DIR=$HOME/Desktop/marble-ios/work/adsp-fw
cd $HOME/Desktop/marble-ios/work/xnu-gh

# 1. 取 CI 产物，校验 sha256，并把 CNTP_CTL_EL0 改成 NOP
GH_REPO=<你的用户名>/ios-port-marble ./scripts/fetch_kernel.sh

# 2. 从手机抽 ADSP 固件
SERIAL=2aa26095 ./scripts/extract_adsp_fw.sh

# 3. 生成 RAM-only 启动镜像
D22AP_ADT=$D22AP_ADT ADSP_FW_DIR=$ADSP_FW_DIR ./scripts/mkimage.sh

# 4. 启动（不写任何分区）
SERIAL=2aa26095 ./scripts/boot_ram.sh images/shim-iosroot-adsp.img

# 5. 取回本轮 ring 日志
SERIAL=2aa26095 ./scripts/pull_logs.sh run1
```

### 交互轮 / 日志轮

```bash
# 停在 iOS 里等人操作
KEEPALIVE=1 bash scripts/mkimage.sh sweethap=1
SERIAL=2aa26095 bash scripts/boot_interactive.sh images/shim-iosroot-adsp.img

# 自动复位并自动拉日志
bash scripts/mkimage.sh <args...>
SERIAL=2aa26095 bash scripts/boot_one.sh images/<img> <label>
```

## 4. 当前验证过的能力

| 能力 | 状态 | 说明 |
|---|---|---|
| 启动到 SpringBoard / 设置 | ✅ | CPU 软件渲染，无 GPU 驱动 |
| 触摸 / 按键 | ✅ | Goodix GT9916R over SPI；多指与负载体验仍有限制 |
| UFS / HFS+ root | ✅ | root 在 userdata 尾部；f2fs 重叠风险见 `docs/08-dual-boot-plan.md` |
| 8 核 SMP | ✅ | `gicrstride=0x40000`；发热偏大 |
| 电池 | ✅ | 本机电流为负 = 充电、为正 = 放电 |
| 关机 | ✅ | userland 关机会真正断电 |
| ADSP 控制面 / 数据面 | ✅ | `GRAPH_OPEN/PREPARE/START`、`DATA_BUFFER done status=0` |
| Wi-Fi 只读 handoff + PCIe 端点识别 | ✅ | 第 74 轮；不代表联网 |
| 音频出声 | ⏸ 未证 | 音频暂停在 73/120；0298 只完成离线验证 |

## 5. 音频数据面参考配置（当前暂停，不要自动重开）

> 音频已暂停。下面只保留数据面已知-good 的参数形状，**不是下一轮启动指令**。
> 重新开启前必须重新构建完整内核与镜像，并通过所有本地门。

```bash
D22AP_ADT=$D22AP_ADT CPUS=8 GPU_PROBE_LEVEL=0 \
  ./scripts/mkimage.sh sweetbufs=30 \
  sweetpcm=1 sweets1=1 sweetiova=1 \
  sweets1va=0x1FFE8000 sweetnolpass=1 sweetshmhi=1 sweetshmflag=4 \
  sweets1ap=3 sweetampfirst=1
```

数据面成立必须同时看到：

```text
SWEET MARK: s1ap leaf=0x7c1
SWEET GLINK: graph_start rsp … status=0
SWEET I2C: geni amp 0x34 r=0 id=0x2032 …
SWEET GLINK: data_buffer: dst=0x40bd done status=0
```

注意：

- `sweetshmhi=1` 把 SHM_MAP 地址高字当 **stream index** 发，不是地址位；
- `sweetshmflag=4` 不是 offset 模式：`property_flag = 1 << (mem_pool_id-1)`，pool=3 得 4；
- `sweetampfirst=1` 把 PA 配置挪到开图之前；
- `sweetampverify=1` 的 GENI 读回有已知两层坏（pad mux 被 bit-bang 切走、芯片 NACK），不要重复踩。

## 6. Wi-Fi 当前入口

```bash
# 只读 handoff survey（0299，已通过第 74 轮）
bash scripts/mkimage.sh sweetwifi=1
SERIAL=2aa26095 bash scripts/boot_one.sh images/wifi-handoff-0299-20261120.img wifi-survey

# 0302/0303 的整机集成仍在进行，尚无新的可启动镜像
```

详细状态、门与失败关闭规则见 [`wifi-foxlinne-port.md`](wifi-foxlinne-port.md)。
**没有端点 marker 不进 DMA；没有 owner 授权不放 BME；不能照抄参考机 bank/slot。**

## 7. 开机前验证

```bash
# CNTP_CTL 必须恰好 NOP 1 处
# boot-args ≤ 590 字节，超长 mkimage.sh 直接失败
LC_ALL=C grep -ac "sweetbufs=30" images/<img>
LC_ALL=C grep -ac "<本补丁的 marker 字面量>" images/<img>
LC_ALL=C grep -ac "sweettime=<本轮值>" images/<img>

# 拉完 ring 先对身份
grep -m1 -o "sweettime=[0-9]*" logs/ring-<label>.txt
```

两类假信号：

- 裸 grep 到 flag 名不等于它在 cmdline 里（可能命中 `PE_parse_boot_argn` 的常量串）；
- `%#x` 打印 0 时输出是 `0` 不是 `0x0`，数故障值要用 `=[0-9a-fx]*`。

## 8. 文档索引

| 文档 | 用途 |
|---|---|
| [`STATUS.md`](STATUS.md) | 当前状态、预算、下一道门 |
| [`chronology.md`](chronology.md) | 唯一编年史 |
| [`audio-closeout.md`](audio-closeout.md) | 音频当前判决与证据索引 |
| [`audio-hal-listen.md`](audio-hal-listen.md) | 0293→0298 / R1 合并记录 |
| [`audio-hal-bridge.md`](audio-hal-bridge.md) | iOS 音频架构与 ABI |
| [`wifi-foxlinne-port.md`](wifi-foxlinne-port.md) | 当前 Wi-Fi 移植 |
| [`gpu-status.md`](gpu-status.md) | GPU 当前判决 |
| [`haptics-abi-2026-10-30.md`](haptics-abi-2026-10-30.md) | 触感 ABI 与 gate |
| [`smmu-dead-ends.md`](smmu-dead-ends.md) | 已证伪的 SMMU 路线 |
| [`sweetlog.md`](sweetlog.md) | ring 日志布局与提取 |
| [`required-assets.md`](required-assets.md) | 自备文件与许可 |
