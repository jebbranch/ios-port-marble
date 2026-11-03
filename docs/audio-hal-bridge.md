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
| F5′ | **订正（我原先把参照物当成了我们自己）**：`iphone-x-recon/artifacts/kernel.bin` 是 **Apple 的 iPhone X 内核缓存**，不是我们启动的那颗。我们真正启动的 `out/kernel.adsp` 里 `IOAudio2Device`=**0**、`IOAudio2DeviceUserClient`=**0**、`AppleAOPAudioController`=**0**、`AppleEmbeddedAudioDevice`=**0**，而 `AppleSPUHapticsAudioDriver`=3、`IOPMPowerSource`=3 只是因为那两个**是我们自己写的**（0196、0062）。⇒ 我们的内核里**没有**任何 Apple 音频类可继承。 || F6 | Apple 的 iOS 侧 HAL 插件认的是 AOP 控制器 | `/System/Library/Audio/Plug-Ins/HAL/AppleAOPAudioPlugin.driver/Info.plist`：`AudioServerPlugIn_LoadingConditions.IOProviderClass = AppleAOPAudioController`，`IOKitUserClients = {AppleAOPAudioUserClient, AppleAOPVoiceTriggerUserClient}`，`MinimumOSVersion 16.7` |
| F7 | **我们启动的那份 ADT 要求的硬件 marble 没有** | `python3 scripts/adt.py dump …/d22ap.adt` 的行：`1434 audio-codec`、`1597 audio-speaker-top` 且 `1607 compatible = 'audio-control,cs35l26'`、`871 aop-mca { device_type = 'mca' }`、`864-868 spkamp-config_arc/_arc_p1/_btmspk`、`771 aop { role = 'AOP' }` + `790 firmware-name = 'iphone10baop'` |
| F8 | marble 真实播放拓扑：PA 挂在 **TERTIARY** 那条 TDM 上 | `logs/android-proc-asound-pcm.txt`：`00-31 TDM-LPAIF-RX-TERTIARY multicodec-31 playback`；主后端 `00-00 CODEC_DMA-LPAIF_RXTX-RX-0`；组件名 `aw882xx_smartpa.2-0034` / `…0035` / `wcd938x-codec` / `pm8350b-swr-haptics.1f0170220` |
| F9 | 我们已经有"发布一个 IOKit 服务 + user client 并把 client 的每一次调用打出来"的可用样板 | `work/ci-src/xnu-xnu-8796.141.3/iokit/Kernel/arm/hid/SweetHaptics.cpp:47 class SweetHapticsUCBase : public IOUserClient`、`:102 externalMethod(uint32_t selector, …)`、`:17` 注释写明"log IOServiceOpen, every externalMethod selector" |
| F11 | **"发布一个 Apple 名字的驱动，iOS 用户态真会来 open"这件事在本移植上已经被证明过一次**——不是推测 | `grep -rh "SWEET HAPTICS" logs/ring-*.txt` 聚合计数：`AppleSPUHapticsAudioUC open by pid 57 type 0` ×99、`AppleAOPButtonHapticsServiceUserClient open by pid 57` ×90、`… sel=1` ×90、`… map type=0 options=0` ×88、`AppleSPUHapticsAudioDriver setProperties count=1` ×92、`SWEET HAPTICS: asset count 18` ×72，还有 `closed (pid 57)` / `died (pid 57)` |
| F12 | 那个 pid 57 是 **backboardd** | 同一批 ring 里 `CODE SIGNING: cs_invalid_page(…): p=57[backboardd] … allowing` |
| F13 | **电池电流遥测已经在打印带符号的 mA，且会真的变**（一条现成的时变量） | `grep -rh "SWEET BATT: tick" logs/ring-*.txt`：159 行是 `ok=5/5`，例 `tick 7 ok=5/5 cap=99% 4396mV -367mA status=1 usb=1`、`tick 7 … -156mA`、`tick 25 … 0mA`；另有 887 行 `ok=0/5`（读失败的那批不能用作证据） |
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
**F5 订正带来的两个直接后果（都是我原以为的反面）**：
1. **没有类冲突风险**：另外单独查了编译树本身 —— `grep -rl "class IOAudio2Device" ci-src/...` 无结果，
   `grep -in audio iokit/conf/files.arm64` 也无结果 ⇒ 这颗内核根本没编译任何 IOAudio2/音频驱动，
   我们自己起名叫 `IOAudio2Device`/`AppleAOPAudioController` 不会撞链接符号。
   顺带记一次方法失败：想用 `strings kernel.bin | grep '^__ZTV<类名>$'` 判断"类是否已链接"，
   **七个名字全为 0，包括我们自己成功定义过的 `AppleSPUHapticsAudioDriver`** ⇒ 该检查看不到东西，
   不能拿它的 0 当结论，判据换成上面两条直接查源码/查我们自己的产物。
