# iOS 侧出声的桥：从「内核合成的音」到「iOS 应用出声」的差距清单

日期 2026-11-02。写这一篇的动机不是内核那条路已经赢了——**独立于 `done status=0` 的出声正证据至今没有**
（① 的 J3 未命中，② 落在 I2，③ 因为拿不到厂商参考值而按目标规则收掉，见
`docs/audio-closeout.md` 最后四节）。是另一件事：内核侧再往下走，卡点已经从"寄存器/参数"
挪到了"iOS 用户态到这条 GPR 路之间**没有任何桥**"，而那条桥是设计问题，不是开机问题。

本文只写**我自己在仓库里复核过的**东西。子智能体（`外设与HAL适配专家`、`Explore`）提供的
线索凡是被我采用都在这里标了"我核过"；我没能复现的（见 §6 末）也照写。

---

## 1. 一句话结论

iOS 的音频服务在我们这颗机器上**是活的**，它缺的是**下面没有任何一个它认识的音频设备**；
而 Apple 认识的那个设备所依赖的硬件（AOP + MCA + CS35L26）**marble 物理上没有**，所以桥只能
是"我们造一个 Apple 认识的 IOAudio2 设备，把它的 PCM 灌进已经打通的 AudioReach 那条路"，
不是"把 DT 补全让 Apple 的驱动自己跑起来"。

## 2. 已复核的事实（每条都给出处）

| # | 事实 | 出处（我核过） |
|---|---|---|
| F1 | 音频服务是 **mediaserverd**：rootfs 有 `/System/Library/LaunchDaemons/com.apple.mediaserverd.plist`(4716B) 与 `/usr/sbin/mediaserverd`(188416B) | `work/iphone-x-recon/hfs/iosroot16.img` 目录树（经 `hfs/hfsread.py` 遍历 336,732 路径；catalog 全本地） |
| F2 | 它在我们自己的移植上**确实起来了**：`exec mediaserverd` | `work/xnu-gh/logs/ring-h2-dst-tdm.txt:778`；当前音频 ring 也有（如 `ring-nobb.txt:790`） |
| F3 | 它在当前音频 ring 里**没有崩溃**：只有签名页被放行 | `ring-nobb.txt:1064` `CODE SIGNING: cs_invalid_page(0x1cb834000): p=28[mediaserverd] … allowing (remove VALID) page` |
| F4 | 历史上 mediaserverd 崩溃**只存在于 GPU 探测期那 24 份 ring**，不是普遍状态 | `grep -l "CRASH mediaserverd" logs/ring-*.txt` → 24 份，全名如 `ring-gpu-m*-…`、`ring-amp8`、`ring-daily-v93-freeze` |
| F5 | 我们启动的内核里有全套 IOAudio2/AOP 类，**`AppleHDA` 字符串为 0** | `strings work/iphone-x-recon/artifacts/kernel.bin` → `IOAudio2Device` 1 / `IOAudio2DeviceUserClient` 1 / `AppleEmbeddedAudioDevice` 1 / `AppleARMIISAudioDevice` 8 / `AppleAOPAudioController` 1 / `AppleSPUHapticsAudioDriver` 2 |
| F6 | Apple 的 iOS 侧 HAL 插件认的是 AOP 控制器 | `/System/Library/Audio/Plug-Ins/HAL/AppleAOPAudioPlugin.driver/Info.plist`：`AudioServerPlugIn_LoadingConditions.IOProviderClass = AppleAOPAudioController`，`IOKitUserClients = {AppleAOPAudioUserClient, AppleAOPVoiceTriggerUserClient}`，`MinimumOSVersion 16.7` |
| F7 | **我们启动的那份 ADT 要求的硬件 marble 没有** | `python3 scripts/adt.py dump …/d22ap.adt` 的行：`1434 audio-codec`、`1597 audio-speaker-top` 且 `1607 compatible = 'audio-control,cs35l26'`、`871 aop-mca { device_type = 'mca' }`、`864-868 spkamp-config_arc/_arc_p1/_btmspk`、`771 aop { role = 'AOP' }` + `790 firmware-name = 'iphone10baop'` |
| F8 | marble 真实播放拓扑：PA 挂在 **TERTIARY** 那条 TDM 上 | `logs/android-proc-asound-pcm.txt`：`00-31 TDM-LPAIF-RX-TERTIARY multicodec-31 playback`；主后端 `00-00 CODEC_DMA-LPAIF_RXTX-RX-0`；组件名 `aw882xx_smartpa.2-0034` / `…0035` / `wcd938x-codec` / `pm8350b-swr-haptics.1f0170220` |
| F9 | 我们已经有"发布一个 IOKit 服务 + user client 并把 client 的每一次调用打出来"的可用样板 | `work/ci-src/xnu-xnu-8796.141.3/iokit/Kernel/arm/hid/SweetHaptics.cpp:47 class SweetHapticsUCBase : public IOUserClient`、`:102 externalMethod(uint32_t selector, …)`、`:17` 注释写明"log IOServiceOpen, every externalMethod selector" |
| F10 | 内核侧那条路本身已通到底层一步：DSP 真收我们的 PCM | `docs/START-HERE.md` §三 与 `docs/audio-closeout.md`：`sweets1ap=3` → `0x7c1` → `DATA_BUFFER done status=0`，1200 buffer 正常复位 |

