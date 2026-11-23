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
