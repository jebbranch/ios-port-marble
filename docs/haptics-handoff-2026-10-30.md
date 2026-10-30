# Marble iOS Haptics Handoff

日期：2026-10-30（Asia/Shanghai）

目标：让 Redmi Note 12 Turbo（`marble`）上的真实 iOS 用户态触感请求到达
PMIC HAP520。只允许 `fastboot boot`，不刷分区；不得用按坐标、任意 selector
或内核定时器模拟脉冲。测试必须来自真实 UIKit、CoreHaptics、ButtonResolver
或系统 HID/触感事件。

## 当前状态

- 最近一次设备状态：TWRP。
- 交接时 `adb`/`fastboot` 未检测到设备，后续实机测试前需要重新连接。
- 当前分支：`fix/hfs-invalid-mangled-name`。
- 本地与远端 HEAD：`2fb665c haptics: match Callan stub to ARM IO node`。
- 工作树有大量未提交日志和脚本；不要整树提交。
- `xnu/patches/0207-sweet-callan-platform-provider.patch` 尚未提交。
- 最终可启动的 Callan 测试镜像来自二进制 personality 修正，不是 0207
  的 CI 构建产物。

## 新会话从这里继续

新会话应先完整阅读本文件，不要重新从 HAP520 或 ADT 节点开始。

建议第一步：

1. 检查设备是否连接，并要求进入 TWRP 后再做任何 HFS 写入。
2. 读取 `xnu/patches/0200-sweet-haptics-callan.patch`、
   `0206`、`0207` 和 `scripts/mkadt.py`。
3. 从 P0 开始反汇编并确认 `AppleHapticsSupport` 父类及 Callan 会话协议。
4. 在父类/ABI 未确认前，不要只改继承声明，也不要继续添加“看起来合理”的属性。
5. 如需复现当前状态，直接启动
   `images/test-callan-platform-binary-keepalive.img`；它已证明 Callan 注册成功。

## 一句话结论

Callan 已经作为 IOKit 服务成功注册到
`/arm-io/haptics-support-interface`，但系统没有打开
`AppleHapticsSupportUserClient`，也没有观察到任何 Callan 会话回调。当前
阻塞点已经从“设备树/服务匹配”转移到“真实类层级、会话协议和消费者”。

## 已确认可用

### HAP520 硬件通路

- 电源键可以产生真实物理振动。
- 有效驱动参数包含 `vmax=0x1d`（1450 mV）。
- HAP520 播放时仍可能读到 `fault=0x2`，但物理振动正常，不能把它继续
  当作首要阻塞。
- 当前电源键日志会走：

```text
SWEET KEYS: key=0 down=1
SWEET HAP: ...
SWEET HAP: play ...
SWEET HAP: stop ...
```

### iOS 启动和 HFS

- HFS `cat_desc` 损坏导致的固定 panic 已修复。
- 修复提交：`3c2e397`，补丁：
  `xnu/patches/0202-sweet-hfs-invalid-mangled-name.patch`。
- 后续启动能稳定进入 SpringBoard。
- `dasd` 仍持续崩溃，但这是既有 DriverKit/系统服务问题，暂不作为触感主线。

### AOP 触感桩

backboardd 会打开：

```text
SWEET HAPTICS: AppleAOPButtonHapticsServiceUserClient open by pid 57
SWEET HAPTICS: AppleAOPButtonHapticsServiceUserClient map type=0
SWEET HAPTICS: AppleAOPButtonHapticsServiceUserClient sel=1
SWEET HAPTICS: AppleSPUHapticsAudioUC open by pid 57
SWEET HAPTICS: AppleSPUHapticsAudioDriver setProperties
SWEET HAPTICS:   prop GlobalConfig
```

- selector 1 当前返回 1 个 supported asset。
- 此后没有 selector 2/3、资产更新或真实播放数据。
- 返回 1 是实验值，尚不能证明正确；真实
  `GetNumberOfSupportedAssets` 的返回值仍未动态确认。

### Callan 服务注册成功

最终测试日志中已经出现：

```text
SWEET HAPTICS: AppleHapticsSupportCallan registered
```

但同一次运行没有：

