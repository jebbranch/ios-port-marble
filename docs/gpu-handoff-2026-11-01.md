# GPU 硬件启动：交接文档（2026-11-01）

> 给接手的人 / 智能体团队。目标不是"移植 GPU 驱动"，而是**一步步把
> Adreno 725v1（a730 家族、gen7 GMU）的硬件从 XNU 里点亮**：先 CX/GX 电源与
> 时钟，再 GMU 的 CM3，再 HFI，最后才是 CP 与 Metal。
> 事实全部来自真机（serial `2aa26095`，Redmi Note 12 Turbo / marble / SM7475）。

## 0. 硬规则（先读）

1. **只允许 `fastboot boot`**。禁止 flash/erase/format/set_active/oem 写命令，
   不碰 GPT/persist/modemst。
2. 仓库是公开的纯代码/补丁/文档仓库，**不收** Apple/Qualcomm/小米二进制、
   IPSW、HFS 镜像。
3. 工作树里有三个长期未提交的脚本（`boot_interactive.sh`、`boot_ram.sh`、
   `mkadt.py`）和大量 `logs/`，**不要整树提交**；补丁以 `xnu/patches/` 为准。
4. 日志：内核 ring 在 userdata 尾部，`SERIAL=2aa26095 ./scripts/pull_logs.sh <label>`；
   探针镜像用 `./scripts/gpu_pwr.sh build/run`，UDP console 会自动监听。
5. 卡死时设备没有 adb/fastboot，只能**长按电源**；PMIC 看门狗通常会在几十秒后
   自己复位到 Android。

## 1. 环境与今天（2026-11-01）的起点

- 启动链：ABL → 自写裸机 shim（AArch64）→ XNU 8796，RAM-only，**没有 Android**，
  没有任何厂商 GPU 驱动参与。
- CI：GitHub Actions 构建内核；**macos-14 已被 GitHub 下架（brownout/退休）**，
  已迁到 `macos-15`（Xcode 16），构建全部成功。CI 会校验内核里必须存在
  `SWEET SNAP: cx_gdscr` 与 `SWEET RAIL: done blocks` 两个标记。
- 关键地址（真机核对）：
  - KGSL `0x3d00000`、GMU `0x3d68000`、GPU_CC `0x3d90000`、kgsl-smmu `0x3da0000`、
    RSCC `0x3d50000`、PDC `0xb290000`、GICD `0x17100000`。
  - GMU 寄存器是 kgsl regmap 的**字偏移**：`字节 = 0x3d68000 + (word-0x1a000)*4`。
    ITCM `0x3d6d000`、DTCM `0x3d79000`、CM3_SYSRESET `0x3d7e000`、
    FW_INIT_RESULT `0x3d7e070`、HFI_CTRL_INIT `0x3d7e618`。
  - GPU_CC：`CX_GDSCR 0x9108`、`GX_GDSCR 0x905c`、`ahb 0x911c`、`cx_gmu 0x913c`、
    `hub_cx_int 0x9148`、`smmu vote 0x7000`、gmu RCG `0x9318/0x931c`、
    hub RCG `0x93ec/0x93f0`、hub_ahb_div `0x9430`、AO fence `0x23b10/0x23b11/0x23b14`。
  - kgsl-smmu：`IDR0=0x4c017e18`、`IDR1=0x40000016` → **CB0=0x3dc0000**。

## 2. 已确证的事实

1. **ABL 交接时 GPU 是塌陷的**：`CX_GDSCR=0x22f001`（PWR_ON=0、SW_COLLAPSE=1），
   `ahb=0`、`cx_gmu/hub_cx_int/smmu vote` 全部 halted。旧文档里"PWR_ON=1 是
   ABL 遗留"的说法**是错的**。
2. **CX/GX 能上电**（shim，`GP_PWR=1 GP_RAIL=1`）：
   `cxgdr=0xf822f000`（PWR_ON=1、SW_COLLAPSE=0、RETAIN_FF=1、CLK_DIS_WAIT=0xf）、
   `cxgmu=0x2221`、`hubcx=0x1`、`vote=0x1`；随后 `GX_GDSCR=0x6822f000`。
   **只要不做 collapse**；主动 collapse 之后永远回不来（已列为禁止操作）。
