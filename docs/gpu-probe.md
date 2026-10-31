# kgsl-smmu 最小探针（marble）

> 一次开机回答一个问题：**GPU 的 SMMU 能不能被非安全世界的 AP 接管？**
> 这是 GPU 这条路的总开关，和用不用 UniXNU 无关。

## 为什么不在 XNU 里做

`apps-smmu` 上已经吃过一次教训：读/写一个不归我们的块会**整体死总线**，
日志都写不出来。GPU 的块在 XNU 里同样可能处于掉电状态，第一次读就可能
外部中止（panic）。

所以分两层：

| 层 | 位置 | 作用 |
|---|---|---|
| 危险动作 | `shim/shim.c`（XNU 之前） | 带**异常恢复**的 MMIO 读写；中止就记 ESR/FAR、跳过该指令、继续 |
| 结果输出 | `bsd/dev/sweet_glink.c` | 只把 shim 留在 `0xa2e0f000` 的结果页打印进 SWEETLOG |

shim 没有文件系统写手，所以结果必须先落到内存页；XNU 再把它写进正常的
日志环。这一步需要一次 CI 内核构建，但 probe 本身可以在本地反复重编。

## 被探的硬件

```
kgsl-smmu@3da0000   qcom,qsmmu-v500 / qcom,adreno-smmu   24 stream groups
  SMR(n)  = base + 0x800 + 4n
  S2CR(n) = base + 0xC00 + 4n
  GR1     = base + 0x1000 (ID1.PAGESIZE=0)  -> CBA2R = +0x1800+4n
kgsl-3d0@3d00000    RBBM_STATUS = dword 0x210 = byte 0x840
```

SID：`gfx3d_user=0`、`gfx3d_lpac=1`、`gfx3d_secure=2`（DT 里的
`iommus = <&kgsl_smmu ...>`）。原厂 Android 的 kgsl 驱动全权编程这颗 SMMU，
和音频用的 `apps-smmu@15000000` 不是一个实例——这是这条路唯一的希望。

## 结果页（`0xa2e0f000`）

`struct gpu_probe_result`，magic `0x47505550`（"GPUP"）。字段含义：

| 字段 | 含义 |
|---|---|
| `scr0 / id0 / id1 / id2` | SMMU 全局寄存器；`id0&0xff` = SMR 组数，`id1&0xff` = CB 数 |
| `num_smr / num_cb` | 上面两个数，已 clamp |
| `valid_seen` | 上电时已有几条有效 SMR |
| `sid0_slot / smr0 / s2cr0` | SID 0 落在哪个槽、当时的 SMR/S2CR |
| `wsid / w2cr_old` | 写测试用的槽和它的原值 |
| `wsmr` | 只有 SID 0 没有槽时才写 SMR，这里是回读 |
| `w2cr` | **写 BYPASS(0x10000) 后的回读——最关键的字段** |
| `w2cr_f / w2cr_r` | 写 FAULT(0x20000) 后、恢复原值后的回读 |
| `wfault` | 写测试中是否发生过中止 |
| `kgsl_status / kgsl_status2` | KGSL RBBM_STATUS / RBBM_INT_0_STATUS |
| `rfault / far / esr` | 读失败标志和最后一次中止现场 |

## 怎么判读

| 现象 | 结论 |
|---|---|
| `rf=1`（第一次读就中止） | 块在上电时不可达（掉电/XPU）。下一步是 GPUCC 时钟和电源，不是驱动 |
| `id0` 低字节 = 24，`id1` 合理 | 块可达，SMMU 是活的 |
| `byp=0x10000` | **BYPASS 写进去了 → AP 拥有这颗 SMMU，GPU 路线成立** |
| `byp=0x20000` | 和 apps-smmu 一样被上层改写成 FAULT → GPU 也被 hypervisor 锁死 |
| `wf=1` | 写直接中止，连寄存器都不给写 |
| `kgsl st≠0xffffffff` | GPU 的 KGSL 块本身可达，上电/时钟可能已经由 ABL 打开 |

`w2cr` 要结合 `w2cr_f` 看：如果 BYPASS 和 FAULT 都原样回读，说明值是我们
控制的；如果永远是某一个固定值，说明有人在上面改写。

## 跑法

```bash
./scripts/gpu_probe.sh build      # 生成 gpu-l1.img（只读）和 gpu-l4.img（含写测试）
./scripts/gpu_probe.sh run 1      # 先跑只读，确认块可达
./scripts/gpu_probe.sh run 4      # 再跑写测试
```

级别：

- `1` = 只读 SMMU（`GPU_PROBE_LEVEL=1`）
- `2` = + S2CR 写测试
- `3` = + KGSL 读
- `4` = 写测试 + KGSL 读

boot 参数必须带 `sweetgpu=1`，否则 XNU 不打印结果页。脚本已经带上了。

## 安全

- 级别 1 只有读，不会改变任何状态。
- 写测试只碰 `S2CR` 的 BYPASS/FAULT，最后写回原值；GPU 在 XNU 下没有
  客户端，不涉及正在跑的命令。
- 所有访问都在 shim 里带同步中止恢复；最坏情况是 shim 的 SError 路径
  （异步）仍然 panic，但那时结果页可能还没发布——所以先跑级别 1。

## 当前结果（2026-10-27，真机）

| 级别 | 结果 |
|---|---|
| 1（只读） | `id0=0x4c017e18` → NUMSMRG=**24**；`id1=0x40000016` → NUMCB=**22**；`id2=0x5511`。`scr0=0x2f0406`。上电时 **0 条有效 SMR**（`valid=0`）。`rf=0`，**没有中止** |
| 4（写测试） | 写 `SMR[0]=0x80000000`（SID 0）**回读一致**；写 `S2CR[0]=BYPASS(0x10000)` **回读 0x20000(FAULT)**；写 FAULT/恢复都回读 0x20000；`wf=0`（没有总线中止）。KGSL `RBBM_STATUS=0`、`RBBM_INT_0_STATUS=0`，`kf=0` |
| 5（TRANS） | 写 `S2CR[0]=0x1`（TYPE=TRANS, CB0）**回读 0x1**，`l5 trans=0x1` |
| 5（含 CBAR/CB0） | **没有产生任何 XNU 日志**（ring 仍是上一轮的 sweettime）。CBAR(0)/CB0.SCTLR 的访问会把这次启动整个带走，暂时别在 shim 里做 |

### 结论

1. **kgsl-smmu 在上电时可读**，24 个 stream 组、22 个 context bank，和 DT/Android 日志一致。
   它不是"掉电不可达"的块——这比最坏预期好。