```text
SWEET HAPTICS: AppleHapticsSupportUserClient open by pid ...
```

也没有 `notifyHapticsSessionChange`、`maybeSendAKCIRequest` 或 AOP selector
新增。控制中心、设置开关、长按图标、键盘等真实 UI 操作均未触发 Callan UC。

## 注册路径：纯 IOKit，不是 launchd/XPC

Callan 的路径是：

```text
ADT /arm-io/haptics-support-interface
  -> SweetPlatformExpert 创建 IOPlatformDevice
  -> KernelConfigTables personality 匹配
  -> 实例化 AppleHapticsSupportCallan
  -> start(provider)
  -> IOService::registerService()
  -> 等待消费者调用 IOServiceOpen()
```

- 没有 LaunchDaemon plist。
- 没有 MachServices。
- 没有 XPC service。
- 没有 `bootstrap_register()` 或 `bootstrap_check_in()`。
- `OSDefineMetaClassAndStructors()` 完成编译期 IOKit 类注册。
- `registerService()` 完成运行期服务发布。
- 设置 `IOUserClientClass` 只提供打开入口，不会主动创建 UserClient。

ADT 节点生成位置：

```python
haptics_support_props = d22ap_arm_io_child(
    'haptics-support-interface')

if haptics_support_props is not None:
    arm_io_children.append(node(
        'haptics-support-interface',
        [ph()] + haptics_support_props))
```

最终 personality 应为：

```cpp
"   {"
"     'IOClass'         = AppleHapticsSupportCallan;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'haptics-support-interface';"
"     'IOMatchCategory' = AppleHapticsSupportCallan;"
"   },"
```

不能把 personality 写成 `IOResources` 或 `AppleARMIODevice`。当前
`SweetPlatformExpert` 生成的节点类是 `IOPlatformDevice`。

当前 `start()` 内仍会设置：

```cpp
setProperty("IOProviderClass", "AppleARMIODevice");
setProperty("IOMatchCategory", "IODefaultMatchCategory");
```

这些是为贴近真机 IORegistry 的运行时属性，不参与实例创建匹配，但后续
消费者是否读取这些属性仍需验证。

## 关键新发现：类继承链不一致

真机 IORegistry：

```text
IOService
└── AppleHapticsSupport
    └── AppleHapticsSupportCallan
```

当前实现：

```cpp
class AppleHapticsSupportCallan : public IOService
```

当前内核符号表中没有真正的 `AppleHapticsSupport` 父类，只有本项目自己创建的
Callan/UserClient 类。因此 `OSDynamicCast(AppleHapticsSupport, ...)` 不会把
当前 Callan 识别为真机对象。

但不要只做空父类：

- 仅改继承声明不会自动产生会话行为。
- 普通用户态进程不能对内核 C++ 类执行 `OSDynamicCast`。
- 若当前内核没有 `AppleHapticsSupport` 消费者，空父类可能只改变 IORegistry
  形状，不会让系统开始调用 Callan。
- 必须先从 Callan 代码还原父类虚函数、状态和方法签名。

Callan 中已确认存在这些字符串/类名：

```text
AppleHapticsSupport
AppleHapticsSupportCallan
AppleHapticsSupportUserClient
AHSTemperatureReporter
notifyHapticsSessionChange
maybeSendAKCIRequest
AHSFunctionNotifySessionChange
com.apple.ahs.userclientaccess
```

`notifyHapticsSessionChange` 和 `maybeSendAKCIRequest` 是有力线索，但当前
本地证据只证明函数名/元数据存在，尚未证明运行时会调用。

## Callan UserClient 的判断

不能直接下结论说“Callan 不应该有 UserClient”。

真机服务属性明确包含：

```text
IOUserClientClass = AppleHapticsSupportUserClient
DeviceOpenedByEventSystem = Yes
```

kext 中也存在 `AppleHapticsSupportUserClient` 和
`com.apple.ahs.userclientaccess` entitlement。

更准确的判断是：

- UserClient 接口确实存在。
- 它不一定由普通 App 直接打开，可能由系统事件框架在特定时机打开。
- 某个 IORegistry 快照中没有 UserClient 子节点，只说明当时未打开。
- 当前没有观察到 `newUserClient()` 尝试，因此“没人请求”和“请求失败”尚未区分。

