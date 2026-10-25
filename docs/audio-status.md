# Marble XNU 出声 — 当前状态（2026-10-26 00:10，build v57）

> 目标：给 Redmi Note 12 Turbo（marble，序列号 **2aa26095**）RAM-only 引导
> XNU/iOS 16，让扬声器真的出声。**只做 `fastboot boot`，不刷分区、不动 GPT。**
> 时间线见 `STATUS.md`；上手流程见 `HANDOFF.md`。
> 本文件只写**有证据的结论**；推测必须标「假设」。

---

## 〇、一句话现状

iOS 能起并**进过 GUI（SpringBoard 起来了、屏也点亮过）**；音频控制面全通；
数据面**今晚有决定性进展**：找到了让整机不再挂死的地址表达方式，
并且证明 SMMU 能成功翻译我们的页表——见 §2.12。

---

## 一、已验证可用（真机）

| 环节 | 证据 |
|---|---|
| XNU 8796 arm64 引导 | shim + 16K 页 + ADT |
| **GUI / SpringBoard / Siri 欢迎页** | `KEEPALIVE=1` + 单核 + 无 `sweetpcm` 时出现 |
| ADSP PAS 固件加载 + Q6 释放 | `pas_init_image`/`pas_mem_setup` = 0 |
| GLINK `adsp_apps` 通道 + GPR | VERSION/OPEN/OPEN_ACK + intent 全通 |
| LPASS 时钟 | PRM 投票 status 0；clk 0x307/0x30e ok |
| AudioReach 图 open/prepare/start | status 0（含 WR 合并图） |
| TDM 配置 | `tdm_intf\|ep_mf\|frame\|power\|clk` = 0；**`tdm_rate`/`tdm_cfg` = 0**（v39 修好，见 §2.8） |
| AW882xx I2C 初始化 | 两片 0x2032，profile 44/44 |
| SHM_MAP | 44B 包，`pa=0xa2e00000`，`status=0`，handle `0xb0e15558` |
| 开机图（PSD） | 1080x2400 |
| **震动（电源键触发）** | 用户实测按电源键有震动 ⇒ 0183 成立 |
| 电池图标不再谎报充电 | 0182：无采样时报 battery-only |

---

## 二、音频卡点：DSP 读主存（当前唯一硬阻塞）

控制面、共享内存映射、图启动全部成功；**第一次 `DATA_BUFFER_V2` 之后 DSP
挂死，APM 不回包，随后总线卡死**（连函数自带的 100 ms 超时打印都出不来）。

### 2.1 apps-smmu 事实（厂商源码 + 真机回读，均已核对）

| 项 | 值 / 结论 | 来源 |
|---|---|---|
| 基址 | `0x15000000`，DT `reg` 1 MiB；`tcu-base=0x151ce000` | 本机 DT |
| 属性 | `compatible=qcom,qsmmu-v500`，`#iommu-cells=2` | 本机 DT |
| ID0 / ID1 | `0x4c017e71` / `0x6000004b` | ring |
| numpage | ID1.NUMPAGENDXB=6 ⇒ 128 页 ⇒ CB(n)=`0x80000+0x1000n` | `arm-smmu.c` |
| 可读区 | `0x0..0x3FFF`（GR0/GR1+2页）、`0x80000..0x8F000` | 实测 |
| **空洞** | **`0x15004000` 读就外部中止**（ESR 0x96000010） | run86 |
| CBAR(0..5) | `0x1f000` → **Type=1 = S1 翻译 + stage-2 BYPASS** | run87 |
| CBAR(6..) | `0` → Type=0（S2 翻译），但同页 SCTLR/VTCR 值形似指令 | run87 |
| CB0..5 SCTLR | `0xa9f00e0` / `0xe0`，VTCR=0，VTTBR=0（未启用翻译） | run87 |
| sACR | `0x1c` ⇒ **CACHE_LOCK(bit26) 本来就是 0**，不是拦路者 | run80 |
| sCR0 | `0x200406` ⇒ **USFCFG(bit10)=1 且清不掉**（未匹配流会被拦） | run81 |

### 2.2 写入能力（这是全部关键）

