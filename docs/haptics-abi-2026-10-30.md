# Haptics：Callan ABI 与 UI 触感 gate（2026-10-30）

配套文档：`docs/haptics-handoff-2026-10-30.md`（逐轮时间线与操作手册）。
本文是**结论与证据的汇总**，工具与原始数据在
`iphone-x-recon/artifacts/2026-10-30-callan-abi/`（私有，不入库）。

---

## 1. 参考二进制与地址修正

- `iphone-x-recon/artifacts/kernel.bin` 是 **fat 容器**（magic `cafebabe`），
  thin Mach-O 从 `0x1c` 开始；Callan kext 的 thin 头在 `0xf9709c`。
- 旧用的 `__TEXT_EXEC 0x2137000` / `__DATA_CONST 0x2a72d40` 各**低 0x1c**，
  会读出错位虚表。正确值：`0x213701c` / `0x2a72d5c`。
- thin Mach-O 带 `LC_SYMTAB`（5858 个符号）。`refsyms.json` + `a.py` 现在能把
  kext 的每条 `BL → __stubs → __got` 还原成真实符号名，是后续全部结论的基础。

## 2. Callan kext ABI（已完整还原）

类链与尺寸：

```text
IOService (168 个虚函数)
  └─ AppleHapticsSupport           0x170, 自有虚表槽 168..191
       └─ AppleHapticsSupportCallan 0x200
AppleHapticsSupportUserClient      0xe0  : IOUserClient
AHSTemperatureReporter             0xe8  : IOHIDEventService
AHSCommandQueue 0x1a8 / AHSCal 0x1f0 / AHSCalCallan 0x1f8
AHSFunctionBase 0x38 + 6 个子类（NotifySessionChange/NotifyWatchdog/
SetAveragePowerBudget/SetPeakPowerBudget/SetTravelScaler/SetHapticDebugChannels）
```

虚表规则（iOS，`APPLE_KEXT_VTABLE_PADDING=0`）：`[0][1]` 析构对、`[7]`
`getMetaClass`；IOService 占 0..167，IOUserClient 自 168 起
（168 `externalMethod`、170 `initWithTask`、172 `free`）。

父类自有槽位（关键）：

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
| 6 | AOP 符号内存布局（`Address/Length/Type/Stride/Begin/End/Granules/Index/Symbol/Offset`） |

`initWithTask` 要求 `com.apple.ahs.userclientaccess == kOSBooleanTrue`，
否则直接失败。

## 4. 实机验证（P1，已完成）

内核侧改动（main）：

```text
0207 provider=IOPlatformDevice（原 hunk 计数错误一直被跳过）
0208 AppleHapticsSupport 父类 + 诊断
0209 触感日志直写 SWEETLOG ring（extern "C"）
0210 AOP 资产数默认 18 + ahsassets=N
0211 UC 共享缓冲 1 s 变化监听
```

ring 确认：

```text
AppleHapticsSupport chain AppleHapticsSupportCallan<-AppleHapticsSupport<-IOService<-IORegistryEntry<-OSObject
provider=haptics-support-interface(IOPlatformDevice)
asset count 18
```

`0209` 之后早期日志成对出现，说明直写 ring 生效；否则 console 在
`exec bluetoothd` 后不再输出，ring 只覆盖前 ~160 KiB。

## 5. 决定性否定结果

完整会话（SpringBoard 存活、Clock/Settings 实际启动、音量键与电源键都投递到
backboardd、ring 覆盖 725 s）中：

```text
AppleHapticsSupport::newUserClient        0 次
AOP UC  sel=                              仅启动时 sel=1（18 与 1 结果相同）
SPU UC  sel=                              无；只有 setProperties GlobalConfig
64 KiB 映射缓冲                           全程全 0，从未写入
```

结论：**用户态在 backboardd/ButtonResolver 内部就放弃了**，三条内核通路
（Callan / AOP / haptic-audio）都还没轮到。Callan UC 更彻底：iOS 根卷内没有
任何二进制引用 `com.apple.ahs.userclientaccess`，即没有消费者。

## 6. SpringBoard TCC 崩溃（已修，HFS 写）

```text
CRASH SpringBoard reason ns 11 … must contain an NSMotionUsageDescription key
CRASH SpringBoard reason ns 11 … must contain an NSHomeKitUsageDescription key
```

修法：给 `SpringBoard.app/Info.plist` 补 16 个 usage 键（39→55），保持在原
2 个 4 KiB 块内，只改 catalog `fileSize`（4377→5916）。备份与回滚：