真机 Callan 还包含子服务：

```text
AHSTemperatureReporter
└── IOHIDEventServiceUserClient
```

当前桩没有该子服务，也没有 `AppleFirmwareUpdateKext` 子节点。服务树不完整
可能影响后续系统匹配。

## 用户态能力和偏好已基本排除

当前设备卷读取到的真实值：

```text
com.apple.preferences.sounds/effects-haptic = true
com.apple.springboard/ring-vibrate = true
com.apple.springboard/silent-vibrate = true
com.apple.springboard/SBButtonForceFeel = 2
com.apple.keyboard.preferences/KeyboardVisceral = 2
```

因此“系统触感开关关闭”不是当前主要解释。

已从 20H392 二进制确认并写入以下能力修正：

- CoreHaptics `.13`：
  - `CHDefaultHapticDeviceCapability.supportsHaptics`
  - `HapticServerConfig.supportsHapticPlayback`
- UIKitCore `.03`：
  - `UIDevice._feedbackSupportLevel`
  - 文件偏移 `0x23506d8`
  - 原始：`fd 7b bf a9 fd 03 00 91`
  - 补丁：`42 00 80 52 c0 03 5f d6`（返回 2）

注意：这些补丁没有让 UI 请求到达内核。它们可能是必要条件，但不是充分条件。

## 临时测试和已恢复改动

### 系统 App 原生触感探针：失败并已恢复

- 曾临时将 `/Applications/Preferences.app/Preferences` 替换为自行编译的
  UIKit/CoreHaptics 探针。
- App 能进入进程，但白屏后 abort。
- 第一次 UIApplication 探针和第二次直接 `main` 探针都未在 SWEETLOG 中产生
  `HAPTIC PROBE MAIN`，因此不能证明 `main` 已执行。
- 原 Preferences 可执行文件已完整恢复。
- 原文件备份：
  `iphone-x-recon/artifacts/2026-10-29-haptics/Preferences.exec.orig.blocks.bin`

不要在未解决签名/启动约束前继续替换系统 App。

### Settings

- 交接时 Settings 已恢复为原二进制。
- 当前没有系统 App 探针残留。

### UI feedback 能力补丁

- `_feedbackSupportLevel` 补丁仍保留在活动 cryptex 中。
- 原始 4 KiB 块备份：
  `iphone-x-recon/artifacts/2026-10-29-haptics/ui_feedback_block_55918501.orig.bin`
- 若需回退，必须在 TWRP 中根据当前 live catalog 重新解析 extent 后写回。

### `effects-haptic` 偏好

- 当前已经是 true，因此没有写入。
- `enable_system_haptics_pref.py` 的 dry-run 因本地 catalog 的 extents 已过期
  而停止，没有修改设备。

## 重要工具和数据

### 当前源码

- ADT 节点：
  `xnu-gh/scripts/mkadt.py`
- Callan/UserClient 桩：
  `xnu-gh/xnu/patches/0200-sweet-haptics-callan.patch`
- 首次 provider 改动：
  `xnu-gh/xnu/patches/0206-sweet-callan-armio-provider.patch`
- 最终 provider 修正：
  `xnu-gh/xnu/patches/0207-sweet-callan-platform-provider.patch`
- AOP 按钮触感桩：
  `xnu-gh/xnu/patches/0203-sweet-aop-button-haptics.patch`
- AOP selector 1 返回资产数：
  `xnu-gh/xnu/patches/0205-sweet-aop-haptics-assets.patch`

### 当前镜像

最近可启动基线：

```text
xnu-gh/images/normal-aop-assets.img
sha256 ae297a980a01b28438bdfe8ef1fcb5aacc605aa607196e3ad9139cbb091458f5
```

Callan 已注册的测试镜像：

```text
xnu-gh/images/test-callan-platform-binary-keepalive.img
sha256 c05093ee432f899c446ffab6c7543f2881be7a4c78ad541f4a8ee1caf7452852
```

测试内核：

```text
xnu-gh/out/callan-platform-binary/kernel.adsp
sha256 e6052a8df3ff992d3b856b6af1fbd3cfc7bf3bd3b7a07ea882573f5925de3de0
```