2. **R1 的形状因此变了**：不是"给 Apple 的 IOAudio2 类补一个 provider"，而是**照 0196 的样子自己写一个
   `IOService` + `IOUserClient`**（`SweetHaptics.cpp:47/:102` 已经证明这条路能用、能收 open/selector/map），
   然后把注册名取成 mediaserverd 会去查的那些字符串。工作量的主体从"逆向 Apple 驱动内部"
   变成"实现它被查的那组 selector"，这两件事的成本完全不同。

- 成立的前提：`mediaserverd` 会在出现一个匹配的 IOAudio2 服务时来 `IOServiceOpen`。
  **这条今天从"未知"升级为"同类已被验证过一次"**：F11/F12 显示我们发布的
  `AppleSPUHapticsAudioDriver` / `AppleAOPAudioButtonHapticDevice` 这类 Apple 名字的服务，
  被 backboardd(pid 57) 真实 `open` → `map` 缓冲区 → 发 `sel=1` → `setProperties` →
  最后 `closed/died` 走完整流程（99/90/88/92 次计数）。所以"发服务就有人来"在本移植上不是赌。
  仍未验证的是 **mediaserverd 会不会**（它是另一个进程、另一套查找逻辑），别把 F11 直接当成 R1 成立。
- 最便宜的判别实验（一次开机，零 ABI 工作量；**不需要先做注册表普查**——
  0196 已经把三件套形状给全了：新 `.cpp` + `iokit/KernelConfigTables.cpp` 注册 +
  `iokit/conf/files.arm64` 编译项，服务在自己的 `start()` 里 `publishNamed`，
  user client 把 `open/selector/map/close` 全打进 ring）：照 F9/F11 的样板发一个**空壳**设备
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

---

## 8. 候选见证 #5：电池电流——**一条已经存在、且是唯一时变的通道**（今天新发现）

`docs/audio-closeout.md` 里 ①②③ 用的全是**静态快照**，而 ① 的 J3 未命中已经把静态快照这条路
压到很窄：四个寄存器在流前后逐位相同。第二意见给的批评我接受——静态读数原理上分不清
"配置过"与"在驱动"。按这个方向查现有日志，发现我们**早就在打一条时变量**（F13）：

```
SWEET BATT: tick 7   ok=5/5 cap=99%  4396mV -367mA status=1 usb=1
SWEET BATT: tick 7   ok=5/5 cap=100% 4412mV -156mA status=4 usb=1
SWEET BATT: tick 25  ok=5/5 cap=100% 4413mV    0mA status=4 usb=1
```

为什么它比寄存器更值钱：**功放真的在推喇叭时，唯一无法伪造的就是能量**。
它不需要新补丁、不需要新寄存器、不需要厂商参考值（③ 就是死在参考值上）。

但有三条必须先钉死，否则它和 ①②③ 一样会退化成不能证伪的仪器：

1. **`ok=0/5` 的那 887 行一律作废**。只有 `ok=5/5`（159 行）才是有效读数；
   写判据时要把"这一格有没有 ok=5/5 的行"当成前置有效性检查，就像 ② 的 I5/J5。
2. **符号已核（读源码，不是猜）**：`iokit/Kernel/arm/SweetBattery.cpp:266-270` 转 mA 的那行
   注释写着 `Linux: negative=charging; IOPMPowerSource: positive=charging`，代码是
   `ma = -(signedRaw * scale)/1e6` ⇒ **打印的正数=充电、负数=放电**，`-367mA` 就是放电 367 mA。
   但更要紧的是**有两条不同的电流来源**，我先前指错了那条：ring 里的 `SWEET BATT: tick …`
   出自 `sweet_glink.c` 的 **pmic_glink** 通道（`g_batt_req(0x30, 9, "ibat")`，ADSP 电量计缓存，
   **3 秒轮询、每 6 tick≈30 秒才印一行**，见 `xnu/patches/0181-…patch:3592-3606`）；
   直接读 PMIC 的是 `SweetBattery.cpp`（SPMI `0x4805` subtype∈{3,4}、`0x48c2` 裸值、
   `0x8000`=无数据、scale 152588/305176）。30 秒的打印周期对上 180 ms 的音，
   命中概率约 0.6%，**这条统计上不可能测到**——#5 必须走 SPMI 直读的密集突发
   （规格在 `docs/audio-closeout.md` 的 0243 一节），不能拿现有 tick 行当仪器。
