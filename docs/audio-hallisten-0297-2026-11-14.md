# 0297 厂商TDM位时钟票：离线判据与第73枪事前门

## 目标与单变量

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

## 严格应答与资源规则

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

## 离线结果

- 实际period/runtime/GLINK adapter：52项通过。
- 生产period核心：65536个S16值、136656断言、ASan/UBSan与`-Werror`通过。
- AW88266实际状态机与44项表：10场景通过；失败路径双关断。
- 0297单文件`git apply --check`、apply、post-image逐字节相等。
- GLINK全链：applied=238，历史skip=49不增加，post-image等于链树HEAD。
- request/release包逐字段、默认路径不投票、request拒绝、START失败、STOP失败、
  CLOSE失败、PLL前门失败、DSP失败、release拒绝均有主机对照。

证据在`verify/hallisten-0297/`。这些仍是离线/模拟传输测试，不证明设备PLL会锁。

## 第73枪事前判据（新完整构建与镜像冻结后才允许执行）

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

## 第73枪冻结身份

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