| 操作 | 结果 | 证据 |
|---|---|---|
| `SMR[n] = VALID\|0x1801`（MASK=0，n=0x4d） | ✅ 可写，回读一致 | run77/81 |
| `SMR` 带任何 MASK≠0（`0x800f1800`/`0x800f1801`） | ❌ 写即中止 | run69/82 |
| `SMR` 用 SID `0x1800` | ❌ 写即中止 | run70 |
| `S2CR[n] = type0\|cbndx`（0x3/0x6/0x34） | ✅ 可写，回读一致 | run74/81 |
| `S2CR = 0x10000`（BYPASS） | ❌ 回读变成 `0x20000`（FAULT） | run71/73 |
| `CBAR(n)` 写（CB6/CB52） | ❌ 外部中止（读正常） | run77/79/80 |
| `CBA2R(n)` 写 | ✅（回读 0x1） | run77+ |
| CB 窗口 `VTCR/VTTBR0/SCTLR` 写（`0xB4000`） | ✅ 四个回读全等 | run81 |
| 写 `sACR` | ✅ 但值本来就对 | run80 |

### 2.3 DT 里的 SID 语义

```
/soc/spf_core_platform/qcom,msm-audio-ion
  iommus              = <&apps_smmu 0x1801 0x0>    (#iommu-cells = 2)
  qcom,smmu-sid-mask  = 0xf
```
⇒ 官方语义是「匹配时忽略 SID 低 4 位」，但**本机 SMR 写不进 MASK**
⇒ 只能精确匹配 `0x1801`（**这就是 2.2 里那条矛盾的根源**）。

### 2.4 已经试过的四种配置，挂死表现完全一样

| 配置 | 结果 |
|---|---|
| `S2CR=0x20000`（FAULT） | 挂死 | run68/71/73 |
| `S2CR=0x3`（bootloader 的 CB3） | 挂死 | run75 |
| 自建 CB52 + 两级页表（翻译，回读全对） | 挂死 | run81 |
| `sweetcb=0`：引到 **stage-2 bypass** 的 CB0 | 挂死 | run88 |

四种互不相同的 SMMU 配置表现一模一样 ⇒ 只有两种解释：

1. **DSP 的 SID 根本不在我们写的 SMR 上**（`sCR0.USFCFG=1`，未匹配即 fault），
   那么配什么 bank 都无效；
2. **卡点不在 SMMU**（在 WR 数据通路 / GPR 语义上）。

另外：`cbm6/cbm7` 的 SCTLR/VTCR 每轮都在变（0x201d2716↔0x201d2516、
0x1d00↔0x1c00）⇒ 那段地址是易变数据，**不是稳定寄存器**，
进一步说明 CB6+ 不可依赖。

### 2.5 ★ 修正（外部评审指出，已核对）

1. **run90 作废**：`0xd0c00000` **不在** ADSP carve-out 内。
   区域 = `[0x9fd00000, 0x9fd00000+0x03100000) = [0x9fd00000, 0xa2e00000)`，
   我算成了 0xd0…，越界 783 MiB。所以"换成 DSP 必然可访问的内存也不行"
   **没有得到证明**。
2. **`DATA_BUFFER_V2` 的地址语义可疑**：上游 AudioReach 结构的前两个
   payload 字是 `buf_addr_lsw/msw`（地址），不是 offset。我们发的是
   `{0 或 0x800, 0, handle, size}` —— 如果固件按地址解释，DSP 会去读
   物理地址 0 / 0x800，必然出错。**这能一致地解释所有冻结**。
3. **`S2CR.Type=0` + `CBAR.Type=1` 并非非法组合**，且 CB0 的 `SCTLR.M=0`
   意味着 S1 翻译未启用。所以"四种配置表现一样"不能推出 SMMU 配法错。

### 2.6 v31：按评审建议改的两个实验

| 开关 | 内容 | 判据 |
|---|---|---|
| `sweetpamode=1` | `DATA_BUFFER` 的地址字段改发 **`g_shm_pa + off`**（真地址）| 不再冻结 ⇒ 根因是地址语义，不是 SMMU |
| 默认 | 发送后、**读 GLINK FIFO 之前**先快照 `sGFSR` / `sGFSYNR0/1/2`，以及 CB0..5 的 `FSR/FAR/FSYNR0` | `GFSR.USF=1` ⇒ 没有 SMR 匹配，`GFSYNR1[14:0]` 就是 DSP 真正的 SID；某个 CB 有 `FSR` ⇒ 事务进了上下文但翻译失败 |