3. **信噪比是最可能判死的地方**：现在这枚音是 1/16 幅度、`sweetbufs=30`（≈180 ms），
   而 tick 周期看起来是秒级平均。所以判据必须写成"能不能测到"而不是假设测得到：
   - 前置：拔掉 USB（去掉充电电流的支配项），仍然要 `ok=5/5`；
   - 单变量对照：**同一张图、同一段流，只切 PA 使能/静音**（一路喇叭、一路不接喇叭/静音），
     看两次电流差；差值出现且可复现 ⇒ 这才是"能量确实被喇叭吃掉了"的证据；
   - 若两次电流在 `ok=5/5` 下无可测差异 ⇒ 说明这条仪器也判不动，
     那"独立于 DONE 的正证据"在内核侧就只能承认拿不到，按止损走 §5 的桥清单。

第 2、3 点合起来意味着：**这条仪器值得一次开机，但它的前提是"能测到"，
所以它的判据里必须写死一条否证格**——否则又会变成 ① 那种"修好了仪器、没拿到证据"的一轮。

---

## 9. 2026-11-03 离线更正：桥这一轮的答案是"没测到"，不是"没人来 open"

### 9.1 上一次开机对桥没有给出任何结论

`sweetaudioprobe=1` 在 cmdline 的**偏移 613**，而 boot-args 字段只有 **607 字节**
（`scripts/mkpayload.py:36-38`：CMDLINE 0x48 + `BOOT_LINE_LENGTH` 608 → SEG_TABLE 0x2a8；`:186` 以前静默切片）。
内核回显走 `bsd/kern/kern_sysctl.c:2167-2170` 的 `strlcpy(buf, PE_boot_args(), BOOT_LINE_LENGTH)`，
所以 ring 里那行 `boot-args =` 就是内核真正拿到的东西 ⇒ **ab 那轮的"没有 open"是参数没到，作废**。
细节与逐 boot 偏移表见 `docs/audio-closeout.md` 的 2026-11-03 一节。

### 9.2 名字按真机 ioreg 校正（三处，全部有行号）

证据文件 `work/iphone-x-recon/dyld/ioservice.txt`（D22AP，`xnu-8792.82.2`，`20D67`）：

| 用途 | 真机写法 | 行号 | 我原来写的 |
|---|---|---|---|
| 控制器 UC 类名 | `"IOUserClientClass" = "AppleAOPAudioUserClient"` | `:3559` | `AppleAOPAudioControllerUC`（编的） |
| 设备 UC 类名 | `IOAudio2DeviceUserClient`（dump 内 9 次命中） | — | `IOAudio2DeviceUC`（编的） |
| UID 键 | **`"device UID" = "Speaker"`**（带空格） | `:13112` | `IOAudio2DeviceUID`（该 dump 里 **0 次命中**） |
| 类链 | `IOService:AppleBaseAudioController:AppleAOPAudioController` | `:3545` | 直接挂 `IOService` |
| 匹配类别 | `"IOMatchCategory" = "AppleAOPAudioController"` | `:3569` | 无 |

`IOUserClientClass` 这个字符串正是 `IOService::open()` 查的东西 ⇒ **名字错了就会永远读到
"没人来 open"**，而且读起来像结论。0244 v5 已改。

### 9.3 空壳现在回答三层问题，不是一层

原来只问"有没有人来 open"；三种失败会打印同一片安静。v5 覆写匹配钩子把安静分层：
`IOService.cpp:7798` 调 `where->matchPropertyTable(table, &score)`，
基类两参版又转调一参版（`IOService.cpp:1294-1297`）⇒ 覆写一参版能同时接住两条入口
（`matchInternal` 在这颗 xnu 里**不是**虚函数，`IOService.h:1659`，所以不能用它）。
匹配字典的键一起打出来，因为平台 kext 自己 publish 时的匹配必须和用户态
`IOServiceGetMatchingServices` 的查询区分开。

