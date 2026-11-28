# marble / XNU 编年史 —— 唯一按时间排列的记录

> 本页是项目从立项到当前的**唯一编年史**。其它文档只按主题保留判决与证据，
> 不再各自记日期；要改日期就改这里。
> 10-21 → 10-28 的时刻来自当时的会话记录（逐条带时间戳），10-29 之后来自提交时间戳、CI 构建记录与当时的交接文档。

---

## 0. 一页速览

| 日期 | 那一刻 |
|---|---|
| 2026-10-21 15:58 | 提问：「红米 Note 12 系列能刷 iOS 吗？」回答是不可能 |
| 2026-10-21 23:19 | 屏幕打出 `X2 RAMDISK: userland alive` |
| 2026-10-22 10:39 | UFS 设备醒了，XNU 读出分区表 |
| 2026-10-23 01:05 | Apple logo + 进度条：iOS 在引导 |
| 2026-10-23 10:21 | 进 Setup |
| 2026-10-23 15:11 | 进桌面 |
| 2026-10-27 17:24 | 8 核开起来（在音频攻坚中间顺手做的） |
| 2026-10-30 | 按键直驱真能震；触感接系统这条线暂停 |
| 2026-11-02 | 音频数据面打通 |
| 2026-11-03 | mediaserverd 来 open 我们的壳 |
| 2026-11-07 | 图内容判死；GPU 转冻结 |
| 2026-11-15 | 定位到功放没有 TDM 时钟；声音仍未证明 |
| 2026-11-20 | 引入 FoXLinne marble 参考；Wi-Fi 优先，GPU 第二 |
| 2026-11-20 23:12 | Wi-Fi 第 74 轮只读 handoff 通过：QCA6490/PCIe0 交接后保持关闭；未验证端点 |
| 2026-11-23 18:58 | Wi-Fi 第 75 轮镜像冻结（未执行）；0302/0303 随后改为整机集成路线 |
| 2026-11-28 | 0302/0303 把无线栈、固件、DMA/SMMU 核心离线接进同一个对象；尚未整机启动 |

---

## 1. 2026-10-21 · 从一句「不可能」到 userland alive

- 15:58 提问；得到的回答是**不可能**：iOS 不开源、Secure Boot、SHSH 签名、Secure Enclave，只认 A 系列芯片。
- 16:05 决定动手：换了 12+1TB 之后，原来那台 16+256 空出来做研究机；先备份，再适配 XNU、提取 IPSW。
- 18:14 路线定为 **shim + CPU 软件渲染**；UEFI + Vulkan 接管 Metal 那条更难，暂缓。
- 18:34 本地工作目录建立。
- 18:41–19:02 采集设备事实（公开内核树没有设备 DTS，地址从真机 live FDT 取）；设备进 fastboot。
- 19:05 第一次 shim 上机：屏幕乱码、字体倾斜不可读、有斜条纹，先红页后蓝页，然后重启。
- 19:09 修复版 shim 的诊断画面拍到了：`MMFR0.tg16=1`（16 KB 页）、boot CPU = Cortex-A510（MIDR `0x410FD463`）、GICv3 + VTIMER INTID 27（30 次中断）、SGI1（10 次）、framebuffer `0xb8000000` + stride **4320**。同一条结论：`CNTP_CTL_EL0` 在 EL1 不可用，XNU 只能走 CNTV。
- 19:18–19:25 建仓、走 SSH key 推送。
- 19:31 **CI run #1 开始**并成功；19:51 下载产物 `kernel.development.sweet`（17,206,056 B）。本机磁盘不够，所以内核构建从第一天就交给 Actions。
- 22:55–23:13 X1：XNU 早期 console 正常，依次打出 `zone_init`、`VM bootstrap done: 85 maps`、OSLog/Firehose、`mig_table_max_displ`、`mach_kobj_count`、`kdp_core zlib memory 0x8000`。
  - 23:00 那次 panic 是 `Undefined kernel instruction`，pc=`0xfffffe0007491348` instr=`d51be220` —— 就是 `fiq_context_init` 里的 `msr CNTP_CTL_EL0, x8`，NOP 掉之后继续。
  - 23:13 终点是 `bsd_init` 找不到 root media → 写 `boot-picker-bringup-reason` 到 NVRAM 失败（marble 无 NVRAM）→ `waitForService("IODTNVRAM")` 永久等待。这是无 root 时的**预期停点**，不是 panic。