该内核是直接在 IOKit personality 明文字符串内做等长空间内压缩改写，将 Callan
provider 改为 `IOPlatformDevice`，机器码和段布局未变。

### 关键日志

```text
xnu-gh/logs/ring-callan-platform-binary.txt
xnu-gh/logs/ring-callan-platform-explicit.txt
xnu-gh/logs/ring-ui-feedback-support.txt
xnu-gh/logs/ring-haptic-probe-crash.txt
xnu-gh/logs/ring-haptic-probe-direct.txt
```

最关键的 Callan 注册行：

```text
xnu-gh/logs/ring-callan-platform-binary.txt:77
```

## 已知构建/仓库状态

- CI 运行 `40891665940` 在 macOS runner 长时间排队后失败/取消。
- 尚未成功构建 0207 的完整 CI 内核。
- `scripts/boot_interactive.sh`、`scripts/boot_ram.sh` 有此前未提交的默认值改动，
  不要误提交。
- `scripts/mkadt.py` 同时包含此前未提交的 haptics 开关和本次节点复制逻辑。
- `out/current-buildtree/src` 是旧构建副本，不能当当前源码真源。
- `xnu/patches` 才是补丁真源。

## HFS 写入注意事项

本地 `iphone-x-recon/hfs/iosroot16.img` 的 catalog 可能过期，文件的 extents
会变化。绝对不要在 Android `/data` 已挂载时直接写底层 `sda33`。

已新增：

```text
iphone-x-recon/hfs/livehfs.py
```

它会先通过 adb 预取当前 live HFS catalog，再解析当前文件 extents。

任何后续原位修改必须：

1. 确认设备是 recovery/TWRP，而不是 Android。
2. 使用 live catalog 解析当前 extents。
3. 备份完整目标 4 KiB 块。
4. `umount /data`。
5. 写后逐字节回读。

此前一次误判设备为 TWRP 时，实际设备已启动 Android。恢复 Settings 前已先
`adb reboot recovery`，进入 recovery 后才完成原文件恢复。

## 推荐下一会话优先级

### P0：还原真实类型和会话协议

1. 在 `callan.text`/`callan.asm` 中定位以下字符串的真实 xrefs：

```text
notifyHapticsSessionChange
maybeSendAKCIRequest
AHSFunctionNotifySessionChange
runAction
AHSCommandQueue
```

2. 从反汇编还原：

```text
AppleHapticsSupport 父类虚函数和状态
AppleHapticsSupportCallan 的会话回调签名
AHSTemperatureReporter 子服务创建路径
AppleHapticsSupportUserClient 的真实 selector 协议
```

3. 只有签名确认后再实现最小兼容父类；不要先写一个空
   `class AppleHapticsSupport : public IOService` 就宣称修复。

### P1：增加可区分诊断

在 Callan 及其父类中记录：

```text
start
registerService 返回值
完整 OSMetaClass 继承链
provider 名称/类名
newUserClient 每次尝试和错误码
notifyHapticsSessionChange
maybeSendAKCIRequest
AHSTemperatureReporter 创建/启动
```

这样可区分：

```text
系统没请求
请求了但 UserClient 创建失败
内核会话到达 Callan
Callan 收到命令但未映射到 HAP520
```

### P2：补服务树

优先评估：

- `AppleHapticsSupport` 兼容父类。
- `AHSTemperatureReporter : IOHIDEventService` 子服务。
- 真实 UserClient 外部方法表。
- 是否需要 `AppleFirmwareUpdateKext` 兼容子节点。

### P3：真实触发

建议的确定性触发器：

- 时钟 App 的计时器分钟滚轮。
- 控制中心手电筒长按/切换。
- 长按桌面图标。
- 键盘触感。
- 相机屏幕快门。

不要再用替换 Settings 可执行文件的方式测试，除非先解决系统 App 签名/启动约束。

### 验收标准

最低：

```text
Callan 类链包含 AppleHapticsSupport
AND 出现真实消费者或会话回调
```

有效中间态：

```text
AppleHapticsSupportUserClient open
notifyHapticsSessionChange
maybeSendAKCIRequest
AOP selector 超出启动期 selector 1
```

