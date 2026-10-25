# HANDOFF — marble XNU 音频/电源/震动（2026-10-25 13:00）

给接手的人：**10 分钟读完这页就能继续。**
细节看 `CURRENT.md`（结论快照）和 `STATUS.md`（时间线 + 每次失败原因）。

## 0. 一句话现状

iOS 16 能在 marble 上 RAM-only 起来，**GUI 已经显示成功**（Siri 欢迎页）。
音频控制面全通，数据面卡在「DSP 读主存」这一步（详见 §2）；
电池协议打通但字段待校准；震动已真机验证（按电源键有反馈）。

## 1. 能跑的快速流程

```bash
cd ~/Desktop/marble-ios/work

# 1) 改驱动（权威副本）
vim sweet-audio/src/sweet_glink.c

# 2) 生成补丁 + 在复现树验证（必须，CI 失败只打 warning）
diff -u --label a/bsd/dev/sweet_glink.c --label b/bsd/dev/sweet_glink.c \
  ci-src/xnu-xnu-8796.141.3/bsd/dev/sweet_glink.c sweet-audio/src/sweet_glink.c \
  > xnu-gh/xnu/patches/0181-sweet-shm-system-ram.patch
( cd ci-src/xnu-xnu-8796.141.3 && git apply --check \
  ../../xnu-gh/xnu/patches/0181-sweet-shm-system-ram.patch )

# 3) 推 → CI 出 kernel-latest
cd xnu-gh && git add xnu/patches && git commit -m "..." && git push

# 4) 等构建 + 自动 RAM 引导 + 抓 ring
cd ../sweet-audio
MKARGS="sweetpcm=1" bash tools/watch_and_boot.sh <当前asset-sha256> <run#> "<内核里存在的marker>"
#   ⚠️ marker 必须是编译期字符串，不能是运行时 printf 出来的（如 "S2 CB52" 会被误判）

# 5) 看结果
grep -aE "SWEET SMMU|buffer probe|tone" logs/ring-run<run#>.txt
```

- 设备序列号固定 **2aa26095**；任何 adb/fastboot 命令都带 `-s`。
- 镜像只 `fastboot boot`，不刷写。

## 2. 当前状态（run87 → v28，详见 CURRENT.md §二）

音频控制面全通，**数据面卡在 DSP 读主存那一步**：第一次 `DATA_BUFFER_V2`
之后 DSP 挂死、APM 不回包、总线卡死。

已核对清楚的 SMMU 事实：

| 项 | 结论 |
|---|---|
| CBAR(0..5) | `0x1f000` → Type=1 = **S1 翻译 + stage-2 BYPASS** |
| CBAR(6..) | 0（Type=0），但同页其它值形似指令 ⇒ 不是可靠 CB 页 |
| CB0..5 SCTLR | `0xe0`/`0xa9f00e0`，VTCR=0、VTTBR=0（未启用翻译） |
| 可读区 | `0x0..0x3FFF`、`0x80000..0x8F000`；**`0x4000` 是洞，读即 panic** |
| sACR | `0x1c`，CACHE_LOCK 本来就是 0（不是拦路者） |
| sCR0 | `0x200406`，USFCFG=1 且清不掉（未匹配流被拦） |

可写性（决定性）：

```
SMR[n]=0x80001801 (MASK=0)        ✅      SMR 带 MASK≠0        ❌ 写即中止
S2CR=type0|cbndx                  ✅      S2CR=0x10000(BYPASS) ❌ 变异成 FAULT
CBA2R(n)                          ✅      CBAR(n)              ❌ 写即中止
CB 窗口 VTCR/VTTBR/SCTLR          ✅
```

**四种配置都试过，挂死表现一模一样**：S2CR=fault、bootloader 的 CB3、
自建 CB52 翻译（回读全对）、以及引到 stage-2 bypass 的 CB0（`sweetcb=0`，
run88）。⇒ 要么 DSP 的 SID 不在我们写的 SMR 上（USFCFG=1 未匹配即 fault），
要么卡点根本不在 SMMU。

**★ 已封死（run91）**：`CBAR(0)=0` 的写同样外部中止（CBAR 0/6/52 三个 bank
全是如此）⇒ **CBAR 对当前执行环境整体不可写**。配合「带 mask 的 SMR 被拒」
（bootloader 自己的 SMR3 就是 mask=0x402，硬件支持）和「BYPASS 被改成 FAULT」，
指向同一条策略：我们只有「精确 SID + 指向已授权 bank」的权限，没有配置
context bank 的权限；安卓有（能写 CBAR(52)）。

⇒ **XNU 侧自行配置 apps-smmu 这条路已封死**，突破需要 Gunyah 授权。

## 3. 别再试的死路（每条都有 run 记录）

| 尝试 | 结果 | 证据 |
|---|---|---|
| `S2CR=0x10000`（BYPASS） | 回读变 `0x20000`（FAULT） | run71/73 |
| `SMR` 带任何 MASK≠0 | 写就外部中止 | run69（0x800f1800）、run82（0x800f1801） |
| `SMR` 用 SID `0x1800` | 写就外部中止 | run70 |
| `CBAR(n)` 写 | 读正常、写中止 | run77（CB6）/run79/80 |
| 逐页扫 SMMU 寄存器块 | `0x15004000` 是洞，读即 panic | run86 |
| kprobe `@0x15000934:u32` 读 SMMU | 线性映射读不到 ioremap，全 0 | 本次 |
| 数据面走 bootloader 的 CB3 | DSP 冻结 | run75 |
| 引到 stage-2 bypass 的 CB0（`sweetcb=0`） | 同样冻结 | run88 |
| **写 `CBAR(0)`** | **同样中止 ⇒ CBAR 整体不可写** | run91 |
| `cpus>1` | `pe_init_fiq` 写未映射地址，必 panic | 本次 |