| 观测 | 判读 |
|---|---|
| `start` 行有、`gate flag=1` 有、`registered` 有 | 空壳活着且发布成功 |
| 之后**任何** `matchq` 都没有 | 没人拿这两个类名去查 ⇒ 问题在 HAL/名字层，不在 open 层 |
| 只有 publish 期自己的 `matchq`（键里是 `IOProviderClass=IOResources` 一类） | 有人问过，但不是音频 client ⇒ 仍算"没人来 open"，但已排除"注册路径不通" |
| `matchq` 带 `IOAudio2Device`/`AppleAOPAudioController` 却无 `open by pid` | 名字对了、匹配到了、客户端没开 ⇒ 卡在用户态决策（权限/依赖/选择哪台设备） |
| `open by pid N` + `sel=` 序列 | B2 的原料：按序列反推 selector ABI |

`matchq` 上限 40 行并** announce 一次封顶**，因为系统里每一次 registry 查询都会走这里；
不这么写会把"40 次之后安静"读成"没人问过 40 次"。

### 9.4 能量侧（E 组）与我们的运行模式有结构冲突，先说清楚

本节 §8 的前置写的是"拔掉 USB"。但本项目只 `fastboot boot` 纯 RAM，**设备必须插线**，
所以拔 USB 这条前置在我们的运行模式里做不到。已核实的节拍：
`xnu/patches/0181-sweet-shm-system-ram.patch:3604-3607` 是 `tick_n++; if (tick_n % 6 == 1) printf(...)`，
注释自己写 "(30 s)" ⇒ **tick 5 s、打印 30 s**。0243 的密集突发（12 次 × 10 ms 直读 SPMI）
正是绕开这条打印通路的，但充电电流这个支配项绕不开。
因此 E 组判据加一条硬门：**同一 phase 里 `usb=1` ⇒ 该 phase 记未运行**（0243 自带 `usb=` 字段，
不用凭印象），若三段全 `usb=1`，能量侧就在 RAM-only 模式下**原理性不可判**，
按止损把"内核出声未证"写死，全部转桥。这不是坏消息的遮掩，是把一格不该被
"看起来测过了"的结论污染的地方提前封掉。

### 9.5 顺序

1. 一次开机（E+P 合一，cmdline 585 字节，仪器参数最前）⇒ 先看 9.3 的五层落点；
2. 有 open ⇒ B2 复原 selector ABI，再谈把 PCM 后端接到已通的 SHM/GPR 环；
3. 无 open 但 matchq 到了音频类名 ⇒ 问题在用户态（HAL bundle 加载/依赖），不许跳去写内核音频驱动；
4. matchq 也没到 ⇒ 名字/匹配域还错，回到 9.2 的证据面继续挖离线材料（`artifacts/kernel.bin`
   里四个类名都在，需要 im4p 解包 + 反汇编，这条**不需要设备**）。

---

## 10. 2026-11-03 深夜：B2 的内核侧 ABI 已经离线拿下，而且它把主攻对象换了

素材 `work/iphone-x-recon/artifacts/kernel.bin` **是已解密的 Mach-O**（fat 容器，arm64 thin 切片的
banner 我本地读到的是 `Darwin Kernel Version 22.3.0 … root:xnu-8792.82.2~1/RELEASE_ARM64_T8015`），
跟 §9.2 那份 ioreg 是**同一个 build**，所以两边可以互相印证。`kernelcache.img4` 仍是 IMG4/IM4P，没用上。

### 10.1 更正：真正被音频客户端 open 的是 `IOAudio2Device`，不是 AOP 控制器

我自己数那份 dump：

| 事实 | 数值/位置 | 为什么重要 |
|---|---|---|
| `IOUserClient:IOAudio2DeviceUserClient` 实例 | **8**（节点在 `:10572 :12913 :13021 :13131 :13310 :13420 :13495 :13607`） | 音频客户端集中在这条线上 |
| `IOUserClient:AppleAOPAudioUserClient` 实例 | **1** | AOP 控制器只有 1 个客户端 |
| `"device UID" = "Speaker"`（`:13112`）属于哪个节点 | `:13094` 的 `Speaker`，类链 `IOService:IOAudio2Device:AppleARMDMAAudioDevice:AppleARMIISAudioDevice:AppleEmbeddedAudioDevice` | §9.2 我把它当成了 AOP 控制器的属性——**不是**，它是 IOAudio2Device 的 |
| `Speaker` 节点是否发布 `IOUserClientClass` | **否**（`:13094-13140` 内 0 命中；只有 `AppleAOPAudioController` 在 `:3559` 发布） | 真机的 UC 解析走 `IOAudio2Device::newUserClient` 的类名回退 |