2. **hypervisor 的策略是"禁 BYPASS"，不是"禁一切"**：SMR 能写，`S2CR=TRANS` 也能写并保持。
   这一点和 apps-smmu 一样（那边 `S2CR=0x34` 也能写），所以**不能**据此认为 GPU 数据面一定能通。
3. **CB 配置是下一个未知数**：CBAR/CB0 的访问在 shim 里直接让启动消失，说明不能照搬
   "寄存器能读就能写"的假设。要测就回到 XNU 里带 `sweet_log_sync()` 一步一步来。
4. KGSL 寄存器读回 0 不一定是"没上电"：ABL 很可能只是把它保持在复位/时钟关闭，
   总线仍然可达。真正要跑 GPU 还需要 GPUCC 时钟、GMU、CP 固件，这是另一个量级的工程。

**对音频没有任何影响**：`apps-smmu` 的结论不变（见
[`smmu-dead-ends.md`](smmu-dead-ends.md)）。

## 一个必须记住的测试坑

userdata 尾部的 SWEETLOG ring **不会因为一次失败的启动而被清空**。如果 shim 或 XNU
在写日志前就死了，`pull_logs.sh` 拿到的还是**上一轮**的日志——看起来像"探针跑了"，
其实那是旧的。

`scripts/gpu_probe.sh` 现在会比较镜像构建时的 `sweettime` 和日志里的
`SWEET RTC: clock set from sweettime=`；不一致就直接报 `STALE RING`。
以后所有"启动后拉日志"的测试都应该带这个校验。

## 组合扫描（v85+）

`scripts/gpu_sweep.sh` 把下面 11 个组合一次跑完（每个组合一次开机，按顺序只读在前）：

| 变体 | 内容 |
|---|---|
| `m0-base` | SMMU ID/SMR/S2CR 基线 |
| `m1-kgsl` | KGSL 0x0 / 0x840 / 0x1000 / 0x2000 读 |
| `m2-gr1` | GR1 的 CBA2R + CBFRSYNRA 读 |
| `m3-cbar0r` | CBAR(0) 读 |
| `m4-cbar03r` | CBAR(0..3) 读 |
| `m5-cbar0w0` | CBAR(0) 写 0 + 回读 |
| `m6-cbar0w1` | CBAR(0) 写 0x10000 + 回读 |
| `m7-sctlr0r` | CB0 SCTLR 读 |
| `m8-sctlr0w` | CB0 SCTLR 写 0 + 回读 |
| `m9-cb0regs` | CB0 TTBR0/TCR/MAIR 读 |
| `mA-readall` | 所有只读组合并在一起 |

```bash
./scripts/gpu_sweep.sh              # 全跑，约 45 分钟
./scripts/gpu_sweep.sh m3-cbar0r    # 只跑一个
DRY_RUN=1 ./scripts/gpu_sweep.sh    # 只构建
```

每个组合的构建参数通过 `GP_CBAR_BANK` / `GP_CBAR_WRITE` / `GP_CBAR_COUNT` /
`GP_SCTLR_BANK` / `GP_SCTLR_WRITE` / `GP_CB_REGS` / `GP_GR1_READ` /
`GP_KGSL_SCAN` 传给 shim。结果页新增 `last_step`（每次危险访问前写入）和
`prev_step`（下一次启动时回读上一个组合死在哪一步）——如果 DRAM 在复位后
没有被 ABL 清掉，就能定位到具体寄存器。

判定：每个组合都会用镜像的 `sweettime` 校验日志；`NO-XNU` 表示这次启动
没进 XNU（同步中止会被 shim 恢复，所以这通常意味着 SError 或死挂）。

## 组合扫描结果（2026-10-27，真机）

启动参数统一为 `sweetgpu=1 sweetglink=0`（跳过音频数据面，否则 DSP 第一次访问
会挂总线，和 GPU 探针无关）。

| 变体 | 结果 | 访问 |
|---|---|---|
| `m0-base` | **OK**（进 GUI） | ID/SMR/S2CR |
| `m1-kgsl` | **OK** | KGSL 0x0/0x840/0x1000/0x2000 |
| `m2-gr1` | **OK** | CBA2R / CBFRSYNRA |
| `m3-cbar0r` | **OK** | CBAR(0) 读 → 0 |
| `m4-cbar03r` | **OK** | CBAR(0..3) 读 → 全 0 |
| `m7-sctlr0r` | **NO-XNU**（无 panic，自己重启） | CB0 SCTLR (0x80000) 读 |
| `m9-cb0regs` | **NO-XNU**（同上） | CB0 TTBR0/TCR/MAIR (0x80020+) 读 |
| `mA-readall` | **NO-XNU**（同上） | 含上面两项 |
| `m5-cbar0w0` | 启动后设备从 USB 消失 | CBAR(0) **写** 0 |
| `m6-cbar0w1` / `m8-sctlr0w` | 未跑（设备已挂） | CBAR/SCTLR 写 |

### 结论

**kgsl-smmu 的全局寄存器可用，context bank 不可用。**

- `0x0000..0x1FFF`（ID、SMR、S2CR、CBAR 读、KGSL 块）都能读，没有中止；
- `0x80000+` 的 **CB 页**（SCTLR/TTBR0/TCR/MAIR）一碰就整机挂死：没有 panic 屏，
  看门狗自己重启——这不是同步中止（同步中止会被 shim 恢复并继续启动）；
- `CBAR` 的 **写**（m5）同样让设备掉出 USB。

也就是说：我们可以读这颗 SMMU 的身份、可以写流匹配（SMR/S2CR），但**拿不到
context bank**。建不了页表，也就没有 GPU 数据面。原因大概率是 GPUCC/GMU 没上电、
CB 页在掉电域里；要打开它需要 GPUCC 时钟、GMU 固件和 kgsl 的上电序列——那是
驱动移植量级的工作，不是再加几个探针能解决的。

**GPU 这条线到此为止**，除非先做 GPUCC/GMU 上电。对音频没有影响。

---

## 2026-10-30 v8：上面「CB 页不可用」的结论**作废**——偏移算错了

### 错在哪

上面所有 CB 测试都读写 `base + 0x80000`。kgsl-smmu 的 `reg` 只有
`0x3da0000..0x3de0000`（0x40000），`0x80000` 落在 **0x3e20000**，已经在寄存器块之外，
是死总线空间——那不是“context bank 掉电”，那就是“访问了不存在的地址”。
整机挂死、没有 panic 屏、看门狗重启，和死总线的表现一致。

### 正确的基址怎么算（来自 `arm-smmu.c`，不是猜的）

