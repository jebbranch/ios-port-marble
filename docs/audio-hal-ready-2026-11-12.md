# 0292 HAL 捕获镜像：离线验收与设备只读预检（2026-11-12）

## 当前状态

- 内核提交 `a9ae8d66736fb86c0a02b928dc4f81fcc5db9735` 已在远端 main。
- 11 月 10 日发布的完整内核已续传取回，摘要与对应资产配对相等。
- 启动镜像已打包并通过逐段、参数、符号和摘要检查。
- 用户 11 月 12 日明确允许重取 HFS 卷头；只读取了研究机 `2aa26095`。
- **未安装 HAL，未 boot 新镜像，未写设备分区。预算仍 68/120。**
- 当前设备在 Android，不满足安装器的 recovery + userdata 未挂载写入门。
- 临时覆盖现有 iOS HAL 文件仍需用户单独明确授权；连接设备本身不是写入授权。

## 固定产物身份

| 产物 | 路径 / 摘要 |
| --- | --- |
| 启动镜像 | `images/hal-capture-0292-20261112.img`，51286016 B |
| 镜像 SHA256 | `13bb9272e9905f16ef58fc695432a324e2cb93228821ae799e35fa155515d533` |
| release raw SHA256 | `f4c600f176cdd1265a284cef6fcee8e758ebae90accd0291be5e342ae5eaf93f` |
| CNTP NOP 后 kernel SHA256 | `df802b82580e8747aabbb569650d772cb67d5aeb33a86f82b4aeb5276a5c241f` |
| CNTP 唯一位点 | `0x4aa348`；仅该 4 B 从 `28e21bd5` 改为 `1f2003d5` |
| HAL executable | `userland/audio/out/MarbleAudio.driver/MarbleAudio`，87264 B |
| HAL SHA256 | `0afe36f9043c4fbcace57375e2582634ee0a1955ab45065a403ecfdd9982bda0` |
| 本轮 identity | `sweettime=1794493381` |

`out/hal-capture-0292-20261112/manifest.json` 是本轮构建清单；
`verify/halcapture-0292-20261112/` 保存可提交的摘要、主机验收和只读计划。
专有原文件、128 MiB live catalog 和完整镜像只在本地 out 中，不提交到公开仓库。
没有覆盖默认 `out/kernel.adsp` 或旧静默试玩镜像。

## 实际配置与硬件隔离

镜像是 **8 核 KEEPALIVE 日志捕获配置**，不是无日志日用镜像。cmdline 448 B，
没有重复 key，没有 `sweetreboot`、`sweetpcm`、图/EOS/功放/GPU 实验参数。

关键值：

```text
cpus=8 sweethalbridge=1 sweetlog=1 ufslogsecs=1
sweetadsp=0 sweetglink=0 sweeti2c=0 sweetamp=0 sweetgpu=0 sweetbatt=0
```

仅“不带 sweetpcm”不够：现有 ADSP 路径在图/PCM 测试之前会调用
`sweet_i2c_probe_amps()`，GLINK 后段还会再次调用该函数。该函数的正常 GENI 分支
可以写 ACF 表。因此这轮直接关掉 ADSP，并额外关闭 GLINK/I2C/amp。
新节点由 UFS 初始化独立发布，不依赖 ADSP。

shim 的 GPU_PROBE_LEVEL=0，构建环境清除了继承的 GP_* / PCM_* 实验变量。
HAL 的音量 ioctl 只记录衰减和静音状态，不接硬件；不操作 ACF、TDM、功放或听筒。
`/dev/marble-haptic`、`/dev/marble-mic` 没有本项目实现，不把四个 HAL UID 当成
马达/麦克风已经接通。

代价：本轮不刷新 ADSP 电池，不经过 GLINK 的 sweettime 设置路径，因此电池 UI 和
系统日历时间不能用于验收。HAL 时钟使用 mach_absolute_time，时间戳主机检查通过；
是否在 iOS 上持续正确运行仍待实机。`ufsrw=1 rootrw=1` 和 SWEETLOG 是既有运行时写入，
与“不 flash 启动分区”要区分。

## 已执行的门

- 0292 三个实际文件的 apply --check、apply、post-image 对照通过。
- HAL contract 和真实内核 ring/ioctl 函数 ASan/UBSan 检查通过。
- iOS arm64 MH_BUNDLE 严格编译、导出符号、plist 和 ad-hoc 签名检查通过。
- release raw 配对摘要相等；77 个 workflow marker 在完整内核中均存在。
- init/consume/open/write/ioctl/ring 六个桥接符号均被链接。
- 内嵌所有 kernel segment 与 CNTP 修补后的内核逐字节相等。
- ADT 8 个 MPIDR 为 `0..0x700`、步长 `0x100`，boot v4 包体与 payload 对照通过。
- 18 项隔离与安装保护测试通过，包含失败对照。
- `tools/build_hal_capture.py --verify-only` 对冻结镜像再次通过。

