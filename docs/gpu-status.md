# GPU：当前判决与证据索引

> **当前状态（2026-11-28）**：旧 XNU 8796 GPU 线仍冻结；FoXLinne 的上游
> iOS 18 / XNU 11417 实现已引入作为板级参考，但 **iOS 16 适配尚未开始**。
> GPU 当前是第二优先，排在 Wi-Fi 之后。没有用户明确许可，不开任何 GPU 枪。

## 1. 目标与边界

把 Adreno 725v1（a730 家族、gen7 GMU）从 XNU 8796 里点亮：
先 GMU/CM3，再 CP/HFI，最后才是 Metal。启动链是
ABL → 自写裸机 shim → XNU，**没有 Android，也没有厂商 GPU 驱动参与**。

FoXLinne 的上游实现证明这颗 GPU 在 iOS 18 上有 submit/retire 与桌面/锁屏运行证据，
但 Safari 黑屏/花屏、撕裂、模糊、壁纸、任务预览和稳定性仍未完成。
其 Apple ABI 与 iOS 16 不兼容，只能抽取相同硬件的板级与传输层，禁止整仓覆盖。

## 2. 已确证（全部真机回读）

- **GPU SMMU 是独立的一颗**：`kgsl-smmu @0x3da0000`，
  `IDR0=0x4c017e18`（24 stream）、`IDR1=0x40000016` ⇒ **CB0 = 0x3dc0000**。
- `S2CR=BYPASS` 会被改写成 FAULT；stage-1 + CB0 可写可回读；SMR 可写。
- GPU_CC 基址 `0x3d90000`：
  - `CX_GDSCR @0x9108`（bit31 PWR_ON / bit1 HW_CONTROL / bit0 SW_COLLAPSE）
  - `ahb @0x911c`、`cx_gmu @0x913c`、`hub_cx_int @0x9148`
  - `smmu vote @0x7000`、gmu RCG `0x9318/0x931c`、hub RCG `0x93ec/0x93f0`
- **GMU 寄存器是字偏移**：GMU base `0x3d68000`、ITCM `0x3d6d000`、
  DTCM `0x3d79000`、`CM3_SYSRESET 0x3d7e000`、
  `FW_INIT_RESULT 0x3d7e070`、`HFI_CTRL_INIT 0x3d7e618`。
- GMU 固件块格式 `{u32 addr; u32 size; 0xdeadbeef; 0xdeadbeef}`；
  `gmu_gen70000.bin` 可正确解析并写入 ITCM/DTCM，逐字节回读一致。
- **CM3 真的会执行**：放复位 200 ms 再拉回，XNU 早期启动必挂；
  瞬时放/拉（CM3 来不及执行）则正常 ⇒ 这 200 ms 里 CM3 在跑，并留下持久副作用。
- 厂商完整上电序列已抓到（kprobe，8.47–8.53 s）：
  `lcx.lvl=7`、`lmx.lvl=5`、`CX_GDSCR=0x228800`
  （EN_REST_WAIT=2 / EN_FEW_WAIT=2 / CLK_DIS_WAIT=8 / RETAIN_FF）
  → gmu RCG → cx_gmu → hub RCG → hub_cx_int → smmu vote →
  GCC `0x81010/0x81018` → `gpu_cc_ahb=1` → `mxc.lvl`。
  **全程没有 collapse→restore 周期。**

## 3. 当前卡点

1. **CX 域电源无法真正打通**。ABL 交接时 `PWR_ON=1`；做一次
   「清 HW_CONTROL → 置 SW_COLLAPSE → 再清」之后永远回不来（`0x228801`）。
   厂商 trace 里根本没有这一步，collapse 已从默认流程移除。
2. **RPMh ARC 投票「完成」但不生效**。`apps_rsc` drv-2 @`0x17a20000`、
   `tcs-offset=0xd00`、`CFG=0x800c8104`（TCS=8 / NCPT=16，非 solver）；
   ACTIVE TCS + AMC 投 `lcx/lmx/mxc` 后命令状态 `ISSUED|COMPLETE`、TCS 回 idle，
   但 CX 依旧回不来。命令 `msgid=0x10108`（LEN=8 | RESP_REQ | WRITE）已与
   `rpmh-rsc.c` 逐位核对，格式没问题，剩下的是资源语义/仲裁/因果关系。
3. **kgsl-smmu context bank 页不可达**。全局寄存器可读写，但 CB 页一碰整机挂死；
   BYPASS 被上层改写成 FAULT。没有 CB 就建不了页表。

## 4. 已撤回 / 不要重走

- 「一放复位机器就停」已撤回：`delay()` 会阻塞、G4 跳过 SPI mask、
  0228 数的是串扰。
- 「GP_SMPREL 牺牲核方案能让 XNU 启动」已证伪。
- 不要主动 collapse GPU——那样永远回不来。
- 不要把 CB 故障寄存器当负证据：`FSR=0x400` 清不掉，`FAR` 是陈旧值。

## 5. Reopen 门

- 一条真正的 spin heartbeat；
- 一条能活过 stall 的输出通道；
- 不依赖「读回一个看起来合理的值」作为电源已就绪的证据。

## 6. 还没开始的部分

RPMh 完整实现（solver/batch/ARC 语义）、互连/BCM 带宽投票、PMIC 轨道接管、
GPU PAS（ZAP）、GMU HFI 协议与 CP 固件、a725 的 HW-init 表、gpucc 时钟树完整化、
Metal 前端（metal2vulkan/Turnip）。

## 7. 证据索引

| 文档 | 内容 |
|---|---|
| [`smmu-dead-ends.md`](smmu-dead-ends.md) | apps-smmu 寄存器地图与已证伪路线 |
| [`wifi-foxlinne-port.md`](wifi-foxlinne-port.md) | FoXLinne 参考来源与 iOS 16 适配边界 |
| [`STATUS.md`](STATUS.md) | 当前优先级与下一道门 |
| [`chronology.md`](chronology.md) | GPU 从 10-27 到 11-28 的时间线 |