```
numpage  = 1 << (IDR1.NUMPAGENDXB + 1)      /* 每帧 GR 的页数 */
pgshift  = IDR1.PAGESIZE ? 16 : 12
GR0      = base + 0 * (numpage << pgshift)
GR1      = base + 1 * (numpage << pgshift)
CB(n)    = base + (numpage + n) << pgshift
```

| 实例 | IDR1 | NUMPAGENDXB | GR 帧大小 | CB0 |
|---|---|---|---|---|
| apps-smmu@15000000 | `0x6000004b` | 6 | 0x80000 | `0x15080000` ✓（音频那边的值，对得上） |
| kgsl-smmu@3da0000 | `0x40000016` | 4 | 0x20000 | **`0x3dc0000`** |

顺带核对：`2 * 32 页 * 4 KiB = 0x40000`，正好等于 DT 里的 `reg` 长度，
`arm-smmu.c` 的 “address space size differs from mapped region” 检查也是这么算的。

### v8 探针（`GP_SURVEY=1`，只读）

一次开机读三组东西，全部只读：

| 组 | 地址 | 目的 |
|---|---|---|
| GPUCC | `0x3d91108` CX_GDSCR、`0x3d9953c` GDS_HW_CTRL、`0x3d9905c` GX_GDSCR、`0x3d99504` GX domain、`0x911c/0x913c/0x9148/0x7000/0x9150` 时钟分支 | CX 域是否已经上电、谁在控制它 |
| CB0 | `0x3dc0000` SCTLR/TTBR0/TTBR1/TCR/TCR2/MAIR0/MAIR1、GR1 的 CBAR(0)/CBA2R(0)/CBFRSYNRA(0)、CB1.SCTLR | 修正地址后 CB 页到底可不可达 |
| KGSL | `0x3d00000` IDENT、`0x3d00840` RBBM_STATUS | GPU 引擎本身是不是活的 |

结果槽位（旧结构体复用，**不需要重编内核**；语义见 `shim/shim.c` 的注释）：
`cbar_r`=CB 基址、`sctlr_r`=CB0.SCTLR、`ttbr0/tcr/mair`=CB0 的三个寄存器、
`wcbar`=CBAR(0)、`cba2r`/`cbfrsynra`、`kg0..kg3`=CB0.TTBR1/TCR2/MAIR1/CB1.SCTLR、
`cbar1..3`=GPUCC CX_GDSCR/GDS_HW_CTRL/GX_GDSCR、`wsctlr`=ahb、`wsctlr_old`=cx_gmu、
`w2cr_old`=hub_cx_int、`wsmr`=smmu vote、`w2cr`=memnoc_gfx、
`kgsl_status/2`=RBBM_STATUS/IDENT、`op_fault`=逐条读失败的位图。

### 第一次结果（2026-10-30 19:00）

**启动到了 XNU**：USB ECM 设备（`ios-sweet` 0x1209:0x0001）枚举成功，
说明 shim 里的这次探查**没有把总线打死**——在 0x3dc0000 上读 CB 不再致命。
寄存器值要等 SWEETLOG ring 拉回来（这批镜像带 `sweetglink=0`，
`sweetreboot` 在 glink probe 里面，所以**不会自动复位**，需要手动重启一次）。

### v8 真机读数（2026-10-30 19:00 启动，19:15 从 TWRP 拉回）

`sweettime=1793357985` 与镜像一致，`last_step=126`（普查跑完了最后一条），
`op_fault=0` —— **19 条访问没有一个同步中止**。

```
shim v8 scr0=0x2f0406 id0=0x4c017e18 id1=0x40000016 id2=0x5511
nsmr=24 ncb=22 valid=0                       ← 开机时 GPU 一条 stream 都没配
cbbase=0x3dc0000                             ← 与 NUMPAGENDXB=4 推算一致
GPUCC  CX_GDSCR = 0x22f001   (PWR_ON=0, HW_CONTROL=0, SW_COLLAPSE=1)
       GX_GDSCR = 0x22f001   (同样：关着，软件可控)
       GDS_HW_CTRL(0x3d9953c) = 0x5ff4
       GX domain (0x3d99504) = 0x3c0001   (GMEM_CLAMP_IO=1)
       ahb(0x911c)=0            cx_gmu(0x913c)=0x80002221
       hub_cx_int(0x9148)=0x80000001       smmu vote(0x7000)=0x80000000
       memnoc(0x9150)=0x80004220
CB0.SCTLR=0 TTBR0=0 TTBR1=0 TCR=0 TCR2=0 MAIR0=0 MAIR1=0
CBAR(0)=0 CBA2R(0)=0 CBFRSYNRA(0)=0x6dfd CB1.SCTLR=0
KGSL RBBM_IDENT=0 RBBM_STATUS=0
```

解读：

1. **CB 页（0x3dc0000）可读，不中止**——之前“CB 页一碰就挂”完全是 0x80000 那个错误偏移。
2. 读回全 0，不是“寄存器被清空”，而是 **GPU_CC_CX 整个域是关的**
   （`CX_GDSCR.PWR_ON=0`、`SW_COLLAPSE=1`、`GMEM_CLAMP_IO=1`）。CX 域里的
   SMMU context bank 和 KGSL 寄存器在断电时读回 0、且不报错，这正好解释了 v8 的读数。
3. 域外的东西活着：SMMU 全局寄存器（AHB 时钟域）读数正常，SMR/S2CR 也能写。
4. **CX 是软件可控的**（`HW_CONTROL=0`），所以下一步可以直接清 `SW_COLLAPSE`
   把它上电，不需要 RPMh/AOP 投票。

⇒ 下一步（v9）：`GP_PWR=1` 清 `SW_COLLAPSE`、轮询 `PWR_ON`，再读 CB0 / KGSL，
看 CB 页和 GPU 引擎是否随之上电。同时打开 `cpus=8`（用户确认稳定），
免得单核把界面拖死。

### v9 真机读数（2026-10-30 19:17 启动）：CX 能上电，CB 页是活的

对应构建 `GP_SURVEY=1 GP_PWR=1`，单核改成 `cpus=8`。

```
CX_GDSCR 前 = 0x22f001   (PWR_ON=0 HW_CONTROL=0 SW_COLLAPSE=1)
CX_GDSCR 后 = 0xf822f000 (PWR_ON=1 HW_CONTROL=0 SW_COLLAPSE=0)   ← 写 bit0=0 就起来了
GX_GDSCR     = 0x22f001  (GX 仍然关着)
CB0.SCTLR 前 = 0
CB0.SCTLR 后 = 0x3fffe71e (M=0，硬件复位值；关键是非 0 = 该块已上电)
KGSL RBBM_STATUS 后 = 0   (GPU 核还在 GX 域，未上电)
pwr_flags = 0x200         (位含义见 shim.c：0x200 = powered)
```