不把发布资产的存在等同于 Actions 每一步 green；上述是对实际产物的验收。
也不把这些检查当作 iOS HAL 已加载、PCM 已收到或扬声器有声。

## 设备当前 HFS 只读证据

设备 serial、Android marble 身份和只读 root 通道已核对。userdata：

```text
sda33 start=24858624 sectors
sda33 size=470183896 sectors
```

本次从设备当前卷头发现 catalog fork，再预取其 extent，不依赖已缺失的本地
`hfs/iosroot16.img`。计划为：

| 目标 | HFS volume 4 KiB block | sda33 relative 4 KiB block |
| --- | ---: | ---: |
| AppleAOPAudioPlugin executable | 455890，连续 65 块 | 53985490 |
| Info.plist | 455956，1 块 | 53985556 |
| 对应 catalog size field | 194420，块内 offset=1862 | 53724020 |

现有 executable logical size=264256 B；Info.plist logical size=1172 B，新安装
plist=479 B。现有 executable/info 原始块与 11 月 10 日保留的原始块 **cmp 相等**。
尚未覆盖；这些位置也不能跳过下一次 recovery 下的重新读取。

只读文件位于：
`out/hal-capture-0292-20261112/hfs-live-readonly/`。
其中 `live-readonly-plan.json` 明确不是安装备份，不能直接拿它执行恢复。

## 安装器的改进与使用边界

权威脚本为 `tools/install_marble_audio.py`，旧 hfs 路径保留相同兼容副本。

- 固定 serial，拒绝其他设备。
- 写入只允许 recovery，且 userdata 未挂载；Android 中只允许 root 只读。
- 使用当前设备卷头、catalog 和几何，不必重新拉 2.68 GiB 前缀。
- size field 在 8 KiB B-tree node 的后半时，按真实 field offset 定位 4 KiB 块。
- 写前备份及 manifest 绑定目标块和摘要；短读、短写/读回不符均停止。
- 恢复验证备份摘要、目标位置和当前内容；只在新读 catalog block 中恢复目标
  8 B size，不把旧整块覆盖回来，保留邻居记录的后续变化。
- restore 不依赖新 HAL bundle 是否还在本地。

安装和恢复都必须使用同一个专属 `--outdir`。不得在 Android 上改脚本绕过写入门。
进入 recovery、写入 HAL、一次 XNU 引导和触发短音均须在授权后按下节执行。

## 首轮事前判据（第 69 枪候选，尚未执行）

唯一问题：mediaserverd 是否将真实非静音 S16 stereo PCM 交给 capture ring。
不以听感、功放读回、DSP DONE 或 EOS 判定。

1. recovery 状态、userdata 未挂载、serial 正确；当前几何/目标文件再次核实；
   原始块和安装 manifest 保存完成、写后逐字节相等，才可引导。
2. 启动前再次验证固定镜像 SHA；只能 fastboot boot，一轮最多一次 XNU 引导。
3. 日志必须先匹配 `sweettime=1794493381`；raw SWEETLOG 头和解码有效窗口相符。
   若被环形覆盖而失去身份，不从旧尾部补身份。
4. 当前窗口中必须有完整 halbridge marker 和 `SWEET ADSP: disabled`。
   一旦出现 Q6 released、amp probe/guard/write、graph_open、data_buffer、
   pcm tone、GPU/GMU 实验路径，则硬件隔离门失败，不继续。
5. 明确记录用户触发的一个系统/应用短音动作；捕获期间不应听到声音，这是设计结果。
6. 至少一次 node open；至少两个递增的 PCM counters，最大 frames>=4096，
   sound_frames>0、8<peak<=32768，queued<=65536；代表已观察到非静音 PCM。
   overwritten 增长在无消费者的 stage 1 中是预期，不判 DSP 失败。
7. 只有 open 或只有静音不能判通过；日志缺证人或用户没触发成功时为“不确定”，
   不直接判 HAL 没出流。现有写日志只有前 8 次和每 256 次，不能拿后期无打印
   判停流；也不能从累计帧数认定实时 48 kHz 或长期连续稳定。
8. KEEPALIVE 无自动 PSCI reset，因此旧的“done+PSCI”收尾门不适用。由用户正常退出/
   长按电源回 recovery，拉取并核身份后恢复 HAL；恢复逐块读回和 catalog size 完成
   才结束。若 panic，停手，不追加第二枪。
9. 本轮不得修改任何数字/模拟增益；不新增硬件音频、听筒或其他停车线实验。

状态仍是 **awaiting explicit install/test authorization**，不是“已通过第 69 枪”。
