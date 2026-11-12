# 音频重开：HAL PCM 捕获阶段（2026-11-10）

> **2026-11-12 续接**：完整 0292 内核已取回并验收，8 核硬件隔离捕获镜像已打包；
> 用户授权重取的当前 HFS 卷头/catalog/原文件只读检查通过。**未安装、未 boot，
> 仍 68/120**。安装器已改为读取设备当前卷头，回滚只恢复目标 8 B size，避免覆盖
> 邻居目录记录。具体身份、设备只读计划、首轮判据见
> [`audio-hal-ready-2026-11-12.md`](audio-hal-ready-2026-11-12.md)。

## 授权、范围与预算

用户明确要求开始音频工作，并要求先完成离线计算；需要设备测试时，必须先把
完整内核和启动镜像打包好，再通知用户连接设备。

预算保持 **68/120**，本页写作时没有新开机、没有设备读写。GPU、Wi-Fi 和基带
没有因音频重开而解除原有门禁。

## 硬件安全红线

用户明确提醒：**不能乱调增益，会烧扬声器和听筒。**

因此：

1. 0292 第一阶段仅接收、缓存、统计 HAL PCM，不发送 AudioReach buffer。
2. 不开启 TDM，不操作 I2C，不启动功放，不操作听筒。
3. volume ioctl 只记录；衰减量夹在 0..720 个 1/8 dB，永远不转换成硬件写入。
4. 后续硬件阶段禁止任何正增益；第一轮 PCM 峰值不得高于既有
   `sweetvol=16` 的 16384（约 -6.02 dBFS）。
5. 扬声器与听筒必须使用各自的拓扑、profile 和安全门，不得共用或互套。
6. 任何 ACF/profile 都先用本机资产离线解析，不能把 mayfly 的 AW88261 表写进 marble。

## 阶段 1 的唯一问题

是否能让 iOS 16.7.16 的 `mediaserverd` 通过 AudioServerPlugIn 持续交付真实
48 kHz、16-bit signed、stereo PCM：

```text
iOS app / system sound
  -> mediaserverd
  -> MarbleAudio.driver DoIOOperation(WriteMix)
  -> /dev/marble-audio
  -> capture-only ring + counters
```

这一步不以听感、DSP DONE、功放寄存器或 EOS 为判据。

## 离线实现

### 用户态

`userland/audio/` 来自 HakiOS/ios-topaz 的 MIT AudioServerPlugIn，改为：

- bundle id `com.marble.audio`，factory `MarbleAudio_Create`；
- `Speaker`、`Hawking`、`Actuator`、`Codec` 四个 UID 保持 iPhone X 路由所需形状；
- Speaker 写 `/dev/marble-audio`；
- sink 最多重试打开 8 次；
- NaN 音量强制为静音；
- 部分 write 也计为失败，不把短写当成功。

Xcode 15 / iPhoneOS 17 SDK 以 deployment target iOS 16.0 构建，产物是 arm64
`MH_BUNDLE`，ad-hoc 签名。主机 contract test 覆盖 factory、设备列表、PCM 写入、
时间戳和 NaN 静音。

### 内核

`0292-sweet-hal-pcm-capture.patch` 新增 `sweet_audio_bridge.c`：

- `sweethalbridge=1` 才创建 `/dev/marble-audio`；
- 65536 frame（约 1.37 s）S16 stereo ring；
- 写入永不等待 DSP；满时覆盖最旧帧并明确计数；
- 统计 open/close/write/frame/non-silent/peak/overwritten/queued；
- 首 8 次及每 256 次写入直写 SWEETLOG，确保 userland 后期事件可见；
- 提供尚无人调用的 consume API，缺帧只补零，不做增益；
- volume ioctl 只记录并夹紧，不连接硬件。

节点在 UFS/root 初始化后、ADSP 后台线程启动前发布，所以 HAL 捕获不依赖 ADSP
固件是否成功启动。

## HFS 安装策略

为避免创建新 HFS 目录或分配新 extent，测试时复用现有：

`/System/Library/Audio/Plug-Ins/HAL/AppleAOPAudioPlugin.driver`

- 现有 executable：264256 B、连续 65 个 4 KiB 块；
- MarbleAudio：约 85 KiB，填零放入同一 allocation，catalog 大小不变；
- Info.plist：复用现有 1 个块，仅修改该 catalog record 的 logical size；
- 不改目录、extent、GPT 或其他分区；
- 安装脚本先逐块备份，支持逐块恢复，并在写后逐字节读回。

`work/iphone-x-recon/hfs/install_marble_audio.py --offline` 已对本地 HFS catalog
生成块级计划；实际写入必须在目标 serial 的临时 recovery/TWRP 状态显式使用
`--write`，Android 正常运行状态会拒绝写入。

## 离线门

- HAL production bundle 严格编译通过；
- HAL host contract + ASan/UBSan 通过；
- bridge 真实函数抽取测试 + ASan/UBSan 通过；
- 三文件 patch 在纯基线 `git apply --check`、apply、逐字节 post-image 对照通过；
- workflow 必须检查完整唯一 marker：
  `SWEET MARK: halbridge v1 node=/dev/marble-audio ring=65536 capture-only`；
- 完整 XNU CI、kernel digest/CNTP、最终镜像 cmdline 和 HFS live plan 尚待完成。

## 首轮设备判据（完整构建后、开机前另行冻结）

首轮只允许：

- 新内核 + `sweethalbridge=1`；
- 不带 `sweetpcm`、`sweetvgraph*`、`sweeteos`、功放听测或 GPU 参数；
- 保留日志，触发一个明确的系统短音/应用短音；
- 只判断 HAL marker、node open、PCM write、frame rate、sound frame 和 peak；
- 无任何 DSP/TDM/功放/听筒动作；
- 完成后正常复位并恢复 HAL bundle 原文件，再决定阶段 2。