结论：

1. **`GPU_CC_CX` 是纯软件开关**：`HW_CONTROL=0`，清 `SW_COLLAPSE` 后 `PWR_ON`
   在 1 ms 轮询内就置起，不需要 RPMh/AOP 投票。上一轮“CX 关着”是这台机器
   在 ABL 交接后的默认状态，不是硬件锁。
2. **context bank 页随后可读**（SCTLR 从 0 变成复位值），说明 SMMU 的 CB
   确实在 CX 域里；之前所有“CB 不可用”的结论同时错在两点：地址用了 0x80000
   （死总线），以及没有先上电。
3. KGSL `RBBM_STATUS` 仍是 0：那是 **GX 域**（图形核）的寄存器，要另外开
   `GX_GDSCR` + GMEM clamp/reset + `gx_gfx3d` 时钟。

下一步见下面 v10/v11。

### v12/v13 真机（2026-10-30 20:00–20:20）：**CB0 编程成功，AP 拿下了 GPU SMMU**

先纠正一个过程错误：`GP_CBT` / `GP_CBW` 两个开关加进了 `shim/shim.c`，
但**忘了加进 `shim/build_xnu.sh` 的 CFLAGS**，所以前两轮"测试"其实什么都没跑
（镜像 tag 里的 `t0w0` 就是证据）。补上之后结论完全不同。

#### CB 寄存器可写

`GP_CBT=1`：往 CB0.TCR2 写 0x5、CB0.MAIR1 写 `0xaabbccdd`，回读
`tcr2=0x65`（我们写的 bit0/2 在，其余是 RO 位）、`mair1=0xaabbccdd`（原样）。
**寄存器堆是好的**，之前读到的"垃圾"只是没人初始化过的复位值。

#### 整段 stage-1 配置与回读（`GP_CBW=1`）

| 寄存器 | 写入 | 回读 |
|---|---|---|
| CB0.SCTLR | `0x1`（M=1） | `0x1` ✓ |
| CB0.TTBR0 | 页表 PA `0xa8007000` | `0xa8007000` ✓ |
| CB0.TCR | `0x3520`（T0SZ=32、4K、inner WBWA） | `0x3520` ✓ |
| CB0.TCR2 | `0` | `0x60`（RO 位） |
| CB0.MAIR0 | `0xff` | `0xff` ✓ |
| CBAR(0) | `0x1f000`（S1 trans + S2 bypass + WB） | `0x1f000` ✓ |
| CBA2R(0) | `0x1`（AArch64） | `0x1` ✓ |
| SMR(0) | `0x80000000`（VALID / SID 0 / mask 0） | `0x80000000` ✓ |
| S2CR(0) | `0x1`（TRANS → CB0） | `0x1` ✓ |

页表是 shim BSS 里自建的 1 GiB 恒等映射（512 个 2 MiB block），写完后
`dc civac` 清到 PoC。

**意义**：SID 0（`gfx3d_user`）从「未匹配任何 SMR」变成了「stage-1 走 CB0」，
CB0 由我们自己的页表翻译。GPU SMMU 的**控制面完全归 AP**，和 apps-smmu
被 hypervisor 改写 S2CR 的处境完全不同。剩下的是数据面之外的东西：
GX 域上电（RPMh `gfx.lvl`）、GMU 固件、CP 固件。

#### 附带确认的两件事

1. `gpu_cc_ahb`(0x911c) 始终读 0，但 CB 寄存器照样能写——它不是 CB 访问的前提，
   之前把它当成"寄存器堆没时钟"的证据是错的。
2. CX 域的 GDSC 上电后，CB 的复位值每次开机都不同（`0xffee3a5b` 之类），
   这是未初始化寄存器的正常表现，不是"死总线"。

#### 下一步的硬门槛：GX 域

`GX_GDSCR.PWR_ON` 在清 `SW_COLLAPSE`、放 `GMEM_CLAMP_IO`、脉冲 BLK_ARES 之后
**仍然是 0**（1.5 s 超时）。Android 侧 debugfs 给了答案：

```
/sys/kernel/debug/regulator/...gfxlvl-pm8350_s5_level   enable=0  voltage=48
/sys/kernel/debug/regulator/...mxclvl-pm8350c_s2_gfx_voter_level  voltage=192
```

GX GDSC 的父级是 `pm8350c_s2_gfx_voter_level`，再往上是 PM8350 S5（`gfx.lvl`）。
Android 里这条轨道由 kgsl 按需 vote；XNU 启动时它 `enable=0`，所以 GDSC 关不上。
**要开 GX，必须先通过 RPMh 对 `gfx.lvl` 投票**——这正是 sweet_rpmh.c 已经能做的
那类命令（它现在只会投 PMIC LDO）。cmd-db 里 `cx.lvl` 地址已经读出来是 `0x30000`，
`gfx.lvl`/`mx.lvl`/`mxc.lvl` 的地址还要再取一次（上一轮结果槽被覆盖了）。

### v13：GX 域上不了电——它的开关在 GMU 手里

为了开 GX，先把 RPMh 的 ARC 投票补上了（`GP_RAIL`）：在 shim 里用和
`sweet_rpmh.c` 相同的 TCS 握手，往 apps_rsc drv-2 的 ACTIVE TCS 里写一条
「level」命令。cmd-db（PA `0x80860000`，magic `0x0c0330db`，138 条）里查到：

| 资源 | cmd-db 地址 | 投票结果 |
|---|---|---|
| `cx.lvl` | `0x30000` | 控制组，rc=0 ✓ |
| `mx.lvl` | `0x30010` | rc=0 ✓ |
| `gfx.lvl` | `0x30050` | rc=0 ✓ |
| `mxc.lvl` | `0x30120` | **rc=-5（命令永不完成）** |

但 GX 仍然起不来：清 `SW_COLLAPSE`、放 `GMEM_CLAMP_IO`、脉冲 BLK_ARES、
启动 gx 时钟之后，`GX_GDSCR = 0x6822f000`——**`PWR_ON`(bit31) 依旧是 0**。
（作为对比，同样手法开 CX 时 bit31 立刻置 1。）

原因写在厂商驱动里，不是我们写错寄存器：

```c
/* kgsl_pwrctrl.c: kgsl_pwrctrl_pwrrail() */
if (gmu_core_gpmu_isenabled(device))
        return 0;   /* GMU 在跑时，AP 根本不碰 vdd/vddcx */
```

