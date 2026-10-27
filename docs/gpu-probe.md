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
