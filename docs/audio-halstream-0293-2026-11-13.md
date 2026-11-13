# 0293：HAL 到真实 AudioReach 的有界连续流

## 范围与状态

用户希望尽快听到声音；优先级继续音频，不因设置UI障碍回退HAL。
第69枪capture结论和用户态常驻不变。0293是下一段真实GPR适配，
先确认多period的数据归还和安全收尾，**本版强制禁止功放配置，不声称将出声**。
无新增设备操作，预算69/120。完整编译、最终镜像身份尚待取得。

这不是另写一套DSP或换上mayfly的板级图：复用marble已接受的856 B图、
当前S1/IOVA映射和现有GLINK链路，新增独立所有权与控制应答分派。

## 实际代码路径

`sweethalstream=1` 是默认关闭的新门：

- 新模块 `sweet_audio_periods.{c,h}`、`sweet_audio_stream.{c,h}`。
- GLINK开始前检查互斥参数；失败立即返回，不启动新流。
- 图描述符从0x4000开始，四个PCM页占0..0x3fff；均在现有0x5000区域内。
- 对Device映射只使用对齐32位写，不使用`flush_dcache64()`或未对齐memcpy。
- 控制请求有独立匹配邮箱；主/动态收包均在共享`g_rsp`之前分派。
- open/prepare/start/stop/close/unmap都匹配完整token、命令、域和端口。
- DATA_BUFFER保持完整32位token；DONE再核地址/句柄/status才释放对应period。
- TX检查FIFO可用空间、remote intent与正在等待的DMA deadline。
- 同一个ADSP线程负责收发；sweetbatt=0防止电池线程成为第二个FIFO消费者。
- 新入口在旧tone/默认图分支之前返回，不播放shim或kernel合成音，不回落到旧图。

每次启动最多等待120s非静音HAL输入，然后最多提交30个4096 B period；
数据量约0.64s，不是无限播放。每个period固定除32衰减、绝对峰值≤1024。
缺帧补零，mute状态经原bridge锁读取。没有可调正增益。

首次非静音输入保存至多512 B原始字节，分8条短日志，用于S16/F32/波形检查。
第69枪只证明非零字节被当S16统计，不等同于波形正确；在检查原始字节前不启用听测。

## 功放边界

`sweet_i2c_probe_amps()` 在stream模式下，**先于任何MMIO**打印bypass并返回。
新流亦跳过发往AW模块的两项旧cal记录。没有新ACF、寄存器、硬件增益或听筒调用。
该绕过表示“不执行我们的功放初始化/读写”，不是已测得物理PWDN值；
因此不把“amp=off”marker当真实功放掉电读回，也不依此宣称可以任意加大PCM。

stream阶段仍会启动ADSP/图并驱动TDM，和capture阶段不同；但不会绕过设备预算或
事前镜像门。GPU/Wi-Fi/基带保持原门禁。

## 默认安全门

必须为1：sweethalbridge、sweetpcm、sweetdescsep、sweets1、sweets1ap、
sweetiova、sweetshmhi。sweettdmvendor=7，I2C/amp必须显式0。
sweetshmflag允许缺省4但不接受其他值，region缺省0x5000或显式0x5000。

sweetbatt、sweetampfirst、sweetampverify、sweetnowait、sweetdbb、sweetexp、
sweetgpu、sweetgmuboot、sweethap、sweetsmp、sweetreboot、sweetmfc、sweetmfcget、
sweetvcfg2、sweetep4308、sweettdmclk都不得非零。最终打包必须无重复key。

部分旧模块配置仍沿用已验证序列；新marker只能证明新分支进入，不把参考板
MI2S参数或未验证DEC/CNV输出格式说成已修复。

## 故障与收尾

- DONE错误/超时：停止新提交，不假释放或改写DSP仍持有的页。
- STOP/CLOSE确认成功后才发UNMAP，并分别记录结果；任一缺ACK则不释放映射、不强制PAS shutdown，
  不自动复位，不自动重开流。open缺ACK也保留映射，因为“没收到ACK”不等于图未打开。
  操作者需要退出本轮，由用户长按电源返回TWRP。
- 同轮只有一次stream尝试；既不重置ticket自动重开，也不让静音不足触发tone补救。
- callback发送返回成功后才记sent；attempted、sent、done分别报告，失败不能伪装已发送。
- 旧tone路径在未设新flag时不变，0290/0291回归保护仍通过。

## 本地验证

- 生产period源码对完整S16域共136647断言，严格编译与ASan/UBSan通过。
- 新runtime及实际GLINK适配函数33项主机检查通过，包括即时DONE、异步DONE、
  stale事件、timeout、错误ACK、短PCM补零、控制邮箱被后续包覆盖的负对照、
  STOP/UNMAP失败不假释放及二次调用不重开。
- HAL mute accessor/原ring/ioctl函数严格编译与ASan/UBSan通过。
- 功放probe12条路径通过；stream bypass无map/init/profile/reset/MMIO。
- 0291寄存器WRITE/READ及原表模型回归通过。
- 8文件独立baseline apply/check/post-image通过；
  GLINK全链234 applied、49个原有单文件基线skip不变，post-image==chain HEAD。
- workflow新增7个完整marker，完整编译后必须存在；仅host通过不等于完整XNU通过。

0292 replay脚本改为固定已发布conf/bridge链位，不再依赖会随0293改变的HEAD^。
最后一次发布基线写进`emit_halstream_patch.py`；新编号不混入未发布本地链位。

## 第70枪候选的事前规则（尚未执行）

具体sweettime/镜像SHA须在完整构建打包后另存并提交。本页先固定不可变的逻辑门：

1. 配对摘要、CNTP唯一NOP、全部marker/符号和真实参数/内嵌段检查通过；预算记一次。
2. 本轮当前ring匹配唯一sweettime；不从旧tail取身份/收尾。
3. guard=1、amp-bypass存在、awcfg-bypass存在；无旧tone、旧fallback、功放表写入、GPU路径。
4. open/prepare/start匹配回包status0，新856B/off0x4000见证存在。
5. HAL输入发生，原始witness足够解码；统计source>0且0<peak-out≤1024。
6. attempted=sent=done=30、reject=0、fault=0；地址/token真实匹配，
   才判“有界连续提交/归还通过”。无数据或只静音为不确定，不据听感判断。
7. stop/close/unmap全部0、无kernel panic、安全返回TWRP才关闭设备轮；
   任一步失败停手，不追加第二枪，不开启功放模式。
8. 无听测：本版不以人耳为判据。若链路与字节格式通过，下一版才准备已验证本机
   功放状态机的短时听测；继续禁止正增益和听筒复用。

当前仍69/120，无新完整内核、无新镜像、无新实机结果。
