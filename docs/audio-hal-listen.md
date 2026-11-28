# 音频 HAL / 听测合并记录（0293 → 0298 / R1）

> 本页合并 0293、0295、0296、0297与两次 R1 的逐轮身份、判据和负对照，避免同一状态链散在六份小文件里。
> 第 70 轮的独有记录见 [`audio-halstream-0294-2026-11-13.md`](audio-halstream-0294-2026-11-13.md)；
> 0298 的当前离线状态见 [`audio-hallisten-0298-2026-11-15.md`](audio-hallisten-0298-2026-11-15.md)。
> 音频当前判决与证据索引见 [`audio-closeout.md`](audio-closeout.md)。

## 当前判决

- **0293**：第一次把 HAL 接到真实 GLINK；但守卫写了 `sweets1ap=1`，与已验证的 `sweets1ap=3` 基线不符，产物在开机前被拒绝，未上机。
- **0295 / R1**：120 秒等待内 HAL 全静音，流以 0/0/0 安全 teardown；功放 `pre_play` 从未调用。R1 身份成立，但**不是电声负读数**。
- **0296 / 第 72 轮**：有效无声。两颗 AW88266 的 profile / ID / vol 正确，清 PWDN 后 `SYSST=0`、PLL 全 0，安全门在任何 period 提交前拒绝并双关断 ⇒ **功放没有 TDM 时钟**。
- **0297 / 第 73 轮**：确认无声，但判为 **PRM 回包解析失配**：等待的是 `0x02001005`，历史实测应答是 `0x02001002` + `0x0800102c`。5 秒 timeout 后安全门不 OPEN、不碰功放、不发 PCM，UNMAP=0 ⇒ **不能判 PRM 接受/拒绝**。
- **0298**：已离线修好绑定；adapter 56 项、功放 10 场景、全 S16 136656 断言、全链 applied 239 通过。**尚无完整内核 / 镜像 / 开机**。

---


## 0293：HAL → AudioReach 有界连续流

### 范围与状态

用户希望尽快听到声音；优先级继续音频，不因设置UI障碍回退HAL。
第69枪capture结论和用户态常驻不变。0293是下一段真实GPR适配，
先确认多period的数据归还和安全收尾，**本版强制禁止功放配置，不声称将出声**。
无新增设备操作，预算69/120。完整编译、最终镜像身份尚待取得。

这不是另写一套DSP或换上mayfly的板级图：复用marble已接受的856 B图、
当前S1/IOVA映射和现有GLINK链路，新增独立所有权与控制应答分派。

### 实际代码路径

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

### 功放边界

`sweet_i2c_probe_amps()` 在stream模式下，**先于任何MMIO**打印bypass并返回。
新流亦跳过发往AW模块的两项旧cal记录。没有新ACF、寄存器、硬件增益或听筒调用。
该绕过表示“不执行我们的功放初始化/读写”，不是已测得物理PWDN值；
因此不把“amp=off”marker当真实功放掉电读回，也不依此宣称可以任意加大PCM。

stream阶段仍会启动ADSP/图并驱动TDM，和capture阶段不同；但不会绕过设备预算或
事前镜像门。GPU/Wi-Fi/基带保持原门禁。

### 默认安全门

必须为1：sweethalbridge、sweetpcm、sweetdescsep、sweets1、sweets1ap、
sweetiova、sweetshmhi。sweettdmvendor=7，I2C/amp必须显式0。
sweetshmflag允许缺省4但不接受其他值，region缺省0x5000或显式0x5000。

sweetbatt、sweetampfirst、sweetampverify、sweetnowait、sweetdbb、sweetexp、
sweetgpu、sweetgmuboot、sweethap、sweetsmp、sweetreboot、sweetmfc、sweetmfcget、
sweetvcfg2、sweetep4308、sweettdmclk都不得非零。最终打包必须无重复key。

部分旧模块配置仍沿用已验证序列；新marker只能证明新分支进入，不把参考板
MI2S参数或未验证DEC/CNV输出格式说成已修复。

### 故障与收尾

- DONE错误/超时：停止新提交，不假释放或改写DSP仍持有的页。
- STOP/CLOSE确认成功后才发UNMAP，并分别记录结果；任一缺ACK则不释放映射、不强制PAS shutdown，
  不自动复位，不自动重开流。open缺ACK也保留映射，因为“没收到ACK”不等于图未打开。
  操作者需要退出本轮，由用户长按电源返回TWRP。