- 23:16 改试 ramdisk。**23:19 屏幕打出 `X2 RAMDISK: userland alive`**：16 MB HFS+ 最小根由 shim 以 `rd=md0` 直接带进内存，里面只有一个静态编译的 `/sbin/launchd`（34 KB），它完成了 `open("/dev/console")` + `write()`。**此时 UFS 还没醒**——这一步完全不碰磁盘。

## 2. 2026-10-22 · UFS 醒过来，以及 marker 门的由来

- 09:59 提出问题：XNU 不识别 UFS 怎么办。
- 10:09 `SWEET UFS: command 0x00 failed: ocs 0x0 upiu 0x21 response 0x1 status 0x2` + panic。
- 10:39 **UFS 设备唤醒打通**：ABL 交接时设备处于掉电，普通 LUN 的 START STOP 无效，必须向 **W-LUN `0xD0`** 发 `START STOP UNIT`（`cdb[4]=0x10`）。之后 INQ / TUR / CAP / READ / GPT 全部成功：SAMSUNG KLUEG8UHGC-B0E1、4 KiB 块、约 256 GB。
- 10:48–11:10 取内核产物、进 recovery、确认 TWRP 能解密 data。
- 13:26–16:10 备份方案与 SSD 折腾：`tar` 报 short write，磁盘 307 GB 写满。
- 18:58–22:53 文件级备份完成：core 5.03 GiB、appdata 32.50 GiB、media 90.93 GiB、APK 21.15 GiB（22:53 完成，首头 + EOA 校验通过）。
- 23:02 开始向 userdata 尾部写 iOS root；23:42 Mac 死机重启；23:48 / 23:51 取回 `xnu-buildtree` 与 `xnu-kernel`。
- 同一天发现 **0082 的 hunk 行号写成 496（应为 663）导致 CI 静默跳过补丁** —— 这就是 marker 门的起点：每个补丁自带一条 printf 字面量，补丁没打上时红的是 CI，不是一次开机。
- XNU 读 UFS 实测通过：
  ```
  SWEET UFS: "userdata" on LUN 0: first block 0x2f6a00, 229581 MiB, read-only
  BSD root: ufsroot0, major 1, minor 0
  ```
  整块 userdata 是 metadata-encrypted f2fs，`hfs_ValidateHFSPLusVolumeHeader: unknown Volume Signature : 3030` 是预期。
- 注意 `sweet_usb_probe()` 挂在 `sweet_ufs_init()` 末尾，读 sweet（SM7150）的 GCC USB GDSC 在 marble 上 data abort，测试期用 `sweetusb=0 sweetkeys=0 sweettouch=0` 关掉未适配驱动。

## 3. 2026-10-23 · 一天之内：引导界面 → Setup → 桌面

- 00:01 内核与镜像打包完成（0082 字符串齐全、CNTP NOP、payload + shim），尾部写入进行到 4/16 块（1 GiB × 16 块）。
- 00:21–00:45 `XNU did not boot`、进 recovery、进度条滚了然后炸。
- **01:05 屏幕上出现 Apple logo + 进度条** —— `launchd_cache_loader` 过了，launchd 已经在跑用户态引导，不再是内核 panic 屏。
- 02:05–02:07 日志确认：launchd `Early boot complete. Continuing system boot.`、SpringBoard（pid 26）与 backboardd（57）都在运行、framebuffer 交换计数 19.4 万次。停在进度条的原因是等不到系统信号：
  | 缺口 | 日志证据 |
  |---|---|
  | 没有真正的 preboot 卷 | `libbignition: preboot: failed to get preboot device: 25`、`No cookie: /private/preboot/…/.roots-installed` |
  | 没有独立数据卷 | `mount: data volume missing, but not required in env: 0`，`/private/var` 直接落在根 HFS 上 |
  | 无 SEP / 数据保护 | `init_data_protection: No SEP present`、`MKB_INIT: NO CPROTECT` |
  | 前台会话设置失败 | `UMD_INIT: FAILED to Set Foreground ID + multiuser in commpage successfully` |
  | 无 GPU 加速 | backboardd 反复 `IOAcceleratorES -> 0 found` |
  | 无基带 / 蓝牙 / SEP 守护进程 | CommCenter、bluetoothd、fairplayd.H2 秒退，launchd 把它们丢进 crash-loop（`Too many corpses`） |