最终：

```text
真实 UIKit/CoreHaptics 命令
  -> Callan/AOP 协议
  -> SweetHaptics
  -> HAP520
  -> 物理振动
```

## 构建/测试命令参考

构建当前 ADT + 指定内核：

```bash
KERNEL=/Users/huxiaoshi/Desktop/marble-ios/work/xnu-gh/out/callan-platform-binary/kernel.adsp \
D22AP_ADT=/Users/huxiaoshi/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt \
ADSP_FW_DIR=/Users/huxiaoshi/Desktop/marble-ios/work/adsp-fw \
KEEPALIVE=1 CPUS=8 \
./scripts/mkimage.sh
```

设备进入 TWRP 后 RAM-only 启动：

```bash
SERIAL=<device-serial> \
./scripts/boot_interactive.sh \
images/test-callan-platform-binary-keepalive.img
```

测试后回到 TWRP 拉日志：

```bash
SERIAL=<device-serial> ./scripts/pull_logs.sh <label>
```

## 不要重复的无效方向

- 不要继续只补 `compatible`/`device_type`；节点已经存在。
- 不要把 provider 改回 `IOResources`。
- 不要把 provider 改成 `AppleARMIODevice`；当前 SweetPlatformExpert 生成的是
  `IOPlatformDevice`。
- 不要因为 AOP selector 1 存在就认为 Callan 或通用系统触感已经接通。
- 不要把 `registerService()` 当作 UserClient 已打开。
- 不要直接写底层 HFS 而不校验 live extents。
- 不要在 Android `/data` 挂载时写 iOS 根卷所在块。
- 不要用硬件按键、任意 selector 或触摸坐标伪造触感请求。

---

# 进展更新（2026-10-30 续）

## P0 已完成（参考二进制反汇编）

工具与结论在
`iphone-x-recon/artifacts/2026-10-30-callan-abi/findings-abi-2026-10-30.md`。
要点：

- `kernel.bin` 是 fat 容器，thin Mach-O 从 `0x1c` 开始；Callan kext 头在
  `0xf9709c`。旧用的 `__TEXT_EXEC 0x2137000` / `__DATA_CONST 0x2a72d40`
  各低 0x1c，会读出错位数据；正确值为 `0x213701c` / `0x2a72d5c`。
- thin Mach-O 带 `LC_SYMTAB`（5858 个符号）。`refsyms.json` + `a.py` 现在能把
  kext 的每个 `BL → __stubs → __got` 还原成真实符号名（`safeMetaCast`、
  `OSUnserializeXML`、`IOReporter::configureAllReports`、`__ZTV12IOUserClient`…）。
- 类链与尺寸：`IOService → AppleHapticsSupport(0x170) →
  AppleHapticsSupportCallan(0x200)`；`AppleHapticsSupportUserClient(0xe0) :
  IOUserClient`；`AHSTemperatureReporter(0xe8) : IOHIDEventService`。
- 虚表：`slot0/1` 析构对、`slot7 getMetaClass`；IOService 0..167，
  IOUserClient 自 168（`externalMethod`/`initWithTask`）；AppleHapticsSupport
  自有 168..191，其中 174 = `notifyHapticsSessionChange`，175..180 在父类为
  pure virtual、由 Callan 实现。
- UserClient 协议：`externalMethod` selector **0..9**，输入输出是 **XML**
  （`OSUnserializeXML`/`OSSerialize`），键至少包含 `Symbols`、`Values`、
  `AccessOptions`；selector 0 = 符号→地址（服务槽 168，action 0xd593），
  selector 1 = 写 Values（服务槽 169，action 0xdeb5），selector 6 = AOP 符号
  内存布局（`Address/Length/Type/Stride/Begin/End/Granules/Index/Symbol/Offset`）。
  UC `initWithTask` 要求 `com.apple.ahs.userclientaccess == kOSBooleanTrue`。

## P1 已实现并构建通过

`xnu/patches/0208-sweet-callan-parent-diag.patch`：

- 新增真实父类 `AppleHapticsSupport : IOService`，Callan 改为继承它，
  属性在 `registerService()` 之前发布。
