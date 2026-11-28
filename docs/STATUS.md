# 当前状态（2026-11-28）

> 本页只回答「现在在哪、下一步是什么」。按时间的记录看 [`chronology.md`](chronology.md)；
> 可复现的构建与启动配方看 [`START-HERE.md`](START-HERE.md)。

## 一句话

iOS 16.7.16 用户态已能在 Redmi Note 12 Turbo 上以 RAM-only 方式启动到 SpringBoard 与设置；
触摸、按键、UFS/HFS+ 根、8 核 SMP、电池和关机已验证。**音频暂时停止在 73/120**；
当前主线是 **Wi-Fi（0299–0303，总开机 74/120）**，GPU 转为第二优先。

## 子系统

| 子系统 | 状态 | 当前位置 |
|---|---|---|
| 启动到 SpringBoard / 设置 | ✅ | 首次 2026-10-23 15:11 进桌面；CPU 软件渲染 |
| 显示 / 触摸 / 按键 | ✅ | Goodix GT9916R over SPI；多指与负载体验仍有限制 |
| UFS 读写 + HFS+ 根 | ✅ | root 位于 userdata 尾部；f2fs 未界定，Android 写满会覆盖，见下文 |
| 8 核 SMP | ✅ | 8/8 起核；代价是发热偏大 |
| 电池 | ✅ 基本实现 | capacity / vbat / temp 单位正确；**本机电流为负 = 充电、为正 = 放电** |
| 关机 | ✅ | userland 关机会真正执行断电 |
| USB | 🟡 | 枚举与控制请求有证据，稳定数据传输未收口 |
| 震动 | 🟡 | 按键与开机震动已解决；UIKit / CoreHaptics 接入 iOS 系统层面未解决 |
| RTC | 🟡 | 显示时间来自启动参数；PMK8350 RTC 归 ADSP，AP 写入被拒 |
| ADSP：PAS / GLINK / GPR / APM | ✅ | 控制面与数据面已通；声音未证明 |
| 音频（HAL / TDM / 功放） | ⏸ 暂停 | 第 73 轮判为 PRM 回包解析失配；0298 已离线修好绑定，尚无新内核/镜像/开机 |
| Wi-Fi / 蓝牙 | 🚧 当前主线 | 第 74 轮只读 handoff 通过，确认 QCA6490/PCIe0 交接后保持关闭；端点识别尚未实机验证。0302/0303 已离线集成，尚未整机启动 |
| GPU | 🚧 第二优先 | 旧 XNU 8796 线仍冻结；FoXLinne 的上游 iOS 18 实现可参考，iOS 16 适配未开始 |
| 基带 / modem | ⬜ 未开实验 | 先决门是 306 MB `mpss_region` 能否在本内核启动时持有 |

## 音频：停在哪儿

- 数据面 2026-11-02 已通：S1 叶子 `AP` 位修正后 `DATA_BUFFER done status=0`，1200 buffer 正常复位。
- 856 B A 形式图（含 `PCM_DEC` + `PCM_CNV`）能 open/start、能推 120 帧，DSP 归还缓冲；三次有效听测均无声。
- HAL capture 与 30-period HAL → AudioReach 连续流已通过（`30/30/30`，六条控制应答全 0）。
- 第 72 轮有效无声定位到功放没有 TDM 时钟（`SYSST=0`、PLL 全 0）。
- 第 73 轮不是 PRM 拒绝：0297 等错了应答类型。正确应答是 `0x02001002` + `0x0800102c`；0298 已离线修好绑定。
- **当前不做音频开机**。重新开启前必须重新构建完整内核与镜像，并通过全部本地门。

## Wi-Fi：当前主线

- 来源：FoXLinne `UniXNU_marble`，固定 commit `7a0d68f…`，BSD-2-Clause-Patent。
- 0299：纯只读 handoff survey；第 74 轮实机通过，未写 Wi-Fi 寄存器、未触发 DMA。
- 0300：QCA6490 上电、PCIe Gen2 ×1 链路与端点身份；只构建，未上机。
- 0301：修正 TCS IRQ 窗口与 C 接口；0300 明确禁用。
- 0302：把 ath11k/manifest、固件、DMA 路由与 SMMU 配置核心打进同一个可重定位内核对象；109 个无线栈对象，Clang 19 通过。
- 0303：runtime boot hook；**还未进入 XNU 的实际启动路径**。
- 下一道门：取得本机 SMMU bank/stream 的独占授权与 wired/无缓存 DMA backing，再接 MSI/MHI worker。不能照抄参考机 bank/slot，也不能因 MMIO 回读成功就放开 BME。
- 详见 [`wifi-foxlinne-port.md`](wifi-foxlinne-port.md)。

## 已知风险：f2fs 与尾部 root 重叠

root 镜像写在 userdata 尾部，但 **f2fs 没有被缩小，GPT 也没有改**，所以 f2fs 仍声明占有整块 224.2 GiB；
Android 写满就会吃掉 root。研究机日常还要开 Android，因此这是**活的风险**，不是理论问题。

目前没有机制阻止它，只能事后发现：开机前重读 HFS+ 卷头签名与 free 块数，发现被覆盖就重新分块写尾部。
长期方案二选一（都还没做）：把 f2fs 超级块的可用块数改到 root 之前，或建立自动预检/重写流程。
详见项目根 `docs/08-dual-boot-plan.md` §2。

## 预算与纪律

- 总开机预算 **74/120**；音频暂停在 73，Wi-Fi 第 74 轮只读 survey 用掉 1 次。
- 只允许 `fastboot boot`；禁止 flash / erase / format / set_active / oem 写命令。
- 一次开机一个会话；每次开机前完成本地验证；判据表先写死再开机。
- 不主动 collapse GPU；没有用户明确许可不开 GPU 枪。
