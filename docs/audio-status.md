# Marble XNU 出声 — 阶段状态（2026-10-24 12:20）

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

## 0137–0139：SHM_MAP 的失败形态已经清楚
- 高位地址（XNU 只能分配到的 ~35 GB）：APM 立即回 `GPR_BASIC_RSP_RESULT`
  `status=1`（拒绝）
- 厂商自己的 DMA 地址 0x1FFE8000、SMEM 0x80900000、SPU 共享区
  0x8BB80000/0x8BBE0000：**完全没有响应**，但前后 SPF 状态查询都正常
  → APM 没有重启，是 map 请求本身卡住
- 因此不是端口、令牌或报文格式的问题；剩下两个方向：
  1. AP 侧需要先建立 LPASS 的 SMMU/上下文（XNU 没有 SMMU 驱动）；
  2. 调用顺序要按厂商来（厂商在 GRAPH_OPEN 前映射 PCM 缓冲）

## 0140：SMMU 现场与侧线（ring-run33）
- `apps_smmu @0x15000000` 可读：IDR0 `0x00200406`，+0x10 `0x0000001c`
- **`lpass_tbu @0x151e9000` 一读就 fault**：XNU 不能直接访问 TBU 寄存器，
  日志停在那一行，后面的 GLINK 初始化没有执行
- 同时发现侧线（RTC/触觉）**从未真正工作**：`sweet_spmi.c` 仍在用 0093
  之前的旧基址，init 读到 `arb=0x08000100 APIDs=0`，所有 SPMI 读都失败
- 现场 DT 的正确窗口：`cnfg 0x0c42d000, core 0x0c400000,
  chnls 0x0c500000, obsrvr 0x0c440000`；observer 每 APID 0x80 字节
  （keys 驱动已经用这个布局工作）

## 设备内核版本
- 本机 Android 内核是 **5.10**（不是 5.15）。厂商参考源码用
  `MiCode/Xiaomi_Kernel_OpenSource` 的 **`marble-s-oss`** 分支；
  5.15 的 msm 代码只能当旁证。

## 0141–0142：RTC 与触觉
- 0141：修正 SPMI 基址；读 PMK8350 `0x6148`（4 字节小端 Unix 秒），
  值在 2020–2050 才调用 `clock_set_calendar_microtime()`
- 0141：PM8350B（HAP520）直驱：VMAX 3600 mV、sine、
  T_LRA=5882 us、DIRECT_PLAY 满幅 300 ms
- 0142：写通道（chnls）步长先用**触觉 revision 字节**验证，
  且每次写都要求目标寄存器经 observer 与 chnls 读到相同值，
  否则一个字节都不写（避免错误步长误写其他 PMIC 外设）