- 诊断：provider 名称/类名、完整 `OSMetaClass` 链、`newUserClient` 每次尝试
  与返回码、`callPlatformFunction`（AHSFunction 名字与参数）、
  `notifyHapticsSessionChange`、UC `initWithTask` 的 entitlement 结果；
  UC 的 selector/标量/struct/XML 记录沿用 `SweetHapticsUCBase`。

`xnu/patches/0207-sweet-callan-platform-provider.patch` 原来是坏的：
hunk 头写 7+7 行、实际 6+6，dxb 只打警告并跳过，所以此前 CI 内核里的
personality 一直是 `AppleARMIODevice`。现已按 0206 之后的真实上下文重写。

验证（GitHub Actions，main）：

```text
commit aa00e31  BUILD OK，Verify our patches applied OK
release kernel-latest / kernel.development.sweet 更新于 2026-10-30T04:52:30Z
```

新内核内已确认存在：

```text
SWEET HAPTICS: AppleHapticsSupport::start
SWEET HAPTICS: notifyHapticsSessionChange
SWEET HAPTICS: AHSUC initWithTask
AppleHapticsSupport::newUserClient
com.apple.ahs.userclientaccess
'IOProviderClass' = IOPlatformDevice; 'IONameMatch' = 'haptics-support-interface'
```

已用新内核打出 keepalive 镜像：

```text
xnu-gh/out/kernel.adsp                       （fetch_kernel.sh 下载 + CNTP_CTL 修补）
xnu-gh/images/shim-iosroot-adsp.img          （KEEPALIVE=1 CPUS=8）
```

注意：`/tmp` 之外没有留下旧镜像；`images/test-callan-platform-binary-keepalive.img`
仍是旧的二进制 personality 镜像，可作为回退。

## 下一步（需要 marb​le 实机）

1. 把 marble 接上并进入 TWRP，RAM-only 启动新镜像：

```bash
SERIAL=<marble-serial> ./scripts/boot_interactive.sh \
    images/shim-iosroot-adsp.img
```

2. 在 SWEETLOG 里区分四种情况（这正是 P1 的目的）：

```text
AppleHapticsSupport::start / chain ...        -> 父类重建生效
AppleHapticsSupportCallan::start provider=... -> 匹配到 IOPlatformDevice
AppleHapticsSupport::newUserClient type=...   -> 系统来请求 UC
AHSUC initWithTask ... entitlement=...        -> UC 真的打开，entitlement 有无
```

3. 若出现 UC 打开但没有 selector：说明消费者在前置步骤（XML/属性）就放弃，
   用 `SweetHapticsUCBase` 打出的 XML 前 256 字节复刻 selector 0/1/6。
4. 触发顺序仍按 P3：时钟分钟滚轮 → 控制中心手电筒长按 → 长按桌面图标 →
   键盘 → 相机快门。不要替换系统 App，不要用按键/坐标伪造请求。

---

# 进展更新（2026-10-30 第二轮：实机 P1 与用户态 gate）

## 已完成并在实机验证

- `0207` 修好（原来 hunk 计数错误，CI 一直静默跳过 → personality 仍是
  `AppleARMIODevice`）；`0208` 新增真实父类 `AppleHapticsSupport`；`0209`
  让触感日志直写 SWEETLOG ring；`0210` 把 AOP 资产数改为 boot-arg 可配。

实机 ring 已确认（`logs/ring-callan-p2/p3/p4/p5.txt`）：

```text
AppleHapticsSupportCallan::start provider=haptics-support-interface(IOPlatformDevice)
AppleHapticsSupport::start … self=AppleHapticsSupportCallan
AppleHapticsSupport chain AppleHapticsSupportCallan<-AppleHapticsSupport<-IOService<-IORegistryEntry<-OSObject
AppleHapticsSupport::start registerService done class=AppleHapticsSupportCallan
```

`0209` 之后早期日志成对出现，证明“直写 ring”通道生效（console 在
`exec bluetoothd` 之后不再输出，旧 ring 只覆盖前 ~160 KiB）。

## 关键否定结论（本轮最重要）

在一次完整会话里（SpringBoard 存活、Clock 与 Settings 都实际启动、音量键/电源键
均投递到 backboardd），内核侧三条触感路径**零请求**：