- 08:44 定策：绕过 `datamigrator`，把激活页面调出来走过场、什么都不做。
- 09:43–10:04 重取镜像、分块重写尾部（标准速度 20–30 MB/s）。
- **10:21 「成了，进 setup 了」** —— 首次进 Setup。随后开始修触摸：这台是 **Goodix GT9916R over SPI**（`spi@990000`、15 MHz、CS0；复位 GPIO20、中断 GPIO21、TLMM 基址 `0xF000000`；供电 PM8350C L2 iovdd 1.8V + L9 avdd），而现成驱动是 sweet 的 GT9896 over I2C @0x5d / SE7 —— 总线、协议、电源、引脚全不一样，改常量救不了，只能新写一段。
- 14:40–14:46 Setup 里触摸可用，能设语言与地区；到快读开始页点手动设置后触摸没反应，锁屏正常。
- **15:11 「进桌面了，但是没有壁纸，锁屏时钟是红色的」** —— `SetupDone` 生效，用户态彻底过了 Setup。壁纸空白与红色锁屏时钟归到 WallpaperKit / 颜色管线（还没有 GPU 合成），不影响使用。
- 15:22 触摸可用但响应偏慢；音量- 无效 → 转去修 USB。
- 傍晚起 ADSP 线开工：LPASS/WCD938x 只读探针 → AOP command DB → 走 TZ PAS 装载与释放 ADSP → 最小 GLINK/GPR 客户端 → 通过 q6prm 投 LPASS。

## 4. 2026-10-24 · 厂商图打开，功放第一次被摸

- 08:18 起继续音频：打开厂商自己的 AudioReach 播放图、往里灌 1 kHz、开始碰功放 I2C。
- 12:20 `status-log.md` 开始按轮次记录（此前的历史就是上面三节）。
- 13:24–14:18 带外 SHM_MAP 语义确认、recovery 直读。
- 19:50–20:18 两次关键纠错：SPMI 基址是错的；**arbiter 是 v7 布局，我们一直按 v5 读**。
- 20:54 run39 panic：另一个 EE 的 chnls 槽会外部中止。
- 21:30 run41：**时钟通了，触觉寄存器全部落地** —— 第一次真的驱动 HAP520 引擎。
- 23:20–23:55 hBoost 修好；GENI 卡在 SE15 时钟 → 引脚功能 → 收到 NACK → 复位正常，随后内核 OOM panic。

## 5. 2026-10-25 · I2C 打通，多核仍是已知 bug

- 00:05 **I2C 打通**，两颗功放读到 `0x2032`；00:15 功放初始化全部完成。
- 00:40 记下「CI 会静默跳过冲突的 patch」。
- 01:05 质疑 shim 只给 iOS 分配 4 GB RAM —— 用户空间很卡。
- 09:2x–13:1x SMMU 一条路走到底：物理地址是真的但地址选错 → vendor 抓包 + FDT 把数据面讲清楚 → run64 低区 PA 被 APM 接受 → SMR 越界读 → SID `0x1800` 本身 → NS 写不进 S2CR.BYPASS → CB6 的 CBAR 写不了（hypervisor 拥有 CB0..51）。
- 10:02 震动后 panic；13:49 按键有震动、但没显示 GUI。
- **13:53 「多核会panic，这是个还没修的bug」** —— `cpus=8` 当时必 panic。
- 14:1x **再次**进到 GUI（Siri 欢迎页）——首次是 10-23；随后又是 io_submap 撑爆。
- 14:05 记下用户态的一堆毛病：密码明明是空的仍要求输入、部分软件打不开。
- 15:2x **run93：根因是 `DATA_BUFFER` 的地址语义，不是 SMMU**。

## 6. 2026-10-26 · SMMU 配好，数据面仍无回应

- 13:0x sACR 假设被证伪，但顺带解决了 CBAR；13:1x **SMMU 配好（重大进展）**，数据面仍无回应。
- 13:3x mask 形式的 SMR 硬件拒绝；13:4x io_submap 又耗尽（这次连 16 KiB 都要不到）。
- 13:5x run84：震动真机确认；CB 页假设被推翻；电池字段错位。
- 18:02–18:32 构建完成；23:08 成了（`pmap_assert_free` 那两轮之后）。

## 7. 2026-10-27 · 八核开起来（顺手），触感接系统开工