```text
iphone-x-recon/hfs/fix_springboard_plist.py
iphone-x-recon/artifacts/2026-10-30-callan-ui/sb-info.plist.orig.blocks.bin
iphone-x-recon/artifacts/2026-10-30-callan-ui/sb-info.catalog.orig.block.bin
```

副作用：壁纸消失（PosterBoard 只有 `exec`，无渲染/崩溃记录），属既有用户态问题。

## 7. 用户态取证（下一阶段的钥匙）

dyld 共享缓存 `.01` 子缓存（sda33 块 0x23e2e9 起）里的符号：

```text
BKSButtonHapticsController / BKSButtonHapticsDefinition
loadButtonHapticOfType:atSlot:error:
loadButtonHapticOfType:withParameters:atSlot:error:
hapticAssetType:hasAudio:hasHaptic:error:
hapticAssetURLForOutputEvent:
CHHapticServerInterface / CHHapticDeviceCapability / CHHapticPatternPlayer
```

说明按钮/UI 触感是**按资产 URL 加载后走 haptic-audio**（资产为
`/Library/Audio/Tunings/D22/**/*.wav`，ButtonHaptics 共 18 组），与
`AppleSPUHapticsAudioDriver` 对应；播放侧根本不会碰 IOKit selector。

MobileGestalt 路线：`libMobileGestalt.dylib` 在 `.01` 子缓存，但其镜像内
**没有** haptic/vibrate 命名的能力名，无法直接对应 `homeButtonHapticKind`。

## 8. 下一步

反汇编 `BKSButtonHapticsController`：按 chained-fixup 解析 `class_ro_t` →
`baseMethods`，重点看上面三个 `loadButtonHaptic*` 与
`hapticAssetType:hasAudio:hasHaptic:` 的提前返回分支，找出“无定义 / 无 slot /
能力位为假”的判定，再做定点修补或补服务。

## 9. dylibsTrie 解析进度（未完成，下一会话接着做）

主缓存头（sda33 块 0x23e289，384 KiB）解析结果：

```text
mappingOffset=0x200 mappingCount=1
imagesOffset/imagesCount = 0/0（旧字段未用）
dylibsImageArrayAddr = 0x1a4610000
dylibsTrieAddr       = 0x1a46251b0   trieSize = 0x15530
```

按子缓存 `.11`（sda33 块 0x26099d，映射 VA 0x1a25e4000，fileoff 0）换算，
trie 应落在该文件偏移 `0x20411b0`；但实际字节是 **ARM64 代码**
（`e0 03 00 91 1f ec 7c 92 …`），说明 VA→file 换算或头部字段语义还有一处不对。

下一步排查顺序：

1. 确认 `dylibsTrieAddr` 是 **未滑动偏移** 还是 VA（可能需要加
   `dyldBaseAddress` 或按 `sharedRegionStart` 换算）；
2. 确认 `.11` 的 mapping 是否只有一条（`mappingCount=1`）以及
   `fileOffset` 是否 0；
3. 若 trie 确实在别的子缓存，用「每个子缓存头 + mapping」逐个比对 VA 区间
   再定位（脚本方法已在会话中验证可行：先 dd 前 8 KiB 解析 mapping）。

定位到 trie 后的剩余步骤（已写入第 8 节）：解 trie → `image_info.address`
→ 该 VA 所在子缓存的 mach-o 头 → `LC_SEGMENT_64` → 定位
`__objc_classname` 的 `BKSButtonHapticsController` → 解 `class_ro_t`
（chained-fixup）取 `baseMethods` → 反汇编三个 `loadButtonHaptic*` 与
`hapticAssetType:hasAudio:hasHaptic:`。

### 9.1 追加发现（同一会话）

- `.01` 子缓存里 `BKSButtonHapticsController` 字符串只有一处：文件偏移
  `0x1e5e630`；
- 它前面最近的可解析 mach-o 头在 `0x1b78000`，`LC_ID_DYLIB` 是
  `/System/Library/Frameworks/CoreServices.framework/CoreServices`（ft=6），
  该字符串落在 CoreServices 的 `__LINKEDIT` 段内（符号表区），
  **不是 BackBoardServices 的 `__objc_classname`**；
- 也就是说 BackBoardServices 镜像不在 `.01` 的这段范围（或其 `__TEXT`
  采用不同映射），下一步应先在全部子缓存里定位
  `.../BackBoardServices.framework/BackBoardServices` 的 **LC_ID_DYLIB 拷贝**
  （不是符号池拷贝）：在候选子缓存中搜索该字符串，再检查其前面是否紧跟
  `0xfeedfacf` 且 `LC_ID_DYLIB` 指向它。