- 同轮只有一次stream尝试；既不重置ticket自动重开，也不让静音不足触发tone补救。
- callback发送返回成功后才记sent；attempted、sent、done分别报告，失败不能伪装已发送。
- 旧tone路径在未设新flag时不变，0290/0291回归保护仍通过。

### 本地验证

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

### 第70枪候选的事前规则（尚未执行）

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

---


## 0295：AW88266 有界短听测状态机

### 前提与边界

第70枪已证明HAL双声道S16通过四period被AudioReach连续消费，30/30 DONE，
安全STOP/CLOSE/UNMAP。0295才首次把这个路径接到本机两颗ID=0x2032（AW88266）
的功放控制；用户期待听到声音，但安全红线优先。

本版不使用mayfly的AW88261 ACF，不移植其正增益guard，不改听筒。
沿用marble本机已在第68枪真实写入并读回的44项Music表与寄存器值：
SYSCTRL表值0xb440、SYSCTRL2=0x05a9。不会把0x05改得更响。

默认`sweetlisten=0`时0294行为不变：功放绕过、除32、峰值≤1024。
只有显式`sweetlisten=1`进入听测，PCM divisor=1但硬夹绝对峰值16384（-6dBFS），
仍最多30×4096B，约0.64秒的数据量，不循环重开。

### 功放状态机

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

### 离线门

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

### 首轮听测候选（尚未登记镜像身份）

第71枪候选仍需完整构建后冻结SHA/sweettime。不可变判据：

- guard通过，listen awcfg-sent，safe-profile两颗ID/ctrl/vol正确；
- speaker-switch=0；两颗PLL locked=1、healthy-muted、unmuted状态顺序完整；
- started marker必须在第一条DATA_BUFFER前；
- attempted=sent=done=30、reject/fault=0，peak-out≤16384；
- stopped marker和两颗normal-stop powerdown齐，STOP/CLOSE/UNMAP全0；
- 无reset兜底、无功放故障、无panic。
- 听感由用户报告：听到/没听到/不确定独立记录，不能由DONE或PLL代替。

当前仍70/120，无0295完整内核或实机结果。

### 0295 完整产物与第71枪身份

GitHub release raw SHA：
`b63b685e39b189c8fe533b08ea30c7d48d116d949759b4f0153f99dade706790`。
CNTP唯一0x4aa348 NOP后kernel SHA：
`5b47165e1b734cdcaba74a1fe998fa0ea89aa0166068198075c3e2dfa86a781e`。

最终镜像：
`images/hallisten-0295-20261114.img`，51306496 B，
SHA=`693ee73e672f502cbba24b398227ad8a2f16ebbcb442f0d3d45361f4c077cbd9`，
sweettime=`1794588906`。

实际cmdline恰好590B、无重复key、8核KEEPALIVE、无sweetreboot。新增唯一变量
`sweetlisten=1`；其余参数与第70枪一致。sweetshmflag仍省略，默认/guard均为4。
87个workflow marker、新listen markers、17个bridge/stream/period/listen符号、
全部kernel segment、8 MPIDR、boot v4和GPU probe level0验证通过。

听测前固定动作：

1. 再跑`build_hallisten_image.py --verify-only`与镜像SHA；
2. 设备serial必须2aa26095，只fastboot boot一次；
3. 用户需留意约0.64秒极短、可能很轻的系统音，并反馈听到/没听到/不确定；
4. 不调高音量、不重复触发、不追加第二枪。功放gate不通过时按设计不会出声；
5. 用户返回TWRP后拉日志，以sweettime唯一身份和本页判据评分。

当前镜像尚未启动，预算仍70/120。

---


## R1：免费重放的事前身份

用户明确说明上一轮因睡着未能听测，要求再启动一次且不计预算。
因此本次记为第71枪的免费重放R1，扣除0，预算保持71/120。

上一轮没有取得用户听感，也尚未在TWRP拉取ring，听感记为**未知**，
不得改写为“没听到”。设备当前直接在目标机fastboot，不能在重放前取ring；
R1完成后必须回TWRP拉取，以R1唯一sweettime判日志。

固定策略：

- 同一个0295冻结镜像、同一kernel/ADT/ADSP/功放状态机/PCM ceiling/cap；
- 只原位替换唯一10位`sweettime`，不重建ADT，不重新编译；
- 对比必须证明变化仅落在sweettime的十个数字范围；
- 只fastboot boot一次，禁flash，扣除0；
- 不调高音量、不连续触发；约0.64秒，用户明确反馈听到/没听到/不确定；
- 回TWRP后拉ring并核功放/DSP/双关断门，不追加第二次免费重放。

R1身份：

