# 从这里开始（marble / Redmi Note 12 Turbo，XNU 8796 + iOS 16）

> 目标：**只做 `fastboot boot`（纯 RAM 运行，不刷分区、不动 GPT）**，把
> Redmi Note 12 Turbo（marble，SM7475）跑到 iOS 16 用户态，最终能当一台
> 能用的机器（显示/触摸/USB/电池/RTC），并让扬声器出声。
>
> 本文是入口；细节都在 `docs/`，按下面的索引查。

## 一、现在能做什么（2026-10-27，内核 v90）

| 能力 | 状态 | 说明 |
|---|---|---|
| 启动到 SpringBoard | ✅ | shim + 自建 ADT + XNU 8796，RAM-only |
| 显示 / 触摸 / USB | ✅ | 自建驱动；触摸是 GPIO 软件 SPI |
| UFS root | ✅ | userdata 尾部的 HFS+ 镜像 |
| 8 核 SMP | ✅ | GICR stride 0x40000 修复后 8/8 起核，调度器分配工作 |
| ADSP / GLINK / GPR / APM 控制面 | ✅ | GRAPH_OPEN/PREPARE/START、TDM SET_CFG 全 status 0 |
| GPU SMMU（kgsl-smmu） | 🟡 探测完成 | 全局寄存器可读，SMR/S2CR=TRANS 可写，BYPASS 被禁，CB 页访问挂死 |
| 音频出声 | ⏸ 暂停 | 挂死原因已找到（见下），DSP 仍不回 DATA_BUFFER_DONE |
| 电池 | 🟡 通道已开 | pmic_glink 能 open，属性请求的响应格式不对 |
| RTC | 🟡 读到了 | PMK8350 RTC 由 EE4（ADSP）拥有，AP 写入被拒 |

## 二、几个必须知道的结论

### 1. 音频：挂死的真正原因不是 hypervisor
厂商 `SHM_MAP` 的地址是 `{addr_lsw=0x1FFD0000, addr_msw=1}`。高字不是地址
（33 位 IOVA 会让 DSP 翻译 fault），而是 **stream index**。我们一直发
`msw=0`。加上 `sweetshmhi=1` 后**不再挂总线**，控制面全绿；剩下的缺口是
厂商那块 **18 KB 的 APM 级 SET_CFG**（`handle=0xb0e36868, size=18128`），
kprobe 只抓到包头、没抓到共享内存内容。详见 `docs/audio-closeout.md`。

### 2. 多核：GICR stride
marble 的 redistributor 间距是 **0x40000**，XNU 默认 **0x20000**。
`cpus=1` 永远只用 offset 0，所以一直没暴露。修法：`gicrstride=0x40000`
（`mkimage.sh` 默认已带）。AMP / E-P 调度需要 CLPC/Edge policy，暂时
没跑起来（`docs/status-log.md`）。

### 3. GPU：CB 页不可达
`kgsl-smmu` 的 `0x0000..0x1FFF` 可读写（SMR、S2CR=TRANS 都能保持），
但 `0x80000+` 的 context bank 页一碰就整机挂死；BYPASS 被上层改写为
FAULT。没有 CB 就建不了页表。详见 `docs/gpu-probe.md`。

### 4. RTC：AP 没有写权限
PMK8350 RTC（SID 0，ctrl `0x6146`、write `0x6140..0x6143`、read `0x6148`）
读出来是 1972（未设置）。AP 侧写入被 SPMI 拒绝——SweetBattery 的探测日志
直接写着 `ppid 0x61 ... owner=4` 和 `write refused ppid=0x61 owner=4`，
即 **RTC 外设归 EE4（PMIC 固件）所有**；Vendor DT 的 `rtc@6100` 也没有
`allow-set-time`，TWRP 的 `hwclock -w` 因此同样 EPERM。
**iOS 的时间实际来自 `sweettime`**（构建镜像时写入 cmdline），不依赖 PMIC
RTC；要真正持久化 RTC 需走 ADSP/PMIC 固件的服务，不建议抢 APID 所有权。

### 5. 电池：通道能开，协议还没对上
`PMIC_RTR_ADSP_APPS` 能 open（ack=1、remote=1、rcid=6），请求也发出去了，
但返回的 24 字节看起来是请求回显/ack，不是属性值（所有请求都解析成 1）。
需要对厂商的 pmic_glink 收发做一次抓包（和当年抓 GPR 一样的方法）。

## 三、构建与启动

```bash
# 1) 取内核（CI 的 kernel-latest）
./scripts/fetch_kernel_api.sh        # 或手动下载 release asset

# 2) 生成 RAM-only 启动镜像
D22AP_ADT=/path/to/d22ap.adt ./scripts/mkimage.sh

# 3) 启动 + 拉日志
./scripts/boot_one.sh images/shim-iosroot-adsp.img <label>

# 多核 + 调度采样（测试用；默认不带）
KEEPALIVE=1 CPUS=8 ./scripts/mkimage.sh \
  sweetsmp=1 sweetsmpn=20 sweetsmpms=1000 sweetglink=0
```

**注意**：`sweetsmp=1` 是测试开关，采样 20 秒后主动复位；**默认 cmdline
里没有它**，正常启动不会自己重启。

## 四、文档索引

| 文件 | 内容 |
|---|---|
| `docs/status-log.md` | 按时间追加的完整实验日志（最新结论先看这里） |
| `docs/audio-closeout.md` | 音频收尾：SHM_MAP 高字、剩余 18 KB 缺口 |
| `docs/smmu-dead-ends.md` | apps-smmu 的寄存器地图与已证伪/已修正的路线 |
| `docs/gpu-probe.md` | kgsl-smmu 探针、组合扫描结果、CB 页墙 |
| `docs/speaker-path.md` | 扬声器 = TERTIARY TDM + 2×AW882xx（无 WSA） |
| `docs/handoff.md` | 常用命令、boot args 速查 |
| `docs/required-assets.md` | 需要的 Apple 资产及来源/法律说明 |

## 五、下一步

1. **电池**：抓厂商 pmic_glink 的收发（`dumpsys battery` 时），把属性
   ID/响应偏移对上；接上后 `SweetBattery` 会自动用 `sweet_batt_get()`。
2. **RTC**：找 ADSP 的 RTC 服务（或 pmic_glink 里的 RTC client），在 ADSP
   侧设置时间；AP 直接写 SPMI 已被证明不通。
3. **启动时间**：kernel 到 launchd 约 39 秒（4 核/8 核一样），需要打点找
   最大的一段；E/P 调度以后再说（需要 AMP/CLPC）。
4. **音频（可选）**：抓那 18 KB APM 配置，再回到 `sweetshmhi=1` 的路线。