## 10. SPU 属性 dump（p8–p10）

```
AppleSPUHapticsAudioDriver setProperties count=2
  GlobalConfig    = dict(1)     ← getCount()==1，但迭代器取不到任何键
  GlobalForceFeel = 1 (0x1)     ← 用户态强制触感为开
```

- `GlobalForceFeel = 1`：用户态没有关闭触感，配置看起来是“启用”。
- `GlobalConfig` 声明有 1 个条目，但 0213（接受 OSSymbol/OSString 键）与 0214
  （任何键类型都打印元类名/字节，取值走 OSMetaClassBase 重载）都**没有任何子行输出**
  → 说明 `OSCollectionIterator::withCollection(sub)` 对这个字典返回的迭代器
  取不到对象（或返回了 NULL，被 `it != NULL` 静默跳过）。
- 该 UC 之后仍然只有启动期这一次 setProperties，没有任何 selector。

### 下一步（下轮直接做）

1. 在 dict 分支显式记录 `it == NULL` 与否，以及 `withCollection` 前后
   `getCount()`；
2. 若迭代器为空但 count=1，改用 `sub->copyCollection()`/直接按已知键名探测
   （`getObject("…")` 无法枚举，可先用 `OSCollection::getCount` 对照）；
3. 反查 backboardd 里构造 `GlobalConfig` 的代码（字符串 "GlobalConfig" 在
   `.01` 子缓存可搜到），看它把什么键塞进这个字典——这比在核心里猜更快。

## 11. GlobalForceFeel 的构造方已定位（ButtonResolver）

- 全子缓存扫描：`"GlobalForceFeel"` 仅命中一处，子缓存 `.19`（sda33 块
  0x2a729d），文件偏移 `68928953`；
- 该处前面 256 KiB 内解析到的 mach-o 头在 `0x41b1000`，`LC_ID_DYLIB` 为
  `/System/Library/PrivateFrameworks/ButtonResolver.framework/ButtonResolver`；
- 镜像段布局：

```text
__TEXT       vm 0x1f0e31000  fileoff 0x41b1000  size 0xe000
__DATA_CONST vm 0x20dc0d158  fileoff 0xdc1158
__LINKEDIT   vm 0x217040000  fileoff 0x5c000
```

- 字符串本身位于 `__TEXT` 内（abs 0x41bc5b9 → vm 0x1f0e3c5b9），即
  `GlobalForceFeel`/`GlobalConfig` 的属性名由 **ButtonResolver** 构造。

### 未完成：反汇编该镜像

第一次按 `fileoff 0x41b1000` 取 56 KiB 时，头部之后是一整片 0（capstone 全
`udf #0`），说明该镜像的段在缓存里不是从 header 起连续排布（或 dd 的块换算
需按 mapping 而非段 fileoff）。下一步：

1. 用 `.19` 子缓存头的 mapping 表（address/size/fileOffset）把
   `__TEXT` 的 **vm 0x1f0e31000** 映射到文件偏移，而不是用 `LC_SEGMENT.fileoff`；
2. 重新取 `__TEXT`（0xe000）后，用 capstone 找 `adrp/add` 指向
   `0x1f0e3c5b9` 的引用，反汇编其所在函数，看 `GlobalForceFeel`/`GlobalConfig`
   在什么条件下设置、之后等待什么；
3. 由此决定是在驱动侧补齐期望的响应，还是需要改 ButtonResolver 的判定。

### 11.1 反汇编进展

- `.19` 子缓存 mapping：VA `0x1ecc80000`、size `0x62ac000`、fileoff 0 →
  `__TEXT` vm `0x1f0e31000` 对应文件偏移 `0x41b1000`（与 LC_SEGMENT 一致）；
- 取回 56 KiB 后确认：`0x1000` 是段间填充，`0x2000` 起是真实代码
  （`04 02 80 52 …`），字符串位于 `0xb5b9` 处，上下文为
  `"…SetProperty:forKey:]\0GlobalForceFeel\0StateBut…"`；
- 两种 `adrp/add` 规则（rd==rn 与 rn==adrp_rd）扫描均未命中该字符串地址 →
  引用很可能通过 `__DATA_CONST` 里的常量指针（chained-fixup 编码），或由
  另一段的代码加载。
- 下一步：解 `__DATA_CONST` 的 chained-fixup 指针，找指向
  `0x1f0e3c5b9` 的 rebase 目标，再回溯引用它的函数。