- source image SHA：`693ee73e672f502cbba24b398227ad8a2f16ebbcb442f0d3d45361f4c077cbd9`
- sweettime：`1794620381`
- R1 image SHA：`530b750fd457ebcfedbec9b214d09bea8bc84e4ce10e70cd3614d8ee77c0324c`；
- 预算：71/120，本轮扣除0。

原镜像与R1只有sweettime十位字段中的6个实际变化字节，偏移均位于
`11637392..11637397`；总大小、590B cmdline和其他字节相同。
当前尚未启动。

---


## R1：无声但未触发——不是电声负读数

用户明确反馈“没有声音”，设备已回TWRP。该人类观测如实保留；
R1扣除0，预算仍71/120。

当前ring身份成立：

- sweettime=1794620381；
- total=140574、seq=181，raw/decoded窗口匹配；
- guard/open/prepare/start成立，AW DSP cal两项status0；
- 功放I2C bypass只出现在ADSP启动前的旧probe位置。

但决定性时序是：

1. `halstream ready ... wait-ms=120000`；
2. 120秒内没有非静音HAL帧；
3. `no non-silent HAL frames before deadline`；
4. end为attempted/sent/done=0/0/0；
5. stop/close/unmap全部0，安全teardown结束；
6. **之后**才有volume generation及PCM write，第三块开始非静音。

因此功放pre_play从未被调用：没有safe-profile、PLL、healthy-muted、unmuted、
listen started/stopped或normal-stop powerdown行；也没有发送任何PCM period。
这轮“没听到”不是对功放/电声链的有效负读数，只证明操作晚于安全等待窗口。

安全性成立：超时前功放从未启动，图和映射正常关闭，无panic。不能把没有listen
marker误读成状态机丢日志，因为它前面的流明确记录attempted=0。

下一步只延长“功放仍关闭时等待HAL输入”的deadline到300秒；period数量、PCM上限、
功放状态机、I2C/ACF、图及所有听测安全门不变。必须产生新marker或可区分构建，
完整编译/镜像冻结后再登记；不直接重放旧镜像。

---


## 0296 / 第 72 轮：有效无声，定位到 TDM 时钟门

R1已证明旧0295在120秒内没有非静音HAL输入，功放未启动、period=0；
用户没听到不是电声负读数。0296仅延长功放关闭时的等待窗口到300秒。

完整身份：

- raw SHA：`99e47ec410b078323661e37f29c9d66896476966b7d19d54e784ea1737f5208d`
- patched kernel SHA：`5d659e984f43da91f22b5230fea113dadef378823e51e95404eaa292ee524dec`
- image：`images/hallisten-0296-20261114.img`
- image SHA：`9d7fcf7ace11936386c7437259d26e0e9f2a5c9d1229505f01c5491d10ba6843`
- sweettime：`1794624169`
- cmdline：590B、无重复key、8核KEEPALIVE。

88个workflow marker、17个bridge/stream/period/listen符号、CNTP唯一NOP、
所有kernel段/ADT/boot包通过。与0295运行逻辑的唯一差异是
wait-ms=120000→300000及循环12000→30000。

第72枪只准一次fastboot boot，预算72/120。进入iOS后五分钟内尽快触发一次声音；
不提高音量、不重复。拿到非静音PCM前功放保持关闭；之后最多30 period约0.64秒。
用户听感须明确听到/没听到/不确定，回TWRP后按0295完整日志门判决。

### 第72枪判决：有效无声，失败点在TDM时钟门

用户明确反馈“没有”。本轮身份与窗口成立：

- `sweettime=1794624169`
- raw `total=145822`、`seq=174`
- raw窗口与`logs/ring-hallisten-boot72.txt`逐字节长度一致
- image SHA仍为`9d7fcf7ace11936386c7437259d26e0e9f2a5c9d1229505f01c5491d10ba6843`

非静音HAL输入在300秒内及时到达：第3次write时已有3072帧、701个非静音帧，
峰值17372。GPIO52明确为0，只选speaker，从未切handset。两颗AW88266均读回
ID `0x2032`、安全控制值`0xf543`和原Music表音量寄存器`0x05a9`。

第一颗功放清PWDN后的`SYSST=0`，因此PLLS=0、CLKS=0。安全门随即拒绝pre-play，
`attempted/sent/done=0/0/0`；两颗功放均重新读回`ctrl=0xf543`，没有解除静音，
也没有向DSP提交任何听测period。

判决边界：

- “没有声音”是有效的人类负读数。
- 它不是PCM过小、HAL未触发或Music表未写入。
- 它证明当前图START成功仍没有让AW88266看到TDM bit clock。
- 它不测试功放输出级或扬声器振膜，因为硬件在PCM提交前已被安全关闭。

