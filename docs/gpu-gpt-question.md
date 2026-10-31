# marble（SM7475）GPU 硬件启动：现状与问题

## 环境

Redmi Note 12 Turbo（SM7475，GPU = Adreno 725v1，DT `adreno-gpu-gen7-4-0`，Android 内核按 a730 驱动）。
XNU 8796 经 `fastboot boot` RAM-only 启动，链路 ABL → 自写裸机 shim → XNU，**没有 Android、没有厂商 GPU 驱动**，SMMU/时钟/GMU 全部由我们从零编程。设备上有 `gmu_gen70000.bin`、`a730_sqe.fw`、`a730_zap.mdt`。已用 kprobe 抓到原厂完整 GPU 首次上电序列。

目标：先让 GMU 的 CM3 跑起来，再谈 CP/HFI。

## 已确证（真机回读）

- kgsl-smmu 独立于被 hypervisor 管的 apps-smmu：`IDR0=0x4c017e18`、`IDR1=0x40000016` → **CB0=0x3dc0000**。
- `S2CR=BYPASS(0x10000)` 被改写成 FAULT；`S2CR=0x1`（stage-1+CB0）可写可回读；SMR 可写。
- GPU_CC 基址 0x3d90000：`CX_GDSCR=0x9108`(bit31 PWR_ON/bit1 HW_CONTROL/bit0 SW_COLLAPSE)、`ahb=0x911c`、`cx_gmu=0x913c`、`hub_cx_int=0x9148`、`smmu vote=0x7000`、gmu RCG `0x9318/0x931c`、hub RCG `0x93ec/0x93f0`。
- GMU 基址 0x3d68000，gen7 寄存器是"字偏移"，实际字节：ITCM 0x3d6d000、DTCM 0x3d79000、CM3_SYSRESET 0x3d7e000、FW_INIT_RESULT 0x3d7e070、HFI_CTRL_INIT 0x3d7e618。
- GMU 固件块格式 `{addr,size,0xdeadbeef,0xdeadbeef}`（magic 大端）解析正确，写入 ITCM/DTCM 后逐字节回读一致。
- RPMh：`apps_rsc` drv-2 @0x17a20000、`tcs-offset=0xd00`；`CFG=0x800c8104` → drv2 TCS=8、NCPT=16；solver bit24=0（非 solver 模式）；DT 分组 ACTIVE 2 / SLEEP 3 / WAKE 3。

## 卡点（两条互相矛盾的现象）

1. **CX `PWR_ON` 回不到 1**：ABL 留下的初始状态是 `PWR_ON=1`；我们做一次"清 HW_CONTROL → 置 SW_COLLAPSE → 再清 SW_COLLAPSE"后读回 `0x228801`，永远回不来。
2. **但此时 GPU_CC/GMU 仍可读且值合理**：GMU 返回 `0x80000000`（RCC ROOT_OFF）、ITCM 读 0、CB1 读 0，**不是 0xffffffff、也不挂总线**。⇒ 到底是"域其实有电、只是 GDSC 状态语义"还是"没电"？
3. **RPMh 投票显示成功但不生效**：用 ACTIVE TCS(0/1)+AMC 触发投 `lcx.lvl(0x30030)=7`、`lmx.lvl(0x30040)=5`、`mxc.lvl(0x300a0)=6`，CMD_STATUS=ISSUED|COMPLETE、TCS 回 idle、IRQ 有 bit——但 CX 域依旧回不来。
4. 其它：`gpu_cc_ahb(0x911c)` 写 1 不 latch（厂商写 1 后 1 ms 自清 0，属正常门控）；写 `CM3_SYSRESET` 放复位会卡住执行核（已用"牺牲核"绕开，但读到的仍是没起来）。

