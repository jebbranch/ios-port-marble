# Haptics：Callan ABI 与当前 gate

> **当前状态（2026-11-28）**：输出侧已解决——AP 侧 SPMI 直驱 PMIC HAP520
> 44/44 次有震感；按键与开机震动已解决。剩下的缺口在用户态：
> UIKit / CoreHaptics 的请求被 `backboardd` 的 gate 拦下，内核各路径看不到 IPC。

本文保留 ABI、决定性证据和下一阶段入口；逐轮时间线与操作手册已合并进本页。

## 1. 参考二进制与地址修正

- `iphone-x-recon/artifacts/kernel.bin` 是 fat 容器（magic `cafebabe`），
  thin Mach-O 从 `0x1c` 开始；Callan kext 的 thin 头在 `0xf9709c`。
- 正确段地址：`__TEXT_EXEC 0x213701c`、`__DATA_CONST 0x2a72d5c`；
  旧值各低 `0x1c`，会读出错位虚表。
- thin Mach-O 带 `LC_SYMTAB`（5858 个符号）；`refsyms.json` + `a.py`
  能把每条 `BL → __stubs → __got` 还原成真实符号名。

## 2. Callan kext ABI

```text
IOService (168 个虚函数)
  └─ AppleHapticsSupport           0x170, 自有虚表槽 168..191
       └─ AppleHapticsSupportCallan 0x200
AppleHapticsSupportUserClient      0xe0  : IOUserClient
AHSTemperatureReporter             0xe8  : IOHIDEventService
AHSCommandQueue 0x1a8 / AHSCal 0x1f0 / AHSCalCallan 0x1f8
AHSFunctionBase 0x38 + 6 个子类
```

虚表规则（iOS，`APPLE_KEXT_VTABLE_PADDING=0`）：`[0][1]` 析构对、`[7]`
`getMetaClass`；IOService 占 0..167，IOUserClient 自 168 起。

| slot | 含义 |
|---|---|
| 168 | 符号→地址查询（异步 action 0xd593） |
| 169 | 写 Values（action 0xdeb5） |
| 174 | `notifyHapticsSessionChange` |
| 175–180 | 父类 pure virtual，Callan 实现（AOP 协议） |

## 3. UserClient 协议

`externalMethod` selector **0..9**，输入输出都是 **XML**
（`OSUnserializeXML` / `OSSerialize`）：

| sel | 语义 |
|---|---|
| 0 | 输入 XML → `Symbols`(OSArray)+`AccessOptions`(OSNumber) → 服务槽 168 查询 → 序列化回 structureOutput |
| 1 | 输入 XML → `Values` + `AccessOptions` → 服务槽 169 |
| 6 | AOP 符号内存布局（Address/Length/Type/Stride/Begin/End/Granules/Index/Symbol/Offset） |

`initWithTask` 要求 `com.apple.ahs.userclientaccess == kOSBooleanTrue`。

## 4. 内核侧已验证

```text
0207 provider=IOPlatformDevice（原 hunk 计数错误一直被跳过）
0208 AppleHapticsSupport 父类 + 诊断
0209 触感日志直写 SWEETLOG ring
0210 AOP 资产数默认 18 + ahsassets=N
0211 UC 共享缓冲 1 s 变化监听
```

ring 确认类链、provider 与 asset count 18。`0209` 之后早期日志成对出现，
证明直写 ring 生效。

## 5. 决定性否定结果：用户态在 backboardd 内部就放弃

完整会话（SpringBoard 存活、Clock/Settings 实际启动、音量键与电源键都投递到
backboardd、ring 覆盖 725 s）中：

```text
AppleHapticsSupport::newUserClient        0 次
AOP UC  sel=                              仅启动时 sel=1
SPU UC  sel=                              无；只有 setProperties GlobalConfig
64 KiB 映射缓冲                           全程全 0，从未写入
```

结论：**用户态在 backboardd/ButtonResolver 内部就放弃了**，三条内核通路
（Callan / AOP / haptic-audio）都还没轮到。Callan UC 更彻底：iOS 根卷内没有
任何二进制引用 `com.apple.ahs.userclientaccess`，即没有消费者。

## 6. 输出侧已解决（44/44）

- AP 侧 SPMI 直驱 PMIC HAP520，**44/44 次有震感**，震感与设置一致。
- `fault during play=0x2` 由本次脉冲产生，清除后为 0，与「没动」无关。
- `auto_res` 与 DT `lra-f0-*` 不是同一字段，比较永久停用。
- 按键与开机震动正常；设置里的「声音与触感」、键盘反馈、响铃/静音振动开关都已出现。

⇒ 早期「直驱从没让马达动过、B4 与音频是同一堵墙」的结论已撤回。

## 7. SpringBoard TCC 崩溃（已修）

```text
CRASH SpringBoard reason ns 11 … must contain an NSMotionUsageDescription key
CRASH SpringBoard reason ns 11 … must contain an NSHomeKitUsageDescription key
```

修法：给 `SpringBoard.app/Info.plist` 补 16 个 usage 键（39→55），
保持在原 2 个 4 KiB 块内，只改 catalog `fileSize`（4377→5916）。
副作用：壁纸消失（PosterBoard 只有 `exec`，无渲染/崩溃记录），属既有用户态问题。

## 8. 下一阶段：用户态 gate

已定位的符号与资产：

```text
BKSButtonHapticsController / BKSButtonHapticsDefinition
loadButtonHapticOfType:atSlot:error:
loadButtonHapticOfType:withParameters:atSlot:error:
hapticAssetType:hasAudio:hasHaptic:error:
hapticAssetURLForOutputEvent:
CHHapticServerInterface / CHHapticDeviceCapability / CHHapticPatternPlayer
```

按钮/UI 触感按资产 URL 加载后走 haptic-audio（资产为
`/Library/Audio/Tunings/D22/**/*.wav`，ButtonHaptics 共 18 组），
与 `AppleSPUHapticsAudioDriver` 对应；播放侧不碰 IOKit selector。

`GlobalForceFeel` / `GlobalConfig` 的属性名由 `ButtonResolver.framework` 构造。
下一步是解 `BKSButtonHapticsController` 的 `class_ro_t → baseMethods`，
反汇编三个 `loadButtonHaptic*` 与 `hapticAssetType:hasAudio:hasHaptic:` 的提前返回分支，
找出「无定义 / 无 slot / 能力位为假」的判定，再做定点修补或补服务。

## 9. 约束

- 只允许 `fastboot boot`。
- 不得用坐标 / selector / 内核定时器模拟脉冲。
- 测试必须来自真实 UIKit、CoreHaptics、ButtonResolver 或系统 HID 事件。
- 当前暂停；恢复前先读 [`STATUS.md`](STATUS.md) 与 [`chronology.md`](chronology.md)。
