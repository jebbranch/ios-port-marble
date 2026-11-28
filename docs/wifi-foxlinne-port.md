# FoXLinne Wi-Fi/GPU 移植：Wi-Fi 第一阶段

## 固定来源与边界

来源仓库：

```text
https://github.com/FoXLinne/UniXNU_marble
commit 7a0d68fff2557f12be9f8c47f618fd332b7a3e67
license BSD-2-Clause-Patent
```

FoXLinne 的目标是 iOS 18.7.11 / XNU 11417 arm64e；本项目是 iOS 16.7.16 /
XNU 8796。硬件同为 marble / SM7475 / QCA6490 / Adreno 725，因此复用板级寄存器、
电源、PCIe、SMMU、MHI、GMU和scanout事实；Apple80211、IOGPU、Metal和用户态ABI
必须按iOS 16重新适配，禁止整仓覆盖。

已审阅的Wi-Fi真机证据：

- PCIe端点`17cb:1103`、Gen2 ×1、WCN6855 hw2.1。
- 固件Mission Mode、原生en0扫描、WPA2认证和DHCP地址`172.20.10.13`。
- 用户界面等待、重新扫描和网络持久化仍未完善。

GPU保留为第二优先：有实际submit/retire和桌面/锁屏运行证据，但Safari黑屏/花屏、
撕裂、模糊、壁纸、任务预览和稳定性仍未完成。先移植底层，不直接套iOS 18 Metal栈。

本地只读参考：

```text
refs/FoXLinne-UniXNU_marble
```

## Wi-Fi 分段计划

1. 只读GCC/TLMM handoff。
2. 精确电源轨、XO/WLAN_EN/PERST、PCIe PHY、Gen2链路与端点身份。
3. Wi-Fi独占SMMU bank、stream `0x1c01`、IOVA `0xa0000000`、128 MiB DMA。
4. MSI、MHI/BHI、WCN6855固件。
5. ath11k/mac80211/cfg80211与数据面。
6. iOS 16 ifnet/Apple80211/认证服务。

每一段默认关闭，上一段有真机证据后才启用下一段。第一轮设备操作只允许第1段。

## 0299：纯只读handoff survey

新增`bsd/dev/sweet_wifi.c`，启动参数：

```text
sweetwifi=0  默认，零映射、零输出
sweetwifi=1  只读GCC/TLMM handoff
其他值       明确拒绝，不执行
```

读取内容与FoXLinne实现一致：

- PCIe0 GDSC：GCC `0x7b004`
- 10个PCIe分支时钟halt位
- WLAN_EN GPIO80的IO值
- PERST GPIO94的IO值

XNU 8796的`ml_io_map`来自固定8 MiB IO子映射，所以0299不映FoXLinne使用的整块
1 MiB窗口，而只缓存实际涉及的4个4 KiB页。代码不含MMIO write、RPMh、GPIO修改、
PHY、PCI config、BAR、DMA、线程或动态内存入口。

唯一marker：

```text
SWEET MARK: wifi handoff v1 readonly=1 stage=%d gdsc=%#x halted=%#x wlan=%#x perst=%#x
```

接入时序：UFS ready、持久SWEETLOG启动、输入/USB/HAL节点建立之后，ADSP线程之前。

## 离线结果

- 实际`sweet_wifi.c`主机模型4场景通过：
  默认零副作用、只读值、非法阶段拒绝、映射失败安全返回。
- 源码门确认不存在写寄存器、RPMh、连续DMA、线程启动入口。
- 三文件apply/check/post-image严格相等。
- `sweet_wifi.c`全链无skip；`bsd/conf/files`与`sweet_ufs.c`历史skip数不增加，
  post-image均等于对应链树HEAD。
- 未访问设备，未增加任何boot预算。

证据：`verify/wifi-0299/`。

## 第一次设备轮的前置门

0299完整CI和最终镜像已通过：

```text
raw SHA      112f3c116083a0e97977057e72cadc287cf39e408d8fbf53fcfad386492ba7b4
kernel SHA   655bb2296804dbc6906fb667fce62809d42c6b59e502a0e86dc2fde00f8504e9
image        images/wifi-handoff-0299-20261120.img
image SHA    2ab0775cb1d59ef58dc01044881ddf3400e31cb2af3253933693f29de3adc139
sweettime    1795187571
cmdline      460 B
markers      91
symbol       _sweet_wifi_init
```

CNTP raw恰一处、最终kernel为0；8核、全部内嵌Mach-O段、ADT MPIDR和GPU probe0
通过。`--verify-only`复跑通过。证据在`verify/wifi-0299-image/`。

执行只准：

```text
serial 2aa26095
fastboot boot images/wifi-handoff-0299-20261120.img
```

