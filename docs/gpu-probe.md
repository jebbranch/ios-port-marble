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