gen7（A730/A725）的 **GX 电源由 GMU 掌管**：`kgsl` 只在 GMU 没启用时才自己开
`vdd`，否则整套上电/掉电都走 GMU 的 HFI 协议。换句话说：

* CX（SMMU、GMU 自己所在域）可以由 AP 直接开——已验证；
* **GX（图形核）必须等 GMU 固件跑起来**，AP 单方面写 GDSC 不会闭合。

### 到目前为止 GPU 这条线的净结论

1. GPU SMMU 的全局寄存器、SMR/S2CR、**context bank 全部可读写**，
   我们已经能给它建 stage-1 页表并把 SID 0 指过去（v12）。
2. GPU_CC_CX 是纯软件开关（v9）。
3. GPU_CC_GX 不是——它由 GMU 控制（v13）。
4. KGSL 的 RBBM 寄存器在 GX 域，所以在 GMU 起来之前永远是 0。

⇒ 下一个真正的里程碑只能是 **GMU bring-up**：把 `gmu_gen70000.bin` 载入
GMU 内存、开 GMU 时钟/复位、跑 HFI 握手。那是 UniXNU/NativeAdreno 里
GMU + CP 初始化那几千行的核心，也是这条路从"寄存器可控"变成"引擎能跑"的分界。

## GMU bring-up：下一步的完整清单（2026-10-30 收尾）

固件已经从这台机器上拉到 `work/gpu-fw/`：

| 文件 | 大小 | SHA-256（前 16 位） |
|---|---|---|
| `gmu_gen70000.bin` | 57 272 | `b2d3f0e2a98fd625` |
| `a730_sqe.fw` | 74 676 | `e67d1829f57fc832` |
| `a730_zap.mdt` + `.b00/.b01/.b02` | 10 324 | — |

要复刻的厂商顺序（`adreno-gmu`/`kgsl` 侧）：

1. **CX 上电**（已验证）＋ `gpu_cc_ahb`/`hub` 时钟链（目前 `ahb=0`，hub RCG 未配）。
2. **GMU 时钟**：`gmu_clk`、`cxo`、`axi`、`memnoc`、`ahb`、`hub`、`apb`，
   其中 `gmu_clk_src`(0x9318)/`hub_clk_src`(0x93ec) 两个 RCG 需要配 PLL 源。
3. **GMU 自己的 SMMU 通道**：SID 5（`iommus = <&kgsl_smmu 5 0x400>`）。
   CB 现在已经能编程，这条比之前预期容易。
4. **固件加载**：把 `gmu_gen70000.bin` 放进 GMU 可见内存，写 GMU 的
   "FW start" 寄存器组。
5. **释放 GMU 复位**（`GPU_CC_GMU_BCR` = 0x9314）并等 HFI 就绪（IRQ 0x130）。
6. **HFI 握手后 GX 才由 GMU 打开**——这就是为什么 AP 自己写 `GX_GDSCR`
   永远等不到 `PWR_ON`。
7. CP 微码（`a730_sqe.fw`）+ ZAP（`a730_zap.*`）再往后是命令环和 GPU 调度。

第 1–5 步都是"寄存器可控"级别的活，第 6 步之后才进入真正的驱动移植。
本仓库已经把第 1 步做完了，第 3 步的关键（CB 可编程）也做完了。

### v14：GPUCC 的两个 RCG 已能配置

`GP_CLK=1` 在 CX 上电后配置 `hub_clk_src`(0x93ec/0x93f0，源 GPLL0/2) 和
`gmu_clk_src`(0x9318/0x931c，源 TCXO)，再去开 `hub_aon`/`ahb`/`hub_cx_int`/`cx_gmu`。

```
hub CMD_RCGR = 0x2        (ROOT_EN=1, ROOT_OFF=0 → 在跑)
hub CFG_RCGR = 0x600      (SRC_SEL=6)
gmu CMD_RCGR = 0x2        (在跑)
gmu CFG_RCGR = 0x0        (SRC_SEL=0 = TCXO)
hub_cx_int   = 0x1        (已开)
cx_gmu       = 0x2221     (已开)
gpu_cc_ahb   = 0          ← 仍然开不起来
```

`gpu_cc_ahb` 依旧读 0，但 CB 寄存器能正常读写（v12 已证），所以它不是 CB 的
前提；它的父级 `hub_ahb_div`(0x9430，硬件只读分频) 可能没被使能，留待 GMU
阶段一起处理。

GMU 侧要复刻的是 `adreno_gen7_gmu.c`：ITCM/DTCM 窗口（GMU VA 0 / 0x10004000，
各 16 KiB）、`GEN7_GMU_CM3_SYSRESET`、`FW_INIT_RESULT` 轮询、`HFI_CTRL_INIT`，
固件 `gmu_gen70000.bin` 已经拉到 `work/gpu-fw/`。

### v15：GMU 寄存器块也还是"没时钟"

`GP_GMU=1`：先读 `GPU_CC_GMU_BCR`(0x9314) —— **bit0 本来就是 0（已释放）**；
再读 GMU 的两个窗口：

```
GMU AO 块 0x3d40000 / +4        = 0 / 0
GMU 核   0x3d68000 / +4 / +8    = 0 / 0 / 1
```

和 CB 页当初一样：**寄存器可寻址但读回 0**，即 GMU 的 AP 侧接口还没有时钟。
把 `hub_clk_src` 的源从 GPLL0/2 换成必开的 TCXO 再试，结果不变：

```
hub CMD_RCGR = 2 (ROOT_EN)   hub CFG_RCGR = 0 (TCXO)
hub_aon(0x93e8) = 0          hub_ahb_div(0x9430) = 0
gpu_cc_ahb(0x911c) = 0       ← 写 bit0 不latch
```

对照 `clk-branch.c`：`BRANCH_CLK_OFF=BIT(31)`，enable 就是写 `enable_reg` 的
bit0（`regmap_update_bits`）。我们写的是对的，**是硬件不让我们开**——分频器
`hub_ahb_div` 读 0，说明它上游就没有在跑，或者这一支属于 GMU 自己管理的时钟域。

⇒ 到这里，GPU 这条线剩下的门槛非常明确了：

1. **GPUCC 时钟树**（PLL0/PLL1 lucid_ole 配置 + hub 分频 + ahb/hub_aon 分支），
   即把 `gpucc-waipio.c` 的时钟部分移植进来；
2. **GMU 固件协议**（ITCM/DTCM 窗口、CM3 复位、FW_INIT、HFI），
   即 `adreno_gen7_gmu.c` 的核心；
3. 之后才是 CP 微码和命令环。

