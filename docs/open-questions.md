# marble ADSP 音频：突破 + 一个具体的页表问题（求第二意见）
> **⚠️ 本文件含已被推翻的结论（2026-11-07 更新）。当前权威状态见
> [`unixnu-sheng-audio-2026-11-07.md`](unixnu-sheng-audio-2026-11-07.md) §§13–48，全项目九条线的一句话状态见 **[`STATUS-2026-11-07.md`](STATUS-2026-11-07.md)**。**
> 已作废/收窄的旧说法：① "18 KB / 18128 / 18504 cal 是墙" —— 降级为未检验假设；
> ② "ACDB 重建容器 = EBADPARAM，内容被否" —— 那些拒绝来自 **B 形式（ACF 索引）**，
> 而 B 形式开头 16 字节被 APM 当参数头读会得到 `param_size=0xB0000002`（2.75 GiB），
> 它是 GSL/ACDB 侧结构、**从来不是 APM wire payload**；③ `g_graph_vendor4`(2724) 是**上一代**
> 且自身索引超界 4 B，由它得出的一切结论作废；④ 旧 2960 五枪用默认 `off=16/len=2944`
> 削掉了 entry[0] 的头，其 `0x2` 是自伤不是判决；⑤ "图里缺 DEC/CNV" 与 "各缺一半" 两次都被
> 逐字 dump 推翻 —— 被接受的 `g_graph_vp_wr` 同时有 WR 与硬件段；⑥ `DATA_BUFFER_DONE` 只证明入队；
> ⑦ "EOS 无应答是 100 ms 窗太短" 已撤回（`g_apm_xfer` 等 5 s）。
> **当前事实**：856 B A 形式（+PCM_DEC/PCM_CNV）`graph_open=0`、`graph_start=0`、
> `pcm tone done (120 buffers)`、DSP 回 121 个 DONE —— 用户**三次**都没听到 ⇒ 三条有效负读数。
> `EOS_RENDERED` 在收包仪器被证明可用的前提下从未到达（第 64 枪 137 包、`0x05001001` = 0）
> ⇒ AP 侧事件路线按止损关闭，现在做 B 线（第 65 枪 = 0288 摘掉 AFE id，内核已过门，只差打包开机）；
> **预算 64/120**。新会话第一入口 = [`STATUS-2026-11-07.md`](STATUS-2026-11-07.md)。


## 0. 环境（不变）

XNU 8796.141.3 经 `fastboot boot` RAM-only 启动，**Android 不在运行**；
ADSP 固件由 XNU 自己 PAS 加载；没有 Android 音频 HAL/audio-ion；
只有内核自维护的 ring buffer 当日志，总线一挂就什么都读不到。

## 1. ★ 突破：之前所有"整机冻结"的根因是**地址表达方式**

厂商 trace 里 `SHM_MAP` 登记的区域地址是 **`0x1_1FFE8000`（33 位 IOVA）**，
DATA_BUFFER 用**裸偏移** 0 / 0x800。我们之前一直用自己的 32 位物理地址
`0xa2e00000` 当区域地址 —— 结果任何合法数据帧都让**整个 LPASS 子系统**
（SMMU 与 GLINK FIFO 一起）失去响应，整机冻结。

改成登记厂商那个 IOVA、并用 S1 把 `0x1_1FFE8000` 翻到我们的页之后：

```
shm_flat pa=0x11ffe8000 (region addr, buffer PA differs) flag=4 size=20480
shm_map rsp status=0
shm mapped pa=0xa2fe8000 mode=offset
data_buffer send
data_buffer sent; gfsr=0 fsynr1=0
poll GFSR=0 FSYNR1=0 reached=3          ← GFSR 读取成功
post cb5 FSR=0x402 FAR=0x1ffe8000 FSYNR0=0x40001
data_buffer: no rsp (dst=0x40bd)
===== done =====  PSCI SYSTEM_RESET        ← 整机正常复位
```

**不再挂死**，而且拿到了一个真实的 context fault：

| 配置 | post cb5 |
|---|---|
| 32 位物理地址（此前全部轮次） | `FSR=0x400`（未变）→ 事务根本没到 CB5 |
| 33 位厂商 IOVA | `FSR=0x402 FAR=0x1ffe8000` → **事务到了 CB5，页表没走通** |