厂商 trace（kprobe，8.47–8.53 s）关键差异：它写 `CX_GDSCR=0x228800`（EN_REST_WAIT=2/EN_FEW_WAIT=2/CLK_DIS_WAIT=8/RETAIN_FF），然后 gmu RCG→cx_gmu→hub RCG→hub_cx_int→smmu vote→GCC 0x81010/0x81018→ahb；**全程没有 collapse→restore 周期**，第一次上电时 CX 本来就是 ON。

## 要问的

1. **CX GDSCR 语义**：`PWR_ON=0` 但寄存器仍可读——SM8450/7475 这一代的 `gpu_cc_cx_gdsc` 是不是 VOTABLE？有没有 `rsupply`？清 `HW_CONTROL` 后要恢复，是否还需要 `SW_OVERRIDE(bit2)` 或 `CFG_GDSCR(+0x4)` 的 `POWER_UP_COMPLETE(bit16)` 握手？
2. **RPMh 事务**：我们的写法（临时清本 TCS 的 IRQ_ENABLE → 写 msgid=0x10108/addr/data → WAIT/CMD_ENABLE → CONTROL=AMC_EN → CONTROL=AMC_EN|TRIGGER → 轮询 COMPLETE/idle → 还原）和 `rpmh-rsc.c` 等价吗？Linux 在 probe 时就置上 ACTIVE 的 IRQ_ENABLE 且从不清——**我们临时清位会不会让 TCS 不执行**？命令"完成"代表 RPMh 已执行，还是只代表 RSC 收下了（真正下发由 AOP 异步做）？
3. **验证手段**：除 SPMI 读 PMIC SMPS 的 VOUT/MODE/EN 外，RSC 里有没有可读的"已解算状态/BCM 状态/msg-RAM/sleep-wake set 内容"能证明投票落地？RPMh 控制时 PMIC 的 VOUT 寄存器还等于实际 setpoint 吗？
4. **GMU 前提**：`GPU_CC_GMU_BCR(0x9314)` 要不要先脉冲？CM3 除 GPU_CC GMU RCG（父 TCXO）外还需要 `gmu_ao`/CX_AO 的时钟或电源吗？GCC `0x81010/0x81018`（疑似 memnoc/ahb 投票）是必需的吗？ITCM 回读正确是否已能证明 TCM 时钟在跑？
5. **放复位的卡死**：写 `CM3_SYSRESET` 后执行流停在这次写（POSTED/early-ack 也无效），是"写完成永不返回"还是其它？从非安全 EL1 放 CM3 复位有已知前置条件（AHB fence、GX clamp、CX_AHB 等）吗？
6. **判定 CM3 活着**：GX 未上电、SMMU 未映射时，最便宜可靠的生命信号是什么（FW_INIT_RESULT / HFI_CTRL_STATUS / CM3 写 DDR / AO PMU 计数）？
7. **方向**：如果这个 hypervisor+ABL 组合下 AP 真控不了 CX/GX，可行替代是——完全靠 ABL 留下的 CX 状态（不碰电源，只解决 ahb 不 latch）？由最早的裸机 shim 先把 GPUCC 时钟/电源全部点亮再交给 XNU？还是通过已有 AOP/GLINK 通道请 AOP 开轨道？

## 已核对的更正

之前文档里把 TCS 的 `msgid` 简写成 `0x108`，那是个笔误：代码里实际写的是
`8 | BIT(8) | BIT(16)` = **0x10108**（`LEN=8 | RESP_REQ | WRITE`），和
`rpmh-rsc.c` 的 `CMD_MSGID_LEN | CMD_MSGID_WRITE | RESP_REQ` 一致。

## 最想被指出错的三点

1. "`PWR_ON=0` 但仍能读寄存器 ⇒ 域其实有电" ——这个解读对吗？
2. 在没有 Linux rpmh 客户端初始化的 XNU 里，ACTIVE TCS + AMC 投 ARC level 是否完整正确？
3. 厂商根本没做 collapse，我们却主动做了一次 —— 这一步是不是从开始就不该做？