```text
AppleHapticsSupport::newUserClient            0 次
AppleAOPButtonHapticsServiceUserClient sel=   仅启动时 sel=1
AppleSPUHapticsAudioUC sel=                   无（只有 setProperties GlobalConfig）
SWEET HID: key 0xe9 / 0x30 … (1 clients)      按键确实到达 backboardd
```

即 UIKit/CoreHaptics/ButtonResolver 在**用户态**就没有发起触感请求，gate 不在
Callan、也不在我们补的服务名上。

## SpringBoard TCC 崩溃已修（HFS 写入）

SpringBoard 曾在启动后 SIGABRT：

```text
CRASH SpringBoard reason ns 11 … must contain an NSMotionUsageDescription key …
CRASH SpringBoard reason ns 11 … must contain an NSHomeKitUsageDescription key …
```

修复：给 `/System/Library/CoreServices/SpringBoard.app/Info.plist` 补 16 个
usage 描述键（39 → 55），仍放在原 2 个 4 KiB 块内，只改 catalog fileSize
（4377 → 5916）。脚本与备份：

```text
iphone-x-recon/hfs/fix_springboard_plist.py            （--write / --restore）
iphone-x-recon/artifacts/2026-10-30-callan-ui/sb-info.plist.orig.blocks.bin
iphone-x-recon/artifacts/2026-10-30-callan-ui/sb-info.catalog.orig.block.bin
```

修复后 SpringBoard 稳定，首次启动会询问“家庭 / 体能”两项权限（点允许）。
副作用：壁纸丢失——PosterBoard 只看到 `exec PosterBoard`，没有渲染/崩溃记录，
属于既有用户态问题，暂未定位。

## 正在验证的假设（0210）

真机 `/Library/Audio/Tunings/D22/ButtonHaptics` 有 **18** 个资产
（`Variant{1,2,3}.{Single,Second}Click.{Normal,SemiSoft,Soft}`），而 AOP 桩
对 selector 1（资产数）一直返回实验值 **1**。若 ButtonResolver 因“资产数=1”
提前放弃，就会表现为“只有 sel=1、再无播放 selector”。

`0210` 改为默认返回 18，并可用 boot-arg 扫描：

```bash
./scripts/mkimage.sh ahsassets=18
```

判定标准：音量键之后出现 `AppleAOPButtonHapticsServiceUserClient sel=2/3/…`
即说明按钮路径被打通，可以开始翻译成 HAP520 脉冲。

## 下一步

1. 用 0210 复跑音量键实验；若出现新 selector，按真实协议实现播放。
2. 若仍只有 sel=1，则转向用户态取证：ButtonResolver/AOPHaptics 的二进制在
   dyld 共享缓存的 `.dylddata` 里已定位（路径表偏移 271225 / 319777，sda33
   块 56232577 起），下一步提取镜像并反汇编 gate 条件。
3. 壁纸/PosterBoard 单独排查，不影响触感主线。

## 追加结果（p6：资产数 18 无效）

`0210` 实测（ring `logs/ring-callan-p6.txt`，覆盖 725 s，音量键 0xE9/0xEA 与电源键
0x30 均到达 backboardd）：

```text
SWEET HAPTICS: asset count 18
AppleAOPButtonHapticsServiceUserClient sel=1   ← 仍然只有这一个
```

结论：资产数不是 gate。ButtonResolver 拿到 18 后依旧不发播放命令。

## 用户态取证进展

从 dyld 共享缓存（`.01` 子缓存，sda33 块 0x23e2e9 起）拿到关键符号：

```text
CoreHaptics:      CHHapticServerInterface / CHHapticClientInterface /
                  CHHapticDeviceCapability / CHHapticPatternPlayer
BackBoardServices: BKSButtonHapticsController / BKSButtonHapticsDefinition
AOPHaptics/BR:    loadButtonHapticOfType:atSlot:error:
                  loadButtonHapticOfType:withParameters:atSlot:error:
                  hapticAssetType:hasAudio:hasHaptic:error:
                  hapticAssetURLForOutputEvent:
```