下一单变量是厂商精确的PRM硬件时钟请求：
`clock=0x204, rate=12288000, attr=4, root=0`。必须在图OPEN前请求，图
STOP/CLOSE后成对释放；request失败时不得启功放或发PCM。禁止用提高音量绕过。

归档：`verify/hallisten-boot72/result.json`、`hearing.json`和`SHA256SUMS`。

---


## 0297 / 第 73 轮：PRM 回包解析失配

### 目标与单变量

第72枪已经证明HAL非静音及时到达、两颗AW88266安全profile成功，但第一颗清PWDN后
`SYSST=0`，PLLS/CLKS均0。0297只在既有`sweetlisten=1`路径补厂商真实播放使用的
PRM硬件时钟生命周期：

```text
REQUEST  opcode=0x0100100f src=7 dst=2
param    0x0800102c size=20 num=1
clock    id=0x204 rate=12288000 attr=4 root=0

RELEASE  opcode=0x01001010 src=7 dst=2
param    0x0800102c size=8 num=1 clock=0x204
```

顺序固定为：

```text
现有VA/RX clocks和SHM_MAP
→ TDM clock REQUEST status=0
→ GRAPH_OPEN/配置/PREPARE/START
→ 等HAL非静音
→ AW安全状态机和最多30 period
→ AW关断
→ GRAPH_STOP/CLOSE
→ TDM clock RELEASE status=0
→ SHM_UNMAP
```

不复用旧`sweettdmclk`开关；guard继续要求其为0。`sweetlisten=0`的默认HAL流不请求
`0x204`。不改图正文、TDM endpoint配置、PCM格式、等待300秒、峰值16384、30 period、
Music表、`0x05a9`、GPIO52 speaker=0或任何GPU路径。

### 严格应答与资源规则

PRM与APM控制应答共用同一严格mailbox，但每次同时匹配：

- response route：远端src必须等于请求dst，本地dst必须等于请求src；
- 完整32位token；
- BASIC_RSP opcode `0x02001005`；
- 被应答的原opcode；
- status=0。

安全规则：

- REQUEST拒绝/timeout：不OPEN图、不启功放、不发PCM；只回收此前已确认的SHM映射。
- OPEN未确认：可能已开图，所以不假STOP/CLOSE/UNMAP；但时钟票可以独立RELEASE。
- START失败：STOP/CLOSE确认后RELEASE，再UNMAP。
- PLL门、DSP错误或DONE timeout：功放状态机先双关断；STOP/CLOSE确认后RELEASE。
- STOP或CLOSE未确认：不RELEASE、不UNMAP，避免在可能仍活动的图下撤时钟或DMA。
- RELEASE拒绝：不UNMAP、不假释放资源、不自动重开。

新增唯一marker：

```text
SWEET MARK: listen tdmclk v1 request rc=%d
SWEET MARK: listen tdmclk v1 release rc=%d
SWEET MARK: halstream teardown v2 open=%d stop=%d close=%d tdmrel=%d unmap=%d
```

### 离线结果

- 实际period/runtime/GLINK adapter：52项通过。
- 生产period核心：65536个S16值、136656断言、ASan/UBSan与`-Werror`通过。
- AW88266实际状态机与44项表：10场景通过；失败路径双关断。
- 0297单文件`git apply --check`、apply、post-image逐字节相等。
- GLINK全链：applied=238，历史skip=49不增加，post-image等于链树HEAD。
- request/release包逐字段、默认路径不投票、request拒绝、START失败、STOP失败、
  CLOSE失败、PLL前门失败、DSP失败、release拒绝均有主机对照。

证据在`verify/hallisten-0297/`。这些仍是离线/模拟传输测试，不证明设备PLL会锁。

### 第73枪事前判据（新完整构建与镜像冻结后才允许执行）

当前预算72/120；执行一次后为73/120。只允许目标serial `2aa26095`，
只用`fastboot boot`，不flash。完整构建前不启动，0296不得重放冒充0297。

开机前必须补齐：

1. release raw按`kernel.development.sweet`资产名配对SHA，不能取陈旧`build-error.txt`。
2. CNTP模式恰一处并修为NOP；全部累计marker通过。
3. 上述三条0297 marker在最终kernel和镜像各恰一处，旧teardown v1不得作为新身份。
4. 链接符号、实际cmdline≤590B、唯一sweettime、8核、内嵌kernel段逐项核验。
5. 最终image/raw/kernel SHA和sweettime另行写入本文并commit，之后才可boot。

