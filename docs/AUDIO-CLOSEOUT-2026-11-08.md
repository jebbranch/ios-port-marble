# 音频收尾与暂停（2026-11-08）

## 冻结结论

**按用户要求，完成第68枪后暂停音频。预算最终68/120，剩52；R1免费重复听测扣除0。**
不再自动追加音频开机、EOS、PCM格式、功放启动或校准实验。需要用户明确重开再登记新的目标与事前判据。

本轮确实修正了两个AP功放代码缺陷，且最后一轮取得真实寄存器变化：

1. **0290**：正常路径不再跑破坏性位操作；扫描失败不拉低功放RESET；显式诊断恢复引脚。实机RESET保持1/1、I2C实际可读。
2. **0291**：寄存器地址和16位大端值用一条三字节I2C消息发送，不再把WRITE拆为两条。最后实机两颗功放均从默认4003/38变为寄存器表预期b440/05a9，PWDN/AMPPD/HMUTE均清除。

**这证明寄存器表写入现在真实生效，不能仅凭工作位清除或121个DONE宣布扬声器已经出声。**
第68枪听感尚未得到用户明确反馈，保持未知；P1听感同样未知。R1曾明确“没听到”，只对R1记录有效负读数，不能倒填其他轮。

## 最后一轮实机证据

- 标签：`p2-ampwrite-0291`，sweettime=1794114102。
- 真正cmdline：582 B、唯一sweettime，参数与P1/R1一致，固定同一ADT seed；只变内核修复和日志身份。
- raw total/解码：101520/101520，独立身份/窗口门通过。
- 新写法证人：0x34/0x35各一条bytes=3，b0=0、b1=0x55、b2=0xaa；两个soft-reset请求的实际缓冲布局被记录。
- ampguard：early pass1 skip1 init0；late pass2 skip1 init1 profiles2 RESET=1/1；位操作扫描0次。
- 两颗ID=0x2032，8条post-init真实读回均r=0。
- 两颗SYSCTRL=0xb440、SYSCTRL2=0x05a9；PWDN=0、AMPPD=0、HMUTE=0。这是**启动前**状态，不是播放期间PLL/I2S/电声输出测量。
- 唯一dec-cnv图，OPEN/START=0，120tone/121DONE，timeout=0；无回落、无EOS，done+PSCI_RESET正常，panic0。
- 设备恢复Android，最后boot_completed=1、电量99%；没有新启动其他测试线。

## 预算流水（本次续接）

| 记录 | 扣除 | 扣除后 | 声音证据 |
|---|---:|---:|---|
| 续接起点 | — | 64 | 旧记录看历史判决 |
| 65：D0控制 | 1 | 65 | 未留意，未知 |
| 66：D1隔离 | 1 | 66 | 未留意，未知 |
| R1免费重放 | 0 | 66 | 用户明确没听到 |
| 67：P1功放保护 | 1 | 67 | 未单独反馈，未知 |
| 68：P2单消息写入 | 1 | 68 | 尚未反馈，未知 |

所有真实开机流水保留，没有把免费轮删除或改成计费序号。

## 已验证与未决的边界

已验证：正确的A形式图被接受；共享缓冲能获得WR归还；descriptor隔离修正AP覆盖；默认诊断不再复位功放；single-message WRITE使初始化表/工作位实际改变。

仍未验证/仍有代码缺口：

- 扬声器实际出声、播放期间BCLK/LRCLK和PLL/I2S状态，没有正向证据。
- DEC/CNV虽然加入图，当前参数仍未发双方的0x08001008输出配置；厂商接口要求该组合配置两端。不要直接打开旧sweetvcfg2四包混入不存在的iid。
- 必经MUX通道映射、MFC输出/校准和厂商多图流程尚未完整复现；那块18504 B数据的内容/必要性未证实，不能按长度猜。
- EOS仍有已确认的封装差异：厂商28 B、旧XNU44 B；本轮没改/没发EOS，旧零事件不能排除AP请求错误。
- TDM24bit开关存在后续32bit重复写覆盖；未在本轮修改。回包关联/延迟计数也不够严格，不把等待计数当精确播放节拍。
- iOS应用声音还需要CoreAudio/HAL桥。本轮是内核合成音，未经过AudioQueue/Android AudioTrack；应用API的push/pull标签不是这些GPIO/I2C代码缺陷的解释。

WR接口把DONE定义为缓冲已被消费并可归还客户端，边界不是“最终声音已播放”。之前把它一概叫“仅入队”或反过来认领渲染都不准确。

## 可复现产物与入口

- 当前代码构建提交：4afb6ce（0291）；之后只提交镜像/记录，不触发替换本轮内核。
- kernel raw SHA256：ddb10413f3ac306e7692110049edfc8a120098cae71a7f27dedeaabfcd8d77e9。
- CNTP修补后：03727a386741a6cbc7a0aeaac6189d1ee03e84ea54f025a2d2677719e4864c4d。
- 最终镜像：`images/p2-ampwrite-0291.img`，SHA256 b1a9630235c35acc9b07397aeb35e3919b0281a87ba0d2df17837deda88f57d7。
- 代码：`xnu/patches/0290-sweet-preserve-amp-state.patch`、`0291-sweet-aw-write-single-message.patch`；I2C链HEAD=06b4875，已落patch基线9056256。
- 证据：`logs/ring-p1-ampguard-0290.txt`、`logs/ring-p2-ampwrite-0291.txt`、`verify/ampguard-0290/`、`verify/ampwrite-0291/`；raw在本地logs/raw，文本按-text保留原字节。
- 本地检查：功放11路径回归、写/读/表/失败事务模型、strict syntax、ASan/UBSan、纯基线apply、I2C与GLINK全链post-image匹配；最后打分器所有运行门通过。
- 详细流水：`audio-closeout.md`、`audio-ampguard-2026-11-08.md`、`audio-descsep-live-2026-11-08.md`。

其他方面仅整理了既有日志与交接状态，入口为 `OTHER-LINES-2026-11-08.md`。GPU冻结、Wi-Fi/BT停车与基带前置门均没有因音频暂停而解除。
