# 0295：本机 AW88266 有界短听测状态机（2026-11-14）

## 前提与边界

第70枪已证明HAL双声道S16通过四period被AudioReach连续消费，30/30 DONE，
安全STOP/CLOSE/UNMAP。0295才首次把这个路径接到本机两颗ID=0x2032（AW88266）
的功放控制；用户期待听到声音，但安全红线优先。

本版不使用mayfly的AW88261 ACF，不移植其正增益guard，不改听筒。
沿用marble本机已在第68枪真实写入并读回的44项Music表与寄存器值：
SYSCTRL表值0xb440、SYSCTRL2=0x05a9。不会把0x05改得更响。

默认`sweetlisten=0`时0294行为不变：功放绕过、除32、峰值≤1024。
只有显式`sweetlisten=1`进入听测，PCM divisor=1但硬夹绝对峰值16384（-6dBFS），
仍最多30×4096B，约0.64秒的数据量，不循环重开。

## 功放状态机

1. SE15初始化成功后，GPIO52固定拉低：扬声器路径；高电平听筒路径绝不使用。
2. 两颗reset与I2C正常后写相同本机44项表，但初始化阶段强制：
   PWDN=1、AMPPD=1、HMUTE=1、ULS_HMUTE=1；I2S反馈TXEN关闭。
3. 对两颗分别核ID=0x2032、四个安全位全置位、0x05仍为0x05a9。
4. 第一阶段只清PWDN；两颗都必须读到SYSST PLLS+CLKS且无NOCLKS/WDS/过温/
   过流/欠压/boost过流/过压故障。任一失败时没有一颗解除静音。
5. 第二阶段清AMPPD；两颗都必须在仍静音时保持PLL/时钟且无故障。
6. 第三阶段两颗都通过后，才恢复各自ACF I2SCTRL3并清ULS_HMUTE/HMUTE。
   随后再读状态，无故障才返回成功，才允许period发送。
7. 结束或DSP错误/timeout：对两颗依次置ULS/HMUTE、关I2S TXEN、置AMPPD/PWDN。
   若任一I2C关断操作失败，立即把两颗reset都拉低作为硬兜底。
8. SE15初始化失败也直接双reset低。状态机不触碰功放音量增益、不使用听筒配置。

HAL stream仍在图START后等待非静音PCM，拿到PCM才调用功放start，避免功放开着等用户。
若功放gate拒绝，不提交任何DATA_BUFFER；图仍按原安全STOP/CLOSE/UNMAP收尾。

## 离线门

- 生产period核心全65536个S16值、136656断言通过；默认峰1024，听测上限16384，
  output配置在第一批source frame后被锁定。
- 实际stream/GLINK adapter 41项通过；含listen成功、listen gate拒绝、DSP错误、
  DONE timeout时post_play必调及默认模式不变。
- 实际AW88266 listen函数+精确44项表的10场景通过：正常、两颗分别PLL失败/
  状态故障、错误ID、SE15失败、profile写失败、CTRL读失败、speaker-switch强制低、
  正常stop；失败路径双功放关断成立。
- 原0290/0291功放probe、三字节写法、0294连续流回归必须继续通过。
- 六文件独立baseline apply/check/post-image、GLINK全链、完整XNU构建及marker/符号
  尚待完成。未完成前不打包、不上机。

## 首轮听测候选（尚未登记镜像身份）

第71枪候选仍需完整构建后冻结SHA/sweettime。不可变判据：

- guard通过，listen awcfg-sent，safe-profile两颗ID/ctrl/vol正确；
- speaker-switch=0；两颗PLL locked=1、healthy-muted、unmuted状态顺序完整；
- started marker必须在第一条DATA_BUFFER前；
- attempted=sent=done=30、reject/fault=0，peak-out≤16384；
- stopped marker和两颗normal-stop powerdown齐，STOP/CLOSE/UNMAP全0；
- 无reset兜底、无功放故障、无panic。
- 听感由用户报告：听到/没听到/不确定独立记录，不能由DONE或PLL代替。

当前仍70/120，无0295完整内核或实机结果。