本轮记为Wi-Fi第1轮、总RAM-only开机第74次，执行后总预算74/120。它不播放声音，
不需要人耳反馈。

到需要设备时：

- 设备进入TWRP或Android ADB均可先确认serial；随后脚本只执行`fastboot boot`。
- 镜像只增加`sweetwifi=1`，必须关闭GPU与所有旧GPU探针。
- 只记录上述marker，不以寄存器值推断端点已上电或链路存在。
- 结果回来后再决定第2段，绝不在同一轮追加上电。

实机门：

| 观察 | 判读 |
|---|---|
| sweettime匹配、raw窗口成立 | 本轮身份有效 |
| marker恰一条，readonly=1/stage=1 | 只读路径实际执行 |
| gdsc/halted/wlan/perst有确定值 | 保存ABL/Android交接快照；不命名上电状态 |
| 任何`phase 2`、PCIe marker或Wi-Fi写路径 | 本轮无效并立即停止 |
| panic/Debugger | 只读映射风险，禁止进入第2段 |

## Wi-Fi第1轮 / 总第74次判决

身份和窗口成立：

```text
sweettime 1795187571
raw total 151350
seq       253
window    match
```

唯一只读marker：

```text
readonly=1 stage=1 gdsc=0x22f001 halted=0x17f wlan=0 perst=0
```

PCIe/上电marker为0；GLINK、I2C、GPU、GMU、panic均0。ADSP按参数关闭。

判决：

- QCA6490/PCIe0在ABL/TWRP交接后保持关闭：WLAN_EN=0、PERST=0。
- GDSC没有证明上电，10个预登记分支中mask `0x17f`对应多数halt。
- 只读页映射未引发panic，0299成立。
- 结果只授权进入独立的第2阶段代码和新镜像；不能宣称PCIe、端点或Wi-Fi可用。

证据：`verify/wifi-boot74/`。预算74/120。

## 0300：QCA6490上电、PCIe链路与端点身份

0299证明交接时Wi-Fi路径关闭且只读映射安全。0300在独立`sweetwifi=2`中移植
FoXLinne的已验证顺序：

1. 从AOP cmd-db精确查找8条VRM、3条ARC和`SN7`。
2. 独占apps RSC TCS0执行28笔resource vote。
3. XO/WLAN_EN/PERST时序。
4. GCC PCIe0时钟、复位、PHY v0x63表。
5. Gen2 ×1 link训练。
6. 配置空间ATU、BAR sizing和`17cb:1103`硬门。
7. 只读SoC version/subversion后停止；不建立SMMU/DMA，不加载固件。

相对FoXLinne新增安全门：

- 所有MMIO只映实际使用的4 KiB页，不映整块窗口。
- TCS0必须idle/control0/enable0/wait0且无pending IRQ。
- 每笔vote必须观察到新执行进展与issued+complete；超时立即poison本轮且不重试。
- PHY/link/vendor/device/BAR任一失败立即停止。
- `sweetwifi=1`的只读行为保持不变。

成功marker：

```text
SWEET MARK: wifi pcie v1 stage=%d id=%#x subsystem=%#x bar=%#x bytes=%#x soc=%#x sub=%#x
```

离线结果：

- 实际phase2主机模型4场景通过：
  成功28 vote、首vote timeout poison、link timeout、错误vendor/device。
- 0299只读模型4场景回归通过。
- 双文件apply/check/post-image严格相等。
- Wi-Fi全链applied290、touching-file skip0、post-image等于HEAD，marker唯一。

证据：`verify/wifi-0300/`。尚无完整内核、镜像或设备操作。

### Wi-Fi第2轮 / 总第75次逻辑门

完整构建及最终身份冻结前禁止启动。候选必须：

- 只带`sweetwifi=2`；ADSP/GLINK/HAL/I2C/功放/GPU全部关闭。
- handoff marker先出现，随后才允许phase2。
- 成功只接受`id=0x110317cb`，BAR bytes必须非0且≤32 MiB。
- SoC version应与FoXLinne真机基线`0x400c1211`对照记录；subversion同样只记录，
  不因不相等自动写寄存器修正。
- 失败时必须有明确errno；没有PCIe marker则不进入SMMU/DMA。
- 本轮不加载WCN6855固件、不扫描网络、不创建en0。

## 0301：开机前拒绝0300，修正IRQ窗口与C接口

0300明确禁用，未上机。Release错误记录绑定提交
`15b47b511cda30a43a144d930a32bbda0f909d55`：C文件缺少
`IOSleep/IODelay`声明，完整构建失败。主机stub预先定义了这两个函数，未暴露声明缺失。

另一个独立源码错误是RPMh IRQ enable/status/clear少加TCS block偏移：