## 4. 两个会 panic 的通用坑

1. **`ml_io_map` 用一次性共享池**（`osfmk/arm/io_map.c`）。0185 已把
   `IO_MAP_SIZE` 从 8 MiB 调到 **64 MiB**（SPMI 一个窗口就吃 4 MiB，GUI
   起来后必然 `KERN_NO_SPACE` panic：run76/83/GUI 后各一次）。
   新代码仍优先用 `g_map_win()`（`kmem_alloc(kernel_map)`+`pmap_map`）。
2. **低区 PA 不能 `flush_dcache64`**：`phystokv_range()` 只认 ADT 银行
   （`0x840000000+`），否则 panic。用 `dsb sy`。

## 5. 电源与震动的落点

- 电源：`sweet_glink.c` 里 `g_batt_probe()`（`sweetbatt=1` 开启）。
  ADSP 每轮都会发 `OPEN PMIC_RTR_ADSP_APPS`（rcid=6），我们回 ACK 并开
  LCID 2，用 24B `GET_PROP`（owner 0x800A）读 6 个属性。
  显示侧由 `SweetBattery::poll()` 经 `sweet_batt_get()` 取用（0184）。
  **已实测（run84）**：`ack=1 remote=1 rcid=6`，6 个请求都有回应，但全部
  解析成 `prop=0 value=1` ⇒ **响应字段偏移不对**；v25 起 dump 原始 hex。
  **下一步**：跑一轮 `sweetbatt=1` 看 hexdump，对照安卓
  `su -c 'cat /sys/class/power_supply/battery/uevent'`。
- 震动：`sweet_haptics_kick()`（`sweet_glink.c`），由
  `sweet_hid_key(0, down)` 在电源键按下时调用（0183）。
  **已真机验证**（按电源键有反馈）。手动触发仍是 `sysctl -w kern.sweetvibe=1`。
  缺的是系统级触感（Apple haptics 栈），那是另一个课题。

## 6. 补丁索引（`xnu-gh/xnu/patches/`）

| 编号 | 作用 | 默认开关 |
|---|---|---|
| 0181 | SMMU 探测 + stage-2（CB52/VTCR/两级表）+ SHM 低区 | 写 CB 需 `sweetpcm=1` |
| 0182 | 无真实采样时不谎报 AC | 常开 |
| 0183 | 电源键 → PM8350B 震动 | 常开 |
| 0184 | SweetBattery 优先用 pmic_glink 采样 | `sweetbatt=1` |
| 0185 | `osfmk/arm/io_map.c`：IO submap 8 MiB → 64 MiB | 常开 |

## 7. 设备侧卫生

- 已删除 `/data/adb/post-fs-data.d/99_kprobe_early.sh`（备份
  `/sdcard/99_kprobe_early.sh.bak`）与 `99_probe2.sh`。
- 未刷任何分区；GPT 未动；`userdata` 的 HFS 镜像只在 RAM 里用。
- 用完 iOS 直接重启回安卓即可（`adb reboot` 或长按电源）。

## 8. 若要从零重建这台机器的实验环境

1. `ci-src/xnu-xnu-8796.141.3`：从 Apple 源码包解压并 `git init`，
   按文件名顺序 `git apply --check && git apply`（失败跳过）。
2. ADT 需要 iPhone X 的 `d22ap.adt`（`D22AP_ADT` 指向它）。
3. ADSP 固件从**本机**提取：`xnu-gh/scripts/extract_adsp_fw.sh`。
4. 构建：`xnu-gh/scripts/mkimage.sh`（`ADSP_FW_DIR` 指向固件目录）。

## 9. 构建与启动参数速查

```bash
# 日志轮（自动重启，抓 ring）
cd ../sweet-audio
MKARGS="sweetpcm=1" bash tools/watch_and_boot.sh <asset-sha256> <run#> "<build tag>"

# 交互轮（停在 iOS，多核会 panic 所以保持单核）
cd ../xnu-gh
KEEPALIVE=1 D22AP_ADT=.../d22ap.adt ADSP_FW_DIR=.../adsp-fw \
  bash scripts/mkimage.sh
SERIAL=2aa26095 bash scripts/boot_ram.sh images/shim-iosroot-adsp.img
```

| 参数 | 作用 |
|---|---|
| `sweetpcm=1` | 打开 SMMU 探测 / 音频数据面探针（危险，默认关） |
| `sweetcb=N` | 把音频 SID 引到已有 CB N，只写 SMR+S2CR（v28） |
| `sweetpa=ADDR` | 把 SHM buffer 放到指定物理地址（v29） |
| `sweetcbar=1` | 写 CBAR(0) 测可写性（v30，会 panic） |
| `sweetnodata=1` | 只 START、不发 DATA_BUFFER（v30） |
| `sweetbatt=1` | 打开 pmic_glink 电池读取 |
| `KEEPALIVE=1` | 构建去掉 `sweetreboot=1`，停在 iOS |
| `CPUS=n` | 覆盖 `cpus=1`（**>1 目前必 panic**） |

**build tag 规则**：每次改内核都改 `SWEET AUDIO: build vNN (…)` 里的字符串，
看门狗靠它判断产物；marker 必须出现在二进制里（写注释会被编译器剥掉）。