3. **GMU 寄存器与 TCM 可用**（内核 `sweetgmuboot=1` + shim `GP_STAGE=1`）：
   `fw loaded, itcm0=0x10007f80`（真实固件字）、SID5 的 `smmr1=0x80000005 s2cr1=0x1
   cb1=0x1`、预启动配置完成。
4. **CM3 放复位的写有时返回**（`release written`），返回后读 GPUCC 必卡；
   写本身有时也不返回。**放复位之后 AP 对所有 GPU 侧块（GPUCC/GMU/SMMU/RSCC）
   的访问都会把执行核钉死**，最终靠看门狗复位。
5. **失败清单（别再重走）**：主动 collapse；`GP_SMPREL`（shim 牺牲核放复位，
   XNU 起不来，已排除 CPUS=6 与未关中断两个原因）；shim 侧 `GP_EARLY+GP_GMUBOOT`
   放复位（XNU 起不来）；GCC `0x81010/0x81018`；`GP_RSCC` 微码；屏蔽 GMU SPI
   304/305；AO AHB fence 置 ALLOW；在放复位前一刻 arm `gpu_cc_ahb`（bit0 写不进）。
6. **cmd-db（`sweetdb=1`，只读）已能枚举**：
   - slave3（ARC）：`cx.lvl 0x30000 / mx 0x30010 / ebi / lcx 0x30030 / lmx 0x30040 /
     gfx 0x30050 / mss / ddr / mmcx / qphy / mxc 0x300a0 / xo`；
   - slave5（BCM/NoC）：`MC0-3 / SH0-5 / SN0-7 / MM0-1 / CN0 / CE0 / IP0 / QUP /
     CO0 / ALC / ACV`；
   - slave4（PMIC）：`vrm.cx=0x42900(smpc6)`、`vrm.gfx=0x40600(smpb5)`、
     `vrm.lcx=0x42000(ldob8)`、`vrm.mxc=0x42700(smpc2)`。

## 3. 当前代码状态（都在 main 上）

- `xnu/patches/0216-sweet-gmu-kernel-boot.patch`：
  - `g_cmddb_dump()`（`sweetdb=1`，只读枚举 cmd-db）；
  - `g_gpu_snapshot()`（`sweetgsnap=1/2`：SMMU→KGSL→GPUCC，GMU 组只在 =2）；
  - `g_rail_probe()`（`sweetrail=1` 只读两颗疑似轨道，=2 全片扫）；
  - `g_gmu_boot()`（`sweetgmuboot=1`）：CX/GX 上电、RCG/分支、固件块解析与
    写 TCM、SID5 CB1、预启动配置表、屏蔽 GMU SPI、AO fence allow、放复位；
    放复位后**只**读 HFI(DDR)/AO/SMMU，且每条 print 都有 sync；
  - **最新改动（`1916852`，尚未上机验证）**：RCG 更新后**保留 ROOT_EN**
    （之前会把 ROOT_EN 清掉，导致 hub 时钟根关闭、`gpu_cc_ahb` 锁不住）。
- `shim/shim.c`：`GP_SURVEY/GP_PWR/GP_RAIL/GP_STAGE/GP_EARLY/GP_GMUBOOT/
  GP_RSCC/GP_GCC/GP_GCC2/GP_SMPREL` 等开关；牺牲核路径改为等待内核写
  `0x47505552` 到 `0xa2e0ff08` 再放复位；牺牲核已关中断；新增 GCC memnoc/
  snoc-dvm 分支写入（`GP_GCC2`）。
- 工具：`scripts/gpu_pwr.sh`（构建/启动/收日志）、`docs/gpu-probe.md`
  （v1–v30 全部真机读数，按时间顺序）。

## 4. 现在卡在哪（一句话）

**放复位之后 AP 侧没有任何可观测面**。厂商的 AP 在同一时刻可以持续轮询
`FW_INIT_RESULT`；我们的 AP 一旦 CM3 接管就把访问它的核钉死。
最可疑的两条：