## 3. 两侧对照：为什么"补 DT 就行"不成立

| 层 | iOS 期望（F6/F7） | marble 实际（F8） |
|---|---|---|
| 协处理器 | AOP（`iphone10baop` 固件） | ADSP（我们用自己的 PAS 装载，跑厂商 SPF/AudioReach） |
| 数字音频总线 | MCA（`aop-mca`） | LPASS TDM：`LPAIF_RXTX`（主）与 **`TDM-LPAIF-RX-TERTIARY`（PA 进料口）** |
| 功放 | `audio-control,cs35l26`，由 `spkamp-config_*` 描述 | **AW882xx**，I2C 在 GENI SE15 上、地址 `0x34/0x35` |
| codec | Apple 自己的 `audio-codec` | **WCD938x**（SoundWire 从设备）+ `lpass-cdc` rx/tx/wsa/va macro |

地址也对不上：ADT 里那些节点描述的是 Apple 硅片的寄存器空间；marble 连 `/dev/mem` 在 Android 侧都被禁、
我们自己的 LPASS 读数只能走我们建的窗口。**所以桥的对象只能是"类与 ABI"，不是"硬件地址"。**

## 4. 候选路线排序（每条都写清前提与最便宜的判别实验）

**R1（推荐）我们自己发布一个 IOAudio2 形态的设备 + user client，后端把 PCM 灌进已经通的 SHM/GPR 环。**
- 成立的前提：`mediaserverd` 会在出现一个匹配的 IOAudio2 服务时来 `IOServiceOpen`。这条**现在未知**。
- 最便宜的判别实验（一次开机，零 ABI 工作量）：照 F9 的样板先发一个**空壳**设备
  （类名 `IOAudio2Device` 形态、名字 `"Speaker"`）+ 一个 `IOUserClient` 子类，
  把所有 `externalMethod` 的 selector 与标量参数原样打进 ring。
  开机只看两行：**有没有 `IOServiceOpen`**，以及**第一条 selector 是几号**。
  - 有 open ⇒ Apple 的服务确实会来找设备，ABI 未知可以靠"记录它怎么调"来逆向，桥值得做。
  - 没有 open ⇒ mediaserverd 在更早的地方就退出了（HAL bundle 没加载 / audio-mafd 缺失 /
    它在等 AOP 而不是等 IOAudio2），那 R1 就不是第一步，要先解决"它为什么不找设备"。
- 代价估计：空壳 1 个补丁 + 1 次开机；ABI 复原看第 2、3 次；真正的 PCM 后端是最贵的一段。

**R2 换 HAL bundle（`/System/Library/Audio/Plug-Ins/HAL/` 里放一个我们自己的 `.driver`）。**
- 前提：mediaserverd 会加载一个**非签名**的第三方 HAL。我们带 `cs_enforcement_disable=1`，
  所以*可能*；F6 显示官方那个插件的加载条件是 `IOProviderClass=AppleAOPAudioController`，
  也就是说 HAL 插件仍需一个内核侧 IOService 作伴——**R2 并不省掉 R1 的内核工作**，只是把
  设备语义从 IOAudio2 挪进 HAL。
- 判别实验：零开机成本——先用 `hfsread.py` 把 AppleAOPAudioPlugin 的
  `AudioServerPlugIn*` 键全列出来，看有没有 `IOKitUserClients` 之外的强制依赖（比如
  `AudioServerPlugIn_IconName/BundleVersion` 校验）。列完再决定值不值得试。