⇒ **SID 匹配是对的、CB 路由是对的**；之前"SID 可能从未匹配"的猜测**被推翻**。
`CFSR=0x402` 按厂商头文件解：`TF = BIT(1)` ⇒ **Translation Fault**；
`FAR` 正是我们的 IOVA 低 32 位。

## 2. 当前 SMMU 的完整状态（真机回读）

```
IDR0 = 0x4c017e71   S1TS=1, S2TS=0, SMS=1, ATOSNS=1(ATS 不可用)
IDR1 = 0x6000004b   NUMS2CB=0, NUMCB=0x4b, PAGESIZE=0(4K), NUMPAGENDXB=6
IDR2 = 0x5111
CBA2R(5) = 0x1      ⇒ VA64=1 ⇒ AArch64 长描述符
CBAR(5)  = 0x1f000  ⇒ Type=1(S1+S2 bypass), S1_MEMATTR=0xf(WB), S1_BPSHCFG=0
SCTLR(5) = 0xc00027 ⇒ M=1, CFCFG=0(terminate), CFRE=1, SHCFG=NSH
TCR(5)   = 0x351f   ⇒ T0SZ=31(33 位输入), TG0=4K, SH0=inner, ORGN0/IRGN0=WBWA
TTBR0    = 0xa3000000 (高 32 位写 0)
MAIR0    = 0xff     ⇒ AttrIndx0 = Normal WB
TCR2     = 0x60     ← **从未写过**，是读回值
SMR[6]   = 0x80001801  (SID 0x1801, mask=0)
S2CR[6]  = 0x5         (→ CB5)
```

页表（AArch64 4KB 颗粒）：
```
L1[4]   = 0xa3001003   (表指针)
L2[255] = 0xa2e00701   (2 MiB 块: valid|block + AF + SH inner + AttrIndx0)
缓冲区 PA = 0xa2fe8000   (= 0xa2e00000 + IOVA 在 2MiB 块内的偏移 0x1E8000)
```

**三级页表（`L1→L2[0xa3002003]→L3[488]=0xa2e00703` 4KB 页）和这个两级 2MiB 块
都试过，两者都报同一个 TF、同一个 FAR。**

## 3. 想请教的问题

1. **为什么这两种结构都会 TF？** 索引我认为是对的：
   IOVA `0x1_1FFE8000` → L1 = `VA[32:30]` = 4、L2 = `VA[29:21]` = 255、
   L3 = `VA[20:12]` = 488。还有哪些原因会让一次本来正确的 walk 报
   translation fault？（例如：TCR2 未编程、TTBR0 的 ASID/CnP、
   表内存的 NS 属性、SMMU 对表内存的可达性）

2. **TCR2 是否必须编程？** 我们从未写过它，读回 `0x60`。
   `ARM_SMMU_TCR2_PASIZE`(bits[3:0]) 和 `AS`(bit4) 对 stage-1 AArch64 是否必需？
   若必须，应该等于 `IDR2` 里的什么字段？

3. **`CFSR` bit 10（0x400）是什么？** 厂商头文件里只定义了
   MULTI(31)/SS(30)/UUT(8)/ASF(7)/TLBLKF(6)/TLBMCF(5)/EF(4)/PF(3)/AFF(2)/TF(1)。
   `0x402` 里那个 bit10 是保留位，还是能指示 walk 失败在哪一级？

4. **AArch32 vs AArch64 的歧义**：我们按 `CBA2R.VA64=1` 判定为 AArch64 长描述符，
   `T0SZ = 64 − 33 = 31`。如果实际上该 CB 走的是 AArch32（+EAE）路径，
   同样的表结构会怎样失败？有没有不依赖 ATS 的判别方法？

5. **有没有办法直接看到 SMMU 走过的 walk 结果**（除了不可用的 ATS1PR/PAR）？
   比如某个 debug/ECC/parity 寄存器、或让 TCU 记录最近一次 walk 地址？

## 4. 我接下来的做法

已把下面这些做成 boot 参数（一次编译、多次引导，每次失败都是一个干净 fault）：

| 开关 | 作用 |
|---|---|
| `sweets1ns=1` | 描述符加 NS 位(bit5) |
| `sweets1tbl=<pa>` | 页表基址换到别的物理内存 |
| `sweets1tcr2=<v>` | 写 TCR2（测 PASIZE/AS） |
| `sweets1ttbr1=1` | 同时写 TTBR1 |