实机判据：

| 门 | 必须观察 |
|---|---|
| 身份 | ring sweettime等于冻结镜像；raw total/seq与解码窗口一致 |
| 时钟请求 | REQUEST marker `rc=0`且严格control status=0，发生在GRAPH_OPEN之前 |
| 图 | OPEN/PREPARE/START全status=0 |
| HAL | 300秒内有非静音帧；否则功放不得启动，仍需安全收尾 |
| 功放路径 | GPIO52=0；两颗ID2032、ctrl=f543、vol=05a9 |
| PLL主判据 | 两颗PLL读取status=0且locked=1；任一为0则不得解除静音或发period |
| 听测 | 仅全部安全门通过后最多30 period；不加音量、不重复触发 |
| 收尾 | 功放双关断；STOP/CLOSE=0；RELEASE rc=0；UNMAP=0 |
| 人类反馈 | 用户明确“听到/没有/不确定”，与日志结论分开记录 |

判决：

- REQUEST被拒：说明该精确票在当前状态/路径不被接受；不测试功放。
- REQUEST=0但PLL仍0：排除“完全没投0x204票”，下一层才检查物理TDM启动、
  GPIO121–124 mux/drive和endpoint时序；不得提高音量。
- 双PLL锁定但无声：必须结合是否真正发送/归还30 period及功放最终状态，
  才进入功放输出级/振膜链定位。
- 听到声音：只宣称本次有界应用PCM→扬声器链闭合，不自动扩大时长、响度或常驻播放。

### 第73枪冻结身份

完整构建与最终打包已通过：

- release raw SHA：
  `e06d4770f137caf3fecfe99e57ab96107461a99fce3b7c86bbd70498f4678927`
- CNTP修补后kernel SHA：
  `d8f29c6aabf4bc7fc1362dd2ebdb219a3ae9c868d408fac76e2d32098daa0994`
- image：`images/hallisten-0297-20261114.img`
- image SHA：
  `bf1f84c0f6bed68f104a4f4c9f75008a3e6d982ae42c78651e3dc6eba6c211e1`
- sweettime：`1794627872`
- cmdline：590B，无重复key，8核KEEPALIVE
- workflow累计marker：90
- bridge/period/stream/listen符号：17
- CNTP原指令：raw恰1处；最终kernel为0处
- kernel全部Mach-O段、ADT MPIDR、boot v4和GPU probe level0：通过

三条0297 marker在raw/final kernel中各恰一处，旧teardown v1为0。`--verify-only`
独立复跑通过。证据在`verify/hallisten-0297-image/`。

因此只准对serial `2aa26095`执行一次：

```bash
fastboot boot images/hallisten-0297-20261114.img
```

执行即记第73枪，预算73/120。进入iOS后五分钟内触发一次声音；不要重复点击来制造
第二次听测，不提高音量。若听到或没有，均回TWRP后以本文件已冻结判据拉原始ring。

### 第73枪判决（2026-11-15）：确认无声，PRM回包解析器失配

用户明确“确认没声音”。身份与窗口成立：

- sweettime=`1794627872`
- raw total=`131677`、seq=`96`
- raw窗口与解码文本长度一致
- image SHA=`bf1f84c0f6bed68f104a4f4c9f75008a3e6d982ae42c78651e3dc6eba6c211e1`

HAL非静音输入已出现：第3次write时3072帧、709个非静音帧、peak32767。
0297发送了精确的`0x204` request，但`g_hal_xfer`等待5秒后timeout，随即按安全门：

```text
request rc=-1
no graph or amplifier start
unmap status=0
```

因此本轮没有GRAPH_OPEN/START，没有safe-profile/PLL读取/解除静音，也没有PCM period。
无声是真实反馈，但只证明安全门停止后没有播放。

事后用已归档的第64枪全量收包交叉核实，PRM时钟应答不是APM BASIC_RSP：

```text
opcode = 0x02001002
src=2 dst=7
payload = {0x0800102c, status}
```

0297严格mailbox错误地只接受`0x02001005`和`payload={原命令opcode,status}`。
历史第64枪恰有5条PRM request done；旧`g_prm_vote_clock_ex`也正是按共享槽读取
`g_rsp+28`，所以T1曾能得到`0x204 ok`。因此本轮timeout属于解析器失配：

- 不能写成PRM拒绝；
- 不能写成PRM接受；
- 没有进入功放PLL主判据；
- 下一版只修PRM专用应答识别，request wire bytes和时序保持不变。

证据归档：`verify/hallisten-boot73/`。预算固定73/120。

---