先跑 `sweetpamode=1`；无论结果如何，fault 快照都会给出上面二选一的证据。

### 2.7 ★★★ 顺序纠正：BADPARAM 是安全刹车，不是 bug

把三次实验按「参数是否合法」重排（一手证据，run93/96/97/98/99/100/101）：

| 实验 | flag | 地址 | 参数合法？ | 结果 |
|---|---|---|---|---|
| A | 4 = offset | 偏移 0 / 0x800 | ✅ | **整机挂死** |
| B | 4 = offset | 绝对 `0xa2e00000` | ❌ 远超 4096 区域 | `BADPARAM`，**安全** |
| C | 0 = PA | 绝对 `0xa2e00000` | ✅ | **整机挂死** |

两种「正确形式」都挂死，唯一安全的是**参数非法**那次。
⇒ **在 DSP 能安全读内存之前，不可能拿到 status=0**——参数改「对」就等于挂死。
所以工作顺序必须倒过来：先解决可达性，再谈参数。

另一个一手结论（run92 复核）：**MAP + PREPARE + START 是安全的**，
同一块区域、同一个 flag=4，只要不发 `DATA_BUFFER` 就正常走完并复位。
⇒ 挂死只发生在带合法 offset 的数据帧之后，**不是图启动引起的**。

### 2.8 TDM 闭环（v39，run101 已验证）

之前每轮 `tdm_mf = 0x3 (EUNSUPPORTED)` 的原因：**我们对 TDM 端点发了通用
media format `0x0800100C`，而厂商用的是 TDM 专用的一对参数**
`0x08001017`（12B `{96000, 0x30020, 1}`）+ `0x0800101B`（28B
`{0,2,0x10001,7,0x200004,0,1}`）。改发这两个之后全部 `status=0`。
**这条已闭环。**

### 2.9 ★ SMR 表全貌与「没有空闲 CB」

每轮 probe 都打印，12 轮完全一致：

| SMR | SID | mask | → CB |
|---|---|---|---|
| 0 | `0x0e0` | 0 | CB0 |
| 1 | `0x4a0` | 0 | CB1 |
| 2 | `0x0c0` | 0 | CB2 |
| 3 | `0x2800` | `0x402` | CB3 |
| 4 | `0x000` | 0 | CB4 |
| 5 | `0x584` | `0x11` | CB5 |

槽位 6..112 全空。推论：

1. **bootloader 没有配 0x1801**，那条是我们自己写进 slot 0x4d 的。
2. **CB0..5 全被占用**，没有空闲 context bank；CBAR≥6 又写不进去
   ⇒ 「新建一个 CB」彻底不可行。
3. 把 0x1801 引到 CB5 等于与原生 SID `0x584` **共享同一个 bank**，
   开 S1 翻译会同时改变后者的行为（未评估的风险）。
4. DT 的 `qcom,smmu-sid-mask = 0xf` 只忽略 SID 低 4 位 ⇒ 真实 SID 可能是
   `0x1800..0x180f` 任意一个，**只写精确 0x1801 很可能根本不匹配**。
   v40 补全 16 个。

### 2.10 ★ 新工具：SHM_MAP 是无风险的地址探针

`SHARED_MEM_MAP_REGIONS` 只做登记、不搬数据 ⇒ 用返回的 status 就能安全地
探出 DSP 认可的内存窗口（run102 实测）：

| 地址 | flag=4 | flag=0 |
|---|---|---|
| `0xa2d00000`（`adsp_region` 内） | 拒绝 | 拒绝 |
| `0x8ea6dc000`（64 位，XNU 的页） | 拒绝 | 拒绝 |
| `0xa2e00000`（System RAM 起点） | 接受 | 接受 |
| `0x11ffe8000`（厂商 IOVA） | 接受 | 拒绝 |

⇒ DSP 的合法窗口 ≈ **32 位、非保留的 System RAM**。
DT 复核：`adsp_region = <0 0x9fd00000 0 0x03100000>` 且 `no-map`
⇒ `[0x9fd00000, 0xa2e00000)` 半开，`0xa2e00000` **不在** carve-out 内。
`adsp_heap_region`(12 MiB) / `audio_cma_region`(28 MiB) 都是
`shared-dma-pool` + `reusable`，由内核启动时动态分配，**XNU 下不存在**，
不能拿来当目标地址。