- 01:02 「这一套下来，离接入系统发出声音还有多远？」
- 08:48 在教室上课，要求生成的 wav 响度不要太大。
- 11:48 「想要发出声音咋就这么难」→ 把问题整理去问外部模型。
- 16:34 「记得打印调度信息勘错，别开了4core就是一核有难三核围观」
- 17:13 「核心全开试试？」→ 17:24 「8核优化先做一下吧，之前开4核开机速度1分左右，8核反而更慢了」
  ⇒ **GICR stride 改成运行时可配（marble 是 `0x40000`，XNU 默认 `0x20000`）、ADT 里放八个核，8/8 起来**。这是在音频攻坚中间顺手做的。
- 18:54 设备在 GUI 界面但操作卡 —— CPU 软件渲染的瓶颈，且 CPU 一点都不热。
- 19:53 进系统后会自己息屏、注销一下才能用；滑动有时没反应。
- 20:34 「你看看震动能不能接入系统，声音不要了至少震动要有吧」⇒ 触感接系统这条线从这里开始。
- 22:25 电池拔下充电器仍显示充电、电量不更新；关机像没关，屏幕中间一个不转的圈。

## 8. 2026-10-28 · 按键与开机震动正常

- 09:07 电池没反应了；关机成了。
- 10:08 转回震动。
- 12:11 「iPhone 的铃声不是自带震动吗？我在设置里播放没感觉到，**但是按键震动和开机震动正常了**」。
- 12:50 marble 的铃声界面没有震动选项（iPhone X 有）。
- 15:19 设置里「声音」点进去变成「声音与触感」，键盘反馈多了触感选项。
- 17:58 这个会话到此结束，之后换客户端继续（这也是 `status-log.md` 前后风格不一致的原因之一）。

## 9. 2026-10-29 → 11-01

- 10-29 10:13 「还有 8 核 CPU 呢？issue 里有提到吗？没有的话也加进去」。
- 10-29 `hfs: reject invalid mangled` —— HFS `cat_desc` 损坏那条修复的前置。
- 10-30 触感内核侧收口（见下节）；GPU 第一次探针结果（19:00）。
- 10-31 → 11-01 GPU：GMU 固件解码、CM3 释放标记、ABL 交接快照；11-01 写 GPU 交接文档。

## 10. 2026-10-30 · 震动：输出侧解决，系统侧暂停

- AP 侧 SPMI 直驱 PMIC HAP520，**44/44 次有震感**、震感与设置一致 ⇒ 输出侧不再是问题。
- 同一轮修掉 SpringBoard 的 TCC 崩溃：给 `SpringBoard.app/Info.plist` 补 16 个 usage 键（39→55）。
- 剩下的在用户态：UIKit / CoreHaptics 的请求被 `backboardd` 的 gate 拦下，内核各路径看不到 IPC ⇒ 这条线暂停。
- 约束不变：只允许 `fastboot boot`；不得用坐标 / selector / 内核定时器模拟脉冲。

## 11. 2026-11-02 · 音频数据面打通

- 主因不是厂商那 18 KB 校准，而是 cb5 阶段 1 页表叶子 `0x701` 的 `AP=[7:6]=00`（特权访问专用）：DSP 非特权读 `0x1ffe8000` 被 SMMU 终止，于是 `DATA_BUFFER` 一声不响。
- 放到 `0x7c1`（`sweets1ap=3`）之后 `done status=0` 回来、槽位归还与 ping-pong 一致、1200 个 buffer 跑完并正常复位，全程没有新故障位。

## 12. 2026-11-03 · mediaserverd 来 open 我们的壳

- `ring-ep.txt` 里 pid 28 打开我们发的 `AppleAOPAudioUserClient` 并调了 selector 0（`method_Open`）；`IOAudio2DeviceUserClient` 也被 open。
- 线上格式在本机 `libar-gsl.so` 上逐条反汇编复验：记录头 16 B、OOB 包结构、正文 ≤256 走带内、`property_flag = 1 << (mem_pool_id − 1)`。
- SHM 映射的高 32 位不是地址而是 **stream index**；发对之后不再挂总线。

## 13. 2026-11-04 → 11-06

- 11-04 注册 V0–V4 等仪器。
- 11-05 带外 `SET_CFG` 的 E–N 十轮把地址、传输、长度、正文字节逐格排除；N1 的哨兵 `0xa5a5` 证明之前那些 `err=0` 是「没人写过」；run 387 复盘（0261 被 CI 跳过，代价是一次 CI 红、零次开机）。同一天 **Wi-Fi 决定暂缓**、**基带开线**（先做零开机普查）。
- 11-06 TDM 时钟轴（boots 33-36）：`0x204` 投票位置对称性；clamp 到最大 −0.27 dBFS 仍无声；看门狗把等待路径上的 `sweetbufs` 截到 120。