⇒ 空壳的两个节点里，**`IOAudio2Device` 那个才是主战场**；AOP 控制器是配角。
顺带一条对 B3（震动）有用的观察（**仅记录现象，结论未证**）：真机 registry 里有
`AppleSecondaryAudio`（6 个实例）和 `AppleActuatorSecondaryAudio`（1 个），二者的类链都是
`IOService:IOAudio2Device:…`——iOS 把触觉促动器**建模成一种 IOAudio2Device**。

### 10.2 `AppleAOPAudioUserClient` 的方法表（已抄进 0244）

它的 `externalMethod` 用 `table + selector*0x18` 取 dispatch、对 selector 上界做 `cmp #6`，
然后 **tail-call `IOUserClient::externalMethod`**。`0x18 == sizeof(IOExternalMethodDispatch)`
（`iokit/IOKit/IOUserClient.h:160-166`），六条记录与我本地能 grep 到的名字一一对应：

| sel | 名字 | sIn / sInSz / sOut / sOutSz |
|---|---|---|
| 0 | `method_Open` | 0,0,0,0 |
| 1 | `method_Close` | 0,0,0,0 |
| 2 | `method_StartHardware` | 0,0,0,0 |
| 3 | `method_StopHardware` | 0,0,0,0 |
| 4 | `method_GetControlValue` | **1,0,1,0** |
| 5 | `method_SetControlValue` | **2,0,0,0** |

两条门禁字符串也在这份 binary 里（我 grep 到，命中数 ≥1）：
`com.apple.private.aop-audio.user-access`（AOP UC 的 `initWithTask` 断言它）、
`com.apple.private.audio.driver-host`（`IOAudio2Device::newUserClient` 要它，缺了给 `0xe00002c7`）。
**我们的空壳不是那个 family，所以这两条检查不会执行** ⇒ 客户端只要能 open 就会 open 成功，
这正是要的（探针不该自己造门槛）。

### 10.3 由此修掉的三个"探针自己在骗客户端"的问题

1. 原来 `externalMethod` 对任何 selector 直接 `return kIOReturnSuccess`。基类里有真正的校验
   （比较 `dispatch->checkScalarInputCount` / `checkStructureInputSize`，不匹配给
   `kIOReturnBadArgument 0xe00002c2`）。跳过它就等于**回答"是"给真机会回答"否"的调用**，
   而且 `structureOutput` 没填就返回成功 ⇒ 客户端读垃圾，我们要的序列反而不会出现。
   现在：记录之后交还 `IOUserClient::externalMethod`；无 dispatch 时基类报 unsupported，
   与真机"越界 selector"的行为一致。
2. `scalarInput` / `scalarOutput` 解引用全部加 NULL 保护——**count>0 而指针为 NULL 是这个探针
   唯一能把机器搞死机的方式**，而死机的代价是用户去长按电源。
3. `IOAudio2DeviceUserClient` 覆写 `getTargetAndMethodForIndex` / `getTargetAndTrapForIndex`
   （记 index、返回 NULL）：真机的 legacy `IOConnectMethod*`/trap 路径在**发任何 selector 之前**
   先问这两个。不记的话，客户端在这一步放弃就表现为"连接建立但一次调用都没有"。

### 10.4 离线的边界（不许含混）

* **内核侧 ABI：已完备**，不需要设备。
* **用户侧调用顺序（mediaserverd 先调哪个、按什么顺序）：离线拿不到。** 那段代码在
  AudioToolbox/CoreAudio + 用户态 IOKit，也就是 dyld 共享缓存里；
  `dyld_shared_cache_arm64.01` 的 extent 落在卷偏移 **9,633,174,784**，
  而我们本地 HFS 前缀只到 **2,684,616,704**（2.5 GiB）⇒ 在本地数据之外。
  历史上三条提取路子都没成功（19 个 text 子缓存扫 install-name、`dylibsTrieAddr` 落进代码段、
  ObjC relative-method-list 扫描），`/tmp/sc01.bin` 那份拷贝也已不在。
  ⇒ 要么设备回来做一次**只读** `dd`（不写盘，符合红线）补上子缓存 `.01`，要么就靠这轮开机的
  P/Q 日志给出顺序。**探针对象存在的意义正是补上这一块。**