### 2.11 ★★★ 厂商报文逐字段比对（534 个包全解，2026-10-25 21:30）

> ⚠ 以前的解析把 `p0..p23` 的**十进制**当成了十六进制，所以"找不到 opcode"。
> 重解后全部可读，以下是一手数据。

GPR 头布局（与实现一致）：`w0=(cmd<<4)|(len<<8)`、`w1=domains`、`w2=src`、
`w3=dst`、`w4=token`、`w5=opcode`、`w6/w7=addr lsw/msw`、`w8=handle`、`w9=size`。

| 命令 | 形态 | 关键字段 |
|---|---|---|
| `SHM_MAP` `0x0100100c` | 44B，**无描述符**，正文从 w6 起 | pool=3\|cnt1, flag=4, addr=`0x1ffe8000`/msw=1, **size=`0x5000`** |
| `GRAPH_OPEN` `0x01001000` | **40B，图在共享内存里** | addr=0, handle, size=`0x1f0`(流图)/`0xb38`(设备图) |
| **APM 级 `SET_CFG`(dst=1)** | **40B，参数在共享内存里** | addr=`0x400`/`0x1000`/0, handle, size=`0x108`/`0xb0`/`0x70` |
| **模块级 `SET_CFG`(dst=模块 iid)** | 72/88B，**内联 handle=0** | **与我们一致 ✓** |
| `DATA_BUFFER_V2` `0x0400100a` | 68B | src `0x2011` → dst **`0x40bd`**，token **0/1**，buf **偏移 0/0x800**，size `0x480` |

**同一个 handle 承载全部内容**：所有 `GRAPH_OPEN` 与全部 479 个 `DATA_BUFFER`
都用 `handle=0xb0d890e8`。区域偏移分配：`0x0` 图定义、`0x400`/`0x1000` APM
参数块、`0x0`/`0x800` PCM buffer（GRAPH_OPEN 之后复用）。⇒ 区域必须 ≥ `0x5000`。

**我们的差异**：

| 项 | 厂商 | 我们 | 判定 |
|---|---|---|---|
| 区域大小 | `0x5000` | `G_SHM_SIZE = 0x1000` | ❌ 太小，装不下图的布局 |
| 区域地址 | `0x1_1FFE8000`（33 位 IOVA） | `0xa2e00000`（物理） | ⚠ 待定 |
| **APM 级命令（dst=1）** | **40B 描述符形态** | **内联 payload，handle=0** | ★ 结构性差异 |
| 模块级 SET_CFG | 内联 handle=0 | 内联 handle=0 | ✓ |
| DATA_BUFFER | 见上 | 同 | ✓ |

DSP 对两种形态**都回 status=0**，所以"成功"不能证明它按我们的意图解析了。
**若 APM 把内联的图当成"区域 offset=0 处的内容"，它看到的图与我们的设计
完全不是一回事**——这是一个能解释"控制面全通但数据面必挂"的结构性嫌疑。

---

### 2.12 ★★★★ 数据面的决定性进展（run118~121）

**(a) 之前所有"整机冻结"的根因是地址表达方式。** 厂商 `SHM_MAP` 登记的是
**33 位 IOVA `0x1_1FFE8000`**，DATA_BUFFER 用裸偏移；我们此前一直用自己挑的
32 位物理地址 `0xa2e00000` 当区域地址 ⇒ 任何合法数据帧都让整个 LPASS
（SMMU 与 GLINK FIFO 一起）失去响应。改成登记 IOVA 后整机不再挂死，
并拿到真实的 context fault（`FAR` 正是那个 IOVA）。

**(b) SID 与 CB 路由都是对的。** `FAR=0x1ffe8000` 证明 DSP 的访问确实进了 CB5，
"流没匹配上"的猜测被推翻。

**(c) 三条配置对比，规律是反的：**

| 配置 | 翻译 | 整机 |
|---|---|---|
| 33 位 IOVA `0x1_1FFE8000`（T0SZ=31） | 失败（TF, terminate） | **活着** |
| 32 位 IOVA `0x1FFE8000`（T0SZ=32） | **成功** → `0xa2fe8000` | **挂死** |
| 32 位物理地址 `0xa2e00000`（早期） | 成功（恒等） | 挂死 |

⇒ **访问被 SMMU 干净拒绝时整机安然；一旦翻译成功、DSP 真拿到数据，反而挂死。**