| 寄存器 | 0300错误地址 | 0301正确地址 |
|---|---|---|
| IRQ enable | `0x17a20000`，误指RSC ID | `0x17a20d00` |
| IRQ status | `0x17a20004` | `0x17a20d04` |
| IRQ clear | `0x17a20008` | `0x17a20d08` |

正确地址由FoXLinne的`0xd00 + QcomRscLayout`及本项目已有RPMh驱动交叉确认。
新模型固定这些物理地址，不再从被测驱动宏推导硬件布局；旧0300会被模型拒绝。

0301还保持以下失败关闭规则：

- C接口声明与XNU8796 `IOKit/IOLib.h`一致。
- 只接受实机已经确认的RSC major2；其他版本零写返回。
- cmd-db version/magic/范围/资源slave必须通过检查。
- MMIO访问失败后不再读写后续寄存器。
- 全1或陈旧CMD_STATUS不得冒充完成；清理读回失败保留poison。
- BAR必须为2幂、至少1 MiB且不大于32 MiB；端点PCI command读回必须为
  memory decoding enabled、bus mastering disabled。
- 每次启动只执行一次；记录失败milestone便于区分power/clock/PHY/link/endpoint。

新的成功身份必须为：

```text
SWEET MARK: wifi pcie v2 stage=%d id=%#x subsystem=%#x bar=%#x bytes=%#x soc=%#x sub=%#x
```

实际phase2主机16场景和handoff4场景通过，ASan/UBSan/严格警告通过；
0301对0300基线apply/check和最终逐字节比较通过；Wi-Fi全链touching-file skip为0。
证据保存在`verify/wifi-0301/`。

总预算仍74/120。下轮候选只能使用0301，完整构建/raw SHA/最终image SHA、
CNTP/marker/符号/参数和事前冻结提交全部通过后才能执行第75次。
当前没有Wi-Fi链路、固件、DMA或联网成功的新实机证据。

首轮phase2镜像暂时固定`marble_touch=0`和`sweettouch=0`。现有触摸线程会在
延时后使用同一apps RSC TCS0，因此必须避免与独立Wi-Fi owner并发。这仅影响本轮
诊断镜像，不修改日常触摸设置；此轮无需触屏，用实体按键返回TWRP。
参数检查采用完整key白名单，防止额外GPU/音频/RPMh探针混入。

## 0301第75次冻结身份（未启动）

```text
image        images/wifi-pcie-0301-20261123.img
image SHA    7f722647dfdd4fbe8b59e81a4aa8ae1e54438083e9e7dd769e16412342d189a1
raw SHA      2ceac3fbf7591c71367e3b5c09f728a666573dfaa239e20236984ac8580ac1dd
kernel SHA   2db83180e4058518902ab8f15edbea2e9b668e6efb90ad618770c163f1792c1a
sweettime    1795431511
image bytes  51322880
cmdline      460 B
markers      92
```

CNTP raw恰1处，修补点`0x4aa348`，修后0处；`_sweet_wifi_init`链接、8个MPIDR、
全部Mach-O内嵌段、boot包和GPU shim level0验证通过。Wi-Fi v2 marker在image恰1处，
旧v1为0。verify-only复跑通过，见`verify/wifi-0301-image/`。

操作边界：

- 只对serial `2aa26095`执行一次`fastboot boot`，启动后总预算75/120。
- 当前总预算74/120，尚未启动。
- `sweetwifi=2`；`marble_touch=0`、`sweettouch=0`；音频硬件、HAL、GPU全关闭。
- 本轮不需要触屏。启动后等待日志落盘，再用实体按键回TWRP。
- 不在同一轮追加固件、SMMU/DMA、MSI、ath11k或任何网络连接。
- 无设备连接则保留镜像，不猜测设备状态、不添加启动次数。

实机判据：

| 门 | 成功条件 / 失败解释 |
|---|---|
| 身份 | sweettime1795431511；raw有效窗口，无陈旧尾部 |
| 隔离 | touch disabled、ADSP disabled；无GLINK/I2C/HAL PCM/GPU/GMU/panic |
| 初始状态 | stage2 handoff仅1条 |
| 电源 | milestone=1，代表28次资源请求通过本地完成门；不等于实测电压 |
| 端点 | v2 marker仅1条，id=0x110317cb；BAR合法且终止前bus-master读回关闭 |
| SoC | 记录soc/sub，与参考0x400c1211/0x1016d0e1对照；不同不自动调整 |
| 失败 | errno/poison/milestone记录；无端点marker则不进入DMA |

评分器`tools/score_wifi_pcie.py`和8项正负证据测试已提交。成功只宣称
“QCA6490 PCIe端点识别”，不宣称Wi-Fi联网、固件或GPU完成。

## 0302：合并 Wi-Fi 构建，取消独立 PCIe 开机轮