* 已定位未解码：`AppleIPCAudioDeviceUserClient`、`AppleAOPAudioPCMAssetManagerUserClient` 的表。
* 编译状态如实说：CI 新产物（`out/kernel.adsp`，本地重下）里
  `SWEET AUDPROBE: gate flag=` 命中 1 ⇒ **这个 IOKit 文件确实能编过**；
  但 §10.3 那 ~100 行是 v6 才加的，**这个产物里没有**（`aop sel=`、`legacy trap[%u]` 均 0 命中），
  所以 v6 仍是"未过编译器"的状态，等下一次 CI。

---

## 11. 2026-11-03 中午：第一次真答案——mediaserverd 来了（`work/xnu-gh/logs/ring-ep.txt`）

有效性先摆出来，后面每一条都以此为前提：cmdline 584 字节、内核回显也是 584 字节
（**没被切，六个参数逐个 ARRIVED**）、`graph_start status=0` ×1、`done status=0` ×4、
`pcm tone done (30 buffers)`（`sweetbufs` 这次真的生效）、ring 以
`SWEET GLINK: PSCI SYSTEM_RESET` 结束（日志完整）。设备干净复位，全程只 `fastboot boot`。

### 11.1 头条：空壳被 open，selector 被调用，v6 的方法表是活的

```
  79  AppleAOPAudioController start          81  AppleAOPAudioController registered
  80  gate flag=1 exp=0x3                    83  IOAudio2Device Speaker registered
 954  AppleAOPAudioUserClient open by pid 28 type 0
 956  AppleAOPAudioUserClient sel=0 sin=0 sout=0 struct_in=0 struct_out=0
 957  aop sel=0 ran in=0 out=0 sin=0
 958  AppleAOPAudioUserClient map refused type=0
 959  AppleAOPAudioUserClient closed (pid 28)
 960  AppleAOPAudioUserClient died (pid 28)
1367  IOAudio2DeviceUserClient open by pid 28 type 0
1368  IOAudio2DeviceUserClient map refused type=0
```

pid 归属按 P4 的规矩在**同一 ring 内**核验：`1681: CODE SIGNING: … p=28[mediaserverd]`，
且 `797: exec mediaserverd`。⇒ **B1 那个二元问题的答案是"会"**：发一个 Apple 名字的音频服务，
iOS 的音频服务器就来 open，并按 §10.2 那张表发出 selector 0（`method_Open`）。
`aop sel=0 ran` 同时证明"抄来的方法表 + 交还基类校验"这条路真的在跑。

### 11.2 它止步的位置（顺序是事实，因果不是结论）

两个客户端都在 **`clientMemoryForType(type=0)` 被拒**之后结束。一次开机不授权机制结论，
所以做法是把它变成**可否证**的：0244 v7 改为**给**一段 0x10000 零页缓冲区
（用 0197/0211 在 haptics 客户端上已跑通的同一个 `withCapacity(...)` 写法，
引用交给调用方、不额外 release），其它 type 继续拒。
若给了之后仍然只到 map 就 closed ⇒ "拒绝映射导致离开"这条候选被自己否证，线索转 §11.3。

### 11.3 0049 埋点第一次跑起来就撞到的三样东西

`IOK` 共 **396** 行；`mediaserverd` **288**、对照 `backboardd` **108 > 0 ⇒ Q0c 成立**，
所以任何"为 0"都算真负结果。mediaserverd 的查询分布：

| 次数 | 查询 | 结果 |
|---|---|---|
| 270 | `path IODeviceTree:/product` | `IOPlatformDevice`（成功） |
| **3** | **`path IODeviceTree:/product/audio`** | **`none`** |
| 6 | `matching1` | `AppleEFINVRAM -> none`、`IODTNVRAM -> none`、`IONameMatch bluetooth -> none`、`IOPlatformExpertDevice` ×3 成功 |
| 4 | `IOServiceFirstPublish` | `IOAudio2Device`、`AppleAOPAudioController` ×2、**`AppleAOPVoiceTriggerController`（我们没发）** |
| 3 | `IOServiceFirstMatch` | 嵌套 `IOPropertyMatch → IOUserAudioDriverUserClientProperties → {IOClass=IOUserUserClient, IOUserClass=…}`，**在 240 字符处被截断** |
| 1 | `open AppleAOPAudioController` | `0x0` |

