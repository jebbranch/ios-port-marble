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

## 当前结果

**待测。** 结果出来后补在下面，并同步到
[`docs/gpu-feasibility.md`](gpu-feasibility.md) 的结论段。