按用户决定，先把板级初始化、DMA、MSI/MHI、无线栈和固件接到同一个候选内核；
设备运行时用阶段日志区分失败点。0301 镜像只作历史对照，不再作为下一次开机候选。

已经通过的本地构建和验证：

- FoXLinne 快照及锁定的 Linux、FreeBSD、supplicant 输入已到位。现用 ath11k
  manifest 编出 109 个无线栈对象；Clang 19 通过编译。实际 DXB 内核 Mach‑O
  标为 macOS 平台，合成对象使用同平台的普通 arm64 ABI；这不改变 iOS 16 用户态。
- `amss.bin`、`board-2.bin`、`m3.bin`、`regdb.bin` 已进入同一个可重定位
  内核对象。主机执行了实际固件读取函数，核对四个长度、只读借用语义和
  `firmware-2.bin` 缺失时的 API 1 回退约定。没有 GPU addon 或 VFS 依赖。
- 对照现有 XNU 8796，符号检查未发现未满足导入、重复定义或冲突。
  0302 已通过 apply/check；实际 Makefile 展开验证了只有 SWEET 本地
  `SWEET_WIFI_EMBEDDED=1` 才会加对象和链接保留符号。
- DMA 路由规划和 128 MiB S1 表生成已编进对象。离线模型覆盖 SID 掩码重叠、
  bank 已占用、无可用路由、越界地址和页表别名；尚未读写设备 SMMU。

复现入口：`tools/prepare_wifi_dependencies.py`、`tools/fetch_wifi_firmware.py`、
`tools/build_wifi_kernel_archive.py`、`tools/stage_wifi_kernel_bundle.py`。
验证用 `tools/test_wifi_embedded_bundle.py`。本地合成对象及报告在
`out/wifi-full/arm64-dxb-dma/`。固件下载的完整性校验保留为自动步骤；不手工逐阶段验摘要。

**未完成：** XNU 板级 worker 尚未调用此对象，SMMU context 的实际访问权、
DMA 所有权和 MSI/MHI 启停仍待接入；不能把参考机的 bank 编号当成本机的授权。
同一个 bank 在不同引导状态可能会产生外部中止，见 `smmu-dead-ends.md`。
不发布带固件的二进制、不启动设备，直到整机链接、运行时保护和候选验证通过。

## 0302 后续：DMA 分配与 SMMU 配置核心

- 新 bundle 加入 `sweet_wifi_smmu_commit()`：要求板级 owner 确认指定
  context bank、ASID 和 stream slot 的独占访问，且端点 BME 保持关闭。
  检查现有路由、准备并同步独占页表、逐项读回 context 配置、限时等待
  TLB 同步，再先写 S2CR、后装有效 SMR。失败时先撤 SMR 并读回；
  一旦尝试过 MMIO 写入，本次引导保留已分配的 DMA backing 和页表。
- 本机历史 `IDR0=0x4c017e71`、`IDR2=0x5111`；参考机也是相同值。
  FoXLinne 成功关联并拿到 DHCP 的记录里 `AtosStats` 八项全零；历史强行
  打 ATS 时曾得到 `128` 次错误/零 PAR。`ATOSNS=1` 不发 ATS，只有 owner
  明确批准的有限 DMA 诊断可进入配置分支。ATS 可用时必须通过 128 个
  边界地址及一个未映射保护地址的硬件验证；可用但失败时不回退。
  `configured` 仅表示寄存器配置通过，另记 `ats_verified`；
  两者都不能代替 BHI/MHI 的实际传输完成证据。
- 新 DMA arena 保留首个 4 KiB 为 MSI sink，按大小对齐 BHI 缓冲，
  只返回仍属活跃分配的 IOVA，拒绝越界、错误地址释放和重复释放。
  真实板级分配仍须提供独占、全区清零且 DMA 可见的 wired backing，
  以及正确的 CPU cache 属性；allocator 不负责选择物理地址。

本地构建产物 `out/wifi-full/arm64-dxb-dma-runtime/sweet_wifi_bundle.o`：
109 个无线栈对象，缺失导入、重复定义、XNU 符号冲突均为 0。
按上节 `test_wifi_embedded_bundle.py` 改用该 `--build` 验证：
19 个 SMMU 模型场景、1024 次 arena 碎片化循环、固件和 0302
重放/staging/opt-in/主机链接通过。

**还未进入 XNU 的实际启动路径。** 下一项是从板级 owner 取得真实
SMMU 授权和 wired/无缓存 DMA 映射，再接 PCIe 后的 MSI/MHI worker。
不能照抄参考 bank/slot，不能因 MMIO 回读成功就放开 BME。尚无整机
内核链接、候选镜像或新实机日志。不开独立 PCIe 轮，预算仍 74/120。