⇒ **页表已经基本做对**（`L1[0]→L2[255]→2 MiB 块`），剩下的挂死发生在
"DSP 读到 buffer 之后"。

**(d) 已排除的变量**（每个都是单变量）：`TCR2.SEP=7`（run120，故障完全不变）、
末级描述符类型（2 MiB 块 vs 4 KB 页，同一个 Level-1 TF）、
fault 模型（stall/terminate）、表基址、NS 位（后两者开关已就绪未测）。

---

## 三、别再试的死路（省后来者的时间）

1. `S2CR` 写 BYPASS（0x10000）—— 硬件改成 FAULT。
2. `SMR` 带 MASK（任何非零）—— 写就中止。run69/82 两次踩到。
3. `SMR` 用 SID `0x1800` —— 写就中止（那是安全世界的）。
4. 写 `CBAR(n)`（n=0/6/52 都试过）—— 读正常、写中止。
   ⚠️ 严谨说法：只证明**这三次访问**失败，不能断言任何视图下都不可写。
5. **逐页扫 SMMU 寄存器块** —— `0x15004000` 是洞，读就 panic。
6. 在安卓里用 kprobe `@0x15000934:u32` 读 SMMU —— 线性映射读不到
   ioremap 的设备内存，全 0。
7. 用 bootloader 的 CB3 直接跑数据面（`S2CR=0x3`）—— DSP 冻结（run75）。
7b. 把音频流引到 **stage-2 bypass** 的 CB0（`sweetcb=0`）—— 同样冻结（run88）。
    四种配置（fault / CB3 / 自建翻译 / bypass）表现完全一致。
8. `cpus>1` —— `pe_init_fiq` 在次核上写未映射地址，必 panic（未修）。

---

## 四、电源（充电）

- **充电由 ADSP/PMIC 固件管**，不是 SPMI 寄存器：安卓 dmesg 的
  `BATTERY_CHG: [ADSP] fg_update_status:: ssoc/vbat/ibat`。
- 通道现成：ADSP 每轮启动都发 `OPEN PMIC_RTR_ADSP_APPS`（rcid=6）。
- XNU 客户端（`sweetbatt=1`）：开 LCID 2 → 回 OPEN_ACK → advertise 128B
  intent → 发 24B `GET_PROP`（owner `0x800A`, type=1）：
  `op=0x30` 读状态(0)/电量(4)/电压µV(7)/电流µA(9)/温度(13)，
  `op=0x32` 读 USB 在线(0)。
- **已实测**（run84）：`ack=1 remote=1 rcid=6`，6 个请求都有回应，
  但**全部解析成 `prop=0 value=1`** ⇒ 响应字段偏移不对。
  v25 起对前两条回应做**原始 hexdump**，下一轮就能定字段。
- **只读**：真正充电要给 battmgr 发 SET_PROP（`0x31`/`0x33`），尚未实现。

## 五、震动

- 硬件：PM8350B（SPMI SID 3），`0xf066` 清故障 → `0xf046=0x80` 使能 →
  `0xf04c=0x81` play；hBoost 走 SDAM `sdam@9d00`（PBS_ARG 0x42 / TRIG 0xe5）。
- 已接线：电源键按下 → `sweet_hid_key(0,1)` → `sweet_haptics_kick()`
  （带重入保护）。**用户实测有震动 ⇒ 已验证**。
- 缺的是**系统级触感**（键盘等）：要走 Apple 的 haptics 栈，未做。

---

## 六、稳定性 / 环境（血泪规则）

1. **`ml_io_map` 是一次性共享池**：`osfmk/arm/io_map.c` 的 `IO_MAP_SIZE`
   已由 0185 调到 **64 MiB**（原来 8 MiB，SPMI 一个窗口就吃 4 MiB，
   GUI 起来后必然 KERN_NO_SPACE panic）。新代码仍优先用
   `g_map_win()`（`kmem_alloc(kernel_map)`+`pmap_map`）。
2. **低区 PA 能映射读，但绝不能 `flush_dcache64`**（`phystokv_range()` 只认
   ADT 银行 0x840000000+）。用 `dsb sy`。
3. **默认启动不碰 SMMU**：所有 CB 相关写入都在 `sweetpcm=1` 之后。
4. **多核 panic 是已知未修 bug**：`arm_init_cpu → pe_init_fiq` 写
   `0xfffffe000d514008`（次核 pmap 未映射）。交互用 `cpus=1`。