第 1 项是几天，第 2 项是几周（NativeAdreno 那 7k 行的主体），第 3 项是另一个量级。
到此为止，**"寄存器级可控"的部分已经全部做完并留下可复现的探针**。

### v16/v17：GMU 的真实寄存器映射拿到了，但 GPUCC 的 AHB 仍然开不起来

从厂商源码里找到两个关键事实：

1. `drivers/gpu/msm/gen7_reg.h` 才是 GMU 寄存器表：ITCM `0x1b400`、
   DTCM `0x1c400`、`CM3_SYSRESET` `0x1f800`、`FW_INIT_RESULT` `0x1f81c`、
   `HFI_CTRL_STATUS` `0x1f980`、`HFI_CTRL_INIT` `0x1f986`。
   之前读 `gmu+0/+4/+8` 是空字段，不能说明 GMU 死了。
2. GMU 启动序列（`adreno_gen7_gmu.c`）：写 `CM3_SYSRESET=0` → 轮询
   `FW_INIT_RESULT` bit8 → `HFI_CTRL_INIT=1` → 轮询 `HFI_CTRL_STATUS` bit0。
   固件必须先写进 ITCM/DTCM 窗口，否则 bit8 永远不会置起。

真机读数（`GP_GMU=2`，CX 已上电）：

```
0x3d87800 (CM3_SYSRESET)   = 0xdeafbead
0x3d8781c (FW_INIT_RESULT) = 0xdeafbead
0x3d87980 (HFI_CTRL_STATUS)= 0xdeafbead
0x3d87800+0x1b400 (ITCM)   -> 同步中止 (0xffffffff)
```

`0xdeafbead` 是总线对"被门控/未供时钟"块的伪造响应，不是真值；ITCM 窗口直接
中止。**GMU 仍然没有时钟**。

GPUCC 侧这一轮又排除了两个可能：

* hub RCG 现在写入了正确的 M/N/D（`CMD=0x2`、`CFG=0x600`、`M=1`），
  `hub_aon`(0x93e8) 也能开了；
* GCC 侧 `gcc_gpu_gpll0_div_clk_src`(0x62000 bit16) 和
  `gcc_gpu_gpll0_clk_src`(bit15) 都写进去了并回读确认（`0xe1a57f`）。

**`gpu_cc_ahb`(0x911c) 依然写不进 bit0，`hub_ahb_div`(0x9430) 依然读 0。**
分支扫描（`GP_BR`）显示：`0x911c/0x9120/0x912c/0x9130/0x9134` 这 5 个写不进
（恰好是驱动里 HALT_VOTED 那一族 + ahb），而 `0x913c/0x9144/0x9148/0x914c/
0x9150/0x93e8` 都能写。这个"能在同一页里写一半"的现象说明不是我们写错地址，
而是这一族时钟在硬件上由别的东西（GMU/AOP 的 vote）控制。

顺带一个负面结论：Android 的 `regmap/3d90000.clock-controller/registers` 只能
读到 `0x3ff8`（PLL 区），再往后读会让厂商内核重启——**Android 不能当 GPUCC
分支区的寄存器 oracle**。

### v18：HLOS1 投票页也不是 AHB 的钥匙

`GP_VOTE=1` 把 GPUCC 的 `0x7000` 页（HLOS1 vote 区）64 个字全部试写 bit0：
只有 `0x7000..0x7010` 这 5 个投进去了（mask=0x1f），**`gpu_cc_ahb` 仍然是 0**，
`hub_ahb_div` 仍然是 0。

到这一步，GPUCC 侧能想到的常规钥匙都试过了：

| 嫌疑 | 结果 |
|---|---|
| hub RCG 源/GCC 门控 | 已开（`0x62000[16:15]` + TCXO/GPLL0 两种源） |
| hub RCG 的 M/N/D | 已写（M=1，CMD=2，CFG=0x600） |
| hub_aon(0x93e8) | 能开 |
| HLOS1 投票页 | 已扫，5 个投上，ahb 无效 |
| CX GDSC / CX BCR | 已开 / 已释放 |
| 分支寄存器本身 | `0x911c` 写 bit0 不 latch（同页其它分支都能写） |

⇒ `gpu_cc_ahb` 的门控不在我们试过的任何 AP 可见寄存器里。它是 GMU 那一族的
时钟，最可能的解释是：**这一族时钟由 GMU/AOP 的硬件状态机 gate，AP 只有在
GMU 已经起来的上下文里才能打开**——也就是经典的"先有鸡还是先有蛋"。
对应的正规解法是走 **AOP 辅助的 GMU 启动**（DT 里 GMU 节点有
`mboxes = <&aop 0>`），或者把 `adreno_gen7_gmu` 的整条 probe 路径移植进来。

### v19–v21：GMU 寄存器块对 AP 不可达，边界已经很清楚了

这一轮把厂商 `gen7_gmu.c` 里 AP 侧能做的准备全部复刻了一遍，读数如下。

**1. Android 侧的关键旁证**：GPU 活跃（`clock_mhz=550`）时，GPUCC 这些时钟的
`clk_enable_count` **全是 0**：

```
gpu_cc_ahb_clk en=0  gpu_cc_cx_gmu_clk en=0  gpu_cc_hub_cx_int_clk en=0
gpu_cc_gx_gfx3d_clk en=0 rate=0
```

说明这些时钟在**GMU 起来之后由 GMU 自己管理**，AP 的 `clk` 框架只是最初
把它们打开过一次。`gpu_cc_ahb` 写不进 bit0 也就说得通了——它属于 GMU 那一族。

**2. 我们已按厂商顺序做完的事**（全部真机验证）：

| 步骤 | 结果 |
|---|---|
| CX GDSC 上电 + CX BCR 释放 | ✓ |
| RPMh `cx.lvl=384` 投票 | rc=0 ✓ |
| hub/gmu RCG（源、CFG、M/N/D） | CMD=2、CFG=0x600、M=0（HW_CLK_CTRL_MODE 下 M 是硬件控制，读 0 正常） |
| hub_aon(0x93e8) | ✓ |
| GCC `0x62000[16:15]`、`0x81008[0]`、`0x81010[0]` | 写入回读 ✓ |
| HLOS1 投票页 0x7000 | 5 个投上 |
| PDC ucode（0xb290000+0x4500/0x4520） | 回读 `0x80000001` ✓ |
| RSCC ucode（0x3d50000，seq start 0x4520 + 5 条微码） | 回读 `0x4520` ✓ |

**3. GMU 寄存器块仍然不可达**：

