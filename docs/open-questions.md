# marble ADSP 音频：突破 + 一个具体的页表问题（求第二意见）

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