`hapticAssetURLForOutputEvent:` 与 `hasAudio:hasHaptic:` 说明按钮触感是**按资产
URL 加载后经音频通道播放**（资产就是 D22/Haptics/*.wav 与 ButtonHaptics/*/*.wav），
与 `AppleSPUHapticsAudioDriver` 的 haptic-audio 通道一致。这解释了为什么内核
UC 只看得到 `sel=1` 和 `GlobalConfig`：播放侧根本没走到 IOKit。

## 当前判断

1. Callan 路径：类链、服务、UC 全部就绪，但**这个 iOS 用户态没有任何组件会去
   打开 `AppleHapticsSupportUserClient`**（根卷 grep 无 entitlement 引用，
   ring 全程无 `newUserClient`）。不作为主攻方向。
2. UI/按钮触感：gate 在用户态的 haptic-audio 链路（ButtonResolver/AOPHaptics →
   CoreHaptics → 音频/HapticServer）。要做下去需要：
   - 反汇编 `BKSButtonHapticsController` / `AOPHaptics` 找出“何时认为可播放”；
   - 或补全 `AppleSPUHapticsAudioDriver` 的 haptic-audio 端点（与 ADSP 音频
     主线耦合，成本更高）。
3. 壁纸（PosterBoard）是既有用户态问题，与触感无关。

## 本轮新增补丁

```text
0208  AppleHapticsSupport 父类 + P1 诊断        （已合入 main）
0209  sweet_log_append 直写 ring（extern "C"）  （已合入 main）
0210  AOP 资产数默认 18 + ahsassets=N           （已合入 main）
```

## p7：共享缓冲假设也被排除

`0211` 在客户端 map 后挂 1 s 轮询：

```text
AppleAOPButtonHapticsServiceUserClient buffer watch on
buffer len=64 00 00 …（仅首次基线）
```

整个会话那块 64 KiB 映射缓冲**没有任何写入**，`sel=1` 之后也没有新 selector。
三条“刚好耦合”的候选（Callan UC / AOP-SPU selector / 共享缓冲）全部为空，
gate 确认在 backboardd/ButtonResolver 的用户态判定。

## MobileGestalt 路线（未取到键）

从 dyld `.01` 子缓存检索：`libMobileGestalt.dylib` 在 182372（路径），
但该镜像与其扩展里**没有**任何 haptic/vibrate 命名的能力名，无法直接对上
`homeButtonHapticKind` 这类判断。若继续此路线，需要先解析 MG 的 4 字符键表。

## 建议的下一步（P2）

反汇编 `BKSButtonHapticsController`：

1. 镜像已在 `.01` 子缓存（`/tmp/sc01.bin` 本地副本），类名与选择子已定位；
2. 需要按 chained-fixup 解码 `class_ro_t`，取 `baseMethods`；
3. 优先看 `loadButtonHapticOfType:withParameters:atSlot:error:`、
   `hapticAssetType:hasAudio:hasHaptic:error:`、`hapticAssetURLForOutputEvent:`；
4. 找到提前返回的分支（无定义 / 无 slot / 能力位为假），再做针对性修补。

---

# 触感线暂停（2026-10-30 收尾）

**决定：暂停触感，转 GPU。** 理由：UI 触感在 D22 上走 haptic-audio
（`CHHapticEngine → AppleSPUHapticsAudioDriver → AOP/DSP`），与音频主线共用
“DSP 读共享内存冻结”这个阻塞；按钮触感是 iPhone 7/8 的 Home 键特性，D22 没有。
继续 RE 只能拿到协议描述，拿不到播放。

**保留**：补丁 0207–0214（诊断能力，不进入其他子系统）、ring 日志、
`docs/haptics-abi-2026-10-30.md` 第 9–11 节的继续点（chained-fixup 反查
`GlobalForceFeel` 引用）。音频主线推进后可直接回来接。

**GPU 现状（已有结论，勿重复）**：见 `docs/gpu-probe.md` —— kgsl-smmu 全局
寄存器可读、SMR/S2CR 可写，但 **context bank（0x80000+）一碰整机挂死**，
CBAR 写也会掉 USB。缺的是 **GPUCC/GMU 上电序列**，属于驱动移植量级，
不是再加探针能解决的。