**R3 内核 ioctl/sysctl 灌 PCM（绕开 iOS 音频栈）。**
- 明确定位：**这是仪器，不是桥**。没有任何 iOS 代码会调它。它唯一的作用是继续做
  "内核侧到底通到哪一层"的实验（①②③ 就是这个形状），不要把它写进"iOS 能出声"的路径里。

**R4 补 DT 让 Apple 自己的驱动跑起来 —— 由证据判死。**
- 判死依据就是 F7+F8：ADT 要 AOP+MCA+CS35L26，marble 有 ADSP+TDM+AW882xx+WCD938x。
  补节点等于把 Apple 驱动绑到不存在的寄存器上。

## 5. 差距清单（可数，按依赖顺序）

1. **R1 空壳实验**：1 补丁 + 1 开机。产出"mediaserverd 到底会不会来 open"这个二元事实。
2. **IOAudio2 私有 ABI 复原**：只在 (1) 有 open 之后做。产出 selector→语义的表（不许猜，
   全部来自我们记录到的真实调用与 `dyld/mediaserverd` 反汇编的交叉印证）。
3. **采样格式协商**：iOS 侧要什么 rate/format（`AppleARMIISAudioDevice` 一路是 I2S 语义），
   我们后端要的是 48 kHz stereo S16 的 SHM 环；差一个重采样/格式转换的归属决定（AP 做还是 DSP 做）。
4. **路由与音量语义**：F6 的 HAL 与 `Tunings/{D22,D221}/` 那套 `.aupreset/.plist` tuning 图
   是 D22 的，不是 marble 的——音量/路由参数最终会指向不存在的设备 id，这一步决定要不要 stub。
5. **功放后端**：桥建好之后仍然要把 PCM 变成真实声音，那还是要回到本文没解决的 ②/③：
   **设备图/后端绑定**（`0x4165 param=0x080010C4 data={0x428f}` 在 ② 里被拒，见 I2）。
   也就是说：**R1 修的是"iOS 有没有出口"，②修的是"出口后面有没有喇叭"，两件事都得有人做。**

## 6. 我现在还不知道的事（不许下游把这些当已解决）

- mediaserverd 在没有音频设备时是**空等**还是**主动报错**：没有证据，F2/F3 只证明它活着。
- IOAudio2 的 user-client selector 表：一条都没记录过。
- `AppleAOPAudioPlugin.driver` 里 `AudioServerPlugIn_*` 键的**完整**清单：我只核过 F6 列的三项，
  没核完（这一条是子智能体报的，我复核了它引用的键名，但没做全量列表）。
- 子智能体引用 `d22ap.adt` 里 `audio-speaker-top` 的 `reg=<0x9c400000040 0>`、
  `i2c2 reg=<0x2e208000>`、`mca0 reg=<0x2ea00000>`：**这些具体数值我没复现**——
  我的 `adt.py dump` 输出里 `audio-speaker-top`/`aop-mca` 节点存在（F7），
  但 `reg` 字段没有以它给的形式出现在我 grep 的窗口里。结论（route R4 判死）不依赖这些数，
  但在有人拿它们当依据之前要重新核一遍。
- **内核侧"到底有没有真出声"仍未判**：I4（`sweetnobb=1 + sweetvcfg2=1`，PA 三时刻读数差）
  正在跑（任务 `bashgmbkd`）。它落"全部相同"这一格，本文第 5 节就是下一步；落"有差值"，
  就拿到本目标要的那条独立证据，再谈桥的优先级。

## 7. 震动为什么和这一篇是同一条线（也为什么它不会被内核侧顺手修好）

- **共享的一半**：iOS 的触觉走音频栈（`CoreHaptics` → haptic-audio → mediaserverd 这条路），
  所以本文 §5 的 (1)(2) 两步同时是震动的拦路；这与
  `docs/haptics-handoff-2026-10-30.md:790` 记的 `CHHapticEngine → AppleSPUHapticsAudioDriver → AOP/DSP`
  一致，且 `AppleSPUHapticsAudioDriver` 在 F5 里确认存在于我们启动的内核。
- **不共享的一半**：物理后端不同。F8 显示 marble 的触觉是 `pm8350b-swr-haptics`（SoundWire 上的
  PMIC 通道），而喇叭是 WCD938x + AW882xx。所以**内核侧把喇叭打通并不会自动让马达到**；
  它买到的是"用户态那半条路终于有人调我们"，那正是震动 10-30 记录的
  "每条内核路径都看到 zero IPC"的反面。
