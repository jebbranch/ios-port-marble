# Marble XNU 出声 — 阶段状态（2026-10-24 12:20）

> **当前结论速查请看 `CURRENT.md`**；本文件是按时间追加的完整实验日志。
> 目录说明与常用命令见 `../README.md`。

## 目标
RAM-only `fastboot boot` 引导 XNU，让 Redmi Note 12 Turbo（marble，序列号 2aa26095）的
扬声器真正出声。

## 一、已验证通过（真机）
1. **ADSP PAS + GLINK + PRM**：Q6 释放、`adsp_apps` 通道建立、LPASS core 投票成功。
2. **LPASS 时钟**：VA_CORE_MCLK 0x307 @19.2 MHz、RX_CORE_MCLK 0x30e @22.5792 MHz，
   `clock_attri=1` 后 status 0。
3. **AudioReach 图**：
   - `GRAPH_OPEN` → **status 0**（344 字节 wire 载荷；关键修正：capability id
     `0x0b001001`、heap=2、`0x0800124d`=2）
   - `GRAPH_PREPARE` → **status 0**
   - SET_CFG 被接受：SAL `0x08001016`/`0x0800101e`、CDMA `0x08001017`
     (48 kHz/16 bit/2ch/**data_format=1**)、`0x08001018`、`0x08001176`
4. **自动复位**：`sweetreboot=1` 的 PSCI SYSTEM_RESET 有效，探测跑完自动回 Android。
5. **kprobe 抓包**：24 字探针 + “系统音效触发播放”能完整抓到厂商报文。

## 二、当前唯一阻塞（DSP 侧）
`GRAPH_START` → status 1。原因收敛到 **CODEC_DMA 接口参数**：

| 参数 | 结果 |
|---|---|
| `0x0800101B`（厂商 hw ep 用，28 B） | **status 3**（本模块不认这个参数族） |
| `0x08001063` 值 `{3,1,3}` / `{3,0,3}` | status 1 |
| `0x08001063` 值矩阵（0121 在测） | `lpaif_type`∈{0,3} × `intf_index`∈{2,1,0} |

0102/0121 的推断来源：厂商 `0x0800101B` 前两个字是 `{0, 2}`，
即 `lpaif_type=0`、`intf_index=2`；而 0x08001063 是上游格式的同一语义。

## 三、当前阻塞（功放侧 I2C）
- TLMM 基址已修正：**0x0f100000**（GPIO56/57 = 0x0f138000/0x0f139000）。
- 复位按厂商 `aw882xx_hw_reset` 反汇编改成 **raw 0 → 延时 → raw 1 → 延时**。
- **全地址扫描 0 设备应答**（复位释放/复位两种状态都试过）。
- Android 内同一总线同一地址工作正常：dmesg `aw882xx 2032 detected`（bus 2, 0x34/0x35）。
- 0121 新增：25 kHz 慢速总线、SDA 驱动电平回读、`spksw` GPIO52 拉高。
  电平回读结果将区分：
  - `driven low` 仍读 1 → bit-bang/引脚问题 → 改用 GENI SE I2C 控制器；
  - `driven low` 读 0 → 总线正常、功放不响应 → 查功放供电/复位/spksw。

## 四、可直接复用的参数
- 简单图：sg `0xb000013d`、container `0xe00001b8`、SAL iid `0x483f`、
  CODEC_DMA iid `0x4840`。
- 厂商真图：sg `0xb00000d5`、container `0xe0000122`、
  模块 `0x4587/0x4763/0x42d4/0x4308`，wire 载荷 440 B（源码里已内置备用）。
- 厂商 hw ep：`0x08001017` `{96000,32,2,1}`；
  `0x0800101B` `{0,2,0x00010001,3,0x00200004,0,1}`。
- 厂商 SAL 媒体格式：`0x0800100C`
  `{fmt=1, PCM, 18, 48000, 24bit, ...}`。
- I2C：GPIO56/57；复位 GPIO1/GPIO120（0x0f101000/0x0f178000）；
  spksw GPIO52（0x0f134000）。

## 五、目录
```
src/       当前工作源码（sweet_glink.c, sweet_i2c.c）
patches/   0113..0121（与 git 仓库同步的副本）
traces/    真机 kprobe 抓包（trace_ui.txt 含 0x0800101B 完整 28 B）
logs/      ring-run11..15、CI/镜像构建日志、AGM 库清单
images/    最近验证过的引导镜像（0120）与当时内核
tools/     PlayTone.java / player.jar / tone.wav（AudioTrack 测试音）
STATUS.md  本文件
```

## 六、下一步
1. 看 0121 结果：
   - 有任一 `0x08001063` 组合返回 0 → START 应通过 → 观察 `pcm tone` 是否被 DSP 消费。
   - 若全失败 → 切换到厂商真图 `0xb00000d5`，按 `0x4308`/`0x42d3` 的厂商参数配置；
     若仍失败，需要补图级 SET_CFG（厂商在 GRAPH_OPEN 后发过 264 B 的共享内存校准块，
     尚未抓到内容）。
2. 功放：按 0121 的电平回读决定是修 bit-bang 还是查供电。
3. 若供电缺失，用 RPMh 投票候选 1.8 V 轨（pm8350_l7 / pm8350c_l10 等）。

## 七、复现命令
- 编译：`git push` → GitHub Actions `xnu` → release `kernel-latest`
- 取内核建镜像：`bash work/xnu/marble/fetch_and_build_adsp.sh`
  → `cd work/xnu/marble && bash mkimage.sh sweetreboot=1`
- 引导：`adb reboot bootloader; fastboot boot /tmp/shim-iosroot-adsp.img`
- 拉 ring：`tools/pull_ring_after_boot.sh <编号>`（Android 起来后自动 dd userdata 尾部）

---

# 2026-10-24 13:10 更新（TDM 线）

## 已确认
- 扬声器走 **TDM-LPAIF-RX-TERTIARY**：`96 kHz / 2ch / 32 bit`（resourcemanager 的
  `PAL_DEVICE_OUT_SPEAKER` → `TDM-LPAIF-RX-TERTIARY-VIRT-0`）。
- `0x0800101B` = **PARAM_ID_TDM_INTF_CFG**；`0x0700100e` = TDM_SINK；
  `0x0700100f` = TDM_SOURCE；`0x07001000` = WR_SHARED_MEM_EP。
- 0122：把图端点换成 TDM_SINK，图打开/全部 TDM 配置/PREPARE 均 status 0；
  SAL 的 `0x0800100C` 返回 **status 3 (EUNSUPPORTED)**；START 仍 1。
- 0123：输入端换成 **WR_SHARED_MEM_EP**，96k/32 媒体格式 **status 0**；
  START 仍 1。
- 厂商真实播放图（从 AGM 服务内存提取，见 `traces/vendor-graph-notes.md`）：
  - 0xb0000002（带 AW 模块，3ch TDM）：SAL(0x4012) → 0x48ba → AW(0x42d3) →
    0x418e → 0x4006 → 0x4007 → TDM(0x428f)
  - 0xb0000052（6 模块，无 AW）：SAL → 0x07001015 → 0x07001011 →
    0x07001098 → 0x0700101a → TDM(0x42db)
- I2C：外部上拉在线、SDA 驱动正常；0x34/0x35 仍无 ACK（spksw 低/高都试过）。

## 正在做（0125，构建中）
- 给 **TDM sink** 补发输入媒体格式 `0x0800100C`（96k/32/2）
- 补 **HW_INTF_CLK_CFG 0x0800113C** = 12.288 MHz
- 同时包含 0124：在 ADSP 启动 **之前**（60s 等待前）探测功放，
  用于判断无 ACK 是否是 ADSP/PMIC 状态造成

## 下一步（若 0125 仍 START=1）
切换到厂商图 0xb0000002（载荷已提取为紧凑格式，需转 wire 格式），
按厂商 iid 配置 SAL/TDM/AW，再试 PREPARE/START。

## 0126（已推送 7d8d2db，CI 构建中）
- 直接改用厂商图 **0xb0000002**（wire 载荷 712 B，`traces/agm-graph-b0000002-compact.bin` 转出）
- 图内 iid：SAL 0x4012 / AW 0x42d3 / TDM 0x428f；容器 capability 0x0b001001，heap 1
- 配置：SAL MF 96k/32/2（回退 48k/24/2）、TDM MF 96k/32/3（mask=7）、
  HW_EP_MF 96k/32/3、frame=1、power=0、clk 12.288 MHz、
  AW param 0x10013D2A = {0, 0x10013D1B, 1, 0, 0, 0, 0x632B}
- 顺序：厂商图优先，失败再落回 WR 简单图、CDMA
- 源码/补丁已归档：`src/`、`patches/0126-sweet-vendor-playback-graph.patch`

## 0126 结果（ring-run20，2026-10-24 13:24）
- 厂商图 `0xb0000002` 走 **shm** 发 GRAPH_OPEN：**status=1 EFAILED**（payload 712 B 已按
  16 B 段头 + 8 字节对齐重建，与已验证的简单图 344 B 结构逐字节同构；严格解析 compact
  680 B 得 5 段，重建结果与发送内容完全一致 → payload 本身没问题）
- 失败后自动回退 tdm-inline：open/prepare 0、START 仍 1
- 新发现：`g_apm_cmd` 内联缓冲只有 512 B；厂商图 712 B 只能走 shm。而 shm 映射是
  `SHM_MAP(src=0x2002)`、GRAPH_OPEN 却从 `src=0x2010` 发 → **句柄与客户端端口不一致**

## 0127（已推送 e529bf7，CI 构建中）
1. 内联缓冲 512→1024，厂商图先**内联**直发（简单图内联一直能开）
2. 内联失败再用 shm，但 `SHM_MAP(src=0x2010)` 与命令同端口
3. 保留 tdm-inline 回退；内核标记 `graph_open/vp-inline`、`vp-shm`

## 0127 结果（ring-run21）—— 厂商图打通控制面，panic 是自家 bug
- **`graph_open/vp-inline` status=0**：712 B 内联直发成功（问题确实在 shm 端口/路径）
- TDM 全部配置 0（`0x0800100C` 仍 3=EUNSUPPORTED）、AW cfg 0、PREPARE 0、START 已发出
- panic：`far=0x4000`、x0=0x4012。根因 = `g_audio_tone()` 在 PCM shm **未映射**时
  直接写 `g_shm_va + G_PCM_OFF_A(0x4000)` → 空指针偏移 0x4000
- 即 START 很可能已成功，崩溃发生在随后的 tone 填充

## 0128（f8e3f75，CI 构建中）
1. `g_audio_tone()` 开头先 `g_apm_shm_map(src_port)`，失败则跳过（不再空写）
2. 每条 APM 响应后 `sweet_log_sync()`，START 的 status 一定落盘
3. START 成功打印 `vendor graph started` 并同步

## 功放 I2C 无 ACK 的根因（2026-10-24 14:00 发现，0129 修复）
- Android 侧事实：i2c@880000 = **SE15，GPIO56/57，qup15**；@34 复位 GPIO1、IRQ 88、
  spksw 52；@35 复位 GPIO120、IRQ 63；驱动 2.89 s 读到 chipid **2032**
- 播放 3 s 1 kHz 时 regulator_summary **无任何功放电源轨变化** → 功放供电硬连/常开，
  XNU 不缺电（排除了 PMIC 假设）
- **真正 bug**：`sweet_i2c.c` 的复位序列用了 `delay(2)`/`delay(5)`。XNU 的
  `delay()` 单位是**微秒**，厂商是 msleep(1~2 ms) → 复位释放后 5 µs 就开始扫描，
  芯片 I2C 模块根本没起来
- 0129：低电平 2 ms、释放后等 10 ms

## 0128 结果（ring-run22）—— 厂商图 START 成功！
- open 0、TDM/AW 配置全过（SAL 0x0800100C 两种采样率都 3=EUNSUPPORTED）、
  PREPARE 0、**graph_start status=0**、`vendor graph started`
- PCM shm 以 0x2010 映射成功（handle 0x100100c）
- 但 `pcm buffer 0 not consumed`（DATA_BUFFER_V2 → 0x4012 被拒或 DONE 解析不对）

## 0130（e9a98a6，CI 构建中，含 0129）
- DATA_BUFFER_V2 原始响应打印（no rsp / op / w24..w36），只打前 4 次
- tone 前向 **0x4012(SAL) / 0x48ba / 0x4007 / 0x418e** 各发一帧探测，
  第一个回 DONE 的端点作为 tone 目标

## 0130 结果（ring-run23）
- 功放：复位延时改 2 ms/10 ms 后**仍无 ACK**（0x34/0x35 都不应答）→ 延时不是全部原因
- DATA_BUFFER：四个候选端点全部 **no rsp**
- ★ 从 kprobe `gpr_send_pkt` 探头字（trace_ui.txt）解出厂商真实报文：
  - **DATA_BUFFER_V2: src=0x2011**、dst=模块 iid、token 0/1、**44 B 负载**
    （{offset,0,handle,size, 7×0}）、pkt_size=0x44
  - **SHM_MAP: src=0x2002**（0x0100100C）
  - 图形命令（open/prepare/start/set_cfg）: 0x2010
  - 我们一直从 0x2010 发数据 → DSP 不认

## 0131（4c20595，CI 构建中）
- PCM 缓冲改从 **0x2011** 发；SHM_MAP 改用 **0x2002**
- 保留 44 B 厂商负载格式；DONE 事件打印 w16..w36，opcode 允许落在任一槽

## 0131 结果（ring-run24，从 recovery 直读 userdata，验证可行！）
- 数据缓冲改 0x2011 + SHM_MAP 0x2002 后 **仍然 no rsp**
- ★ 发现真正的 handle 解析 bug：SHM_MAP 响应负载 = {echoed opcode, handle}，
  我们读 +24（=0x0100100C，正好是命令码）当 handle；真实 handle 在 **+28**
  - 日志 `handle=0x100100c` 与命令码完全一致，铁证
  - 假 handle 让 DATA_BUFFER 被 DSP 静默丢弃（也解释 0126 shm GRAPH_OPEN status=1）

## recovery 直读（2026-10-24 14:18 实测）
- recovery 的 adbd 是 **root**，`/dev/block/sda33` 可读，XNU 结束后无需等 Android：
  `adb exec-out "dd if=/dev/block/sda33 bs=4096 skip=58771962 count=1025"` → 解 ring
- 当前设备在 XNU 结束后会落到 recovery（连续两次），拉日志更快了

## 0132（cf70d34，CI 构建中）
- `g_shm_handle = le32(rsp+28)`，并校验 +24 是命令码回显

## 0132 结果（ring-run25）
- handle 改读 +28 后得到 `{echo=0x0100100C, 0x1, 0x1}` → **map 本身失败（status=1）**
- 关键差异：我们分配到的 PA = **0x8459D8000（约 35 GB，高 DDR 区）**；
  厂商 SHM_MAP 的 PA = **0x1FFE8000 / 0x1FFE0000（约 5 GB）**，size=0x5000
- 厂商请求结构与我们完全一致（pool=3, regions=1, flag=4），差别只在地址/大小
- 结论：ADSP 无法映射这么高的物理地址

## 0133（构建中）
- `kmem_alloc_contig` 传 `max_pnum=0x200000`（< 8 GB），失败再回退无限制
- 区域大小 0x10000 → **0x5000**（厂商同款；PCM 最高用到 0x4C80）
- SHM_MAP 校验 +28 的 status，非 0 直接报错

## 0133 结果（ring-run26）
- 区域缩到 0x5000 后 PA 仍是 **0x844AEC000（33 GB）** → 低地址分配失败后走了回退
- SHM_MAP 仍 status=1
- ★ 对比厂商报文发现唯一剩余差异：**SHM_MAP 的域字 = 0x10302**（bit16 =
  `src_port_valid`），其他所有命令都是 0x302；厂商只对 SHM_MAP 置这一位

## 0134（构建中）
- SHM_MAP 包头置 `0x10000`（src_port_valid）

## 0135 结果（ring-run28）
- 单页 4 KB + 厂商偏移 0/0x800 + src_port_valid：SHM_MAP 仍 **status=1**
- 分配 PA 仍 **0x844AE0000（约 35 GB）** → `max_pnum=0x200000`（4 KB 页 = 8 GB）
  的约束分配失败后走了回退；说明**低位没有空闲连续页**
- 页大小确认：XNU 在此端口用 **4 KB 页**（4 KB 分配有效）

## 0136（e042a45，构建中）
1. `map_probe`：直接映射 ADSP carveout **0x9FD00000**（2.68 GB，DT 的
   adsp_region，49 MB），只建映射不写数据 → 判断 SHM_MAP 是否只是受地址窗口限制
2. 分配改为**递增上限扫描**：1/2/4/6/8/12 GB，取第一个成功并打印 PA
3. 之后照常映射我们自己的缓冲区并发 tone

## 2026-10-24 归档与仓库化
- /tmp 里的本项目产物已全部迁入本目录：
  `images/`（21 个镜像）、`kernels/`、`logs/`（含 raw ring gzip）、
  `logs/build/`、`dumps/`、`refs/`（AudioReach/AW882xx/qdsp6/构建树）、
  `src-history/`、`patches/`
- 后续实验不再写 /tmp：构建与拉日志脚本都使用本目录/仓库内路径
- GitHub `jebbranch/ios-port-marble` 已补充：
  `shim/`（shim 源码）、`scripts/`（fetch_kernel / mkimage / boot_ram /
  pull_logs / extract_adsp_fw）、`docs/`（audio-status / sound-plan /
  vendor-graph / sweetlog），README 重写为“克隆→构建→部署”全流程
- recovery（TWRP）adbd 为 root，XNU 结束后可直接 dd userdata 尾部，
  不必等 Android 启动

## 2026-10-24 19:05 —— SHM_MAP 响应语义确认 + 0138
- 从 Linux audioreach 头文件确认：
  - `APM_CMD_SHARED_MEM_MAP_REGIONS = 0x0100100C`，请求结构
    `{u16 pool_id, u16 num_regions, u32 property_flag, regions[]}`
  - **`GPR_BASIC_RSP_RESULT = 0x02001005 {opcode, status}`** —— 我们收到的
    `w24=0x0100100C, w28=1` 就是它：map 被 APM 明确拒绝（status=1）
  - map 成功响应应是 `0x02001001` + `{u32 mem_map_handle}`
  - `0x0100100F` 是 **PRM_CMD_REQUEST_HW_RSC**（时钟投票），不是注册命令
- 0136/0137 的多次探针可能把 ADSP 弄崩：对 carveout 等非法地址做 map 后，
  后续探针“no rsp”，最后真实 map 又恢复为 status=1 —— 可疑的 DSP 重启行为

## 0138（fc9c3e3，构建中）
- 每轮只做**一次** map 探针，测厂商自己的地址 **0x1FFE8000 / 0x5000**
- 探针前后各做一次**同步 SPF 状态查询**，检测 DSP 是否重启
- 探针失败后再走原有 map/tone 流程；不做 carveout/CMA/4GB 探针

## 0138/0139 结果（ring31/32）
- 0138：SPF 状态前后一致（APM 未重启）；厂商地址 0x1FFE8000 的 map **无响应**，
  我们的高位地址仍被拒（GPR_BASIC_RSP_RESULT status=1）
- 0139：SMEM(0x80900000)、SPU shared(0x8BB80000/0x8BBE0000) 三个低地址 map
  **全部无响应**；SPF 状态依旧不变
- 结论：APM 对“看起来合法”的低地址卡住（疑似 SMMU/TBU 未就绪），
  对高位地址快速拒绝；不是端口/令牌/请求格式问题

## 0140（构建中，含侧线）
- 只读探测：PMK8350 RTC（SID0 0x6100..0x610F）、PM8350B 触觉（SID3 0xF000/0xF100）
- SMMU 现场：apps-smmu 0x15000000、lpass_tbu 0x151E9000、qsmmu cfg 0x151CE000
- 低地址 map 变体：0x2010 / 0x2011 端口，flag 4 / 0
- 目的：确认 SPMI 权限与 RTC/触觉寄存器；确认 apps-SMMU 的 LPASS TBU 是否被
  bootloader 配置（若未配置，需要 XNU 侧最小 SMMU 身份映射）

## 2026-10-24 19:50 —— ring33：SPMI 基址是错的（关键发现）
- `SWEET BATTERY: unsupported SPMI arb=0x8000100 APIDs=0`
  → `sweet_spmi.c` 仍用 0093 之前的旧基址（core 0x0c440000 / obs 0x0e600000 /
  cfg 0x0c40a000），读到的是 observer 命令寄存器，不是 arbiter 版本
- 因此 ring31/32 的电池一直是 `static fallback`，0140 的 RTC/触觉 dump 全部
  `not ready`；**RTC/触觉从未被真正访问过**
- 现场 DT（`/proc/device-tree/soc/qcom,spmi@c42d000`）给出正确窗口：
  `cnfg 0x0c42d000, core 0x0c400000, chnls 0x0c500000, obsrvr 0x0c440000`
  ——与 0093 给 keys 的修正一致
- `apps_smmu @0x15000000: +00=00200406 +10=0000001c`（可读）
- **`lpass_tbu 0x151e9000` 一读就 fault，日志停在那一行**（sys.log 无后续），
  说明 TBU 寄存器在 XNU 下不可直接访问；以后不再碰
- 设备 Android 内核是 **5.10**（用户现场确认），厂商参考树 `marble-s-oss` 也是
  5.10；之前按 5.15 找的代码只能作旁证

## 现场 DT 的触觉参数（来自手机 /proc/device-tree）
- `hw-haptics@f000`：`lra-period-us=5882`, `vmax-mv=1450`, `drv-sig-shape=1`(sine),
  `brake-mode=1`, f0 160–180Hz 默认 170
- effect_0..5 完全相同：`wf-pattern-period-us=6667`，`wf-vmax-mv=3600`，
  8 个样本 `(amp, period=0, f_lra_x2=0)`：
  31,63,95,127,383,351,319,287
- `nvmem-names = hap_cfg_sdam`，`nvmem-cell-names = hap_cl_brake`，pbs-client 1507
- RTC 在 `pmk8350@0/rtc@6100`；PMK8350 映射：读 **0x6148**，4 字节小端 Unix 秒
  （ctrl 0x6146 / write 0x6140 / alarm 0x6240）

## 0141（5e05480，构建中）
- 修正 `sweet_spmi.c` 的全部基址；增加 chnls 写通道
  （0x0c500000，APID 步长 0x1000/0x10000 现场探测，写前用 observer 值校验）
- 只允许写 SID3 0xF0/0xF2（触觉）和 SID0 0x61（RTC）
- 读 0x6148 并 `clock_set_calendar_microtime()`（值不在 2020–2050 就不动时钟）
- 触觉直驱（qcom-hv-haptics 5.10 的 HAP520 路径）：
  VMAX=72(3600mV), sine, T_LRA=0x0498(5882us/5), DIRECT_PLAY amp=0xFF,
  autores 关, 必要时开 hBoost VREG_EN, force VREG ready, EN=0x80,
  PLAY=0x81(DIRECT_PLAY) 300ms，然后 stop/disable
- 去掉了 lpass_tbu/qsmmu 的 MMIO dump（会 fault）

## 2026-10-24 20:00 —— run34 panic：io_submap 被 4 MiB 映射撑爆
- 现场：`panic kmem_alloc e108ce9048,4194304,0x100021 failed with 3`
- 4194304 = 0x400000 = `sweet_spmi_init()` 给 chnls 窗口一次性映射的大小
- XNU 的 `io_submap` 只有 **8 MiB**（启动日志可见），此前各驱动已用掉不少；
  4 MiB 连续 VA 分配失败 → KERN_NO_SPACE(3)。**与 PMIC 无关**
- ring34 同时证实日志停在 GLINK banner 之前：panic 就在 SPMI 初始化里
- 0144 修法：chnls 改为**按 APID 懒映射单个 4 KiB 槽页**，
  observer 从 512 KiB 缩到 128 KiB（1024 APID × 0x80 的最大范围）
- 这轮同时验证了：0142 的 “dump 可能毒掉 APID” 隐患还没机会暴露，
  0143（只在超时才拉黑 + 先 RTC/触觉后 dump）已包含在 run35 的内核里

## 2026-10-24 20:10 —— 音频重大修正：厂商 PA 的高 32 位丢了
- 从 `traces/trace_ui.txt` 第 136 行的 kprobe 原始报文重解：
  `p8=0x1FFE8000` = lsw，**`p9=1` = msw**，`p10=0x5000` = size
- ⇒ 厂商 SHM_MAP 的真实物理地址是 **0x1_1FFE8000（4.5 GB）**，
  不是我们一直探测的 0x1FFE8000（msw=0，约 512 MB）
- 0136–0139 的 “vendor_pa / audio_cma” 探针**全部测错了地址**，
  “no rsp” 不能证明厂商地址不可用
- 另一个结构性事实：XNU 的 ADT 只描述 0x840000000（33 GB）起的 4 GB，
  所以 kmem_alloc 只能给 ~35 GB；而 ADSP 的可见 DDR 在低 8 GB 内。
  高位缓冲区被 status=1 拒绝是**地址空间**问题，不是格式问题
- 0147：用厂商精确地址 `lsw=0x1FFE8000, msw=1, size=0x5000, flag=4,
  token=0x2d, src=0x2002` 发一次只读 SHM_MAP 探针
- 厂商 map 报文同时证实：**没有 apm_cmd_header**（pkt_size=44），
  payload 从 +24 直接是 {pool=3, regions=1, flag=4, lsw, msw, size}
- 若 0147 成功（APM_CMD_RSP_SHARED_MEM_MAP_REGIONS + handle），下一步才是
  把 PCM 写进这块低地址 DDR 并喂 DATA_BUFFER；功放 I2C 是另一个独立卡点

## 2026-10-24 20:18 —— SPMI 根因：arbiter 是 v7，我们一直在用 v5 布局
- run36 结果：`arb=0x70000000 APIDs=768` 正确，但
  `hap-rev=0`、`RTC 0x6148 read failed`、全部 `(no access)`；
  同一轮 keys 仍能读 PON/GPIO（假象）
- 对照 5.10 `drivers/spmi/spmi-pmic-arb.c`：
  | | v5（keys/sweet_spmi 用的） | **v7（本机实际）** |
  |---|---|---|
  | APID map | core+0x900+4n | **core+0x2000+4n** |
  | owner 表 | cnfg+0x700+4n | **cnfg+4n** |
  | observer 步长 | 0x80 | **0x20** |
  | chnls 步长 | 0x10000 | **0x1000**（已对） |
  | PPID 解析 | `(map>>8)&0xfff` | 同（已对） |
- **keys 驱动也用了 v5 偏移**：它读到的 “pon-apid=536 / gpio6=434” 和
  “pon=0x10 gpio=0x1” 是错误槽位里的巧合值——不是真的在读 PON/GPIO
- 这解释了电池一直 `static fallback`、RTC/触觉全失败
- 0149：sweet_spmi 全面切到 v7 布局；observer 固定 0x20
- 0150：core 映射从 0x2000 扩到 0x4000（v7 map 在 core+0x2000..0x2bfc，
  原来会在第一次 map 读时越界 fault）
- DT 确认：`qcom,ee=0`、bus0 `bus-id=0`；APPS=EE0，observer 无 EE 偏移
- 待办：keys 驱动也应改 v7（目前按键"能用"可能是巧合，需实测）

## 2026-10-24 20:27 —— run37：v7 生效，owner 表把权限讲清楚了
- `obs-stride=0x20`，APID map 全部命中：
  | ppid | 外设 | APID | owner |
  |---|---|---|---|
  | 0x008 | PON | 4 | 4 |
  | 0x4c1 | GPIO | 247 | 4 |
  | 0x061 | PMK8350 RTC | 51 | 4 |
  | 0x3f0 | 触觉 cfg | 465 | 0 |
  | 0x3f1 | 触觉 pattern | 466 | 0 |
  | 0x3f2 | hBoost | 400 | 3 |
- **RTC/PON/GPIO 属 EE4（PMIC 固件）**；触觉 cfg/pattern **属 APPS(0)**，
  hBoost 属 EE3 → 触觉主体可直写，升压不行
- 触觉 dump 实际读通了：`f000=00 f001=02`（REVISION1=0，REVISION2=2=HAP520）、
  `f100=00 f101=02`（PTN V2）；之前 bail 是因为把型号判断放在 0xf000
- 0151：revision 判断与 RW 槽验证都改读 **0xf001**
- RTC：0x6100..0x610f 单字节都能读，唯独 **0x6148 单字节被拒**；
  厂商 `rtc-pm8xxx` 用 `regmap_bulk_read(0x6148, 4)` → 0152 加 4 字节块读
- 音频：`vendor_low`（0x1_1FFE8000, msw=1）仍然 **no rsp** →
  地址/字段已全对，剩下的是 ADSP SMMU 上下文（AP 侧未建立），
  lpass_tbu 寄存器从 XNU 访问会 fault，暂时无解

## 2026-10-24 20:54 —— run39 panic：另一个 EE 的 chnls 槽会外部中止
- 0154（先触觉后 RTC + revision 重试）在 run39 走到触觉写序列时 panic
- 现场：`esr=0x96000010`（**同步外部中止**，不是缺页）、
  far 在内核 io 映射 VA、`x21=0xf246`（hBoost VREG_EN）、
  `x23/x27/x28=0x190`（400 = hBoost APID，owner=3）
- 结论：**访问不属于 APPS(EE0) 的 APID 的 chnls 槽，互连直接给外部中止**，
  不是返回 DENIED；panic 发生在给 hBoost 写做 chnls 槽校验时
- 0155：
  1. 写前查 PPID 的 owner，**非 0 直接拒绝**，绝不映射别人的 chnls 槽
  2. chnls 槽映射**按 16 KiB 向下对齐**，用 delta 访问（XNU 页大小 16 KiB）
  3. hBoost 改为只读（owner=3，PMIC 固件通常已开着升压）
- 另：0152 的 RTC 块读在 run38 失败且**污染了后续 observer 读**
  （触觉 rev2 从 0x02 变 0x10）；0154 已把触觉移到 RTC 之前，
  RTC 块读失败不再影响触觉

## 2026-10-24 21:30 —— run41（0157）：时钟通了，触觉寄存器全部落地
- `SWEET RTC: clock set from sweettime=1792848487` —— 主机时间注入生效
- **触觉写全部 rc=0**（0156 的 3 次重试起作用）：
  vmax / wave / tlra / direct amplitude / autores off / vreg ready /
  fault clear(0x07) / enable(0x80) / play(0x81) / stop / disable
- `fault` 从 0x1(HPRW_RDY) 变为 **0x2**（SC 类），play 期间的故障下一版再读
- **hBoost 触发失败 rc=-1**：原因是 SDAM `9d00` 属于 **PMK8350（SID 0）**，
  不是 PM8350B（SID 3）。0157 用 `sweet_spmi_write(3, 0x9d42)` 查不到
  PPID 0x39d → 返回 -1。diag 里 `ppid 0x9d apid=428 owner=0` 证明
  **APPS 拥有 SDAM，可以写**
- 0159：改用 `sweet_spmi_write(0, 0x9d42/0x9de5)`，白名单改为 SID0 0x9d；
  并在 play 300 ms 期间读一次 fault
- RTC：`pmic=63447993`，与 run40 的 63447065 相差 928 s，**计数器在走**，
  但 epoch 约 2024-10，不是 Unix 时间 → 继续用 sweettime
- 0158（音频）：`kmem_alloc_contig` 的低地址没有生效（限 12 GB 仍给 33 GB），
  改为把 PCM 缓冲区放在 DSP 保留区尾部 `0xa2d00000`（adsp_region，
  0x9fd00000 + 49 MiB），用 `ml_io_map` 直接映射，原分配器只作回退

## 2026-10-24 23:20 —— run42–48：hBoost 修好，GENI 卡在 SE15 时钟
- 0159 修正 SDAM 的 SID（PMK8350 = SID0）：**hBoost 触发 rc=0/0**，
  `boost vreg now=0x80`，升压使能到位
- 0160：SHM_MAP 换用 token `0x100+APM_CMD_GRAPH_OPEN` 并探测两个 ADSP
  保留区地址 —— 仍是 `status=0x1`。Android 取证证明 SHM_MAP 收的是
  **IOVA**（`Default domain type: Translated`，vendor 地址 0x1_1FFE8000
  不在 `/proc/iomem`），固定 PA 实验作废，数据面下一步转 SMMU/SID
- 0162：bit-bang 不再产生 reset 脉冲，补九拍 SCL —— 仍然没有 ACK
- 0163/0165/0166：GENI SE15。ABL 已载入 I2C 微码
  （`fw=0x303 proto=3 status=0 tx=0x201029e4 rx=0x20104800`），
  但 0165 在早期 amp 探测里就跑 GENI，**早于 UFS 挂载**，run47 与 run46
  日志完全相同（新内核在写 ring 前就崩了）
- 0166 改成第二次探测才碰 GENI、每步 `sweet_log_sync()`：
  **run48 精确停在 `se15 cfg step 2`**，后面第一个写是 `SE_DMA`
- 根因：**wrapper2 的 GCC 叶子是冷的**。fastboot boot 不经过 Android
  的 clock 驱动。DT 的 `clocks = <&gcc 120>, <&gcc 138>, <&gcc 139>`
  恰好是 **gcc-waipio** 的 `GCC_QUPV3_WRAP2_S0_CLK`、`WRAP_2_M_AHB_CLK`、
  `WRAP_2_S_AHB_CLK`（diwali 头文件里 120 是 USB 时钟，语义对不上），
  且设备上 `gcc_waipio.ko` 引用计数 103、`gcc_diwali` 为 0
  → 这个 SoC 的 QUP 时钟实际由 waipio 驱动提供
- 0167：碰 GENI 前先投 **GCC+0x62010 = 0x1f**
  （bit0 core / bit1 S-AHB / bit2 M-AHB / bit3 core2x / bit4 SE0），
  RCG `0x2e014` 走 XO/1（19.2 MHz，照抄 sweet_touch 的无 PLL 策略），
  并打印 vote / rcg / halt 三个寄存器

## 2026-10-24 23:15 —— run49（0167）：GENI 活了，卡在引脚功能
- **wrap2 时钟投票生效**：读回原值 0（冷启动确实没开）→ 写后 0x1f；
  RCG 原值 cmd=0 cfg=0x100000（未配置），改成 XO/1 后 **cmd 立即清零**
- **GENI SE15 配置全过**：step 1..5 全部打印，
  `se15 configured for I2C` —— run48 的崩溃点消失
- 但 `geni amp 0x34 r=-1`（不是 -2/NACK）：传输超时，而不是无应答
- vendor `i2c-qcom-geni.c` 对照结论：地址位移 **就是 `<<9`**
  （不是上游 Linux 的 `<<1`）；SCL 表按 **19.2 MHz** 源时钟设计
  （div7/high10/low11/cycle26），我们的值逐项一致 → 协议层没问题
- 剩下的差异：**GPIO56/57 还停在 GPIO 功能**（bit-bang 留下的输出态），
  GENI 的信号到不了焊盘。DT 的 `qupv3_se15_i2c_active` 要求
  function `qup15`、pins gpio56/57、内部上拉；sweet_touch 已验证
  这颗 SoC 的 QUP 引脚 **mux 值 = 1**
- 0168：GENI 前把 56/57 设成 mux1 + 上拉 + 6 mA 输入，打印 cfg/电平；
  并给 se15_msg 的 busy / err / timeout 三条路径加状态打印

## 2026-10-24 23:26 —— run50（0168）：引脚对了，GENI 收到 NACK
- 引脚：`pin 56 cfg=0x87 level=1`、`pin 57 cfg=0x87 level=1`
  （QUP mux 1 + 内部上拉 + 6 mA，总线空闲为高）✓
- `se15 err irq=0x409 st=0` → 拆位：bit0 DONE + bit3 FAILURE +
  **bit10 NACK** —— 从 run49 的“总线异常/超时”变成
  **GENI 真正发出了地址，芯片不应答**
- 但日志仍显示 `r=-1`：`se15_aw_read` 把所有错误统一成 -1，NACK 被吞掉
- 两颗功放（0x34/0x35）都 NACK → 共同原因：供电、复位或芯片未就绪
- DT 的 aw882xx 节点 **没有 vdd-supply**（供电硬常开，不受软件控制），
  但有 reset-gpio（GPIO1 / GPIO120）、spksw-gpio、irq-gpio
- 0169：`se15_aw_read` 透传错误码；GENI 前给两颗芯片发 **复位脉冲**
  （低 2 ms → 高，再等 10 ms），并打印 rstA/rstB/spksw/irq 四个电平

## 2026-10-24 23:55 —— run51（0169）：复位正常，随后内核 OOM panic
- `rstA=1 rstB=1 spksw=1 irq=1`（复位已释放，控制线全部为高）✓
- GENI step 1..5 全过，**但没有 amp 读结果就 panic 了**
- 屏幕 panic 串：`kmem_alloc(0xfffffe1002742e48, 16384, 0x100021):
  failed with 3 @vm_kern.c:178`（cpu0 一次 + cpu8 嵌套一次）
- `handshake structure not initialized` 来自
  `osfmk/kdp/output_stages/out_shmem.c:499`，是 **KDP 崩溃日志共享槽
  未初始化**时的提示，**不是崩溃原因**
- 错误码 3 = KERN_RESOURCE_SHORTAGE → **物理页耗尽**（连 16 KB 都分不到）
- 0169 相对 0168 只多了 8 次 `ml_io_map` 和错误码透传，说明系统本来就
  贴着 OOM 边缘（`ml_io_map` 不释放，bit-bang 期间每次读电平都新建映射）
- 0170：`sw_gpio_drive/read_level` 改为按物理地址缓存映射（最多 8 个），
  并在 GENI 前打印 `vm_page_free_count`

## 2026-10-25 00:05 —— run52（0170）：★ I2C 打通，两颗功放读到 0x2032
- `free pages before GENI=141921`（约 2.2 GB 空闲）→ run51 的 OOM 确系
  GPIO 映射泄漏累积（每读一次电平就新建一个永不释放的 ml_io_map），
  0170 缓存后彻底解决
- `pin 56/57 cfg=0x87 level=1`、`rstA=1 rstB=1 spksw=1 irq=1`
- **`geni amp 0x34 r=0 id=0x2032`、`geni amp 0x35 r=0 id=0x2032`**
  —— 两颗 AW882xx 全部应答，I2C 链路打通
- 结论：bit-bang 一直失败只是因为第一次探测时复位尚未释放；
  GENI 路径（第二次探测 + 0169 的复位脉冲）一次成功
- 0171：用 GENI 把 ACF 里 44 条 Music profile 写进两颗芯片
  （先写 `0x00=0x55aa` 软复位），并把 GPIO121..124 设为 mi2s2（mux 1），
  让功放能看到 I2S 的 BCLK/WS

## 2026-10-25 00:15 —— run53（0171）：★ 功放全部初始化完成
- `tdm pins 121-124 on mi2s2`
- `geni amp 0x34 r=0 id=0x2032` → **`profile 44/44`**
- `geni amp 0x35 r=0 id=0x2032` → **`profile 44/44`**
- 两颗 AW882xx 的 ACF Music profile 全部写入成功，**硬件通路只剩数据面**
- 数据面复盘（对照 audioreach-graphservices 源码）：
  - `mem_pool_id=3`（SHMEM8_4K）✓ 与 vendor 一致
  - 结构 `{u16 pool; u16 nregions; u32 flag; region[12]}` ✓
  - `property_flag` 我们发的是 4 = `IS_OFFSET_MODE`。vendor 的用户态传
    dma-buf **offset**，内核 GPR 驱动发送前把地址字段改写成真实 PA，
    flag 保持 offset 语义 —— 我们直接传 PA 却标着 offset，DSP 会按
    offset 解析，这是 `status=1` 最可能的原因
- 0172：`property_flag=0`（纯物理地址）

## 2026-10-25 00:25 —— run54（0172）：仍是 BADPARAM，找到真正的包格式错误
- `property_flag=0` 后结果不变：`shm_map: failed (status=0x1)`
- 对照 `g_apm_cmd`（graph open/prepare/start 都 status=0）与手写 SHM_MAP
  包，发现**手写包的 GPR 头少了 16 字节 payload 描述区**：
  - `g_apm_cmd`：pkt+24..39 = {payload_address_lsw, msw,
    mem_map_handle, payload_size}，**payload 从 pkt+40 开始**
  - 手写 SHM_MAP 把 payload 直接放在 **pkt+24**，DSP 把 mem_pool_id
    当 payload_size 读 → 必然 AR_EBADPARAM
- 顺带发现 `src/sweet_glink.c` 落后于 CI 实际代码（缺 0158 的固定 PA
  逻辑），已同步，避免后续 patch 基线错位
- 0173：SHM_MAP 改走 `g_apm_cmd`（20 字节 payload 从 pkt+40 起），
  响应 handle 从 w32 取

## 2026-10-25 00:40 —— ★ 发现 CI 会静默跳过冲突的 patch
- run55/56 的 SHM_MAP 仍走旧代码（日志里 `valid=1` 还在），尽管 release
  digest 已经更新、CI 显示 success
- 根因：`blacktop/darwin-xnu-build` 的 `build.sh` 对每个 patch 执行
  `git apply --check`，**失败时只打 warning 就跳过**：
  `Skipping patch ... (already upstream, previously applied, or malformed)`
  —— CI 不会因此失败
- 0173（两个 hunk：改声明 + 改函数体）被整体跳过
- **教训：patch 必须小而单一，上下文只依赖确定存在的行**
- 0174：改成 **单 hunk**，不动声明（复用 `pkt` 缓冲区前 20 字节作为
  `g_apm_cmd` 的 payload），本地 `patch --dry-run` 通过

## 2026-10-25 01:10 —— run61/62：SMMU 关闭、8 个 pool 全灭
- run61 的 SMMU 探测：`idr0=0 idr1=0 scr0=0x200406`
  → **SMMUEN(bit0)=0**，apps-smmu **根本没使能**，流表 **0 条有效项**
- run62：`mem_pool_id 0..7` × `0xa2d00000` × flag=0 → **全部 status=0x2**
- 叠加 run59 的 4 地址 × 2 flag，结论：
  **DSP 拒绝所有“未经它登记”的 DDR 地址**，与 pool/flag 无关
- vendor 能成功，是因为 Android 内核在 SMMU 里替 DSP 建好了映射，
  DSP 拿到的是 IOVA；我们把裸 PA 塞给它，它自己的白名单不认
- 顺带确认：`adsp_heap_region` = 12 MB shared-dma-pool，
  `alloc-ranges = 0..4 GB` → **DSP 的 DMA 内存本来就在 4 GB 以下**
- 0180：先做决定性自证 —— 写魔数到 kmem_alloc 的页，再按
  `pmap_find_phys` 给出的物理地址 `ml_io_map` 回来读：
  - **MATCH** → 物理地址真实，SMMU 方案可行
  - **MISMATCH / 映射失败** → SHIM 虚拟化了物理地址，SHM 路线彻底死
- 同时把 pool 扫描收敛回单次（每轮 200 秒超时会拖死设备）

## 2026-10-25 09:2x —— run63 自检 panic：物理地址是真的，但地址选错了

### run63 panic（pmap_assert_free @ pmap.c:2811）
- `pmap_assert_free: Found at least one mapping to 0x8e93cc000. First PTEP ... is a
  Kernel CPU mapping`，被释放页 PA=0x8e93cc000（高区 0x840000000+，即 XNU 的 DRAM 银行）
- **原因：自检自己作死**——`ml_io_map(pa)` 的内核 PTE 永不解除，紧接着
  `kmem_free(va)` 释放该页 → pmap 断言“页仍被映射”
- **但结论已经拿到**：panic 前 mirror 读取没有 fault，说明 `pmap_find_phys` 返回的
  是真实可访问 DRAM；高区页可以被再映射

### 真正的发现：以前所有 SHM 地址都在保留区里
Android 实机 `/proc/iomem`（2026-10-25 读取）：
| 试过的地址 | 实际身份 |
|---|---|
| 0xa2d00000 | 保留区末尾（ADSP carveout 0x9fd00000–0xa2dfffff），RAM 从 a2e00000 起 |
| 0x80900000 | 保留（smem） |
| 0x9fd00000 | 保留（ADSP 固件） |
| 0xb8000000 | 保留（显示） |
| 0x80000000 | 保留（hyp） |
| 0x844af0000 等 | 高区 System RAM ✓，但只试过 flag=4 |
- **低 4GB 的普通 System RAM 一次都没试过**。Android 的音频缓冲区就在这类区间
  （DMA heap / System RAM），DSP 才认。

### 0181（CI b5974fb）
- `g_phys_selftest` 不再 `kmem_free`（页泄漏掉，同时作为高区候选）
- SHM_MAP 地址表改为 System RAM：`0xa2e00000 / 0x86000000 / 0x80640000` + 自检页
- flag 先 4（vendor GSL 的 offset 模式）后 0（PA 模式）
- 修成功响应解析：`APM_CMD_RSP_SHARED_MEM_MAP_REGIONS (0x02001001)` 的
  handle 在 **+24**（旧代码读 +32，即使 map 成功也会被当成失败）
- PA 模式时 DATA_BUFFER 里的地址改为 `g_shm_pa + off`
- 顺带删掉 g_audio_tone 里 4 个浪费时间的 map_probe

## 2026-10-25 09:3x —— SHIM 开机图（用户 PSD）
- `splash.png`（用户 PSD 导出，1080×2400）→ build_xnu.sh 自动转 BGRA
- `splash.S` 用 `.incbin` 放在自定义 `.splash` 段（**不能放 .rodata**：10 MiB 会把
  `_stack_top/_bss_*` 推出 start.o 的 ADR ±1 MiB 范围，ld.lld 直接报错）
- `shim.ld` 把 `.splash` 放在栈之后、`_end` 之前（payload 偏移仍按 `_end` 计算）
- `con_image()` 按 FB_STRIDE 逐行拷贝；文字底色改成黑块（白底图上可读）
- 真机验证：**显示正常**（无 payload 的测试镜像，10.5 MB）

## 2026-10-25 09:40 —— CI 源码树复现（以后改补丁必须先做这一步）
- 0181 第一次推送后 CI 报 success 但**补丁被静默跳过**（release 里没有 marker）
- 根因：工作副本 `src/sweet_glink.c` 和 CI 实际源码漂移：
  - CI 里 **0172 也是被跳过的**（b5974fb 的树里没有它的内容）
  - CI 的 sweet_glink.c 比工作副本**新很多**（有 `g_prm_vote_clock_ex`、
    `g_apm_state_query_sync`、新的 map_probe 五连等，工作副本都没有）
- **复现方法（可复用）**：
  1. macOS 13.5 → `distribution-macOS/macos-135/release.json` → xnu tag **xnu-8796.141.3**
  2. 下载 `https://github.com/apple-oss-distributions/xnu/archive/refs/tags/xnu-8796.141.3.tar.gz`（14 MB）
  3. 解压后 `git init`，按文件名顺序 `git apply --check && git apply`，失败跳过
     （与 dxb 的 build.sh 完全一致）
  4. 树在 `work/ci-src/xnu-xnu-8796.141.3`；改代码后 `git apply --check` 验证再推
- 本次本地复现的跳过项：0031/0041（依赖 3 个 .sh 暂存的 HFS 源码，CI 先跑 .sh 所以 CI 会应用，
  与 sweet_glink.c 无关）、0172（CI 同样跳过）、旧 0181（已重写）
- 重写后的 0181 已在复现树上 `git apply --check` 通过（commit 0a8fcc1）

## 2026-10-25 09:5x —— vendor 抓包 + FDT 把数据面彻底讲清楚了
### kprobe trace 里的真实 MAP_REGIONS（trace_ui.txt，p5=0x100100C）
```
p0=0x2C60(44B) p1=0x10302 p2=0x2002 p3=1 p4=45 p5=0x0100100C
p6=0x00010003 (pool=3,count=1) p7=4(offset模式) p8=0x1FFE8000 p9=1 p10=0x5000
```
- **MAP 报文是 44 字节：payload 直接在 +24，没有 16 字节 descriptor**；
  只有 graph/data 命令才用 descriptor（GRAPH_OPEN 40B：descriptor 里是 handle+size；
  DATA_BUFFER 68B：descriptor 里是 offset/handle/len，后面 28B 全 0）
- 0175 按 descriptor 布局发 MAP 是错的 → DSP 读到 pool=0/count=0
- p1 的 0x10000 位只有 MAP 用（domain word 0x10302 vs 其他 0x302）
### 地址 0x1_1FFE8000 是什么
- `/proc/iomem` 里 4–32 GB 完全不存在 → **不是物理地址**
- FDT: `/soc/spf_core_platform/qcom,msm-audio-ion`
  - `iommus = <&apps_smmu 0x1801 0x0>`、`qcom,smmu-sid-mask = 0xf`
  - `qcom,smmu-version = 2`、`qcom,iommu-dma-addr-pool = <0x10000000 0x10000000>`
- audio_pkt_dlkm.ko 的 `audpkt_chk_and_update_physical_addr`（反汇编确认）：
  只检查 client_data bit2（GSL 的 offset 模式），然后
  `msm_audio_get_phy_addr(fd, &addr, &size)` 并把返回的 64 位值覆盖到包里的地址字段
  → 厂商路径 = dma-buf → **apps-smmu（SID 0x1801）映射出来的地址** → 所以是 4.5 GB 的 IOVA
### SMMU 现场（之前误读过两次）
- sCR0 @0x00：bit0 是 **CLIENTPD**（0=端口启用），bit10 是 **USFCFG**（1=未匹配流 abort）
  本机 `scr0=0x200406` → SMMU 是**启用**的，且未匹配流**会报错**
- ID0/ID1 在 **0x20/0x24**（不是 0x40/0x44）；SMR @0x800，VALID 是 **bit31**
- FDT `qcom,handoff-smrs = <0x2800 0x402>` → 开机就有一条 SID 0x2800 → CB2(translation)
- 音频 SID 0x1801 在 XNU 启动里没有 SMR → 原本若读音频缓冲区会 fault
### 0181 v4（d8d8e14）
- MAP 改成 44 字节 flat 布局 + 0x10000 domain 位
- 候选地址：低区 System RAM（a2e00000/86000000/80640000）+ 自检页（高区）
  + 厂商 IOVA 0x1_1FFE8000（只探测状态，绝不喂数据）
- **清 sCR0.USFCFG**（单 bit）：让未匹配流（含 0x1801）走 bypass，DSP 直接用 PA 读
  （已有 SMR 的流如 UFS 不受影响）
- 成功响应解析：0x02001001 为 MAP 应答，handle 在 +24

## 2026-10-25 10:0x —— run64 突破：低区 PA 被 APM 接受！
```
SWEET SMMU: idr0=0 idr1=0 scr0=0x200406      (旧探测，偏移错)
SWEET PHYS: va=0xfffffe448ce8c000 pa=0x8ea6d4000
SWEET PHYS: mirror=0xffffffff MISMATCH
SWEET GLINK: shm_map pool=3 pa=0xa2e00000 flag=4 play=1
SWEET GLINK: shm_map rsp op=0x100100c status=0 hop=0x2001001 len=28
SWEET GLINK: shm handle=0xb0e15558 len=28
SWEET GLINK: shm mapped pa=0xa2e00000 mode=offset
→ panic phystokv_range: Illegal PA 0xa2e00000（之后才发生）
```
### 结论
1. **44 字节扁平 MAP 报文正确**，0x02001001 成功应答，handle=0xb0e15558
2. **APM 接受低区 System RAM 物理地址**（a2e00000 + offset 模式）
3. panic 不是 ml_io_map —— **ml_io_map(0xa2e00000) 成功了**；
   是紧接着的 `flush_dcache64(pa)` 调 `phystokv_range()` 才 panic
   → 低区 PA 可以用 io 映射访问，**只是不能做 dcache 刷写**
4. `SWEET PHYS mirror MISMATCH` 是**缓存假象**：写入走可缓存内核映射，
   `dsb sy` 不清 D-cache，随后用 uncached io 映射读 DRAM 当然读不到；
   不代表地址被虚拟化（之前 run63 的 MATCH 推断作废）
5. run64 的 SMMU 探测仍是旧偏移代码（idr0 读 0x40），v5 已修

### v5（8ed048a）
- 低区候选保留 `ml_io_map`（可用），**删除 flush_dcache64**（改用 dsb sy）
- SMMU：ID0/ID1 读 0x20/0x24、SMR VALID=bit31；
  若 SID 0x1801 无 SMR → 占一个空槽写 `SMR=VALID|0x1801`、`S2CR=0x10000`(BYPASS)，
  再 TLBIALLNSNH + TLBGSYNC；已有 SMR 则只报告不覆盖
- 同时清 sCR0.USFCFG
- SHIM 在跳 XNU 前把同样的 PCM 波形预填到 0xa2e00000（双缓冲，96k/32bit/stereo）

## 2026-10-25 10:0x —— run65 panic：SMMU SMR 越界读
- v5 首次真正跑到 SMMU 探测（run64 是旧探测代码）
- panic: Kernel data abort (external), far=0xfffffe6c059e09c4，映射基址 0xfffffe6c059e0000
  → 偏移 **0x9C4 = SMR[113]**
- x19=0x200406(sCR0)、x28=0x1801(SID)、x27=0x71(循环下标 113)
- 根因：循环硬编码 `n < 120`，但这片 apps-smmu 只有 **113 个 stream-matching 组**
  （Android dmesg: "stream matching with 113 register groups"；ID0[7:0]=0x71=113）
  读未实现的 SMR[113] → 总线外部中止
- v6（8558830）：循环上界改为 `ID0 & 0xff`，非法值回退 113；循环前 sync；
  SMR dump 每条也 sync
- 另：手机在 panic 前震了一下（触感初始化），与本次 panic 无因果关系

## 2026-10-25 10:14 —— run66（v6）：SMMU 通了，但发现数据面打错了目标
- **没有 panic**，SMMU 探测完整落盘：
  - `id0=0x4c017e71 id1=0x6000004b scr0=0x200406` → NUMSMRG=0x71=113
  - 6 条有效 SMR：`0xe0` `0x4a0` `0xc0` `0x2800 mask=0x402`（= qcom,handoff-smrs）
    `0x0` `0x584 mask=0x11`；**没有 SID 0x1801**
  - → 强制在 smr6 写 `SMR=VALID|0x1801`、`S2CR=0x10000`(bypass)
  - `USFCFG cleared 0x200406 -> 0x200406`：该位写不进去（RO 或需别的条件），
    但 0x1801 已有匹配项，不再依赖 USFCFG
- SHM：`shm_map pool=3 pa=0xa2e00000 flag=4` → status 0、handle 0xb0e15558、
  `va=0xfffffe3020614000`，**无 flush、无 panic**
- 厂商图 open/prepare/start 全 0；但 DATA_BUFFER 发到 0x4012/0x48ba/0x4007/0x418e
  全部 **no rsp**
- ★ 解 kprobe 原始报文（trace_ui.txt）找到根因：
  - 厂商 DATA_BUFFER_V2：`src=0x2011  dst=0x40BD  token=0/1  len=68`
    载荷 `{offset,0,handle,size}`，与我们的包逐字节一致
  - 同一图里 **0x40BD 收到 SET_CFG `0x0800100C`(media format，30B)**
  - 结论：**AP 输入端点必须是 WR_SHARED_MEM_EP(0x07001000) 的实例，SAL 不是**
    （SAL 对 0x0800100C 一直回 status 3 也印证这一点）
- 厂商是「流图 0xb00000d6（WR 开头）+ 设备图 0xb0000002（SAL 开头）」两张图，
  跨图连接 0x4655:1 → SAL:2

## v7（0426fde，CI 构建中）
- 把 **一个 WR_SHARED_MEM_EP 合并进设备图**：模块表 +{0x07001000, iid 0x40bd}，
  端口表 +{0x40bd,[in=0,out=1]}（取自 0xb00000d6 的 dump），
  连接表 +{0x40bd:1 → 0x4012:2}
- media format 改发 0x40bd；DATA_BUFFER 首个探测目标 0x40bd，token 改 0/1
- 新图 open 失败时回退旧厂商图（对照）
- 判据：`graph_open/vp-wr` status=0 → `vendor graph + WR input started` →
  `buffer probe dst=0x40bd -> 0` → `pcm buffer 0..N`

## 2026-10-25 10:28 —— run67（v7）：图接受 WR 模块，但 map/fill 放在 START 之后把机器冻住
- `graph_open/vp-wr` **status=0**（800 B 包 = 24+16+760，合并 WR 模块的图被 DSP 接受）
- **`vendor graph + WR input started`**：带 WR 输入的图 START 成功
- SMMU/PHYS/SHM 同 run66：0x1801 bypass、handle 0xb0e142e8、va=0xfffffe549c230000
- ring 停在 `shm mapped`，**之后再无任何日志**（没有 TX_DATA、没有 buffer probe、没有 panic 行）
  → 冻死在 g_fill_tone/dsb（START 之后才 map+写低区）
- ★ 解厂商报文顺序（trace_ui.txt）：
  `OPEN → SHM_MAP → SET_CFG → PREPARE → START → DATA_BUFFER`
  **厂商在 START 之前就 map 好共享内存**；我们原来是 START 之后才 map+fill，
  运行中的 WR 模块已经能看到这块区域，AP 再去写就撞上了
- 另：本轮的 I2C 扫描 0 个功放（0x34/0x35 无 ACK），与冻结无因果关系（扫描在 GLINK 之前）

## v8（99cda0c，CI 构建中）
1. 新增 `g_pcm_prepare()`：**OPEN → SHM_MAP(+SMMU+selftest) → fill → SET_CFG → PREPARE → START**
2. `g_audio_tone()` 复用已映射/已填充状态，不再重复写
3. 删除 5 个候选 IID 的盲试，**只发 0x40bd**（证据：厂商全部 DATA_BUFFER 都发给 WR 实例）
4. 判据：`pcm: tone filled before start` → `vendor graph + WR input started`
   → `buffer probe dst=0x40bd -> 0` → `pcm buffer N`

## 2026-10-25 10:52 —— run69（v9）：SMMU 不接受带 MASK 的 SMR，store 直接外部中止
- panic: Kernel data abort，esr=0x96000010（同步外部中止），far=0xfffffe101049c818
- 用 `nm -n out/kernel.adsp` 符号化：pc 落在 **`_g_apm_shm_map + 0x1b0`**（g_smmu_probe 被内联）
- objdump 对上精确指令：`str w10,[x11,x9]`，w10=**0x800f1800**，x11=SMR base，x9=free_slot*4=0x18
  → 崩溃的就是「把带 MASK=0xf 的 SMR 写进 SMR[6]」这一条
- 结论：这台 SMMU 对 **MASK≠0 的 SMR 写入直接报外部中止**；精确匹配的 SMR 写入
  在 run66–68 已证明安全。DTB 的 `qcom,smmu-sid-mask=0xf` 不能在硬件上这样落地
- 注：ring69 里 SMR 表已正确解码（新的 mask 打印）：smr3 0x2800/0x402、smr5 0x584/0x11

## v10（c396beb，CI 构建中）
- 放弃 masked SMR，改为 **16 条精确匹配 bypass SMR：SID 0x1800–0x180f**（槽 6–21）
  每条都写 `{S2CR=0x10000(BYPASS), SMR=VALID|SID}`，只使用已知安全的写模式
- 顺带打包 `0182`：无真实采样时 SweetBattery 不再谎报 AC（拔线不再显示充电图标）
- 判据：`SWEET SMMU: exact bypass smr6..21 sid 0x1800-0x180f` → 之后
  `tone filled before start` → `buffer probe dst=0x40bd`

## 2026-10-25 11:03 —— run70（v10）：不是 MASK 的问题，是 SID 0x1800 本身
- panic pc 0xfffffe00074fc9d4，objdump 对应精确指令：
  `str w13,[x14,x12]`，w13=**0x80001800**（VALID|SID 0x1800，无 MASK），x12=0x18
- far = SMR[6]（0x818），esr=0x96000010 外部中止；**前一条 S2CR[6]=0x10000 写入成功**
- 对照：run66–68 写 `0x80001801`（SID 0x1801）一直安全；run69（masked 0x1800）和
  run70（精确 0x1800）都在 SMR store 崩
  → **NS 世界不能给 SID 0x1800 写 SMR**（该 SID 很可能是 secure/保留），
    与 MASK 无关。0x1801 才是 AP 可用的音频流
- 注：v9/v10 同时改了顺序（S2CR 先），但 0x1800 是两次崩溃的唯一共同点；
  0x1801 的结论不受影响

## v11（f5d98f5，CI 构建中）
- 恢复 **精确 SID 0x1801** 的 bypass，沿用 run66–68 验证过的顺序（SMR 先、S2CR 后）
- 增加**安全回读**（SMR[6]/S2CR[6] 在 dump 阶段已证明可读），确认写入是否真的生效
- 其余保持：WR 合并图、map/fill 在 START 前、DATA_BUFFER 只发 0x40bd

## 收尾路线（用户 2026-10-25 定）
1. 电源：pmic_glink（battmgr/ucsi）→ 真实充电状态 + 图标
2. 声音：控制面已通；数据面卡在“DSP 取第一块 buffer 时全机冻结”
3. 震动：硬件已可用（sysctl），补 iOS 侧触发
4. 稳定性：清掉所有会 panic 的探测，危险实验改为 boot arg 开关
5. 暂停，留文档给后来者

## 2026-10-25 11:13 —— run71（v11）：回读抓到真凶——NS 写不进 S2CR.BYPASS
- 没有 panic，启动走完 SMMU/SHM/GRAPH/START
- **回读结果：`smr=0x80001801 s2cr=0x20000`**
  - SMR 写入生效（VALID|0x1801）
  - S2CR 我们写的是 `0x10000`（TYPE=1 BYPASS），读回却是 **`0x20000`（TYPE=2 FAULT）**
  → **非安全世界不能把 S2CR 设成 bypass**（安全策略拒绝），该流被强制 FAULT
- 这完整解释了数据面冻结：
  SHM_MAP 只是记录地址，DSP 不会碰内存；第一个 DATA_BUFFER 让 DSP 真正去读
  → SID 0x1801 命中 FAULT → DSP 访问中止 → 互连挂死 → AP 在发送路径上冻结
- ★ 结论：**音频数据面必须走 SMMU stage-2 翻译**（厂商路线），bypass 不可用
  - 需要：一个空闲 context bank（CB6+）、一个小页表把 IOVA 映射到我们的 buffer、
    `S2CR = TYPE_TRANSLATION(0) | CBNDX`
  - 参考厂商：IOVA `0x1_1FFE8000`、`qcom,msm-audio-ion`、`qcom,smmu-sid-mask=0xf`
- 本轮同时包含 0182：无真实采样时不再谎报 AC（电池图标修复已进内核）

## 收尾 handoff 要点（给后来者）
1. 音频控制面已通；数据面只差 SMMU stage-2 翻译这一块，bypass 被硬件拒绝
2. 电源走 pmic_glink（ADSP/PMIC 固件），不是 SPMI 寄存器；需 battmgr/ucsi 客户端
3. 震动硬件可用（`sysctl kern.sweetvibe=1`），缺 iOS 侧 HID haptic 事件接线
4. 稳定性：默认路径已无 panic；SMMU 探测是唯一危险区，改动时务必先读回验证

## 2026-10-25 11:3x —— Android 侧 kprobe 取证（音频 SMMU + pmic_glink）
环境：备用机已进 Android（5.10.226-android12-9），SukiSU root，tracefs kprobe 可用。
用 `/data/adb/post-fs-data.d/99_kprobe_early.sh` 在开机早期挂探针。

### 音频 SMMU（arm_smmu.ko，模块符号可 kprobe）
开机早期抓到：
```
s2cr arm_smmu_write_s2cr idx=0x4d
smr  arm_smmu_write_smr  idx=0x4d
s2cr arm_smmu_write_s2cr idx=0x4d      (约 3 ms 后，再次写)
s2cr idx=0x4e / smr idx=0x4e / s2cr idx=0x4e
```
★ 厂商行为：**从高索引（0x4d/0x4e）分配**，顺序是 **S2CR → SMR → S2CR**。
我们之前用 idx 6，且只写一次 S2CR，回读是 FAULT。v13 改为镜像厂商：
高槽位 0x4d + 三步顺序 + 精确 SID 0x1801，然后回读验证。

### pmic_glink（pmic_glink.ko）
`pmic_glink_write` 每秒数千次；header 抓取成功：
```
owner=0x800A (= BATTMGR)  type=1  op=0x30/0x32/0x50  len=0x18
```
- 0x800A 与上游 `PMIC_GLINK_OWNER_BATTMGR` 一致
- 说明充电状态/控制确实走 pmic_glink 的 battmgr 客户端，XNU 需要实现
  「打开 PMIC_GLINK 通道 + 发 battmgr 请求 + 解析响应」
- 探针脚本：`/data/adb/post-fs-data.d/99_kprobe_early.sh`（取证完应删除）

### pmic_glink 协议（kprobe 实抓，2026-10-25 11:2x）
- DT: `qcom,pmic-glink-channel = "PMIC_RTR_ADSP_APPS"`，subsys=lpass，
  protection-domain=`tms/servreg msm/adsp/charger_pd`
- 发送（pmic_glink_write，AP→PMIC）：24 B
  `{owner=0x800A(BATTMGR), type=1, op=0x30, prop_id, 0, 0}`
  - op=0x30 是 GET_PROP，prop_id 在 +12（p1）
  - 另有 op=0x32 / op=0x50（周期查询/时间等）
- 回调（battery_chg_callback，PMIC→AP）：同样 24 B
  `{owner=0x800A, type=1, op=0x30/0x32/0x50, prop_id, value, 0}`
  - 例：op=0x30, prop=0..4/7, value=1/4 …
- ★ XNU 侧实现路径：用现有 GLINK 传输打开 **PMIC_RTR_ADSP_APPS** 通道，
  发 24 B GET_PROP 请求，解析回调里的 prop_id/value，喂给 SweetBattery
- 待办：prop_id 与 power_supply 属性的精确对应（需要 vendor 驱动源码或逐项对照）

## 2026-10-25 11:37 —— run73（v13）：厂商顺序 + 高槽位也没能让 BYPASS 生效
- `vendor-order bypass at smr77 readback smr=0x80001801 s2cr=0x20000`
  - SMR 写入成功（SID 0x1801）
  - S2CR 仍是 **TYPE=2 FAULT**：NS 世界不能写 BYPASS，与槽位/顺序无关
- ring 停在 `vendor graph + WR input started`，无 `buffer probe`：DSP 第一次取 buffer 再次挂死
- ★ 定论：这台 SMMU 的 bypass 对 NS 关闭；音频数据面只能走 **stage-2 翻译**
- v14（3d04784）：写 `S2CR=0x6`（TYPE=0 翻译 / CB6）并回读，**不带 sweetpcm**，
  只验证“翻译型 S2CR 是否可写”，不碰数据面

## 2026-10-25 12:0x —— 从厂商源码核实 SMMU 布局（CB6 的关键修正）
本机 DT：`apps-smmu@15000000`，compatible `qcom,qsmmu-v500`，
`reg = <0x15000000 0x100000>, <0x151ce000 0x20>`，reg-names `base tcu-base`，
`qcom,num-context-banks-override = 0x4e`，`qcom,num-smr-override = 0x78`。

官方内核（Xiaomi marble-s-oss，与本机同源）已核对：
- `arm-smmu.c`: ID1=0x6000004b → PAGESIZE=0（4K）、NUMPAGENDXB=6
  → `size = 1 << (6+1) = 128` 页，`ARM_SMMU_CB(n) = numpage + n`
  ⇒ **CB0 = +0x80000，CB6 = +0x86000**（不是 mainline 的 +0x8000！）
  之前按 +0x8000 写会落进保留页，属于外部中止风险。
- `arm_smmu_write_context_bank()` 写序：
  CBA2R → CBAR → TCR → TTBR0(64-bit) → SCTLR
- S2 域 fmt=AARCH64 ⇒ `CBA2R.VA64=1`；8-bit VMID 放 `CBAR[7:0]`
- `io-pgtable-arm.c` 的 `arm_64_lpae_alloc_pgtable_s2()`：
  IAS=32、OAS=32、4K、coherent walk ⇒ PS=0、TG0=4K、SH=IS、RGN=WBWA、
  SL0=1、T0SZ=32 ⇒ **VTCR = 0x80003560**
- 2 MiB block 描述符：`PA | valid | AF | HAP_RW | OIWB` = `PA | 0x4fd`
- L1 索引 `(IPA>>30)&3`，L2 索引 `(IPA>>21)&0x1ff`
  对 0xa2e00000：L1[2] → L2[0x117]

XNU v17 实现：低区空闲内存 0xa3000000/0xa3001000 放两级表，恒等映射
IPA 0xa2e00000 的 2 MiB；CB6 配 S2 翻译；SMR[0x4d]=0x80001801、
S2CR[0x4d]=0x6。**只在 `sweetpcm=1` 时编程**，默认启动不再碰 SMMU。

## 2026-10-25 12:2x —— run76 panic：io_submap 耗尽（不是 SMMU 本身）
panic: `kmem_alloc(..., 1048576, ...): failed with 3 @vm_kern.c:178`
- `ml_io_map(0x15000000, 0x100000)` 想映射 1 MiB
- `osfmk/arm/io_map.c`: `IO_MAP_SIZE = 8 MiB`，所有 ml_io_map 共用一个
  `io_submap`；`sweet_spmi.c` 的 chnls 映射就占了 4 MiB，余量不足 1 MiB
  ⇒ KERN_NO_SPACE(3)
- 修正：GR0/GR1 用 `ml_io_map(0x15000000, 0x2000)`，
  CB6 页单独 `ml_io_map(0x15086000, 0x1000)`；寄存器访问改走 cb 基址
- ★ 教训：这个移植里 **ml_io_map 不是无限资源**，一次最多几十 KiB 地要；
  需要大窗口时用 `g_win_alloc/g_win_map`（pmap_map 到 kernel_map）

## 2026-10-25 12:3x —— v18：SMMU 窗口改走 kernel_map；新增 pmic_glink 电池读取
- run76 的 panic 出在取 VA 阶段，SMMU 寄存器一个都没写，所以 **CB6 布局仍未验证**
- v18：`g_win_alloc/g_win_map`（kmem_alloc kernel_map + pmap_map）分别映射
  - GR0/GR1：0x15000000 + 0x2000
  - CB6 页：0x15086000 + 0x1000
  不再消耗 8 MiB io_submap
- 同时加入 `sweetbatt=1` 的 pmic_glink 客户端（默认关闭）：
  - 打开 `PMIC_RTR_ADSP_APPS`（ADSP 侧每轮启动都会发 OPEN，rcid=6）
  - 回 OPEN_ACK、advertise 一个 128B intent（id 0x100）
  - 用 24B GET_PROP 读：batt-status(0x30/0)、capacity(0x30/4)、
    vbat(0x30/7)、ibat(0x30/9)、temp(0x30/13)、usb-online(0x32/0)
  - 采样交给 SweetBattery（0184）；没有 sweetbatt 时行为不变
- 已从 Android 移除 `/data/adb/post-fs-data.d/99_kprobe_early.sh`
  （备份在 `/sdcard/99_kprobe_early.sh.bak`），开机不再挂 kprobe

## 2026-10-25 12:4x —— CB6 的 CBAR 写不了：hypervisor 拥有 CB0..51
run77 panic（有 ring 为证）：
- CB6 的预读全部成功：`cbar=0 cba2r=0 sctlr=0x203d6716 vtcr=0x40084815`
- 然后 `str w8,[x19,#0x1818]`（CBA2R）成功，`str w8,[x19,#0x1018]`（CBAR）
  外部中止（ESR 0x96000010）
- 反汇编确认：同一个地址 **读得到、写不了**
- Android 运行时 kprobe（带参数）：
  `__arm_smmu_alloc_cb -> 0x34`，`arm_smmu_write_context_bank idx=0x34`
  第二个域 `-> 0x35`
- 结论：DT 有 `hypervisor` 节点；**CB0..51 归安全世界**，NS 可用的第一个是
  **CB52**。厂商给音频 SID 0x4d/0x4e 用的就是 CB52/53
- 另：厂商 S2 的 VMID 规则是 `vmid = cbndx + 1`（arm-smmu.c），CB52 → VMID 53
- v19：CB52 页 = 0x15000000 + (128+52)*0x1000 = **0x150B4000**；
  CBAR(52)=+0x10D0、CBA2R(52)=+0x18D0；S2CR[0x4d]=0x34；CBAR=0x35

## 2026-10-25 12:5x —— CBAR 写中止的真正嫌疑：sACR.CACHE_LOCK
- run79：CB52 的预读正常（cbar=0 cba2r=0），CBA2R(52) 写成功，
  **CBAR(52) 写再次外部中止**（far = v+0x10D0，同一 PC）
- 即：不是 CB6/CB52 的选择问题，而是 CBAR 本身被锁
- `arm-smmu-qcom.c: qsmmuv500_cfg_probe()` 在 probe 时做：
  `sACR &= ~ARM_MMU500_ACR_CACHE_LOCK (bit 26)`，然后 WARN 回读
- 我们的 XNU 从未清过 sACR → v20 先清 bit26 并回读；
  **回读仍有锁就不写 CBAR**（避免再 panic）

---

# 阶段性结论汇总（2026-10-25 13:00 固化）

> 本节是给接手者的"一眼看全"总结；细节在上面时间线里。
> 逐条都对应一个 run 或一段源码，没有推测成分；推测已标注。

## 1. 四个目标的当前状态

| 目标 | 状态 | 卡在哪 |
|---|---|---|
| 声音 | **控制面全通，数据面卡 SMMU** | CBAR 写不进去（sACR 锁，待 run80 验证） |
| 电源 | **协议与通道已打通，待真机验证** | `sweetbatt=1` 还没跑过 |
| 震动 | **硬件通 + 已接电源键** | 按键触发待真机确认 |
| 稳定性 | **默认启动已无已知 panic** | 危险路径全部 `sweetpcm` 开关隔离 |

## 2. 声音失败的完整归因链（可复用）

```
APM 收下 SHM_MAP(pa=0xa2e00000, flag=4) → handle OK
  ↓
图 OPEN/PREPARE/START status=0
  ↓
DSP 第一次 DATA_BUFFER_V2 需要读 pa=0xa2e00000
  ↓
apps-smmu 上没有覆盖该 PA 的 SID 0x1801 映射
  ├─ 想改成 BYPASS（0x10000）→ 硬件拒绝，回读 FAULT（0x20000）
  ├─ 想用 bootloader 留下的 CB3 → 不覆盖该 PA，DSP 冻结
  └─ 想自己配 CB → CBAR 写外部中止
       ├─ CB6：CB0..51 是 hypervisor 的（安卓 alloc 从 0x34 开始）
       ├─ CB52：CBA2R 能写、CBAR 不能写
       └─ 待验证：sACR.CACHE_LOCK(bit26) 锁住 CB 配置（v20）
```

## 3. 三个"别再试"的死路（省后来者的时间）

1. **S2CR bypass**：`0x10000` 写进去回读 `0x20000`。NS 永远写不了。
2. **SID 0x1800 / 带 mask 的 SMR**：外部中止。只能用精确 `0x1801` 精确写。
3. **CB0..51 的 CBAR**：读得到、写中止。CB52 是 NS 的第一个可用号。

## 4. 两条通用工程教训（与音频无关也会反复遇到）

- **`ml_io_map` 是共享的 8 MiB 空间**（`IO_MAP_SIZE`），不是随便用。
  MB 级映射必须 `kmem_alloc(kernel_map)` + `pmap_map`（`g_win_alloc/g_win_map`）。
  run76 的 panic 就是这个，取 VA 阶段就死了，连 SMMU 都没碰到。
- **低区 PA 可以映射读，但绝不能 `flush_dcache64`**：
  `phystokv_range()` 只认 ADT 银行（0x840000000+），低区 PA 会 panic。
  用 `dsb sy` 保证可见性。

## 5. 现在唯一的判据（run80）

```
SWEET SMMU: sACR 0x........ -> 0x........
  bit26 掉 → SWEET SMMU: S2 CB52 smr=... s2cr=0x34 cbar=0x35 cba2r=0x1
             → 再看 buffer probe dst=0x40bd -> 0
  bit26 没掉 → SWEET SMMU: CACHE_LOCK denied, CBAR untouched
             → NS 无法配置 CB，此路终止，改复用安卓已建上下文
```

## 6. 补丁清单（当前内核里生效的）

| 补丁 | 内容 |
|---|---|
| 0181 | SMMU 探测 + stage-2（CB52、VTCR、两级表）+ SHM 低区 + **只在 sweetpcm=1 时写** |
| 0182 | 无真实采样时不再谎报 AC |
| 0183 | 电源键按下触发 PM8350B 震动（`sweet_haptics_kick`） |
| 0184 | SweetBattery 优先用 pmic_glink 采样（`sweet_batt_get`） |

## 7. 设备状态与清理

- Android 侧的 kprobe 脚本已全部删除：
  `99_kprobe_early.sh`（备份 `/sdcard/99_kprobe_early.sh.bak`）、`99_probe2.sh`
- 只改过 RAM：没有任何分区被写；GPT 未动
- 引导脚本 `../xnu-gh/scripts/boot_ram.sh` 已能等 recovery 120 s

## 2026-10-25 13:0x —— run80：sACR 假设被证伪，但顺带解决了 CBAR
ring80 输出：
```
SWEET SMMU: sACR 0x1c -> 0x1c
SWEET SMMU: CB52 pre cbar=0 cba2r=0 sctlr=0x3010631a vtcr=0x20201280 ttbr0=0x1000b0d101041d0
→ panic (data abort, ESR 0x96000010)
```
反汇编 run80 的 kernel（PC 0xfffffe00074fdc9c）：
```
0x...dc94: str w8, [x19, #0x18d0]   ; CBA2R(52) = 1   ← 成功
0x...dc98: mov w8, #0x35
0x...dc9c: str w8, [x19, #0x10d0]   ; CBAR(52) = VMID 53 ← 外部中止
```
- **sACR = 0x1c，bit26（CACHE_LOCK）本来就是 0** ⇒ 清写没有任何效果，
  不是它挡住 CBAR。此假设作废
- 但注意 `CBAR` 的**回读值是 0**。`CBAR_TYPE_S2_TRANS = 0`、VMID = 0
  ⇒ **未被用过的 CB 本身就是 stage-2 上下文，根本不需要写 CBAR**
- 结论：CBAR 写不进去也没关系，只要不写它
- v21：删掉 CBAR 写，改为「回读确认 type==S2，否则放弃」；
  保留 CBA2R=1（VA64）+ CB 窗口的 VTCR/VTTBR0/SCTLR + SMR/S2CR
- 待验证的下一个风险点：CB 窗口本身的写（VTCR/VTTBR/SCTLR）是否也被拦
  —— 目前只知道它读得到、CBA2R 写得到

## 2026-10-25 13:1x —— run81：SMMU 配好了（重大进展），但数据面仍无回应
ring81（无 panic，CBAR 写删除后一切正常）：
```
SWEET SMMU: CB52 pre cbar=0 cba2r=0 sctlr=0x3010231a vtcr=0x20001380 ttbr0=0xf0d121041d0
SWEET SMMU: S2 l1[2]=0xa3001003 l2[279]=0xa2e004fd
SWEET SMMU: S2 CB52 smr=0x80001801 s2cr=0x34 cbar=0 cba2r=0x1
            sctlr=0xa5f0027 ttbr0=0a3000000 vtcr=0x80003560
```
- **四个寄存器回读 == 写入值**：CB 窗口（VTCR/VTTBR/SCTLR）和 CBA2R 都可写
  ⇒ 「NS 不能配 CB」的担忧解除；CBAR 保持 0（已是 S2/VMID0）即可
- 页表：L1[2]=0xa3001003（table），L2[279]=0xa2e004fd（2MiB block）正确
- 但 ring 停在 `vendor graph + WR input started`，第一次 DATA_BUFFER 没有回应
  （该函数 100 ms 上限会打印 `-> -1`，连这句都没有 ⇒ DSP/总线先挂死）

### 下一步的定位证据（本机 DT 实读）
```
/soc/spf_core_platform/qcom,msm-audio-ion
  iommus              = <&apps_smmu 0x1801 0x0>
  qcom,smmu-sid-mask  = 0xf
/soc/apps-smmu@15000000/#iommu-cells = 2
```
⇒ **匹配时忽略 SID 低 4 位**。我们之前写 `SMR=0x80001801`（MASK=0，只匹配
精确 0x1801）；DSP 只要present 0x1800..0x180f 中别的值就匹配不上，
而 `sCR0.USFCFG=1`（本机实测就是 1，写清无效）会让未匹配流被 SMMU 拦掉
→ 表现为 DSP 挂死、APM 无回应。v22 改为 `SMR=0x800F1801`。

## 2026-10-25 13:3x —— run82：mask 形式的 SMR 也不行（硬件拒绝）
- 手动引导 v22（看门狗因 marker 落在注释里被剥掉而误判成中间产物，未启动）
- panic 现场：`pc=...74fdce8  str w9,[x10,x8]`，`x9=0x800f1801`，
  `x8=0x134`（=4*0x4d），`far=v+0x934` = **SMR[0x4d] 那条 store 本身**
- 与 run69（`0x800f1800` 写 SMR[6] 中止）完全一致
  ⇒ **这台 SMMU 不接受任何 MASK≠0 的 SMR**；DT 的 `qcom,smmu-sid-mask=0xf`
  无法在 SMR 里表达，流只能精确匹配 `0x1801`
- v23：回滚为精确 SMR，并加**只读** CB 布局探测（CB0..15 的 SCTLR/VTCR/VTTBR0）
  理由：未用过的 CB 应全为 0，但 CB52 预读每轮都变（sctlr 0x3010231a /
  vtcr 0x20001380，连 RES1 bit31 都没有）⇒ 怀疑 CB 页换算在本机不成立；
  bootloader 明确用过 CB3（SMR3/S2CR3 → cbndx 3），用它当参照
- 教训（marker）：**watcher 的 marker 必须是代码里的字符串字面量**，
  写在注释里会被编译器剥掉，导致新产物被判成“中间构建”

## 2026-10-25 13:4x —— run83 panic：io_submap 又耗尽（这次是 16 KiB 也要不到）
`panic: kmem_alloc(…, 16384, 0x100021): failed with 3`
- `0x100021 = KMA_NOFAIL|KMA_PAGEABLE|KMA_PERMANENT`，正是 `io_map()` 的
  内部标志 ⇒ **共享的 8 MiB io_submap 已用光**
- 大头：`sweet_spmi.c` 的 chnls 窗口 `0x400000`（4 MiB）；
  其余 keys/touch/i2c/usb/ufs 各占几十~几百 KiB；16 KiB 是 4 KiB 映射按
  16 KiB 页向上取整的结果
- v24：把 `sweet_glink.c` 里**所有** `ml_io_map` 换成 `g_map_win()`
  （inner `kmem_alloc(kernel_map)` + `pmap_map`）：mmio dump、stage-2 表、
  SHM 候选、固定 SHM 页
- ★ 结论写死：**这个移植里任何新代码都不该再用 `ml_io_map`**；
  它是一次性、不可回收、全系统共享的 8 MiB

### 顺带：GUI/震动验证需要不重启的镜像
`mkimage.sh` 的默认 cmdline 带 `sweetreboot=1`（拿完日志自动重启），
所以人还没看到 GUI 就重启了。v24 给 mkimage.sh 加了
`KEEPALIVE=1`：构建时把 `sweetreboot=1` 去掉。用法：

```bash
cd xnu-gh && KEEPALIVE=1 D22AP_ADT=…/d22ap.adt ADSP_FW_DIR=…/adsp-fw \
  bash scripts/mkimage.sh sweetpcm=1
SERIAL=2aa26095 bash scripts/boot_ram.sh images/shim-iosroot-adsp.img
```

## 2026-10-25 13:5x —— run84（v24）：震动真机确认；CB 页假设被推翻；电池字段错位
- ★ **震动实测有效**：用户按电源键振了一下 ⇒ 0183 的
  `sweet_hid_key(0,1) → sweet_haptics_kick()` 在真机成立
- `SWEET AUDIO: build v24 (kernel_map windows)` 出现 ⇒ io_submap 修复生效，
  本轮不再 panic
- **CB 页假设被推翻**（关键）：
  ```
  cb0  sctlr=0xa9f00e0 vtcr=0 ttbr0=0
  cb1..cb5 sctlr=0xe0  vtcr=0 ttbr0=0
  cb6  sctlr=0x201d2716 vtcr=0x40084215 ttbr0=0xa00040a0
  cb7..cb14 值形似指令/数据（0x2d34a700、0x38302504、0x8d00716 …）
  ```
  - bootloader 有 6 条流（S2CR0..5 → cbndx 0..5），必然有 6 个 bank 的
    SCTLR.M=1；可是 `0x80000+0x1000n` 这一带**一个 M=1 都没有**
  - cb6 以后读到的是"指令样"数据 ⇒ **这一段根本不是 CB 寄存器**
  - ⇒ `CB(n)=0x80000+0x1000n` 在本机不成立，之前所有 CB52 的读写都落在
    别的地址空间上（能写能读回只是碰巧）
- **电池**：通道打通了（`ack=1 remote=1 rcid=6`），6 个属性都收到了回应，
  但**每个都是 `prop=0 value=1`** ⇒ 响应字段偏移与我们的假设不符
  （我们按 vendor 的 req 布局去读 resp）
- v25：① 全块扫描 0x15000000..0x150FFFFF 的 256 页，只打印
  `SCTLR.M=1` 或 `VTCR.RES1=1` 的页；② **暂停 stage-2 写入**（不再猜页面）；
  ③ 电池响应前两条做原始 hexdump

## 2026-10-25 14:0x —— 多核是已知未修 bug（`cpus=8` 必 panic）
```
SWEET CPU: cpu=1 CPSR CPU_ON rc=0
panic(cpu 1): Kernel data abort at pc 0xfffffe0007a8c598
  ESR 0x96000010  far 0xfffffe000d514008
```
- 符号化：`pc = pe_init_fiq + 0x14c`，调用链 `arm_init_cpu + 0x8c → pe_init_fiq`
- `far 0xfffffe000d514008` 远超内核镜像末尾（没有对应符号）⇒ 次核起来时
  `pe_init_fiq` 写的那块地址在次核的 pmap 里没有映射
- ★ 结论：**这是 pmap/FIQ 初始化的多核缺陷，与音频无关**；交互测试只能
  `cpus=1`。等以后单独修（方向：次核 FIQ 相关 VA 的 pmap 映射，或
  `pe_init_fiq` 依赖的 per-CPU 区只给 CPU0 建了）
- `mkimage.sh` 现在支持 `CPUS=n`（替换 `cpus=1`）、`KEEPALIVE=1`（去掉
  `sweetreboot=1`），交互版镜像：`KEEPALIVE=1` + 不带 `sweetpcm`

## 2026-10-25 14:1x —— 首次进到 GUI（Siri 欢迎页），随后又是 io_submap
- 交互版镜像（单核、无 sweetpcm）**成功显示 iOS 界面**：屏幕上是
  「只要按住侧边按钮，或随时说"嘿 Siri"」的 Siri 欢迎页
  ⇒ 图形栈、触摸、SpringBoard 都起来了
- 紧接着 panic：`kmem_alloc(…, 16384, 0x100021): failed with 3`
  —— 又是 `io_map()`，即 8 MiB io_submap 再次耗尽
- 根因是**系统性的**：SPMI chnls 窗口 4 MiB + keys/touch/i2c/usb/ufs/audio
  的零散映射，正好在 GUI 起来、设备初始化变多之后见底
- 修法（0185）：把 `osfmk/arm/io_map.c` 的 `IO_MAP_SIZE` 从 **8 MiB 改成
  64 MiB**。这块只是内核 VA（不映射就不占物理页），扩容零成本，
  一次性消灭这一类 panic
- 同时把 build tag 升到 `v26 (64 MiB io pool)`，方便看门狗区分产物

## 2026-10-25 14:2x —— run86：寄存器块有洞，盲目扫描会 panic
- v26 正常启动（`build v26 (64 MiB io pool)`），流列表读完，扫描第一页就崩：
  ```
  pc = ...74fd8b8  ldur w8,[x22,#-0x30]     ; 读 CB 页 +0x0
  x25 = 0x15004000                          ; 正在看 phys 0x15004000
  far = 窗口基址+0x4000  ESR 0x96000010      ; 外部中止
  ```
- ⇒ **0x15004000 这个物理页读就中止**。SMMU 这 1 MiB 不是连续寄存器，
  中间有洞；任何"逐页扫"的做法都会 panic
- 已知可读范围（迄今都成功过）：
  - `0x15000000..0x15003FFF`：GR0、GR1（+0x2000/+0x3000 也读到过）
  - `0x15080000..0x1508F000`：按厂商算式应为 CB0..15
- v27：改成**定点只读**——GR1 的 CBAR(0..7) + 上述 16 页的
  SCTLR/VTCR/VTTBR，打印 `type/vmid`，用来判断哪些 bank 是给 NS 的

## 2026-10-25 14:3x —— run87：CBAR 读出真值，CB0..5 是 stage-2 bypass
```
cbm0 cbar=0x1f000(type=1) sctlr=0xa9f00e0 vtcr=0 ttbr0=0
cbm1..5 cbar=0x1f000(type=1) sctlr=0xe0 vtcr=0 ttbr0=0
cbm6 cbar=0(type=0) sctlr=0x201d2716 vtcr=0x400c4815 ttbr0=0xa00040a0
cbm7 cbar=0(type=0) sctlr=0x2c34a700 ...
```
- `CBAR.Type=1` = **stage-1 翻译 + stage-2 BYPASS**
- ⇒ 只要把音频流引到 CB0..5 之一，DSP 的物理地址就**原样透传**，
  完全不需要我们建页表 ← 比"自己造 CB"简单得多
- 另外确认 0x15004000 读就中止（寄存器块有洞），不能再盲目扫
- v28 加 `sweetcb=N`：只写 `SMR[slot]=SID 0x1801` + `S2CR[slot]=N`，
  一个字都不碰 CB 寄存器
- 安卓侧验证：kprobe 的 `@0x15000934:u32` 读回全 0 —— kprobe 用线性映射
  读不到 ioremap 的设备内存，这条路不能用来 dump SMMU

## 2026-10-25 14:4x —— 结论固化（本轮把 CURRENT/HANDOFF 全量重写）
把这一整轮的结论收进 `CURRENT.md`（快照）和 `HANDOFF.md`（交接），
避免下一轮又从"CB52 应该能写"这条死路重新开始。要点：

1. **能写/不能写已经列成表**（CURRENT §2.2）：
   SMR(MASK=0) ✅ / SMR(MASK≠0) ❌ / SMR(SID 0x1800) ❌ /
   S2CR(type0|cbndx) ✅ / S2CR(0x10000) ❌ / CBA2R ✅ / CBAR ❌ /
   CB 窗口 VTCR/VTTBR/SCTLR ✅
2. **DT 的 sid-mask=0xf 在本机无法表达**（MASK 写不进去）——这是
   "为什么只能精确匹配 0x1801"的正式结论
3. **CBAR(0..5)=0x1f000 → Type=1（S1+stage-2 BYPASS）**，
   这是 v28 `sweetcb=N` 的依据
4. **寄存器块有洞**（0x15004000 读即中止）→ 禁止逐页扫描
5. **kprobe `@地址` 读不到 ioremap 设备内存**（全 0）→ 安卓侧 dump SMMU 这条路作废
6. 环境规则：`io_map` 已扩到 64 MiB（0185）；`KEEPALIVE=1`/`CPUS=n`；
   build tag 必须改；多核必 panic（`pe_init_fiq`）
7. 待办顺序更新：run88(sweetcb) → 电池字段 → `tdm_mf=0x3` → 功放静音
   → 用户态 bug（用户要求暂缓）→ 多核

## 2026-10-25 14:4x —— run88：把音频流引到 bypass 的 CB0，仍然同一处挂死
```
SWEET SMMU: audio -> cb0 cbar=0x1f000 smr=0x80001801 s2cr=0
...
SWEET GLINK: vendor graph + WR input started   ← 之后零输出
```
- `sweetcb=0` 只写 SMR+S2CR，路由确认生效（回读一致），但 DSP 第一次取
  buffer 依旧卡死
- **重要推论**：S2CR 指向 fault（run68/71/73）、bootloader 的 CB3（run75）、
  我们自建的 CB52 翻译（run81）、以及 bypass 的 CB0（run88）—— **四种完全
  不同的 SMMU 配置，挂死表现一模一样**
  ⇒ 要么「DSP 的 SID 根本不在我们写的 SMR 上」（USFCFG=1 直接 fault），
    要么「卡点根本不在 SMMU」
- 另外注意 `cbm6 sctlr` 每轮都在变（0x201d2716 / 0x201d2516，
  `cbm7 vtcr` 0x1d00 / 0x1c00）⇒ 那一段确实是易变数据，不是稳定寄存器
- v29：加 `sweetpa=ADDR`，把 SHM buffer 放进 **ADSP carve-out**
  （DT: `adsp_region@9fd00000`，49 MiB）。DSP 用自己那条流访问这块内存
  是必然通的——如果这样还卡，就说明问题不在"能不能读 host 内存"，
  而在 WR 数据通路本身

## 2026-10-25 14:5x —— run89/run90：carve-out 也不行（但 run89 是我搞错的一轮）
- run89 只传了 `sweetpa`，**没传 `sweetcb`** ⇒ 代码走"跳过 SMMU 编程"分支，
  SMR 根本没写，音频 SID 无匹配项，`USFCFG=1` 必然 fault
  ⇒ **这一轮无效，不能作为判据**（教训：跑实验前先确认参数真的生效，
  日志里要能看到对应的那一行）
- run90 同时传 `sweetcb=0` + `sweetpa=0xd0c00000`（流走 bypass 的 CB0，
  buffer 放在 DSP 自己的 carve-out `0x9fd00000`+49MiB 内）：
  ```
  SWEET SMMU: audio -> cb0 cbar=0x1f000 smr=0x80001801 s2cr=0
  SWEET GLINK: shm pa override 0xd0c00000 / shm handle=0xb0e145f8
  SWEET GLINK: vendor graph + WR input started      ← 之后零输出，整机冻结
  ```
  awk 确认 ring 在该行之后**一个字都没有** ⇒ 确实是硬冻结，不是我们线程阻塞
- 结论：换成 DSP 自己必然可访问的内存，症状不变

### 纪律（用户提出"这是在穷举"之后定的）
1. 每个假设只做一次实验；
2. 实验前写清"什么结果说明什么"（二元判据）；
3. 优先取安卓真值，不靠猜；
4. 日志里必须能验证参数真的生效，否则这轮作废（run89 的教训）

### v30 的两个二元实验（同一次编译，两个开关）
- `sweetcbar=1`：写 `CBAR(0)=0` 并回读
  - 成功 ⇒ CB0..5 属于我们，可以把它改成 S2 上下文（真出路）
  - 中止 ⇒ **CBAR 对当前执行环境整体不可写**，自己配 SMMU 这条路封死
- `sweetnodata=1`：只 START、不发 DATA_BUFFER
  - 存活 ⇒ 冻结 100% 由 buffer 访问引起
  - 仍冻结 ⇒ 问题在 START/图本身，与内存无关

## 2026-10-25 15:0x —— ★ 决定性结果：CBAR(0) 也写不进去
run91（`sweetcbar=1`）panic：
```
pc  = ...74fd86c        写 CBAR(0)=0 的 store
far = x20 + 0x1000      CBAR(0) 的地址
x8  = 0x1f000           写前读到的原值（Type=1）
```
- CBAR(0)、CBAR(6)、CBAR(52) —— **三个不同 bank，全部写即中止**
- ⇒ **CBAR 对当前执行环境整体不可写**，不是"选错 bank"

### 三个现象连成一条一致的策略
| 现象 | 解释 |
|---|---|
| CBAR 写中止 | 不允许我们新建/改写 context bank |
| 带 mask 的 SMR 被拒，但 bootloader 的 SMR3 就是 `mask=0x402` | 硬件支持 mask；是策略不允许我们把别人的 SID 纳入映射 |
| `S2CR=BYPASS` 被强制改成 FAULT | 不允许整段旁路 |
| 安卓能写 CBAR(52)（kprobe），我们不能 | **两者拿到的授权/视图不同** |

⇒ **"自己配置 apps-smmu"这条路判定封死**。再换 bank、换地址都撞同一堵墙。
   要突破只能拿到 Gunyah/hypervisor 的 SMMU 资源授权（SMC 侧），
   不在 XNU 能触及的范围内。

## 2026-10-25 15:1x —— run92：只 START 不发 buffer ⇒ 系统完全正常
同一版内核，只换参数 `sweetnodata=1`：
```
SWEET AUDIO: build v30 (cbar/nodata)
SWEET GLINK: sweetnodata: START only, no DATA_BUFFER
...
SWEET GLINK: ===== done =====
SWEET GLINK: PSCI SYSTEM_RESET
```
- 图 START 成功、后续引导正常走完、正常复位
- ⇒ **冻结 100% 由 `DATA_BUFFER`（DSP 去取 buffer）引起**，不是 START/图本身
- 这条同时给出一个可用的中间态：音频"开着但没数据"不会崩，
  以后要验证其它环节可以用这个模式

## 2026-10-25 15:2x —— ★★ run93：根因是 DATA_BUFFER 的地址语义，不是 SMMU
`MKARGS="sweetpcm=1 sweetpamode=1"`（地址字段发**绝对地址** `0xa2e00000`）：
```
SWEET GLINK: data_buffer addr=0xa2e00000 (pamode)
SWEET SMMU: pre  GFSR=0 FSYNR0/1/2=0
SWEET SMMU: post GFSR=0 FSYNR0/1/2=0
SWEET GLINK: data_buffer: dst=0x40bd done status=0x2 w28=0 w32=0xb0e145f8
SWEET GLINK: buffer probe dst=0x40bd -> -1
→ 之后 I2C/TDM 全部初始化、boot 正常走完、PSCI SYSTEM_RESET
```
**两件事同时发生：**
1. **不再硬冻结**（之前的"总线卡死"消失了）
2. **APM 真的回包了**，`DATA_BUFFER_DONE`，status=0x2（AudioReach 里
   0=OK / 1=FAILED / 2=BADPARAM）⇒ DSP 收到了、解析了，但认为参数不对

⇒ **根因确认：`DATA_BUFFER_V2` 的 `buf_addr` 必须是绝对地址，
不是区域内 offset。** 我们之前发 0 / 0x800，DSP 去读物理 0 → 互连挂死。
（外部评审最先指出这一点，当时我把注意力放在 SMMU 上，是被带偏了。）

### 顺带修正两个"证据"
- `CB0..5` 的 `FSR/FAR/FSYNR` 读出来是 `FSR=0x400, FAR=0x3400505b`
  `0x494a002` `0x1b4020c4`… —— 又是指令样数据，**且 pre/post 完全相同**
  ⇒ 这 6 页也不是可靠的 context-bank 寄存器。之前"CB0..5 存在"的判断
  应当降级为"未知"。
- 本轮 `GFSR` 全程为 0，说明 DSP 那次读**没有触发任何 SMMU 全局 fault**
  ⇒ "DSP 读主存失败被 SMMU 拦住"这个假设，在本轮配置下不成立。

### 下一步（v32，只改一项）
`token = dsp_buf | (len << 16)`（上游格式；我们之前只发 0/1），
并把 `DATA_BUFFER_DONE` 的完整响应 dump 出来。

## 2026-10-25 15:5x —— v32 构建失败：补丁打错函数（我的操作事故）
- 症状：GitHub Action 编译错误，没有新产物
- 根因：`g_put32(pkt + 16, token);` 在文件里出现**两次**，
  我的字符串替换命中了 `g_apm_map_probe()`（该函数只有 `token`，**没有 `len`**）
  → `len` 未定义 → 编译失败；而真正该改的 `g_apm_data_buffer()` 没动
- 修法（v33）：把 map_probe 里那段恢复，token 计算只放在 `g_apm_data_buffer()`
- ★ 教训：**改代码前先 grep 唯一性**（`grep -n "目标字符串"` 看是否只出现一次），
  或者贴更长的上下文锚点。这已经是"替换命中错位置"的第三次了
  （前两次：v29 的 va[n] 缩进、这次的 token）

## 2026-10-25 16:0x —— ★★★ 从厂商 trace 里找到 DATA_BUFFER 冻结的真正根因
之前从未真正解码过 `traces/trace_ui.txt` 里的 GPR 报文（文件里同时混着
tracing_mark 记录，我早先 grep 错了）。这次按十进制解析出 534 个包：

**厂商真实的 `SHARED_MEM_MAP_REGIONS`（2 个）**：
```
p0 0x00002c60   hdr (44B, cmd6)
p1 0x00010302   domains, 含 0x10000
p2 0x00002002   src port = 0x2002
p3 0x00000001   dst = 1
p4 45 / 75      token（递增序号）
p5 0x0100100c   opcode
p6 0x00010003   pool=3, count=1
p7 0x00000004   flag = 4
p8 0x1ffe8000   addr_lsw
p9 0x00000001   addr_msw   ⇒ 地址 = 0x1_1FFE8000 —— **IOVA，不是物理地址**
p10 0x00005000  size = 20 KiB
```

**厂商真实的 `DATA_BUFFER_V2`（479 个）**：
```
tok = 0x0 / 0x1                      ← 就是裸序号，我们最早那版是对的
buf = 0x00000000 / 0x00000800        ← 裸偏移
handle = 0xb0d890e8   size = 0x480   flags = 0
```

### 结论（一致解释了全部现象）
**flag=4 的 offset 模式下，MAP 的地址字段是 IOVA，DATA_BUFFER 里是相对它的偏移。**
我们把物理地址 `0xa2e00000` 当 IOVA 传过去 → DSP 用这个 IOVA 做基址，
在它的 S2 上下文里没有对应映射 → 解析成物理 0 → DSP 读 PA 0 → 互连挂死。
改成绝对地址后绕开了 IOVA 基址，所以不挂，但语义仍不对 → `BADPARAM(2)`。

### 上游那套（另一条自洽路径）
`property_flag=0` + **物理地址** + DATA_BUFFER 里填**绝对地址**。

### v34 实验（run96）
`sweetpcm=1 sweetpamode=1 sweetshmflag=0` —— 选上游那套约定，
其余不变（地址 0xa2e00000、size 0x480、token 已有上游格式）。
判据：`buffer probe -> 0` 且随后 `pcm tone start`。

### 教训
- `traces/trace_ui.txt` 里混着两种记录，用 `grep gprpkt` 才能拿到 GPR 包；
  早先因为没解析这里，绕了十几轮。
- 厂商 trace 永远是第一手证据，优先于任何推断。

## 2026-10-25 16:1x —— ★★★ 机制纠正：这颗 SMMU 不支持 stage-2（我此前方向错了）
厂商头文件（本机内核源）：
```c
#define ARM_SMMU_ID0_S1TS   BIT(30)
#define ARM_SMMU_ID0_S2TS   BIT(29)
#define ARM_SMMU_ID1_NUMS2CB GENMASK(23,16)
#define ARM_SMMU_ID1_NUMCB  GENMASK(7,0)
```
真机读数：`ID0=0x4c017e71` → S1TS=1、**S2TS=0**；`ID1=0x6000004b`
→ NUMS2CB=0、NUMCB=75。

⇒ **这颗 apps-smmu 根本不做 stage-2 翻译**（非安全客户端事务）。
⇒ 之前所有 `VTCR=0x80003560 / VTTBR=L1 / SCTLR=0x...` 的尝试都是在一个
   硬件没有的模式上打转；寄存器能写能回读，但那不等于功能存在。
   这也彻底解释了"四种完全不同的配置表现一模一样"。
⇒ 唯一有效形态是 **stage-1 翻译 + S2 旁路**，即 `CBAR.Type=1`
   —— 而 CB0..5 本来就是 Type=1，CBAR 不可写也就不重要了。

v35（`sweets1=1`）：
- 取 **CB5**（Type=1，绑的 SID 是 0x584，影响最小）
- S1 两级表：L1[2] → L2[0x117] = `0xa2e00000 | 0x703`
  （S1 block：AttrIndx=0、SH=3、AF、valid|block）
- 编程 `TTBR0=0xa3000000`、`TCR=0x3560`（T0SZ=32/TG0=4K/SH=3/WBWA）、
  `MAIR0=0xff`、`SCTLR=0x00c01027`
  （M|TRE|AFE|S1_ASIDPNE|CFRE|SHCFG=NSH；**CFCFG=0 = terminate 不 stall**）
- `SMR[slot]=0x80001801`、`S2CR[slot]=5`，TLBI
- token 默认改回**厂商的裸 0/1**（`sweettok=1` 才用上游复合格式）

风险记录：CB5 原本被 bootloader 的 SID 0x584 使用，我们改它可能影响
那个设备；CFCFG=0 至少不会把总线 stall 死。

## 2026-10-25 16:2x —— run97：S1 路径生效了，但偏移地址仍冻结
```
SWEET SMMU: s1 tables l1[2]=0xa3001003 l2[279]=0xa2e00703
SWEET SMMU: s1 cb5 cbar=0x1f000 sctlr=0xc00027 ttbr0=0xa3000000
            tcr=0x3560 mair=0xff smr=0x80001801 s2cr=0x5
...
SWEET GLINK: TX_DATA iid=110 len=68        ← 数据帧
（之后无输出 → 冻结）
```
- **S1 配置本身是生效的**：`SCTLR=0xc00027`（M=1、TRE、AFE、CFRE，
  **CFCFG=0 = terminate 不 stall**）
- 但写入的 `0xc01027` 回读成 `0xc00027` ⇒ **bit12（S1_ASIDPNE）写不进去**
  （可能该版本不需要/只读，非致命）
- 用**偏移**地址形式（厂商约定）依然冻结
- 我的 bug：S1 分支提前 return，没装 `g_gr_win`，所以快照打印
  `pre/post no gr window`，又一次白丢诊断窗口

v36：把窗口安装提到函数入口；本次实验用 **S1 + 绝对地址**（`sweetpamode=1`）。
这是最后一个逻辑上成立的组合：之前绝对地址那次（run93）是**没有任何翻译**
的裸状态回了 BADPARAM；现在有真正的 S1 恒等映射。

## 2026-10-25 16:4x —— run98：S1 + 绝对地址 = 干净 BADPARAM（SMMU 侧终于可信）
```
s1 cb5 cbar=0x1f000 sctlr=0xc00027 ttbr0=0xa3000000 tcr=0x3560 mair=0xff
        smr=0x80001801 s2cr=0x5
data_buffer addr=0xa2e00000 (pamode)
post GFSR=0 / 所有 cb FSR 与 pre 完全一致
DATA_BUFFER_DONE: buf=0xa2e00000 handle=0xb0e145f8 status=0x2
→ 后续 I2C/TDM/boot 全部正常，无冻结
```
- 回包逐字：`+20 opcode=0x05001004`，`+24 buf=0xa2e00000`（回显我们的地址），
  `+32 handle=0xb0e145f8`，`+36 status=2`（BADPARAM），meta 全 0
- ⇒ **地址/handle 都被接受，DSP 在参数校验阶段拒绝，且根本没访问内存**
  （GFSR=0，CB FSR 与 pre 一模一样）
- 我们的 media format 结构核对无误（`+12 sample_rate`、`+16 bit_width`…），
  只是值不同：我们 96k/32bit/q31，厂商 48k/24bit/q23

v37：发送后**只轮询 SMMU**（不碰 GLINK FIFO），最多 2000 次，
把 `GFSR`/`GFSYNR1` 落盘 —— 这样即使 DSP 随后把总线弄死，
真实 SID 也已经写进 ring。用**偏移模式**运行以触发那次访问。

## 2026-10-25 16:5x —— run99：冻结发生在发送/轮询之间（GFSR 拿不到）
```
s1 cb5 ...（S1 配置成功）
shm handle=0xb0e145f8
pre 快照 6 行
（之后无任何输出 —— 连 "poll GFSR" 都没打出来）
```
- 说明冻结发生在 `g_send_gpr()` 或随后的 GFSR 轮询里
- ⇒ **DSP 一接管 buffer 就把总线弄死**，事后读 `GFSR` 这条路走不通
- v38 策略调整：**回到“安全模式”（绝对地址）反复迭代参数**——该模式下
  DSP 不访问内存、立刻回 BADPARAM，不会挂死，可以快速试错
- 第一个要试的参数：media format 照抄厂商（**48k/24bit/q23**；
  我们之前是 96k/32bit/q31）
- 同时在 send 前后各加一行打印，下次再冻结能区分“死在发送”还是“死在轮询”

## 2026-10-25 17:0x —— run100：排除 media format；定位到 TDM 参数用错
- 把 WR 的 media format 照抄成厂商值（48k/24bit/q23）后：
  `vp/wr_mf status=0`，但数据帧**仍然 status=0x2**，`GFSR=0`，不访问内存
  ⇒ **media format 不是 BADPARAM 的原因**
- 现在的完整图景自洽了：
  - **绝对地址** → 超出映射区域范围 → APM 在参数校验阶段拒绝（BADPARAM），
    不发内存访问
  - **偏移地址**（合法）→ 校验通过 → DSP 真的开始跑 → 然后整机挂死
  - 也就是"冻结"很可能不在读 buffer，而在**下游链路**
- 下游里最可疑、而且**明确已知是错的**一项：`tdm_mf` 一直返回 0x3
  （EUNSUPPORTED）。核对后确认：**我们给 TDM 端点发的是通用 media format
  `0x0800100C`，而厂商对 TDM 用的是 `0x08001017` + `0x0800101B`**
  （vendor trace 逐字节：`{96000,0x30020,1}` / `{0,2,0x10001,7,0x200004,0,1}`）
- v39：TDM 改发厂商这两个参数，其余不动。先仍在安全模式（绝对地址）
  验证 `tdm_*` 是否变成 status=0

### 操作事故记录
这一版第一次替换时因为锚点文本不匹配（`mf, 31` 那段实际是 31 而不是我写的
`mf[30]`）导致 AssertionError，源码没改、补丁与已提交内容相同，`git commit`
报 "nothing to commit" 我却照样 push 并启动了看门狗 —— 白等一轮。
已重新用精确锚点（含 `mf, 31`）替换并重启。

---

## run101（v39）/ run102 / v40 —— 2026-10-25 17:15~17:25

### run101（v39，TDM 用厂商参数）——**成功**

```
SWEET AUDIO: build v39 (tdm params)
vp/wr_mf   rsp status=0
vp/tdm_rate rsp status=0      ← 新增，原来的 tdm_mf=0x3 消失
vp/tdm_cfg  rsp status=0      ← 新增
vp/tdm_ep_mf / tdm_frame / tdm_power / tdm_clk / aw_cfg  全部 status=0
data_buffer: dst=0x40bd done status=0x2   （仍是安全模式，绝对地址）
```

⇒ **TDM 端点配置这条线通了**。之前每轮 `tdm_mf=0x3 (EUNSUPPORTED)` 的根因
确认就是「对 TDM 端点发了通用 `0x0800100C` 而不是厂商的
`0x08001017`+`0x0800101B`」。这条已闭环，不要再回头。

### ★★★ 一手证据：SMR 表全貌（每轮 probe 都打印，12 轮完全一致）

```
id0=0x4c017e71 id1=0x6000004b scr0=0x200406
smr0 sid=0x0e0  mask=0     s2cr=0x0   → CB0
smr1 sid=0x4a0  mask=0     s2cr=0x1   → CB1
smr2 sid=0x0c0  mask=0     s2cr=0x2   → CB2
smr3 sid=0x2800 mask=0x402 s2cr=0x3   → CB3
smr4 sid=0x000  mask=0     s2cr=0x4   → CB4
smr5 sid=0x584  mask=0x11  s2cr=0x5   → CB5
6 valid stream entries        (槽位 6..112 全空)
```

推论（重要）：
1. **bootloader 只配了 6 条流，没有 0x1801**。0x1801 是我们自己写进 slot 0x4d 的。
2. **CB0..5 全部已被这 6 条占用**，没有空闲 context bank 给我们"干净地"用；
   而 CBAR≥6 写不进去（run77/79/91）⇒ 「新建一个 CB」这条路彻底不可行。
3. 我们现在把 0x1801 引到 CB5，等于**和 SID 0x584 共享**同一个 bank ——
   CB5 一开 S1 翻译（M=1），0x584 的访问也一起被改变了。这是目前未评估的风险。
4. DT 的 `qcom,smmu-sid-mask = 0xf` 意味着**只有 SID 低 4 位不参与比较**，
   DSP 真实 SID 可能是 0x1800..0x180f 中任意一个 ⇒ 只写精确 0x1801
   很可能根本不匹配。**v40 就是补齐这 16 个**。

### ★★ 新工具：SHM_MAP 是无风险的地址探针

`APM_CMD_SHARED_MEM_MAP_REGIONS` 只做登记、不搬数据 ⇒ 用「返回 status」
就能安全地探出 DSP 认可的内存窗口。run102 实测（v39，`sweetpa=0xa2d00000`）：

| 地址 | flag=4 | flag=0 | 说明 |
|---|---|---|---|
| `0xa2d00000` | **status=1 拒绝** | **status=1 拒绝** | 在 `adsp_region` 保留区内 |
| `0x8ea6dc000` | **status=1 拒绝** | **status=1 拒绝** | XNU 自己的页，**64 位**地址 |
| `0xa2e00000` | status=0 接受 | status=0 接受 | System RAM 起点 |
| `0x11ffe8000`（厂商 IOVA） | **status=0 接受** | status=1 拒绝 | 厂商专用；只认 flag=4 |

⇒ DSP 的合法窗口 ≈ **32 位、非保留的 System RAM**。`0xa2e00000` 这个选择是对的，
不是随便挑的。也说明 run102 没能测到"可达性"（MAP 就被拒了，根本没发数据）。

DT 复核（`dumps/dtb.tar`，本次解出）：
```
adsp_region@9fd00000  reg = <0 0x9fd00000 0 0x03100000>  no-map
  ⇒ [0x9fd00000, 0xa2e00000)  半开区间，0xa2e00000 不在里面
adsp_heap_region   alloc-ranges 0..0xffffffff, size 0x0c00000, align 0x400000, reusable
audio_cma_region   alloc-ranges 0..0xffffffff, size 0x1c00000, align 0x400000, reusable
```
两个 heap 都是**内核启动时动态分配**的（`shared-dma-pool` + reusable），
XNU 下根本不存在 ⇒ **不能拿它们当目标地址**。

### ★★★ run92 复核：MAP+PREPARE+START 是安全的

```
graph_open/vp-wr  status=0
shm mapped pa=0xa2e00000 mode=offset      ← 同一块区域、同一个 flag=4
graph_prepare     status=0
graph_start       status=0
sweetnodata: START only, no DATA_BUFFER
===== done =====  PSCI SYSTEM_RESET
```

⇒ **"区域映射 + 图启动"本身不会挂死**；挂死只发生在带合法 offset 的
`DATA_BUFFER` 之后。这个结论现在有一手证据，不再依赖推测。

### A/B/C 的正确读法（更正）

| 实验 | flag | 地址 | 参数合法？ | 结果 |
|---|---|---|---|---|
| A（run97/99） | 4 offset | 偏移 0 / 0x800 | ✅ | **挂死** |
| B（run93/95/98/100/101） | 4 offset | 绝对 0xa2e00000 | ❌ 远超 0x1000 区域 | BADPARAM，**安全** |
| C（run96） | 0 PA | 绝对 0xa2e00000 | ✅ | **挂死** |

⇒ 两种"正确形式"都挂死，唯一安全的是**参数非法**那次。
**`BADPARAM` 是我们目前唯一的安全刹车**，不是 bug。
推论：**在 DSP 能安全读内存之前，不可能拿到 status=0** —— 参数改"对"就等于挂死。
所以工作顺序必须倒过来：先解决可达性，再谈参数。

### 这一轮的操作事故（记下来别再犯）

`mkimage.sh` 的额外 boot 参数是**位置参数**，不是 `MKARGS` 环境变量
（`MKARGS` 是看门狗里 `bash mkimage.sh ${MKARGS}` 展开成参数用的）。
我第一次用 `MKARGS=... bash mkimage.sh` → cmdline 里什么都没有，白打一次包。
正确：`bash scripts/mkimage.sh sweetpcm=1 sweets1=1 sweetpa=0x...`
另外 `D22AP_ADT` 必须显式给，否则直接退出。

---

## run103（v40）—— panic，**我的错，踩了自己文档里的死路**

boot-args `sweetpcm=1 sweets1=1`，日志在 `SMR` 表打印后中断。
panic 解码（`IMG_0081`）：

```
pc 0xfffffe00074fdbbc   far 0xfffffe54a5e58818
x27 = SMR 基址 0xfffffe54a5e58800     far - 基址 = 0x18
x28 = 6                                ← 循环变量
x9  = 0x0000000080001800              ← 正在写的值（SID 0x1800）
x21 = 0x1f000  (CBAR5)   x22 = 0xc00027 (SCTLR 回读)
x23 = 0xa3000000 (TTBR0) x25 = 0x3560  (TCR)
```

⇒ 崩在 **`SMR[6] = VALID | 0x1800` 这一笔 store 上**。
而 `CURRENT.md §三 第 3 条` 早就写着「**SMR 用 SID 0x1800 —— 写就中止**」
（run70 踩过），我写循环时从 0x1800 起步，原样又踩一次。

**新增知识（run70 没记下来的部分）**：这次 abort 发生在 **slot 6**，
而历史上成功的写入在 slot 0x4d。所以「写中止」到底是 **SID 触发的**
还是 **槽位触发的**，此前从未分离过。v41 用 `SMR[6] = VALID|0x1801`
（已知可写的值、已知没写过的槽位）正好把这两者分开：
- 打印出 `s1 smr[6..20]` ⇒ **槽位 6 可写，abort 是 SID 0x1800 专属**
- 再 panic ⇒ **槽位 6 本身不可写**，那"SMR 精确匹配"这条路整个要重估

## v41（已提交，**未推送：网络断了**）

两处改动：
1. SID 范围改为 **0x1801..0x180f（15 个，绝不包含 0x1800）**
2. 新增 `sweets1empty=1`：装一张**故意全空**的 S1 页表。

第 2 点的诊断价值：现在 identity map / S2 bypass / FAULT / 自建 CB 四种配置
在合法 DATA_BUFFER 下**挂死表现完全一致**。如果换成空页表后 DSP 的访问被
SMMU 干净地 terminate（CFCFG=0）而**整机不再挂死**，就证明
① DSP 的访问确实走 apps-smmu，② 之前的挂死是"映射到了但读坏了"；
反之若仍然瞬间死，就说明**挂死根本不在 apps-smmu 这条路上**，
四种配置表现一致也就有了统一解释。

提交：`27c1d32 sweet: v41 empty S1 table + SID 0x1801..0x180f (never 0x1800)`

### 网络事故
17:23 还能 push，18:5x 起 `git push` 失败：
- `baidu=200`（国内直连正常）
- 代理端口 7890 在监听，但 `curl -x http://127.0.0.1:7890` 访问外网返回 **502**
- ssh 走 `ssh.github.com:443` 被 TUN 解析成假 IP `198.18.0.45`，连接被关闭
⇒ **代理上游节点挂了**，不是 git/ssh 配置问题。需要恢复 FlClash 才能出 CI。

---

## run104~108 —— GPT 第二轮会诊后的三次修正（2026-10-25 19:00~20:45）

### ★ 更正 1：我差点推出一个错的 v43（TCR）

我曾判断 `tcr=0x3560`（T0SZ=32）非法，理由是「输入宽度 = 32 − T0SZ」。
**这个公式只对 AArch32 格式成立**。外部评审指出：

| 格式 | 32 位输入对应的 T0SZ |
|---|---|
| **AArch64 长描述符** | `64 − 32 = 32` ← 就是我们这个 |
| AArch32 + EAE | `0` |

run108 真机回读 **`cba2r=0x1` ⇒ VA64=1 ⇒ AArch64 格式** ⇒ **`T0SZ=32` 是对的**。
v43（改 0x3500）已回退，v44 起恢复 0x3560。
教训：**先读 CBA2R.VA64 再解释 TCR，不要凭公式套。**

另：`SCTLR` bit12 是 `ASIDPNE`（可只读），**不是**表格式选择位；
回读丢掉 bit12 不能推出「只支持短描述符」。

### ★ 新事实：IDR2 = 0x5111 ⇒ 输入地址空间 33 位

`idr2=0x5111`，IAS 字段 = 1 ⇒ **IAS = 33 bit**。
而厂商 trace 的 IOVA 正是 **`0x1_1FFE8000`（33 位）** —— 这就是它为什么能存在。
我们现在的 TCR 是 32 位空间，**表达不了那个地址**；若要 1:1 复现厂商，T0SZ 必须改成 31。

### ATS1PR 探针：机制找到了，但在这台机器上返回 0

按 GPT 建议加了 SMMU 侧翻译探针（不涉及 DSP、不搬数据）。
第一次读错寄存器（我猜 `PAR@0x810`），查到**厂商自己的源码**才拿到真值：

| 项 | 值 | 来源 |
|---|---|---|
| `ARM_SMMU_CB_ATS1PR` | `0x800`（AArch64 格式下 **64 位写**） | `arm-smmu.h:263` |
| `ARM_SMMU_CB_PAR` | **`0x50`**（**64 位读**，bit0=F 表 fault，[39:12]=PA） | `arm-smmu.h:212` |
| `ARM_SMMU_CB_ATSR` | `0x8f0`（bit0 = ACTIVE） | `arm-smmu.h:288` |

按厂商 `arm_smmu_iova_to_phys_hard()` 的流程重写后（v45）：
```
ats1pr 0xa2e00000 -> par=0 | 0x10000000 -> par=0
mair=0xff cba2r=0x1 idr2=0x5111 tcr2=0x60
```
**两个地址（一个应该命中、一个确定没映射）都返回 0** ⇒ 探针本身没产出结果，
无法据此判断映射对错。（可能 ATS1PR 从 NS 侧不可用，或需要额外的使能。）

### ★★★ 更正 2（真正的 bug）：L2 描述符类型位写错

`cba2r=0x1` 确定是 AArch64 长描述符格式。在该格式下：

| 描述符 | bits[1:0] |
|---|---|
| L1/L2 **块**（2 MB 映射） | `0b01` |
| L3 **页** / 表指针 | `0b11` |

我们的 L2 项写的是 `| 0x703` ⇒ 低两位 `0b11` ⇒ **被当成第三级页表指针**，
而 L3 表根本没建——SMMU 会把 `0xa2e00000` 这个 PCM 缓冲区当页表去读。

⇒ **identity 映射从来没生效过**，这才是「identity map 与空表表现一模一样」的原因。
（`l1[i1] = ... | 0x3` 是对的，L1 表指针就该是 `0b11`。）

v46 改为 `0x701`（valid|block + AF + SH inner + AttrIndx0）。
注意：之前 CB52 stage-2 实验里的 L2 也用了同一个 0x703 ⇒ **那批结论同样作废**。

---

## ★★★ run110 与厂商报文逐字段比对（2026-10-25 21:30）

### 前置更正：以前的 trace 解析把十进制当成了十六进制

`trace_ui.txt` 里 `p0..p23` 的值是**十进制**（kprobe 默认输出）。
之前按十六进制解析，才得出"opcode 找不到"的怪结果。重解后 534 个包全部可读。

### 厂商 GPR 头统一布局（和我们一致）

```
w0 = (cmd<<4)|(len<<8)     cmd=6
w1 = domains
w2 = src_port              w3 = dst_port
w4 = token                 w5 = opcode
w6 = payload_addr_lsw      w7 = payload_addr_msw
w8 = mem_map_handle        w9 = payload_size
w10.. = 正文（若内联）
```

### 厂商各命令的形态（一手，来自本机 534 个包）

| 命令 | 形态 | 关键字段 |
|---|---|---|
| `SHM_MAP` 0x0100100c | 44B，**无描述符**，正文从 w6 开始 | pool=3\|cnt1, flag=4, addr=0x1ffe8000/msw=1, **size=0x5000** |
| `SHM_UNMAP` 0x0100100d | 28B，无描述符 | |
| `GRAPH_OPEN` 0x01001000 | **40B，图在共享内存里** | addr=0, handle=0xb0d890e8, size=0x1f0 / 0xb38 |
| `GRAPH_PREPARE/START/STOP` | 64/80B | 参数内联 |
| **APM 级 `SET_CFG`（dst=0x0001）** | **40B，参数在共享内存里** | addr=**0x400**/0x1000/0, handle, size=0x108/0xb0/0x70 |
| **模块级 `SET_CFG`（dst=模块 iid）** | 72/88B，**内联，handle=0** | 与我们的做法一致 ✓ |
| `DATA_BUFFER_V2` 0x0400100a | **68B** | src=0x2011 dst=**0x40bd**, token=**0/1**, buf=**偏移 0/0x800**, handle, size=**0x480**, 其余全 0 |

### ★ 同一个 handle 承载全部内容

**所有** `GRAPH_OPEN` 和 **全部 479 个** `DATA_BUFFER` 用的都是**同一个**
`handle = 0xb0d890e8`。该区域的偏移分配：

```
0x000  图定义（0x1f0 流图 / 0xb38 设备图）
0x400  APM 参数块（0x108）
0x1000 APM 参数块（0xb0）
0x000  PCM buffer 0（0x480，GRAPH_OPEN 之后复用）
0x800  PCM buffer 1
```

⇒ 区域**必须 ≥ 0x5000**，和 MAP 报的 size=0x5000 对得上。

### 我们 vs 厂商的差异清单

| 项 | 厂商 | 我们 | 判定 |
|---|---|---|---|
| 区域大小 | **0x5000** | `G_SHM_SIZE = 0x1000` | ❌ 太小 |
| 区域地址 | 0x1_1FFE8000（33 位 IOVA） | 0xa2e00000（物理） | ⚠ 待定 |
| **APM 级命令（dst=1）** | **40B，{offset, msw, handle, size}** | **内联 payload + handle=0** | ★ 结构性差异 |
| 模块级 SET_CFG | 内联 handle=0 | 内联 handle=0 | ✓ 一致 |
| DATA_BUFFER | src 0x2011 / dst 0x40bd / token 0/1 / offset / 0x480 | 同 | ✓ 一致 |
| token（APM 命令） | 递增 0x27,0x28,0x2d… | `0x100 + opcode` | 无伤（DSP 回显） |
| 源码 port | GRAPH_OPEN 有 0x2010 和 0x2011 两种 | 0x2010 | ✓ 都在用 |

### 为什么这条可能致命

厂商对 **APM（dst=1）从来不内联**：图、以及 APM 级参数块，一律先写进共享内存，
包里只放 `{偏移, 0, handle, size}`。

我们全部走内联。DSP 对这两种形态**都回 status=0**，所以"成功"并不能证明
它真按我们预期解析了。**如果 APM 把我们的图当成"区域里 offset=0 处的东西"，
那它看到的图和我们以为的完全是两回事。**

### 下一步（按证据强度排序）

1. **把区域扩大到 0x5000**，按厂商布局分配 offset（图 0x0、APM 参数 0x400、
   PCM 0x0/0x800 在 GRAPH_OPEN 之后）。
2. **图改用共享内存形态**：先把 800B 图定义写进 offset 0，再发 40B 的
   `GRAPH_OPEN{offset 0, handle, size}`；APM 级 SET_CFG 同理。
3. 之后再决定地址用 `0xa2e00000` 还是复现 33 位 IOVA。

### ★★★ 地面真值：Android 里音频 DMA 内存的真实地址（2026-10-25 21:40）

在**正常 Android** 上读 `/proc/iomem` 与 `dmesg`（root）：

```
[0.000000] Reserved memory: created CMA memory pool at 0x00000000fbc00000, size 28 MiB
[0.000000] OF: reserved mem: initialized node audio_cma_region, compatible id shared-dma-pool
[7.938014] msm-audio-ion soc:spf_core_platform:qcom,msm-audio-ion: msm_audio_ion_probe
[7.939237] msm-audio-ion ...:qcom,msm-audio-ion-cma: msm_audio_ion_probe
```

⇒ **`audio_cma_region` 落在 `0xfbc00000`，28 MiB**（`shared-dma-pool`，启动时动态分配）。
`adsp_heap_region` 被 `msm-adsprpc-mem` 占用（fastrpc 用），不是音频 buffer 的地方。

**对比**：我们一直硬编码的 `0xa2e00000` 与真实位置差 **1.4 GB**，位于完全不同的
DRAM 区段（`0xa2e00000` 是 System RAM 起点，紧邻 ADSP carve-out 的末端）。

⇒ 这大幅提高「DSP 对 `0xa2e00000` 没有访问权限、被 XPU/防火墙挡住并 stall 住互连」
这一解释的可信度，也解释了为什么五种 SMMU 配置表现完全一致（根本走不到 CB）。

**下一步实验（零代码改动，只需 boot arg）**：`sweetpa=0xfbc00000`，
保持 flag=4 + 合法偏移，其余与 run110 完全一致——只改地址这一个变量。

### run111（v47 + `sweetpa=0xfbc00000`）—— 与 run110 行为完全一致

```
shm pa override 0xfbc00000
shm_flat pa=0xfbc00000 flag=4 size=4096
shm_map rsp status=0            ← 厂商音频 CMA 的地址同样被接受
shm mapped pa=0xfbc00000 mode=offset
graph_start status=0
data_buffer send
data_buffer sent; gfsr=0 fsynr1=0     ← 立刻读仍成功，无 global fault
（然后死）
```

⇒ **换成 Android 真实的音频 CMA 地址（`0xfbc00000`）没有改变任何行为。**
「地址选错 / 没有权限」这个解释**被削弱**（不能算否定：XNU 下那块内存
未必已被划给 ADSP，bootloader 的 XPU 划分仍可能与 Android 不同）。

保持成立的两条硬事实（run110/111 各一次）：
1. 发送后**立刻**读 GFSR 成功，值 0 —— 此刻 TCU 与 SoC 都正常；
2. 紧随其后的刷盘也成功 —— 至少还有几毫秒整机健康；
3. 死亡发生在之后的轮询里 / 几毫秒后，且**不是一次可记录的 fault**。

⇒ 越来越像「DSP 侧发起的那笔事务把 TCU 卡住」，而不是「翻译算错」。

**下一个该试的已经不是地址，而是 §2.11 的结构性差异**：
把区域扩到 0x5000、图与 APM 级参数改走共享内存描述符形态，让发出去的图
和厂商完全同构。在此之前继续换地址收益很低。

---

## run112（v48：按厂商形态改造）—— 挂死点提前到 GRAPH_OPEN

改动（对齐 §2.11 的厂商差异）：
- `G_SHM_SIZE` `0x1000` → **`0x5000`**
- 图定义写进区域 `offset 0x2000`（避开 PCM 的 0/0x800）
- `GRAPH_OPEN` 改为 **40 字节描述符形态** `{offset, 0, handle, size}`
- 顺序改为 **先 MAP 再 GRAPH_OPEN**（厂商顺序）
- 去掉 `g_apm_graph_open_shm()` 里的 `flush_dcache64(g_shm_pa)`
  （低区 PA 调它必 panic，`phystokv_range()` 只认 ADT 银行）

结果：

```
shm_map rsp status=0
shm mapped pa=0xa2e00000 mode=offset
APM graph_open/vp-wr-shm src=0x2010 (shm handle=0xb0d7f408 len=760)
（日志到此为止：无应答，也没有走到 "no response" 分支）
```

⇒ **挂死点从 DATA_BUFFER 提前到了 GRAPH_OPEN。**

这是本轮最重要的收获：内联 `GRAPH_OPEN` 之所以一直"成功"，是因为
**DSP 完全不需要读内存**（参数在包里）；一旦改成描述符形态、必须去读共享
内存里的图定义，立刻就挂。

**统一结论：DSP 只要读我们映射的区域，互连就会挂；读什么内容无关。**

副产品：从此有了一个**更早、更便宜**的探针——发一次描述符形态的
`GRAPH_OPEN` 就能判断"DSP 能不能读我们的区域"，不必走到 DATA_BUFFER。

### 关于哈希

GitHub release 页面报的 digest（`c5b7fc58…`）与实际下发的字节
（`fb4a5548…`）**不一致**，三次下载字节完全相同；但内容 marker 确认是 v48。
⇒ 以后以 **build marker** 为准校验产物，别只信 digest。

---

## run113（v48，**完全不配 SMMU**）—— 与 run112 完全相同

boot-args 只有 `sweetpcm=1`（**没有 `sweets1`**，所以一条 SMR 都不写）：

```
shm_map rsp status=0
shm mapped pa=0xa2e00000 mode=offset
APM graph_open/vp-wr-shm src=0x2010 (len=760)
（同样挂死，同一个点）
```

⇒ **配不配 apps-smmu 都一样。**按规范未匹配流应产生 USF 并终止事务，
实测却是整机挂死。**apps-smmu 的配置不是这个挂死的原因**，
前面十几轮在它上面调参是在绕圈子。

## ★ 从厂商源码挖到的两条新事实

### 1. `qcom,stream-id-range = <0x1800 0x400>`

DT 的 `apps-smmu@15000000/lpass_tbu@151e9000` 明确写了 LPASS TBU 的 SID 范围是
**`0x1800 .. 0x1BFF`**，我们的 `0x1801` 落在里面。该节点还带
`base-status-reg = 0x151ce230`（TBU 状态寄存器）。

### 2. ★★ 板子上 CB0..5 的 `SCTLR = 0xe0` ⇒ **CFCFG=1 = stall**

```
bit0 M=0（翻译未启用）  bit5 CFRE=1  bit6 CFIE=1  bit7 CFCFG=1（stall！）
```

而厂商 `arm-smmu-qcom.c` 里，凡是"模拟 bypass"的 context bank 都写
**`SCTLR = 0`**（不是 0xe0）。

**CFCFG=1 的含义：context fault 时把事务"停住"无限等待**，
而不是终止。这与我们反复看到的「整机冻结、日志在某一行戛然而止、
连 100ms 超时打印都出不来」的形态高度一致。

`arm-smmu-qcom.c` 同时印证了我们早就观察到的硬件行为：
```
/* writing BYPASS will end up written as FAULT */
if (FIELD_GET(ARM_SMMU_S2CR_TYPE, reg) != S2CR_TYPE_BYPASS) { ...bypass_quirk... }
```

## v49（`sweetnostall=1`）

对 CB0..5 逐一把 `SCTLR` 的 **bit7（CFCFG）清 0**，其余位保持不动，
打印前后值。只改 fault 处理模型，不改 M、不动已启用的 bank。

---

## run117（v51，`sweetskipgfsr=1`）—— ★ 冻结范围缩小到整个 LPASS

改动：发完合法 DATA_BUFFER 后**完全不访问 SMMU 寄存器**，改为轮询 GLINK FIFO，
每 40 次（约 200 ms）打一行 `alive N`。

```
SWEET GLINK: data_buffer send
SWEET GLINK: data_buffer sent; gfsr=0 fsynr1=0     ← 落盘 ⇒ 之前那次 sync 成功
（然后死：连第一行 "alive 40" 都没等到）
```

推理链：
1. 那行能落盘 ⇒ 写盘成功 ⇒ **AP、中断、UFS 通路都正常**；
2. 之后只调用了 `g_drain(0)` 读 **GLINK FIFO**（同样是 LPASS 侧 MMIO）；
3. 在第一个 200 ms 心跳前就挂住 ⇒ **GLINK FIFO 读也不再返回**。

⇒ 冻结范围不是"SMMU 寄存器"，而是**整个 LPASS 子系统**：
SMMU 和 GLINK FIFO 一起失去响应，而 AP 其余部分完好。

**与"事务被下层挡住而非拒绝"高度吻合**：被挡住的事务永不返回 ⇒ DSP 的 NoC
端口卡死 ⇒ 之后 LPASS 的所有 MMIO 都不响应。这也统一解释了为什么
SMMU 配成什么样都毫无区别（它根本不在这条路径的关键位置上）。

### 因此下一个实验：1:1 复现厂商的 IOVA 路径

厂商用的区域地址是 **`0x1_1FFE8000`（33 位）**，而我们一直用自己挑的
`0xa2e00000`（32 位）。这条配置从来没被真正试过：
- `SHM_MAP` 报 `0x11ffe8000`（msw=1）——**实测 status=0，是被接受的**；
- 需要 33 位输入地址空间 ⇒ `TCR.T0SZ = 31`；
- 需要三级页表（4 KB 页）：`L1[VA[32:30]] → L2[VA[29:21]] → L3[VA[20:12]]`；
- IOVA `0x1_1FFE8000` 在 2 MB 块内偏移 `0x1E8000`，与我们的 PA 偏移不同，
  所以**不能用 2 MB 块，必须落到 4 KB 页**（L3 索引 0xE8）。

计划用 `sweetiova=1` 开关实现，映射
`0x1_1FFE8000 → 0xa2e00000`（那个预填了音调的页）。

---

## ★★★★ run118（v54，`sweetiova=1`）—— **整机不再挂死，且拿到了真实 fault**

这是本项目第一次：发**合法** DATA_BUFFER 之后整机活下来并完整复位。

```
SWEET AUDIO: build v54
SWEET SMMU: s1 tables va=0x11ffe8000 l1[4]=0xa3001003 l2[255]=0xa3002003 l3[488]=0xa2e00703
SWEET SMMU: s1 cb5 ... tcr=0x351f  mair=0xff
SWEET GLINK: shm_flat pa=0x11ffe8000 (region addr, buffer PA differs) flag=4 size=20480
SWEET GLINK: shm_map rsp status=0
SWEET GLINK: shm mapped pa=0xa2e00000 mode=offset
SWEET GLINK: data_buffer send
SWEET GLINK: data_buffer sent; gfsr=0 fsynr1=0
SWEET SMMU: poll GFSR=0 FSYNR1=0 reached=3          ← GFSR 读成功
post cb5 FSR=0x402 FAR=0x1ffe8000 FSYNR0=0x40001   ← ★ 真实 context fault
SWEET GLINK: data_buffer: no rsp (dst=0x40bd)
===== done =====
PSCI SYSTEM_RESET                                   ← 正常复位
```

### 对比：32 位恒等映射 vs 33 位 IOVA

| 配置 | post cb5 |
|---|---|
| 32 位恒等映射（v46~v51） | `FSR=0x400`（未变）→ **事务根本没到 CB5** |
| 33 位厂商 IOVA（v54） | `FSR=0x402 FAR=0x1ffe8000` → **事务到了 CB5，页表没走通** |

`FSR=0x402` 按厂商头文件解：`TF = BIT(1)` ⇒ **Translation Fault**。
`FAR = 0x1ffe8000` 正是 `0x1_1FFE8000` 的低 32 位。

### 结论（三条，全是一手证据）

1. **之前所有"整机冻结"的根因就是地址不对。** 用厂商的 33 位 IOVA 之后，
   同样的 DATA_BUFFER 不再挂死。
2. **SID 匹配是好的、CB 路由是好的**：DSP 的访问确实进了 CB5（FAR 就是这个 IOVA）。
   前面"SID 从未匹配"的猜测**被推翻**。
3. 现在的状态是**一个干净、可复现、可读的翻译失败**，而 CFCFG=0（terminate）
   让它不会拖死系统 ⇒ 可以快速迭代页表配置了。

### 为什么之前 32 位映射时事务没到 CB5

推测（非证据）：DSP 侧对区域基址有自己的地址空间认知，用 32 位物理地址时
事务在更前面就被挡住/丢弃；换成厂商的 IOVA 表达后才真正下发。
这正是"四维不变性"的合理解释——那四个维度改变的都不是**地址表达**。

## v55：改用单个 2 MiB 块描述符

三级页表（`L1→L2→L3`）报了 TF，于是换成更简单的一级：

```
IOVA [0x1_1FE00000, 0x1_20000000)  →  PA [0xa2e00000, 0xa3000000)
L2[255] = 0xa2e00701      （valid|block + AF + SH inner + AttrIndx0）
```

为了让 `0x1_1FFE8000` 正好落在缓冲区上，缓冲区要放到同块内相同偏移：
`PA = 0xa2e00000 + 0x1E8000 = 0xa2fe8000`（由 `g_map_pa` 自动算出）。

---

## run120 / run121 —— 翻译通了，但"成功反而挂死"（2026-10-25 23:50~00:10）

### run120（v56，`sweets1tcr2=0x38000`，TCR2.SEP=7）

外部评审指出 `FSYNR0=0x40001` 里 `PLVL[1:0]=1` ⇒ fault 在 **Level 1（根表）**，
并怀疑 `TCR2.SEP=0`（按 bit31 符号扩展）与 33 位 IOVA 不匹配。单变量验证：

```
tcr2=0x38060          ← SEP=7 写进去了，回读正确
post cb5 FSR=0x402 FAR=0x1ffe8000 FSYNR0=0x140001
```

**故障完全没变 ⇒ SEP 被干净排除。**

### run121（v57，`sweets1va=0x1FFE8000`，32 位 IOVA）

`FAR` 只读到低 32 位 `0x1ffe8000`，怀疑 DSP 把厂商的 33 位地址**截断**了：
若输入是 `0x1FFE8000`，则 `VA[32]=0`，在 `T0SZ=31` 下 L1 索引是 **0**，
而我们写在 **L1[4]** ⇒ 正好是 Level 1 TF。于是改成直接声明 32 位 IOVA：

```
s1 tables va=0x1ffe8000 l1[0]=0xa3001003 l2[255]=0xa2e00701 buf_pa=0xa2fe8000
s1 cb5 ... tcr=0x3560                    ← T0SZ=32，索引 0
shm mapped pa=0xa2fe8000
data_buffer sent; gfsr=0 fsynr1=0
（挂死：连 "poll GFSR" 都没打出来）
```

### ★★★ 三条配置的对比，规律是反的

| 配置 | 翻译 | 整机 |
|---|---|---|
| 33 位 IOVA `0x1_1FFE8000` | **失败**（TF，terminate） | **活着** |
| 32 位 IOVA `0x1FFE8000` | **成功** → `0xa2fe8000` | **挂死** |
| 32 位物理地址 `0xa2e00000`（早期） | 成功（恒等） | 挂死 |

⇒ **DSP 的访问被 SMMU 干净拒绝时整机安然；一旦翻译成功、DSP 真拿到数据，
整机反而挂死。**

**推论（重要）**：页表这件事已经基本做对了——32 位 IOVA 下的页表配置
（`L1[0]→L2[255]→2MiB 块`）能让 SMMU 成功翻译。
剩下的挂死**发生在"读成功之后"**，也就是 DSP 拿到 buffer 之后做的事。

下一步方向（按信息量排序）：
1. 用 `sweetskipgfsr=1` 在 32 位 IOVA 配置下跑，看挂死是在 SMMU 读、
   GLINK 读，还是更早——确定"挂死发生在 DSP 侧还是 AP 侧"。
2. 若 AP 侧正常，则问题在 DSP 消费数据之后（写回 / TDM / 功放），
   那时 TDM 链路（§2.8 已全部 status=0）就是下一个排查点。

## 2026-10-27 17:1x —— 多核 panic 修好了：GICR stride 0x20000 → 0x40000

自 2026-10-25 起 `cpus>1` 必 panic：

```
panic(cpu 1): Kernel data abort pc 0xfffffe0007a8c598
ESR 0x96000010 far 0xfffffe000d514008
```

pc = `pe_init_fiq + 0x14c`，读 GICR_TYPER。

**根因**：marble 的 redistributor 间距是 **0x40000**（shim 里也是这个值），
XNU 的 `SWEET.h` 还是 GICv3 默认的 **0x20000**。`cpus=1` 只用 offset 0，
所以一直没暴露；开第二个核时 `find_gicr_pe_base()` 用错 stride，读到第一帧
中间的未实现页 → 外部中止（不是缺页）。

**修法**（`0187-sweet-gicr-stride.patch`）：`pe_fiq.c` 把 stride 改成运行时变量，
boot arg `gicrstride=<n>` 覆盖；`mkimage.sh` 默认传 `gicrstride=0x40000`。
QEMU 不传，保持 0x20000。

**验证**（v88，`KEEPALIVE=1 CPUS=4 sweetsmp=1 sweetglink=0`）：

```
SWEET SMP: sampler ncpu=4 n=20 ms=1000
SWEET SMP: cpu0 busy=19/20 last_tid=3684
SWEET SMP: cpu1 busy=11/20 last_tid=2564
SWEET SMP: cpu2 busy=15/20 last_tid=457
SWEET SMP: cpu3 busy=15/20 last_tid=2779
SWEET SMP: done, resetting
```

四个核都起来了，调度器也在分配工作；20 秒后自己复位，无 panic。

**配套**（`0188-sweet-cpu-busy-helper.patch`）：`osfmk/kern/processor.c` 导出
`sweet_cpu_busy()` / `sweet_cpu_tid()`。`kern/processor.h` 把 `processor_array[]`
和 `struct processor` 藏在 `MACH_KERNEL_PRIVATE` 后面，BSD 驱动直接读不到
（v87 就是这样编译失败的）。

注意：ADT 目前只列了 4 个 A510；A710/X2 还没加。要上 8 核需要先改
`scripts/mkadt.py` 的 CPU 列表和 cluster-type。

### 2026-10-27 17:2x —— 8 核全开（真机验证）

ADT 加上 A710/X2（MPIDR 从 stock DTB 实测：`cpu@400`–`cpu@700`），
保持 `cluster-type=0`（SWEET 的 `MAX_CPU_CLUSTERS` 是 1）。内核 v88 +
`gicrstride=0x40000`，启动参数 `CPUS=8 sweetsmp=1 sweetglink=0`。

```
SWEET CPU: cpu=7 CPU_ON target=0x700 rc=0
SWEET CPU: cpu=7 mpidr=0x700 came alive; SGI1 OK
SWEET CPU: 8/8 CPUs initialized, SGI1 verified
SWEET SMP: ncpu=8 samples=20 interval=1000ms
SWEET SMP: cpu0 busy=10/20 last_tid=201
SWEET SMP: cpu1 busy=7/20  last_tid=199
SWEET SMP: cpu2 busy=9/20  last_tid=2862
SWEET SMP: cpu3 busy=6/20  last_tid=221
SWEET SMP: cpu4 busy=4/20  last_tid=2862
SWEET SMP: cpu5 busy=2/20  last_tid=3789
SWEET SMP: cpu6 busy=1/20  last_tid=3538
SWEET SMP: cpu7 busy=1/20  last_tid=221
```

- 8/8 核 PSCI CPU_ON 成功，逐个 `mpidr came alive; SGI1 OK`；
- 启动到 launchd/SpringBoard，无 panic；
- 8 个核都拿到过调度样本。

**当前限制**：所有核仍是 `CLUSTER_TYPE_SMP`（`MAX_CPU_CLUSTERS=1`），
XNU 不知道 E/P 的区分，P 核占用低只是因为这些采样时系统很轻载。
要验证 big.LITTLE 策略，需要把 `MAX_CPU_CLUSTERS` 改成 2、cluster-type 设为
`'E'`/`'P'`，让 XNU 建两个 pset；那是下一步。

### 2026-10-27 17:4x —— AMP / E-P 尝试：未进 XNU，已回退

`MAX_CPU_CLUSTERS=2` + `-D__AMP__=1` + ADT 里 A510 标 `'E'`、A710/X2 标 `'P'`
的 v89 **没有产生新日志**（ring 的 sweettime 仍是上一轮），说明在 XNU 日志
初始化前就死了。AMP 调度器需要 CLPC / Edge policy（sheng 用了专门的
`zzyf-sheng-amp.patch`），不是开两个宏就能用。已 `git revert` 回 v88
（8 核 SMP）继续使用。

另外：4 核和 8 核的 `launchd` 都在 **39.3s** 启动（39.315 vs 39.320），
8 核在日志里并不慢；测试镜像里的 20 秒 sampler 是测试用的，正常启动没有。

### 2026-10-27 18:2x —— v91：battmgr 属性偏移修正；RTC 的 AP 写权限已定性

**电池（pmic_glink / battmgr）**：之前 6 个属性全部返回 `prop=0 value=1`，
不是固件不响应，而是请求字段放错了位置。上游 `qcom_battmgr.c` 的
`qcom_battmgr_property_request` 是：

```
{ hdr{owner,type,opcode}, battery_id, property, value }
  +0       +4   +8        +12         +16      +20
```

我们把 `property` 写到了 **+12**（`battery_id` 的位置），固件于是把
`property` 读成 0（BATT_STATUS），每次都只回答 `prop=0 value=1`。
v91 改为 +12=`battery_id(0)`、+16=`property`、+20=0；响应按
`{hdr, property@+12, value@+16, result@+20}` 解析，并检查
opcode/property/result 后再采用（通道刚打开时的主动通知会被跳过重试）。
另外温度属性是 **12**（BATT_TEMP），不是 13（13 是 BATT_TECHNOLOGY）。

**RTC**：PMK8350 RTC 在 SPMI APID map 里是 `ppid 0x61 apid 51 owner 4`
（EE4 = PMIC 固件），APPS(EE0) 的写会被 arbiter 拒绝，而且 Vendor DT 的
`rtc@6100` 节点**没有 `allow-set-time`**，所以 Android/TWRP 也设不了
（`hwclock -w` 直接 EPERM）。这不是 XNU 的问题，也不该去抢 APID 所有权
（会碰 PMIC 固件负责的 PON/闹钟）。实际的时间来源是 `sweettime`
（mkimage.sh 构建时写入 cmdline，精确到构建/启动之间的延迟），v91 起日志
只打印 raw counter 并注明 EE4-owned，不再尝试注定失败的写入。

### 2026-10-27 19:2x —— v92：pmap_assert_free 根因 = SHM 自测页越界映射

隔离结果：Test B（只加载 ADSP，`sweetglink=0`）**稳定**；Test C
（`sweetglink=1`，不开 PCM）**panic**，断言与第一次完全相同。ring 最后：

```
SWEET PHYS: va=0xfffffe549bde0000 pa=0x91de9c000
SWEET PHYS: mirror=0x4d43493c MISMATCH
...
panic: pmap_assert_free ... 0x91dea0000 ... Kernel CPU mapping
```

panic 页正好是自测页 **+0x4000**，两次（0x92144c000 / 0x91dea0000）都是。

根因：`g_phys_selftest()` 只分配了 **0x4000** 的 `g_self_pa`，
`g_apm_shm_map_gpr()` 却把它当作 SHM 候选，用
`g_shm_region_size()` = **0x5000** 去 `g_map_win()`；span 向上取整成
**0x8000**，于是把 `g_self_pa` 的**下一页**也 `pmap_map` 了。那一页属于
别的分配，等它被释放时内核 pmap 里还留着映射 → `pmap_assert_free`。
（1 核那次 USB 网卡掉线很可能也是这页被复用后踩到。）

v92：
- 不再把 `g_self_pa` 放进 SHM 候选（低区 tone 0xa2e00000 才是厂商在用的）；
- `g_phys_selftest()` 改为 **`sweetphys=1` 才运行**，正常启动不再对
  kernel-managed RAM 做 `pmap_map`。

### 2026-10-27 19:4x —— v93：电池跑通（容量单位修正）；触摸延迟定性

Test C v92 稳定，pmic_glink 电池拿到真实值：

```
batt-status prop=0 value=1        (charging)
capacity    prop=4 value=10000    (0.01 % -> 100.00 %)
vbat        prop=7 value=4427000  (4427 mV)
ibat        prop=9 value=-249000  (-249 mA)
temp        prop=12 value=2000000
usb-online  prop=0 value=1
```

- 值合理（满电、插着 USB）。**capacity 是 0.01% 单位**，v93 改为 `/100`；
  否则 50% 会被读成 5000，SweetBattery 截断成 100%。
- 真实电量要拔线后看百分比变化才能最终确认。

触摸：marble_touch 仍是 **GPIO bit-bang SPI**（GENI SE4 fw=0x102、
proto=1、fifodis=1，不可用），一份报告约 35 字节 ≈ 1 ms，外加 2 ms
等待。滑动后"没反应、然后一次性回放"的主因是 **UI 主线程被软件渲染
卡住**（无 GPU，控制中心动画在 CPU 上跑），HID 事件在队列里堆积，
主线程恢复后一次处理；不是触摸驱动 100 ms 级的延迟。
建议先用 辅助功能 → 动态效果 → 减弱动态效果 验证。

### 2026-10-27 20:1x —— 日常镜像冻结：USB CLEAR_FEATURE 风暴；v94 接 PSCI + 清 stall

daily-v93（8 核 + GLINK/电池，无 PCM、无 USB 日志）在 userland 使用中
CPU 被打满、界面冻结。ring 尾部几乎全是：

```
SWEET USB: setup 02 01 0000 0082 len 0     (301 次)
```

`02 01 0000 0082` = CLEAR_FEATURE(ENDPOINT_HALT)，wIndex 0x82 是
ECM 的**中断 IN 端点**。macOS 每帧重发，是因为我们 `req==1` 分支
什么都不做，DWC3 端点仍然 halt → 主机永远重试；每次 setup 还要
printf + 走控制状态机，把 CPU 和日志队列打满。

v94 修法：
- `CLEAR_FEATURE(value=0)`：按 wIndex 找 EP_RX(0x01)/EP_TX(0x81)/
  EP_INT(0x82)，发 `DEPCMD_CLEARSTALL`；EP_TX 复位 tx_busy 并重踢，
  EP_RX 重新 arm；
- setup 日志限流（CLEAR_FEATURE 前 8 条，其它前 64 条 + 每 256 条）。

同时接 PSCI（用户要求）：
- `SweetPlatformExpert::start` 设置 `PE_halt_restart`；
- `kPEHaltCPU`/`kPEUPSDelayHaltCPU` → PSCI `SYSTEM_OFF` (0x84000008)；
- `kPERestartCPU` → `SYSTEM_RESET` (0x84000009)；
- panic 类型不接管，保持 panic 屏。
用户态“滑动关机/重启”现在应该能用了。

### 2026-10-27 20:3x —— v95：触摸按下短震动（输入层接入）

iOS 的 haptic 栈不在这个内核里（IOHIDFamily 的 haptic 驱动是 Apple
私有 kext），所以先在输入层接入：`SweetHIDTouch::touch()` 在手指
**首次按下**时调用 `sweet_haptics_tap()`，由专门的 `g_hap_thread`
播一个 **15 ms** 的 PM8350B 短脉冲。
- 和物理按键的 300 ms kick 共用 `g_hap_busy`，不会并发碰 SPMI；
- 触摸线程只置位 + `thread_wakeup`，不阻塞触摸轮询；
- `sweetvibetouch=0` 可关；`sweetvibems` 留给后面调脉宽。
后续如果要做真正的系统 haptics（开关/通知不同波形），需要找到
iOS 的 haptic 服务协议（backboardd/hapticsd → 内核），再对接到 HAP520。

### 2026-10-27 21:1x —— v96：去掉触摸震动；关机改走 PMIC PON + SCM 释放 PS_HOLD

用户实测：触摸震动“震感有但用不上” → 移除 `sweet_haptics_tap()`
调用和 worker；开机那一下 300 ms 也改成 `sweethap=1` 才播，正常启动安静。
（物理按键的 `sweet_haptics_kick()` 保留。）

关机：v95 的 PSCI SYSTEM_OFF 不生效，30 s 后
`Halt/Restart watchdog barked`。对照 vendor `msm-poweroff.c`：
Qualcomm 的关机不是 PSCI SYSTEM_OFF，而是
1. 配置 PMIC PON 的 PS_HOLD_RST_CTL 为 `PON_POWER_OFF_SHUTDOWN (0x4)`：
   PMK8350 的 `pon_hlos@1300`，subtype 0x08/0x09 (GEN3) 用 0x1352/0x1353，
   否则 0x135A/0x135B；先清 CTL2 bit7，500 µs 后写 CTL 低 4 位=4，再置 bit7。
2. `qcom_scm_deassert_ps_hold`：SCM `0xC2090902`（SVC_PWR=0x09/
   DEASSERT_PS_HOLD=0x02，SIP，fast call），arginfo=1，arg0=0。
3. 若 SCM 返回，再回退 PSCI SYSTEM_OFF。
SPMI 写白名单增加 SID0 peripheral 0x13（PON HLOS）；重启仍用
PSCI SYSTEM_RESET（已验证可用）。

### 2026-10-27 22:3x —— v97 实测复盘：0190 从未被应用；电量刷新没有 intent

**关机（v97 现象：转圈、按键震动仍在）**：ring 里依次出现
`CPU halted` → `SWEET: skip PM halt/restart notifications`，然后没有任何
`SWEET: PON ...`，30 s 后看门狗 panic。根因不是 PON 写失败，而是
**0190 这个补丁在 CI 里从来没有被应用过**：
- dxb 用 `git apply --check` 应用补丁，**整补丁原子**：任何一个 hunk
  失败就整个跳过；
- 0190 的第一个 hunk（`ud_setup` 里 `printf` 前多一个空行）与 0189
  之后的文件不匹配 → 整补丁被跳过，`sweet_platform_halt_restart`
  和 `PE_halt_restart` 赋值都没编译进去；
- 于是 `haltRestart()` 返回 -1，`halt_all_cpus` 的 `while(1);` 在开中断
  状态下空转：屏幕停在转圈、触摸/震动线程照常运行，30 s 后 panic。
修复：按真实基线重新生成 0190（同时保留空行），并给 CI 增加：
按 basename 检查 `Skipping patch`（dxb 记录的是 `dxb/patches/` 的副本），
以及构建后用 grep 校验 3 个必须存在的内核字符串。

**电量（现象：拔掉充电器仍显示充电、百分比不更新）**：
- v97 的 30 s worker **确实在跑**，但每次请求都打印
  `SWEET BATT: status no intent`：`g_batt_req()` 把远端 intent
  标记 `in_use=1` 后从不归还。GLINK 里对端用 `RX_DONE(_W_REUSE)`
  归还我们 TX 用掉的 intent，而代码只处理了自己本地 intent 的
  `filled`，没清 `g_riids[].in_use`；开机探测用掉 6+ 个后，
  PMIC 通道的 intent 池耗尽，后续永远拿不到。
- 修复：`RX_DONE` 里按 id+chan 归还远端 intent；仍失败时
  `g_batt_reclaim()` 回收该通道的 intent 再试。
- 电流按 signed 解析（负=充电，之前 `4294872296 µA`）；并且只有
  USB 在位或电流为负时才报 external/charging（之前 `charging=status==1`
  与是否插线无关，所以拔线后一直显示充电）。

待验证：v98 镜像的关机是否真的断电；电量是否每 30 s 更新、拔线后
充电图标是否消失。