1. **时钟根没真正跑起来**：日志里 `hub cmd=0`——RCG 的 `ROOT_EN` 在 UPDATE 后
   被清掉（`1916852` 已修，**下一步就是验证它**：若 `ahb` 能 latch、放复位后
   GPUCC 仍可读，这条路就通了）；
2. **AP 的 GPU 总线/互连投票缺失**（BCM 一栏在 cmd-db 里已拿到名字，但还没投）。

## 5. 建议的并行分工（给智能体团队）

每条线都能独立跑，互不阻塞，建议每人对一个分支 + 一个真机队列：

- **A. 时钟与 AHB（优先级最高）**：验证 `1916852`（保留 ROOT_EN）。若 hub RCG
  起来后 `gpu_cc_ahb` 仍不 latch，用 `hub_ahb_div(0x9430)` 与 `hub_clk_src`
  的实际回读判断是分频器只读值的问题还是父级没跑；目标是"放复位后 GPUCC 仍可读"。
- **B. 放复位的可观测性**：修 `GP_SMPREL` 路径（为什么 XNU 起不来）；或在
  内核里把放复位放到独立线程/最后一步，让 boot CPU 存活；或让 shim 的
  fault-safe 读在放复位后采集 RSCC/AO。
- **C. RSCC/PDC 与 GMU 睡眠序列**：对照 `adreno_gen7_gmu.c` 的
  `gen7_load_rsc_ucode/load_pdc_ucode`、RSCC TCSM DRV0 状态语义，确认
  `GP_RSCC` 写的值是否正确、CM3 是否要求在放复位前完成。
- **D. HFI 协议**：`adreno_hfi.h` + `gen7` 队列格式；一旦放复位后可访问，
  按 AOSS/H2F 消息把 INIT 发出去，并用 HFI 队列索引证明 CM3 在消费。
- **E. SMMU/CP**：CB0（SID0）与 CB1（SID5）的 stage-1 表、fault 寄存器；
  CP 固件（`a730_sqe.fw`）的加载窗口尚未开始。
- **F. 轨道/投票物理验证**：SPMI 读 PM8350C S6 / PM8350 S5（`sweetrail=1`），
  做"投票/不投票"同机对照；cmd-db 里的 `vrm.*` 地址可以帮助定位 PMIC 块。

## 6. 一次实验的标准流程

```bash
# 1) 拉最新内核（CI 在 main 上构建）
MARBLE_PROXY=http://127.0.0.1:7890 ./scripts/fetch_kernel.sh   # 或手动下载 + 打 CNTP_CTL 补丁

# 2) 打镜像（shim 开关用 env，启动参数用 EXTRA_ARGS）
EXTRA_ARGS="sweetgmuboot=1" CPUS=8 \
  GP_SURVEY=1 GP_PWR=1 GP_RAIL=1 GP_STAGE=1 \
  ./scripts/gpu_pwr.sh build "GP_SURVEY=1 GP_PWR=1 GP_RAIL=1 GP_STAGE=1"

# 3) 启动并收日志（内核会自己 PSCI reset）
./scripts/gpu_pwr.sh run
# 卡死无 adb 时：长按电源，然后
SERIAL=2aa26095 ./scripts/pull_logs.sh gpu
```

判读要点：

- `SWEET GPU: v9 after pwr ...` 来自 **shim**（fault-safe），是电源/时钟的权威读数；
- `SWEET GMU: ...` 来自内核，最后一条**带 sync** 的输出才代表走到了那里；
- `release written` 出现 ≠ CM3 在跑；之后任何 GPUCC/GMU/SMMU/RSCC 读都会卡；
- 设备回到 Android 不代表成功，可能是看门狗复位。

## 7. 一句话给新接手的人

电源和固件装载这两道门已经过了（CX/GX 上电、TCM 里是真实固件），
**现在唯一的墙是"CM3 接管后 AP 看不见 GPU"**。先去验证 `1916852` 的
ROOT_EN 修复——如果 hub 时钟根真的跑起来，这堵墙可能就倒了。