5. **触摸是 GPIO 软件模拟 SPI**，每轮 `mt_wait(2)` → 天生延迟、易丢帧。
6. **功放开机即初始化且未静音**，无流时有爆音（按键/触摸可闻）。
7. **`tdm_mf` 返回 0x3**（其余 TDM 配置都 0）—— 从未查过，可能是隐患。
8. **build tag**：每次改动必须改 `SWEET AUDIO: build vNN (...)`，
   看门狗用它校验产物（marker 必须出现在二进制里，注释会被剥掉）。
9. `mkimage.sh`：`KEEPALIVE=1` 去掉 `sweetreboot=1`；`CPUS=n` 覆盖 `cpus=1`。

---

## 七、GPR/APM 协议速查

| 命令 | 长度 | 关键字段 |
|---|---|---|
| GRAPH_OPEN（共享内存） | 40 | hdr24 + descriptor16（lsw/msw/handle/size）|
| DATA_BUFFER_V2 | 68 | **dst = WR iid `0x40bd`**，src `0x2011`，token 0/1 |
| SHARED_MEM_MAP_REGIONS | 44 | hdr24 + `{pool=3\|count=1, flag=4, addr, size}` @+24 |
| SET_CFG media format | 30 | `0x0800100C` → **WR 实例**，96k/32/2 |

- domain 字：普通命令 `0x00000302`；MAP/UNMAP 额外置 `0x10000`。
- MAP 成功应答 `0x02001001`，payload 4B handle @+24（包长 28）。
- 顺序必须 **OPEN → SHM_MAP → SET_CFG → PREPARE → START**。

## 八、关键地址

- XNU 管理内存：ADT 银行 `0x840000000 + 0x100000000`（33–37 GB）。
- 低 4 GB 普通 RAM 例：`0xa2e00000`（PCM 页）；S2 页表放
  `0xa3000000`（L1）/`0xa3001000`（L2）。
- 厂商 IOVA `0x1_1FFE8000` 是 SMMU 虚拟地址，不是物理地址。
- SMMU：`0x15000000`；GR1 `+0x1000`（CBAR(n)=`+0x1000+4n`，
  CBA2R(n)=`+0x1800+4n`）。CB 页按厂商算式是 `+0x80000+0x1000n`：
  CB0..5 已证实（CBAR 读得到值），**CB6 及以上未证实**（同页其它寄存器
  读出的值像指令），所以别再假设 CB52 存在。

---

## 九、下一步（按优先级）

> ★ 顺序已改：**先解决可达性，再谈参数**。理由见 §2.7。

1. **★ 定位"翻译成功后为何挂死"**（见 §2.12，当前最高优先级）：
   在 32 位 IOVA 配置下加 `sweetskipgfsr=1` 跑一轮，确定挂死发生在
   SMMU 的 MMIO 读、GLINK FIFO 读，还是更早 ⇒ 分清是 DSP 侧还是 AP 侧。
2. 若 AP 侧正常 ⇒ 排查 DSP 消费 buffer 之后的部分（写回 / TDM / 功放），
   TDM 链路已在 §2.8 全部 status=0。
3. **电池字段**：`sweetbatt=1` 跑一轮，看 v25 的原始 hexdump，
   对照安卓 `/sys/class/power_supply/battery/uevent`。
4. 功放静音（无流时不爆音）—— 与音频数据面解耦，可先做。
5. 用户态 bug（空密码仍要求设置、部分 app 打不开）—— 用户要求**暂缓**。
6. 多核 panic —— 独立课题，需查 `pe_init_fiq` 的 per-CPU 映射。

## 十、约束（务必保持）

- 只操作序列号 **2aa26095**；绝不碰新的 12+1TB 主机。
- 不刷 boot/vendor_boot，不改 GPT；只用 `fastboot boot`。
- 不把 Apple/Qualcomm/小米的专有二进制上传公开仓库。
- 补丁先在 `../ci-src/xnu-xnu-8796.141.3/` 上 `git apply --check` 再推；
  CI 应用失败只打 warning，**必须用 build tag / marker 校验产物**。
- 发 GPR 包前先在 `traces/` 找同 opcode 的包逐字段照抄，不要盲试 IID。