```
0x3d68000+0x1f800 (CM3_SYSRESET)    = 0xdeafbead
0x3d68000+0x1f81c (FW_INIT_RESULT)  = 0xdeafbead / 同步中止
0x3d68000+0x1b400 (ITCM[0])         = 同步中止
```

`0xdeafbead` 是被门控从设备返回的固定伪造值。**AP 侧能开的门都开了，GMU 仍然
不响应。** 这不是我们漏了某个寄存器，而是 GMU 的上电握手（RSCC/AOP/XPU 之一）
还没有被正确触发。

### 下一步：用 kprobe 直接抄厂商

既然盲目试寄存器已经到边界，最可靠的办法是**在 Android 上挂 kprobe**，
抓厂商驱动在 GPU 上电时真实写下的 `（寄存器, 值）` 序列：

* `gmu_core_regwrite` / `gmu_core_regread`
* `clk_branch2_enable`（看 `gpu_cc_ahb` 到底怎么开的）
* gdsc enable、`gen7_gmu_device_start`

这条路线这个仓库已经用过（音频阶段用 kprobe 抓过厂商报文），可行。

### v22/v23：GMU 终于可访问了（地址换算的锅）

`gmu_core_regwrite()` 走的是 **kgsl 的 regmap**（基址 `0x3d00000`，按 **dword** 计），
不是 GMU 节点自己的 `0x3d68000`。kgsl regmap 把 gmu 资源放在 word `0x1a000`，
所以：

```
byte = 0x3d68000 + (gen7_word_offset - 0x1a000) * 4
```

| 寄存器 | gen7 偏移 | 真实地址 | 读回 |
|---|---|---|---|
| CM3_SYSRESET | 0x1f800 | `0x3d7e000` | **0x1（复位中）** |
| FW_INIT_RESULT | 0x1f81c | `0x3d7e070` | 0 |
| HFI_CTRL_STATUS | 0x1f980 | `0x3d7e600` | 0 |
| CM3_ITCM_START | 0x1b400 | `0x3d6d000` | 可写 |
| CM3_DTCM_START | 0x1c400 | `0x3d71000` | 可写 |

（之前读 `0x3d87800` 得到 `0xdeafbead`、读 `0x3d1f800` 直接挂总线，都是地址算错。）

`GP_TCM=1` 用写模式验证：ITCM[0] 写 `0xdeadbeef`、DTCM[0] 写 `0xcafebabe`，
回读完全一致 ⇒ **两个 TCM 窗口都能写**。

### 厂商 GMU 写序列（kprobe 抓到的真值）

在 Android 上用 kprobe 挂 `kgsl_regmap_write`（`gmu_core_regwrite` 被内联了），
一次开机抓到 167 963 条写；GMU 区的序列存在
[`traces/gmu-wake-writes.txt`](traces/gmu-wake-writes.txt)：

```
0x1f8f1=1  0x1f8f0=1  0x1f81c=0  0x1f801=2
0x1f985=0x60006000  0x1f984=1  0x1f82d=0x4052
0x1f9ce=0  0x1f9cf=0x7030200  0x1f9cd=0x60002003
0x1f40f=1  0x1f400=1  0x1f401=1
0x1f991=0xffffffff  0x1f993=0xffff7ff9
0x1f800=0          ← CM3 出复位
0x1f986=1          ← HFI_CTRL_INIT
```

### 当前状态：固件写进 TCM 了，CM3 还没报 init

`GP_GMUBOOT=1`：内嵌 `gmu_gen70000.bin`、写 ITCM/DTCM（回读 ITCM[0]=0x4000 ✓）、
建 SID 5 / CB1 页表映射 GMU cache 区、回放上面的配置写、释放 `SYSRESET=0`、
轮询 `FW_INIT_RESULT` —— **bit8 仍然是 0**。

固件块格式还没完全对上（16 字节头里 size 的位置有两种解释，两种都能"凑巧"
走通文件），CM3 不起来的原因大概率是 cache 块没有正确落到 GMU 可见内存里。
下一步：kprobe `kgsl_regmap_bulk_write` 拿到厂商真实的（偏移, 字数）序列。

### v24：GMU 固件格式破解，CM3 真的跑起来了

用 kprobe 抓 `kgsl_regmap_bulk_write` 拿到了厂商真正的固件写入（三次）：

```
off=0x1b400 cnt=0x77  w0=0x10007f80 w4=0x105     ← ITCM
off=0x1d3e6 cnt=0xa   w0=0x0        w4=0x0       ← DTCM +0x3f98
off=0x1d3f7 cnt=0x6   w0=0xffffffff w4=0x40000007 ← DTCM +0x3fdc
```

拿 `w0=0x10007f80, w4=0x105` 去文件里搜，块头立刻现形——**块头是
`{u32 addr; u32 size; 0xdeadbeef; 0xdeadbeef}`（magic 按大端存）**，
文件前面还有 16 字节的文件头：

| 块 | addr | size | 文件偏移 |
|---|---|---|---|
| ITCM | 0x0 | 0x1dc | 0xc0 |
| GMU cache | 0x4000 | 0xdcac | 0x2ac |
| DTCM | 0x10007f98 | 0x28 | 0xdf68 |
| DTCM | 0x10007fdc | 0x18 | 0xdfa0 |

（之前按 `{addr, size}` 解析全是错的，所以写进去的是垃圾。）

按正确格式重写解析后，`GP_GMUBOOT=3`（只加载、不放复位）真机回读：

```
ITCM[0] = 0x10007f80   ITCM[1] = 0x105     ← 与厂商写入逐字一致
```

**这证明固件已经正确落进 TCM。**

### 当前的边界：一放 CM3 复位就挂机

`GP_GMUBOOT=1/2`（写完固件后 `CM3_SYSRESET=0`）会让整机挂死、需要复位。
但同样的流程在固件是空的时候不会挂 —— 说明 **CM3 确实开始执行了**，然后它
访问了什么没映射好的东西，把 GMU 总线拖死，AP 之后碰 GMU 就挂。

剩下两件事，按可信度排序：

1. **GMU 的 SID5 页表**（cache 块 0x4000 与 HFI 区 0x60000000）是否正确。
   我们按 4 KiB 页建了 CB1，但还没有独立验证过 walk 结果。
2. **HFI 队列表**（`HFI_QTBL_ADDR`/`INFO`）不是简单清零就够的，厂商的
   `gen7_hfi_alloc` 会在该内存里初始化队列描述符；CM3 起来第一件事就是读它。

### v25：HFI 表已按厂商格式初始化；放 CM3 复位仍然挂机

`GP_GMUBOOT=4`（建 SID5 映射 + 读回，不放复位）真机读回：