按"最可能"顺序试：NS 位 → 表基址挪走 → TCR2 → TTBR1。

---

# 求第二意见 #2（2026-11-02）：数据面已通、PA 已前置，但"出声"没有任何独立证据

## 现状（都是实机读数，ring 归档在 logs/）

- 数据面：`sweets1ap=3`（S1 叶子 `0x701→0x7c1`，只改 AP[7:6]）之后
  `DATA_BUFFER done status=0` ×4，回复里的槽位/偏移跟着我们的 ping-pong
  （其中一条归还 slot 1 / offset `0x800`），`buffer probe → 0`，1200/30 buffer 跑完正常复位。
- 排序：历史上 `bringup() → probe_amps()`，PA 的 Music profile 是在音灌完之后才写的；
  0233（`sweetampfirst=1`）把它挪到开图之前，`ring-ampfirst.txt:2281/2299/2456` 各自证明。
- 人耳：人在手机旁，两次开机（17:30 / 17:41）**答"没有"**。但幅度是 1/16 满幅、
  时长 ≈180 ms，所以这条只能记"未观测到"，不能记"证明没出声"。

## 三条仪器全部不能用，而且失败方式各不相同（这才是问题的核心）

| 仪器 | 结果 | 为什么不能用 |
|---|---|---|
| bit-bang 读 PA | `0 amplifier(s) answered` | 已知坏仪器（同一个语料里 GENI 明明 ACK 了 `id=0x2032`） |
| GENI 读回 PA 寄存器（0234） | 8 次全 `r=-1`，配 `se15 timeout st=0x41 irq=0 rx=0 tx=0` | 传输**启动了但没有一点中断**；同一函数在配置时刻还能用（`r=0 id=0x2032`）。中间只夹着我们自己的 bit-bang 那一相（把引脚 56/57 的 TLMM 在 GENI 函数与 GPIO 之间来回切）和音流本身 |
| AP 读 RX macro（`sweetreadrx=1`） | 读完不 fault 不 panic，`rx_macro 00000000 ×4` | 全零**没有对照**：我们不知道这块窗口前 16 字节的寄存器含义，也没有任何已知答案的点，所以零既不能读成"macro 关着"也不能读成"读路径不通" |

## 想问的两件事

1. **PA 总线是谁弄坏的**：设计好的分辨实验是 0236 的 `pre-stream` 快照（落在
   `probe_amps()` 之后、开图之前）。它也超时 ⇒ 是我们自己的 bit-bang 相把 SE15/引脚弄坏，
   修法应是读回前重新 `se15_pins()`（0237）；它正常 ⇒ 才是音流/ADSP 拿走了总线。
   有没有更快的判别办法（例如直接读 `SE_STATUS`/`SE_IRQ` 的期望值、或读回前先做一次
   已知能成功的 `id` 读当作 canary）？
2. **"设备图/后端拉起"到底需要哪几步**（这是我们判断的下一步，但不是仪器问题，是知识问题）：
   我们打开的图里只有 `0x4012/0x4007/0x418E/0x428F/0x4006/0x48BA/0x42D3/0x40BD`；
   厂商两次抓包还配置过 `0x40bf/0x40c0`（param `0x08001008`，30 B）、`0x40c8`
   （`0x08001024`，14 B）、`0x4165`（`0x080010C4`，4 B = `{0x428f}`）、`0x4308`
   （TDM 的 `0x08001017/0x0800101B` 对）——**这些对象一个都不在我们的图中**。
   其中 `0x4165` 的 `{0x428f}` 看起来就是"把流图里的 TDM 端点绑到某个设备后端端口"。
   问题：这个绑定是必须由**设备图**（厂商 4 子图里那个 `0xb00000d6`/device graph）来完成，
   还是可以在我们这张合并图里用一个额外的 `0x4165` 参数包完成？
   厂商 blob 那条路已经在权限修好后重测过并判死（`sweetvgraph=1` 无应答，
   而且会把同一次开机里后面的所有 `graph_open` 一起拖死，见 `audio-closeout.md`）。

## 已排除，不要再来回

18 KB APM 级 SET_CFG（不是主因）；S1 权限（`AP=11` 已解）；缓冲区几何
（我们的 1152/2 槽就是厂商 UI 场景那一档的逐字复制）；AW882xx"I2C 不 ACK"；
ACDB / 文件 / 远程读 shm 三条取字节路线。
