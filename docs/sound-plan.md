# marble / XNU 音频可行性评估（2026-10-23）

## 硬件（来自 live FDT）

- SoC：SM7475（骁龙 7+ Gen 2），音频子系统在 **LPASS**（Low Power Audio Subsystem）
- Codec：**WCD938x**（SoundWire 从设备，带独立的复位 GPIO）
  - 相关节点：`wcd938x-codec`、`tx-slave`/`rx-slave`、`qcom,wcd-rst-gpio-node`
  - 供电：cdc-vdd-rxtx / vddio / vdd-buck / vdd-mic-bias（PMIC 供电轨）
- LPASS CDC 宏：
  - `va-macro@33F0000`（qcom,lpass-cdc-va-macro）
  - `tx-macro@3220000`（qcom,lpass-cdc-tx-macro）
  - `rx-macro@3200000`（qcom,lpass-cdc-rx-macro）
- SoundWire master：`swr0..swr3`（LPASS 内部）
- 音频接口：Primary/Secondary/Tertiary MI2S、TDM、AUXPCM
- 功放：WSA codec（`asoc-codec-names` 里的 wsa-codec1/2；本机 `qcom,wsa-max-devs = 0`，可能未贴）
- ADSP：`remoteproc-adsp@03000000`，Q6 audio + QRTR（Android 用它做音频路由/算法）
- 顶层 `sound` 节点：compatible `qcom,waipio-asoc-snd`

## iOS 侧的现实

iOS 的音频栈是闭源的：
- 内核：AppleHDA / AppleEmbeddedAudio / IOAudioFamily（iOS 版）
- 用户态：mediaserverd / coreaudiod / HAL 插件，通过 private 的 AppleHDA user client 访问硬件
- 单纯的"能出声"≠"iOS 能出声"：必须让 mediaserverd 找到并打开一个它认识的音频设备

## 三条路线

### A. 直接编程 LPASS + WCD938x（不经过 ADSP）
- 需要：GCC 音频时钟/GDSC 上电、LPASS CDC 宏配置、SoundWire master 枚举 WCD938x、codec 初始化序列（数千条寄存器写）、MI2S/音频 DMA 播放通路
- 可移植 Linux 的 `sound/soc/codecs/wcd938x*.c`、`sound/soc/qcom/lpass-cdc*.c`、`lpass-*.c`
- 优点：不依赖 Q6 固件/QRTR；能最快做出"beep"
- 难点：WCD938x 的 SoundWire 枚举与初始化序列非常长；没有 datasheet，只能照 Linux 驱动移植
- 工作量：2–4 周（有 Linux 参考的前提下）

### B. 移植 ADSP remoteproc + QMI/QRTR 音频路由
- 需要加载 ADSP 固件（adsp.mdt，需从原厂固件提取）、QRTR/QMI、audio packet 服务、apr 路由
- 优点：最接近 Android 的真实路径，后续可支持通话/录音/算法
- 难点：remoteproc、共享内存、mailbox、SMP2P、QMI 全链路；XNU 里都要重写
- 工作量：1–3 个月

### C. 用户态 HAL 旁路
- 在 iOS 用户态放一个自定义 HAL/守护进程，直接 mmap LPASS DMA + 控制 codec（内核只提供 IOMMU/寄存器映射）
- 优点：绕开 AppleHDA 的私有 ABI；音频算法可放用户态
- 难点：需要绕过 mediaserverd 的音频会话管理；系统音效/媒体播放仍走 AppleHDA，除非替换 HAL
- **2026-11-02 订正（实测，别再按 AppleHDA 规划）**：我们启动的 `work/iphone-x-recon/artifacts/kernel.bin`
  里 `AppleHDA` 字符串计数为 **0**，而 `IOAudio2Device`(1)、`IOAudio2DeviceUserClient`(1)、
  `AppleEmbeddedAudioDevice`(1)、`AppleARMIISAudioDevice`(8)、`AppleAOPAudioController`(1)、
  `AppleSPUHapticsAudioDriver`(2) 都在；rootfs 的 HAL 驱动
  `/System/Library/Audio/Plug-Ins/HAL/AppleAOPAudioPlugin.driver` 的
  `AudioServerPlugIn_LoadingConditions.IOProviderClass = AppleAOPAudioController`。
  ⇒ 桥的对象是 **IOAudio2 + AOP 类**。另：mediaserverd 在本移植里**能起来**
  （`logs/ring-h2-dst-tdm.txt:778`），当前音频 ring 里它**没有** CRASH 行，缺的是它下面没有设备。
- 工作量：与 A 相当，且更"脏"

## 建议

1. 先做 **A 的最小闭环**：GCC 上电 → LPASS CDC + SoundWire → WCD938x probe → MI2S 播放一段 PCM（内核态自测，不接 iOS 音频栈）
2. 再做 **AppleHDA 兼容层**：把 PCM 通路包装成 mediaserverd 能打开的 AppleHDA user client（关键是 ABI，不是硬件）
3. 最后考虑 B（通话/录音/算法需要 ADSP）

## 今晚的结论

- 没有任何现成代码可复用：`refs/ios-sweet`、`refs/ios-topaz` 都没有音频驱动
- 完整"iOS 能出声"不是一晚能完成的；今晚只做硬件/路径调研与文档
- 下一步可先加 ADT 的 LPASS/WCD 节点和一个空的 SweetAudio IOKit 服务骨架，为 A 路线铺路

## 参考

- Linux: `sound/soc/codecs/wcd938x.c`, `wcd938x-sdw.c`, `sound/soc/qcom/lpass-cdc*`, `lpass-*`
- 硬件节点：`logs/fdt/marble-live.dts`（audio_cma_region / va-macro / tx-macro / rx-macro / wcd938x-codec / sound）