⇒ 三条候选阻塞点，都与 IOKit 名字无关：**(a) 平台设备树缺 `/product/audio`；
(b) 它要 NVRAM（`AppleEFINVRAM`/`IODTNVRAM`），我们两个都没有；
(c) 它在等一个 driverkit 式的 user-client 类**，而那个类名正好被截断 ⇒ 0245 把 240 放到 600。

`P7 legacy = 0 行`，而 `SWEET AUDPROBE: legacy ` 这条 marker 确实在镜像里（门禁 c=2）
⇒ 这个 0 是有意义的：客户端不走 legacy `IOConnectMethod*`/trap，全靠 selector。

### 11.4 E 组这次知道自己为什么不行，而且是我的仪器先违规

三段全 `flag=1 exp=0x3`（送达自证）但 `invalid subtype=255 ok=0/12`。
同一 ring 里 SPMI 本身是好的：`1725 SWEET SPMI: rtc sid=0 base=0x6100: …`、
`1728 SWEET SPMI: hap sid=3 base=0xf000 … rc=0` ⇒ 错的是我选的 `sid=0 / 0x4805`。

查实时 Android 设备树：SPMI 上声明的 PMIC 是 `pmk8350@0 / pm8350@1 / pm8350c@2 /
pm8350b@3 / pmr735a@4 / pm8450@7 / pm8010-debug@8,9 / pm8008i@8,9 / pm8008j@c,d`
（`logs/fdt/marble-live.dts:8007,8400,8441,8673,8842,8891`），**没有任何燃油计兼容节点**；
`qcom,battery_charger` 挂在 `compatible = "qcom,pmic-glink"` 之下（`:9007,9013,9019`）。
⇒ 预测：这颗板的燃油计根本不在 SPMI 上，电量只能经 pmic_glink（5 s 一采样），
而 130 ms 的音在 5 s 的源上原理性看不见。

同时承认我自己的仪器违规：旧代码 `if (read() != 0 || (subtype != 3 && subtype != 4))`
把"读失败"与"读到不认识的数值"合成一行——正是我这一天反复在拆的失效模式。
0243 v3 拆开 `rc`/`val`，读前写 `0xa5` 哨兵，并在 pre 阶段扫 6 个 SID，
把"设备树推断"变成"同一次开机内的测量"，不多花一次 boot。

### 11.5 ep2 的判据（开机前写死，事后不许改）

| 格 | 期望（成立） | 不成立时怎么判 |
|---|---|---|
| 前置 | 六参数在回显里；`tone done (30 buffers`；`done status=0`；ring 以 SYSTEM_RESET 结束 | 缺 ⇒ 依赖格记未运行 |
| **M1** | `map requested type=0` 之后紧跟 `map type=0 handed 0x10000 zeros` | 没有 handed ⇒ v7 没进镜像（门禁已含此 marker，本不该发生） |
| **M2（否证格）** | handed 之后出现**新的** `sel=`（预期 sel=1/4/5）或 `setProperties` | **若仍只到 map 就 closed/died ⇒ "拒绝映射导致离开"被否证**，主攻转 `/product/audio` 与 NVRAM |
| M3 | `IOAudio2DeviceUserClient` 在 handed 之后至少发一个 selector | 仍 0 selector ⇒ 那一路门槛不在此（它连 sel 都没发过） |
| E' | `ibat pre gauge sid=0 … rc=? val=?` + 六行 `ibat sweep sid=…` | 全 `rc!=0` ⇒ "SPMI 上没有燃油计"由测量确认，能量侧按止损关闭；任一 sid `rc=0` 且数值合理 ⇒ 下一版把突发挪过去 |
| Q' | `IOServiceFirstMatch` 那行不再截断，`IOUserClass` 全名可读 | 仍截断 ⇒ 600 不够，再加宽 |
| Q'' | `path` 分布里 `/product/audio -> none` 仍在（尚未补节点） | 只作对照，不判成败 |


