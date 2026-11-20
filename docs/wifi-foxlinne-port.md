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
