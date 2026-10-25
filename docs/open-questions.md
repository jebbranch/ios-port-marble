# marble（SM7475）XNU 驱动 ADSP 出声 —— 最新现状与四个待判问题

## 0. 环境（不变）

- XNU 8796.141.3 通过 `fastboot boot` RAM-only 启动，**Android 不在运行**。
- ADSP 固件由 XNU 自己用 PAS 加载并释放 Q6；没有 Android 音频 HAL、没有
  audio-ion、没有任何 SMMU 上下文由 Linux 建立。
- 唯一日志是内核自维护的 ring buffer（~64 KB）；总线一挂就什么都读不到。

## 1. 已经确定可用

控制面全部打通：GRAPH_OPEN / PREPARE / START、TDM 全部参数（`tdm_rate`、
`tdm_cfg` 等都是 status 0）、AW882xx I2C 初始化、LPASS 时钟投票。

**但所有"成功"的命令都有一个共同点：DSP 不需要读任何内存。**
参数内联在 GPR 包里，DSP 解析完就回 ACK。

## 2. 唯一的硬阻塞（本轮把它收敛成一句话）

> **只要 DSP 去读我们映射的那块内存，整机在几毫秒内冻结。**

四个维度全部试过，结果**完全一样**（都是瞬间冻结、日志在某一行戛然而止）：

| 维度 | 试过的取值 | 结果 |
|---|---|---|
| apps-smmu 配置 | 完整 S1（SMR `0x1801`→CB5 + 恒等映射）/ **一条 SMR 都不写** | 相同 |
| 翻译正确性 | 自建 S1 恒等映射（描述符已修正）/ **故意全空的页表** | 相同 |
| fault 模型 | `SCTLR.CFCFG=1`（stall）/ `CFCFG=0`（terminate） | 相同 |
| buffer 物理地址 | `0xa2e00000`（System RAM 起点）/ `0xfbc00000`（**Android 实测的 `audio_cma_region`**） | 相同 |

关键时序证据（用 `sweetpa` 和不同 CB 配置都复现过）：

```
SWEET SMMU: gfsr cleared -> 0
SWEET GLINK: data_buffer send
SWEET GLINK: data_buffer sent; gfsr=0 fsynr1=0     ← 发完「立刻」读 GFSR 成功
SWEET GLINK: data_buffer sent, polling GFSR        ← 这一行能落盘 ⇒ 紧接着的
                                                     sweet_log_sync() 也成功了
（然后死）                                          ⇒ 至少几毫秒内整机健康，
                                                     含 eMMC 数据通路
```

⇒ 冻结发生在"发送后几毫秒"，**不是一次可记录的 fault**，而是互连失去响应。

## 3. 本轮从厂商源码新挖到的三条事实

1. **`qcom,stream-id-range = <0x1800 0x400>`**
   DT 的 `apps-smmu@15000000/lpass_tbu@151e9000` 明确写了 LPASS TBU 的 SID
   范围 `0x1800..0x1BFF`；该节点还带 `base-status-reg = 0x151ce230`。
   全 SoC **只有一个** apps-smmu，没有第二个 SMMU。

2. **`arm-smmu-qcom.c` 的 bypass quirk**
   ```c
   /* writing BYPASS will end up written as FAULT */
   if (FIELD_GET(ARM_SMMU_S2CR_TYPE, reg) != S2CR_TYPE_BYPASS) { ... }
   ...
   arm_smmu_cb_write(bypass_cbndx, ARM_SMMU_CB_SCTLR, 0);   /* 注意是 0 */
   arm_smmu_gr1_write(GR1_CBAR(bypass_cbndx), CBAR_TYPE_S1_TRANS_S2_BYPASS);
   ```
   我们早就实测到"写 BYPASS 会被硬件改成 FAULT"，厂商源码印证了这一点。

3. **本板 CB0..5 的 `SCTLR = 0xe0`**（bootloader 留的）
   `M=0, CFRE=1, CFIE=1, CFCFG=1`。
   我们已试过把 6 个 bank 的 CFCFG 全部清 0（`0xe0 → 0x60`），**没有任何变化**。

## 4. 已经封死的观测手段

- **ATS1PR / PAR 不可用**：`IDR0 = 0x4c017e71`，bit26 `ATOSNS=1` ⇒ 地址翻译
  操作寄存器未实现，RAZ/WI。实测对"已映射"和"未映射"地址都返回 0。
- **GFSR 从来读不到非零值**：所有未冻结的 run 里 GFSR 都是 0（但那些 run
  DSP 根本没碰内存）；冻结的 run 里根本读不到。
- **CB 的 FSR/FAR 读出来是垃圾**（`FSR=0x400`，FAR 每次开机都变，像随机
  数据），不可信。
- **`SMR` 写某些 SID 会让 store 直接中止**：`0x1800` 必中止（踩过两次）；
  想铺满 `0x1802..0x180f` 时死在区间内某一条上。每踩一个就是一次重启。

## 5. 我现在的推断

四个维度的"完全不变性"最自然的解释是：**DSP 的真实 SID 从来没匹配上我们写
的任何 SMR**。既然不匹配，那么 CB 怎么配、页表对错、fault 模型如何，
当然全都一样。

旁证：DT 的 `qcom,smmu-sid-mask = 0xf` 意味着真实 SID 只能是
`0x1800..0x180f` 之一，而我们只写了 `0x1801`；mask 字段从 NS 侧写不进去
（写即中止）。

## 6. 四个问题

1. **"四维不变性"是否足以支持"SID 从未匹配"这个结论？** 还有没有别的机制
   能让这四种截然不同的配置产生完全一致的行为？（注意：不写任何 SMR 时，
   按规范未匹配流应产生 USF 并终止事务，但实测仍是整机冻结。）

2. **在 `USFCFG=1` 且流确实未匹配的情况下，Qualcomm 的 TBU 会不会"卡住"
   而不是终止？** 有没有公开依据说明 qsmmuv500 的 TBU 对 USF 的处理与 ARM
   通用规范不同？

3. **有没有办法在不重启多次的前提下确定 DSP 的真实 SID？** 我们已经确认：
   没有"最近匹配 SID"寄存器、ATS1PR 不可用、GFSR 读不到。是否还有
   （a）TBU 侧的状态/调试寄存器（本机有 `base-status-reg = 0x151ce230`、
   `DEBUG_SID_HALT_REG` 等）；
   （b）通过 GLINK/APM 命令让 DSP 自报其 SID；
   （c）其他任何不依赖事后读 SMMU 的手段？

4. **共享内存是否必须先在 DSP 侧注册某个 "mempool"？** 现象是：把 `GRAPH_OPEN`
   改成厂商形态（40 字节描述符，图定义放共享内存里）之后，**挂死点从
   DATA_BUFFER 提前到了 GRAPH_OPEN** —— 因为此时 DSP 必须去读那块内存。
   这是否说明问题不在 SMMU 翻译，而在更下游的 XPU / VMIDMT / 目标侧权限
   检查？如果是，有没有办法让 AP 侧"授予"DSP 对某段 DDR 的访问权？

## 7. 我准备做的下一步

在 XNU 里监听 GLINK 的远端 nameservice 通告，确认这台机器的 ADSP 固件是否
真的公告了 `DIAG_DATA` / `DIAG_CTRL` / `DIAG_CMD` 这三个通道。
如果有，才值得去实现 DIAG 握手拿 DSP 内部日志；如果没有，这条路直接排除。
（这一步零风险：只读 nameservice 表，不触发任何 DSP 动作。）