## 14. 2026-11-07 · 图内容判死，GPU 冻结

- 图生成轴离线关闭：2960 = 当代四个 key；旧的 2724 属上一代且自身索引越界 4 B，由它得出的一切结论作废。
- 逐字 dump 两次推翻「图里缺 DEC/CNV」——被接受的 `g_graph_vp_wr` 同时有 WR 与硬件段，真正缺的是 `PCM_DEC 0x40bf` + `PCM_CNV 0x40c0`。
- 856 B A 形式（+DEC/CNV）能开、能起、能推 120 帧，DSP 归还缓冲，**人耳三次都没听到**；`EOS_RENDERED` 在收包仪器被证明活着的前提下从未到达。
- GPU 线转冻结：kgsl-smmu 的 CB 页一碰整机挂死，CX 域电源无法真正打通；绝不主动 collapse GPU。

## 15. 2026-11-08 · 功放两处真实缺陷修好，音频暂停

- 描述符隔离修掉 AP 覆盖 PCM 缓冲的写入路径。
- 0290：正常初始化不再执行诊断复位，RESET 保持 1/1、两颗可读。
- 0291：寄存器地址与 16 位值放进一条三字节 WRITE，两颗实际 reg4/reg5 从 `4003`/`38` 变成表值 `b440`/`05a9`，PWDN/AMPPD/HMUTE 清除。
- 同一 856 B 图 open/start=0、120 tone / 121 DONE、正常复位无 panic —— 这是控制/缓冲归还层证据，**不是末端渲染或出声证明**。

## 16. 2026-11-10 → 11-15 · 从 HAL 捕获到功放没有时钟

- 11-10 音频按用户要求重开，只做离线 capture-only HAL：0292 的节点只入 ring、统计、覆盖旧帧，**不接 DSP/TDM/功放**，也不改任何硬件增益。
- 11-12 HAL 捕获镜像验收；只读重取设备当前 HFS 卷头、catalog 与原插件，核对通过（`free=748380` 块、additional allocation=0）。
- 11-13 第 69 枪取得 524288 帧、500234 非静音帧、peak 32768 ⇒ 只证明**入流**；第 70 枪 HAL → AudioReach 连续流 `attempted/sent/done = 30/30/30`、reject/fault=0、六条 open…unmap 控制应答全 0，安全 teardown 通过。
- 11-14 第 71 枪免费 R1：身份成立但 HAL 全静音，流以 0/0/0 teardown ⇒ 不是电声负读数。第 72 枪**有效无声**：两颗 AW88266 的 profile/ID/vol 都对，清 PWDN 后 `SYSST=0`、PLL 全 0，安全门在任何 period 提交前拒绝并双关断 ⇒ **功放没有 TDM 时钟**。
- 11-15 0297 只在 `sweetlisten=1` 按厂商顺序于图 OPEN 前 request `0x204@12.288MHz attr4 root0`；第 73 枪判为 **PRM 回包解析失配**：应答是 `0x02001002` + `param=0x0800102c`，而 0297 在等 BASIC_RSP `0x02001005`，5 秒 timeout 后安全门不 OPEN、不碰功放、不发 PCM，UNMAP=0 ⇒ 不能判 PRM 接受/拒绝。0298 已离线修好绑定（adapter 56 项含错 opcode/错 param 负对照、功放 10 场景、全 S16 136656 断言、全链 applied 239 通过），**尚无完整内核 / 镜像 / 开机**。

---


## 17. 文档里此前没写的几条（截至 2026-11-18，按设备侧观测记录）

这三条在仓库里没有对应的收口记录，先记在这里，后续有证据再补链接。

- **电池：基本实现。** 充电状态靠**电流的正负号**判定（本机实现为**电流为负 = 充电、为正 = 放电**，见 0193 补丁），
  不再靠某个布尔位；capacity / vbat / temp 按正确单位展示。
  仓库侧最后一条记录还是 11-08 的「字段待校准」，说明这一步做完没写回文档。
- **关机：已解决。** userland 的关机会真正执行**关闭电源**的操作，不再是
  「像没关又像关了、屏幕中间一个不转的圈」。10-27 那次 `CPU 1 caller Halt/Restart`
  看门狗超时是当时未收口的状态。
- **8 核：可用，代价是发热偏大。** 不是「一开就炸」了，但热是真实成本。
- **`空密码仍要求输入`归类为用户态 bug**（10-25 14:05 记录的那批毛病之一），
  不是安全层没关，也不是缺服务。
