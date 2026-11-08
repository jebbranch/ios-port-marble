# 0290 功放诊断副作用修复（2026-11-08）

## 范围与实现

用户要求先修功放。本轮只改 `sweet_i2c_probe_amps()`，不改 AudioReach 图、PCM 格式、EOS 或音幅/时长。

- 默认 `sweetnobb=1`，早期调用与 GENI 失败路径也不会落入位操作诊断。
- GENI 初始化成功后直接返回正常路径，不再把已配置的功放复位。
- 删除“扫描无设备就拉低两条 RESET”的诊断分支；显式启用诊断也不再执行这项破坏动作。
- 显式诊断退出时恢复进入诊断时的 SCL/SDA cfg/io 与 SPKSW cfg/io。
- 新 witness：`SWEET MARK: ampguard v1 pass=%u skip=%d init=%d profiles=%d rstA=%d rstB=%d`。profiles 是成功写事务批数，不是“已出声”。
- 正常 GENI 成功后做一次 `ampread post-init` 启动前只读回查，区分 I2C 成功、工作位与实际听感。

补丁为 `0290-sweet-preserve-amp-state.patch`，6 hunks、+45/-42 行。
I2C 的已落补丁基线为 `1079cf2`（从旧基线重放现有0234/0238/0240/0241），新链提交 `9056256`。
所有新增调用的定义都在 probe 前；不加入新的外部类或链接符号。

## 本轮本地验证

- 11 个真实函数 host 路径通过：旧默认流程在两颗功放写事务后 RESET=0/0；新默认流程 RESET=1/1、诊断扫描0次。
- 覆盖早期调用、GENI失败、映射失败、错误芯片ID、写事务失败、显式跳过与显式诊断；后者恢复引脚，扫描失败不拉低RESET。
- `-Wall -Wextra -Wformat=2 -Werror` 编译及 syntax-only 检查通过；ASan/UBSan执行无报告。
- 6-hunk补丁从纯基线apply后逐字等于编辑源码。
- I2C链：applied=264、历史skip=16，post-image==HEAD；GLINK链：applied=231、历史skip=49，post-image==HEAD。
- 证据：`verify/ampguard-0290/`、`verify/ci-i2c-0290.log`、`verify/ci-glink-0290.log`。

这些是源码/host验证，不代替完整内核构建或实际功放状态。

## 单次设备验证约束

事前表在 `audio-closeout.md` 第67枪小节；只有新产物、CNTP、digest、marker和真实cmdline通过才启动。
沿用R1的856 B图、descsep=1、sweetvol=16、sweetbufs=120，EOS/四包/新格式均不发；只换0290内核与日志身份，固定ADT seed。
最多一枪，预算正常从66/120到67/120；R1免费额度不延伸。本轮不同时修复其他嫌疑。

验证目的首先是确认：不再执行诊断复位、两颗功放实际可读、工作位是什么。即使profile44/44仍不能直接认领“功放已启动”。

## 音频模型的层次

当前短音由XNU内核的正弦表生成，经共享内存、GLINK/GPR发给AudioReach，DSP驱动TDM，外部AW882xx放大。
它不经过iOS AudioQueue，也不经过Android AudioTrack/AudioFlinger；启动后运行的是XNU/iOS用户态，不是通过Android音频服务做代理。
因此应用API的push/pull差异不是当前GPIO诊断复位的解释。NDK在本项目用于交叉编译shim，不构成重写AudioTrack的理由。

将来接iOS应用声音时，CoreAudio/HAL的回调、环形缓冲、格式、时间戳/硬件时钟确实要适配；这与当前内核短音验证是两层工作。

## 当前状态

代码及本地验证已完成；尚未取回0290内核、尚未开第67枪。设备结果与听感必须在真实执行后填写，不能预写。