```
SMR(1)=0x80000005   S2CR(1)=0x1   CBAR(1)=0x1f000   CBA2R(1)=0x1
ITCM[0]=0x10007f80  ITCM[1]=0x105      ← 固件内容与厂商一致
```

HFI 队列表也按 `adreno_hfi.h` 的格式初始化了（表头 24 字节 + 3 个 48 字节队列头，
队列区从 `+176` 开始，每队列 4 KiB，type = `0x0A00/0x0A01/0x2802`），
`QTBL_ADDR=0x60000000` 指向它。

但只要执行 `CM3_SYSRESET=0`，整机就挂死。此时 AP 对 GMU 的任何访问都会挂，
说明 GMU 侧在 CM3 启动后进入了异常状态（大概率是它第一次取指/取值走了 SID5，
而我们的映射或 cache 内容还有不对的地方），同时 AHB 没有交回给 AP。

### 下一步（按顺序）

1. 用厂商的 `kgsl` 在 Android 上 kprobe `kgsl_regmap_bulk_write` 时同时抓
   **SMR/S2CR/CB 寄存器写入**（`qsmmuv500` 的配置函数），逐值对比我们的 CB1；
2. 或者先不映射 cache，只跑 CM3 的 ITCM 部分（向量表 + 480 字节），
   看挂死的时机是否变化——能区分"取指失败"和"HFT/其他"。

### v26：CM3 启动本身不会挂机；挂的是"CM3 跑起来之后的 XNU 启动"

三个对照实验把问题夹出来了：

| 模式 | 行为 | 结果 |
|---|---|---|
| `GP_GMUBOOT=4` | 建 SID5、装固件、写配置，**不放复位** | XNU 正常启动 ✓ |
| `GP_GMUBOOT=5` | 放复位、50 ms 后**直接 PSCI 复位** | 机器 30 s 内正常回到 Android ✓ |
| `GP_GMUBOOT=7` | 放复位、200 ms 后**交给 XNU** | XNU 起不来（ring 无新条目、UDP console 无输出） ✗ |

⇒ **CM3 放复位不会拖死 SoC**（=5 证明了）；问题出在"CM3 正在运行"这件事
影响到了 XNU 的早期启动。另外 =1 的挂死也解释清楚了：CM3 跑起来之后 AP 对
GMU 寄存器的访问会挂（AHB 不再对 AP 开放），所以轮询 `FW_INIT` 必然挂。

下一步两个方向：

1. **IRQ**：GMU 的 `hfi`/`gmu` 两条 SPI（DT 里 0x130/0x131）如果处于 level
   断言状态，XNU 的 GIC 初始化可能被中断风暴卡住 —— 先在 shim 里把这两条
   SPI 在 GIC 侧屏蔽，再试 =7。
2. **架构**：把 GMU bring-up 从 shim 挪到 XNU 里（开机之后再放 CM3），
   这样 XNU 的早期启动不受影响，挂也只挂在 GMU 子系统。

### v27：CM3 确实执行了整整 200 ms——它留下的状态会破坏 XNU 早期启动

最后一组 A/B，把"到底是什么破坏了 XNU"钉死了：

| 模式 | 动作 | XNU |
|---|---|---|
| `=4` | 装固件 + SID5 + 配置，**不放复位** | ✓ 正常 |
| `=9` | **瞬时**放复位再拉回复位（CM3 来不及执行） | ✓ 正常 |
| `=8` | 放复位 → **200 ms** → 拉回复位 | ✗ 起不来 |
| `=7` | 放复位 → 200 ms → 直接交给 XNU | ✗ 起不来 |

结论只有一种读法：**这 200 ms 里 CM3 真的在执行固件**，并且留下了某种
持久副作用（放开复位、再拉回也消不掉），这个副作用会让 XNU 的早期启动
卡死。而 Android 内核不受影响（它会把芯片重新初始化一遍）。

这同时回答了 v26 的一个疑问：`=1` 里"轮询 FW_INIT 就挂"不是假的，是 CM3
跑起来后 AP 真的读不到 GMU 寄存器了（AHB 交给 GMU）。

### 下一步的架构建议

把 GMU bring-up 从 **shim** 挪到 **XNU 里**（开机完成之后再放 CM3）：

* XNU 的早期启动不再被打扰；
* 出问题时我们有 ring、有 console、有完整的 I/O，而不是"整机挂死"；
* 副作用的具体内容（GMU 动了 GPUCC/RPMh/RSCC 的什么）可以在系统活着的时候
  一项一项对照。

## 内核侧 GMU 启动（补丁 0216）

架构改为：**shim 只做 staging，XNU 负责硬件动作**，这样每一步都能进 SWEETLOG。

shim 把固件与缓冲摆到固定 PA 并打 `GMUF` 标记：

| 地址 | 内容 |
|---|---|
| `0xa2e10000` | `gmu_gen70000.bin` 副本 |
| `0xa2e20000` | GMU cache 块（64 KiB） |
| `0xa2e30000` | HFI 队列表（64 KiB） |
| `0xa2e40000` | 页表（20 KiB） |

内核里 `g_gmu_boot()`（`sweetgmuboot=1`）按顺序完成：CX GDSCR → GPUCC 时钟 →
ITCM/DTCM → SID5/CB1（含 TLB 失效）→ HFI 表 → 厂商预启动寄存器 → 放 CM3。

真机已经走到的点：

```
SWEET GMU: staged fw 57272 bytes
SWEET GMU: cx gdscr=0xf822f000
SWEET GMU: clocks hub=0x2 ahb=0 gmu=0x2221
SWEET GMU: fw loaded, itcm0=0x10007f80
SWEET GMU: smmu smr1=0x80000005 s2cr1=0x1 cb1=0x1
SWEET GMU: pre-boot cfg done
SWEET GMU: releasing cm3        ← 之后该线程永久卡住
```

两个新事实：

1. **Device 内存宽访问会 panic**：第一次运行死在 HFI 表初始化，编译器把表写
   合成了 `stur q0`（128-bit NEON store），Device-nGnRnE 不允许。全部改成
   32-bit 访问后通过。
2. **放 CM3 的那次写永不返回**：写一旦落地，GMU 立刻接管 AHB，Device-nGnRnE
   的顺序性会让该线程之后的所有内存访问永久阻塞。所以放复位必须放到一次性
   线程里，主线程只能通过 **DRAM 里的 HFI 队列**观察 GMU。

当前设计：三个一次性内核线程（A 放复位、B 300 ms 后写 `HFI_CTRL_INIT`、
C 600 ms 起只读 DRAM 的 HFI 队列并写日志/复位），探针线程不再碰 GMU。