- **f2fs 与尾部 root 的重叠仍然没有界定，而研究机日常还要开 Android**
  ⇒ 这不是理论风险：安卓写满就会吃掉 root。目前没有任何机制阻止它，
  只有事后发现（重读 HFS+ 卷头签名与 free 块数）。详见
  `../../../docs/08-dual-boot-plan.md` §2。

## 18. 2026-11-20 · 引入 FoXLinne marble 参考，Wi-Fi 优先

- 固定拉取 `FoXLinne/UniXNU_marble` main
  `7a0d68fff2557f12be9f8c47f618fd332b7a3e67`。其 Wi-Fi 有
  QCA6490 PCIe、SMMU/MHI/ath11k、WPA2 与 DHCP 真机证据；GPU 有
  submit/retire 和桌面/锁屏证据，但 Safari、渲染正确性、流畅度与稳定性仍未完成。
- 用户决定 Wi-Fi 与 GPU 都移植，先做 Wi-Fi。iOS 18 / XNU 11417 的 Apple ABI
  不直接覆盖 iOS 16 / XNU 8796；先抽取相同硬件的板级与传输层。
- 0299 第一阶段为 `sweetwifi=1` 纯只读 GCC/TLMM handoff survey：
  只映实际使用的四个 4 KiB 页，不投电源、不写 GPIO/PHY/PCIe、不做 DMA。
  实际源码主机模型、三文件补丁重放及三条全链等价检查通过。
- **11-20 23:12，Wi-Fi 第 1 轮 / 总第 74 轮**：`sweettime=1795187571`，raw 窗口匹配；
  唯一只读 marker 为 `readonly=1 stage=1 gdsc=0x22f001 halted=0x17f wlan=0 perst=0`。
  PCIe/上电 marker 为 0，GLINK/I2C/GPU/GMU/panic 均为 0。结论是
  **QCA6490/PCIe0 在 ABL/TWRP 交接后保持关闭**，只读页映射未引发 panic；
  **没有验证 PCIe 链路、端点或 Wi-Fi**。证据在 `verify/wifi-boot74/`。
- 0300 移植 QCA6490 上电、PCIe Gen2 ×1 链路与端点身份，只构建未上机；
  0301 修正 TCS IRQ 窗口与 C 接口。**11-23 18:58 冻结了第 75 轮镜像
  （`sweettime=1795431511`），但没有执行**；随后 0302 改为整机集成路线，0301 镜像只作历史对照。

## 19. 2026-11-28 · Wi-Fi DMA/SMMU 核心离线接入

- 0302 把 ath11k/manifest 编出的 109 个无线栈对象、`amss.bin` / `board-2.bin` /
  `m3.bin` / `regdb.bin`、DMA 路由规划和 128 MiB S1 表生成打进同一个可重定位内核对象；
  Clang 19 通过，缺失导入、重复定义、XNU 符号冲突均为 0。
- bundle 增加受 owner 约束的 SMMU 配置流程和 DMA arena：19 个场景与 1024 次
  allocator 迭代通过。参考联网记录证实 `ATOSNS=1` 不等于 DMA 不可用；
  ATS 不支持与 ATS 自检失败分别处理。
- 0303 增加 runtime boot hook，但**还未进入 XNU 的实际启动路径**；尚未给板级 worker
  装真实 SMMU 授权、wired/无缓存 DMA backing 与 MSI/MHI。没有新镜像或设备操作，
  仍 74/120。详见 [Wi-Fi 移植记录](wifi-foxlinne-port.md)。

## 20. 当前仍未解决的（2026-11-28）

- 声音：末端渲染从未被证明；音频暂停在 73/120，0298 只完成离线验证。
- Wi-Fi：端点识别已通过，但 0302/0303 还未进入 XNU 的实际启动路径；SMMU 授权、DMA backing、MSI/MHI 均未接通。
- 图形：每个像素仍是 CPU 算的；FoXLinne 的上游 GPU 实现可参考，但 iOS 16 适配未开始。
- 触感：按键直驱能震，系统层面（UIKit / CoreHaptics → backboardd）未接入。
- 用户态：缺 preboot 卷、独立数据卷、SEP、前台会话；部分 App 看门狗超时；空密码仍要求输入。
- 存储：f2fs 与尾部 root 的重叠没有界定，研究机仍要开 Android，写满会覆盖 root。
- 基带：未开实验，先决门是 306 MB `mpss_region`。
