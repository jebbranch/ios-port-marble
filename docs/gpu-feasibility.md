# marble GPU（Adreno725v1）：能不能用 UniXNU 的 NativeAdreno 修？

## 一、从设备上查到的事实

```
/sys/class/kgsl/kgsl-3d0/gpu_model   = Adreno725v1
DT  /soc/qcom,kgsl-3d0@3d00000
      compatible        = qcom,adreno-gpu-gen7-4-0  qcom,kgsl-3d0
      reg               = 0x3d00000 0x40000 | 0x3d61000 0x800 |
                          0x3d50000 0x10000 | 0x3d8b000 0x2000
      qcom,gpu-model    = Adreno725v1
DT  /soc/kgsl-smmu@3da0000
      compatible        = qcom,qsmmu-v500  qcom,adreno-smmu
      reg               = 0x3da0000 0x40000 | 0x3de6000 0x20
      gfx_0_tbu@3de9000 / gfx_1_tbu@3ded000
DT  /soc/qcom,kgsl-iommu@3da0000
      gfx3d_user   iommus = <&kgsl_smmu 0x0 0x400>
      gfx3d_lpac   iommus = <&kgsl_smmu 0x1 0x400>
      gfx3d_secure iommus = <&kgsl_smmu 0x2 0x400>
DT  /soc/adreno-gmu@3d68000
```

Android 开机日志（`dmesg`，真机）：

```
arm-smmu 3da0000.kgsl-smmu: coherent table walk
arm-smmu 3da0000.kgsl-smmu: stream matching with 24 register groups
kgsl-iommu ... gfx3d_user / gfx3d_lpac / gfx3d_secure -> iommu groups 4/5/6
adreno-gen7-gmu 3d68000.qcom,gmu -> iommu group 7
kgsl-3d: firmware a730_sqe.fw, a730_zap.mdt, gmu_gen70000.bin
```

## 二、★ 关键结论：GPU 的 SMMU 是独立的一颗，不在 apps-smmu 上

音频卡死是在 **apps-smmu（0x15000000，SID 0x1801）** 上，被 hypervisor 管着。
GPU 用的是 **`kgsl-smmu@3da0000`**——Qualcomm 给 GPU 单独做的一颗 SMMU-500，
只有 **24 个 stream 组**，由 AP 的 kgsl 驱动全权编程（`coherent table walk`）。

**音频那道墙，理论上管不到 GPU。** 这是这个方向最值钱的一点，也是它和音频
本质不同的地方。

## 三、目标仓库是什么

| 仓库 | 规模 | 作用 |
|---|---|---|
| **UniXNU/NativeAdreno** | ~7.2k 行 | XNU 内核 GPU 驱动：GMU、CP 初始化、命令环、SMMU 上下文、固件加载、上电、IRQ |
| **UniXNU/NativeMetal** | ~7.4k 行 | Metal 前端：metal2vulkan + Turnip + IOKit GPU provider + scanout |

两个仓库都**明确只覆盖 A740 / SM8550**，而且**没有独立的构建文件**——
它们是在 UniXNU 的构建树里由 `build-arm-driver.py Adreno740` 构建的，
平台参数（固件、电源策略、内存资源）来自 UniXNU 的 SoC/设备仓库。

## 四、好消息与坏消息

**好消息**

1. GPU 的 SMMU 独立，不吃音频那道 hypervisor 限制。
2. `compatible = qcom,adreno-gpu-gen7-4-0`——A740 也是 gen7，寄存器表大概率通用
   （驱动里的 `NativeAdrenoA740HwInit.h` 就是从 a6xx.xml / a6xx_catalog.c 生成的，
   a730 的常量本来就在里面被引用）。
3. **固件就在这台机器上**：`/vendor/firmware/a730_sqe.fw`、`a730_zap.mdt`、
   `gmu_gen70000.bin`。不需要从别处找。
4. Android 内核把 GPU 当 **a730 家族**驱动（虽然型号写 Adreno725v1），
   说明 a730 的固件/寄存器就是它实际用的那套。

**坏消息**

1. 这是**两周到两个月量级的移植**，不是改几行：要建 marble 的设备仓库/SoC profile，
   生成 gen7-4-0 的 HW-init 表，适配 GMU gen70000 的配置，把固件嵌进内核，
   还要接 gpucc 时钟和电源。
2. 上面的 Metal 栈（Turnip + metal2vulkan + iOS 的 IOKit provider）是另一整层。
3. 我们目前**连 GPU 的 MMIO 都还没从 XNU 摸过**——第一步应该是先证明寄存器可达、
   SMMU 可配，再谈移植。

## 五、建议的第一步（一天内可完成、不碰大工程）

写一个最小探针（和现有 `sweet_*` 驱动同一套路），在 XNU 里：

1. 映射 `0x3d00000`（kgsl）与 `0x3da0000`（kgsl-smmu），读标识寄存器
   （`RBBM_IDENTIFICATION`、SMMU `ID0/ID1`），确认 **GPU 硬件块在 XNU 下可达**；
2. 读 kgsl-smmu 的 `SMR/S2CR`，看原厂给 gfx3d_user(SID 0) 配了什么；
3. 试写一条 `S2CR = BYPASS`（0x10000），看**会不会像 apps-smmu 那样被改写**。

第 3 条是关键：如果 kgsl-smmu 的 BYPASS 能写进去，GPU 这条路就成立；
如果也被改写，那这台机器上"AP 想用自己的 SMMU"这条路整体被关死了。
