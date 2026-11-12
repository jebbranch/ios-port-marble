# HAL 已入流，连续后端离线核心（2026-11-13）

## 当前结论

第69枪的有效 ring：total=144841、seq=252、sweettime=1794493381。
独立评分器的身份、完整 marker、ADSP disabled、硬件隔离、node open、
非静音递增数据和无 kernel panic 门全部通过。最后快照：

```text
writes=512 frames=524288 sound=500234 peak=32768
queued=65536 overwritten=458752
```

用户操作是铃声页预览尝试和音量条拖动。能证明 HAL 交付了可被 S16 stereo
接收器统计的非零数据，不能证明具体铃声波形、实测48k实时帧率、DSP渲染或扬声器输出。
原始数据曾达到full-scale，不能直接接硬件。

用户明确决定暂不因设置 UI 问题阻断音频。依新授权，同版 capture HAL 已再次写入
现有用户态槽位，安装器每项读回相等；恢复备份使用独立目录
`out/hal-capture-0292-20261112/hfs-retain-boot69/`，保留之前恢复记录。
这是文件级持久化，不是启动分区flash，也不是“声音已通”。
配套运行需要0292内核及`sweethalbridge=1`；旧0291没有这个节点。

11月13日再次尝试独立读取时设备已不在adb，未将该失败读取当作已验证的新证据。
此前安装器的逐项读回结果仍有效；没有再boot，预算仍 **69/120**。

## 参考和不能照搬的部分

新增持久参考：
`refs/hakios-common-mayfly/`，固定commit
`9d3c291a9f9f4082d9571f9b2984d0026778a36b`，来自cotunjr/hakios-xnu-kernel的mayfly-port。
从/tmp移出保留Git对象；只检出了sound/speaker/guard三个音频文件，不声称整棵工作树齐全。

- 本地HakiOS common主分支的sound文件主要走redwood的ASM/ADM/AFE，不能拿来当
  marble的AudioReach后端。mayfly分支才有AR_WRITE_V2/AR_WRITE_DONE_V2。
- mayfly这份源码默认8个period。marble现有0x5000映射内还要留图描述符，不能照搬8页。
  适配候选是4×4096 B，占0..0x3fff；856 B描述符从0x4000开始，总长仍不超0x5000。
- mayfly的图/端口/时钟是其板级MI2S参数，不代替marble的TDM配置。
- 参考guard含默认正增益，**不移植**。用户增益红线优先于上游设计。
- marble当前`g_apm_data_buffer`使用同一个`g_rsp`槽，等待时不严格绑定token，
  且默认将token截成16位。不能直接并发调用4次当作连续DMA队列。

## 已实现的可移植核心

`tools/audio-models/marble_periods.{c,h}` 是离线验证的集成候选，
**尚未连接XNU、真实GPR传输、DSP/TDM或功放**。

1. 四个period独立所有权：FREE → STAGED → INFLIGHT → 匹配DONE后FREE。
2. 每次提交有完整32位token。DONE同时核对GPR长度/域/端口/opcode、
   完整token、数据地址和map handle；错误路由、旧token和重复DONE不释放缓冲。
3. 输出固定S16 stereo，输入只做除以32的衰减，最大绝对值1024。
   没有可配置正增益，也没有浮点增益、合成音或寄存器操作。
4. 缺帧补零；mute时整period为零；不修改仍为INFLIGHT的样本。
5. 发送候选包前先标为INFLIGHT，失败/超时保持内存占用，不能假装DSP已经释放。
6. 第一次接硬件的候选发送上限30块（合计30720帧，约0.64s的数据量），禁止自动循环重启。
7. token耗尽时停下，不绕回重用。真实适配层须保持ticket跨会话单调，并在图STOP/
   CLOSE/UNMAP确认之后才能重建队列，不能靠重置结构体释放仍被DMA持有的内存。
8. 模型要求调用方串行访问；尚未声称真实内核锁/线程并发已验收。

## 主机验收

`tools/audio-models/test.sh` 以`-Wall -Wextra -Wformat=2 -Werror`及ASan/UBSan
编译并执行生产候选源码。136647个断言通过，覆盖全部65536个S16输入值，以及：

- 最大负值与正值、零填充、静音；
- 4period乱序归还、重复/旧token、错误地址/handle/端口/opcode；
- DMA拥有时拒绝覆盖、基本ACK不冒充DONE、DSP错误、超时、发送中止；
- 30次提交预算、descriptor重叠/映射越界/地址溢出、token溢出。

`tools/test_score_hal_capture.py` 的9项正负对照通过，能拒绝旧身份、尾部陈旧marker、
硬件路径、纯静音、仅open、队列越界、缺身份覆盖和短raw。
现有第69枪经该评分器通过，评分器只输出capture通过，不输出“扬声器有声”。

## 下一个实际工程步骤

- 把完整DONE分派从共享控制应答槽分离出来，覆盖GLINK主/动态两条收包路径。
- 在已有ring consume API之后接固定衰减period；按真实映射地址生成wire base。
- 复用marble已验过的图/映射，但新stream模式必须禁旧的tone fill/loop与回落路径。
- 验证输入格式/波形，补足当前统计尚不提供的证据。
- 增加明确的START/STOP/UNMAP与故障安全收尾，再生成累计补丁、完整编译和镜像门。
- 第一轮真实后端仍单独登记；**当前没有0293完整内核或可上机的连续后端镜像**。

UI完善停车，GPU/Wi-Fi/基带门禁不变。设备连接不是离线工作的前提；镜像和门就绪后
再通知用户测试，不为试模型而增加开机。
