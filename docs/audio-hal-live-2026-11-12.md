# 0292 第 69 枪：HAL capture-only 执行记录（2026-11-12）

## 开机前授权补充

用户已进入 TWRP，明确要求直接继续；若可行，直接固化进用户态，空间不足再通知。
这明确授权本轮在目标机当前 HFS 根盘中备份并替换现有 HAL executable、Info.plist
及其 8 B catalog logical-size 字段，以及一次既有隔离镜像的 fastboot boot。

对上一份事前规则的唯一用户授权修订：

- 非静音 PCM 判据和硬件隔离门不变，仍最多一次 XNU 引导。
- 若 capture 门通过且用户态没有新启动/服务故障，则可将安装保留在 HFS 用户态；
  明确只代表 capture HAL 常驻，不代表 DSP/扬声器链完成。
- 若失败、panic 或不可判定，恢复原 HAL，保留失败证据，不擅自追加第二枪。
- 即使保留，也保留本地逐块原文件备份和可恢复 manifest。不 flash 启动分区。
- 不因“固化”而开放 GPU、Wi-Fi、DSP/TDM、功放、听筒或任何增益修改。

固定镜像和 PCM 判据沿用 `audio-hal-ready-2026-11-12.md`；
SHA=13bb9272e9905f16ef58fc695432a324e2cb93228821ae799e35fa155515d533，
sweettime=1794493381。进入 TWRP 和挂载处理不冒充新增 XNU 音频实验；
当前新 XNU 引导仍未执行，预算 **68/120**。

## TWRP 安装前检查

- serial=2aa26095，adb state=recovery，adbd=root。
- userdata 几何为 start=24858624、size=470183896 sectors。
- 初检 TWRP 自动将 `/dev/block/dm-6` 挂到了 `/data`，尚不满足 raw 写入门。
- 已正常 `umount /data`；随后 `/proc/mounts` 确认 `/data` 不再挂载。
- 插件使用原有 65+1 块，修改一个原有 catalog block，不申请额外 HFS allocation。
- 重新读取当前卷头和目标位置、核本地门、建立独立安装备份后才可写入。

本轮安装备份目录固定为：
`out/hal-capture-0292-20261112/hfs-install-boot69/`。
不得用只读计划目录冒充安装备份。

## 执行与判决

### 安装完成，尚未引导

TWRP 下的新读计划与 Android 只读计划一致。HFS 为 4194304 个 4 KiB allocation
blocks，free=748380（约 2.85 GiB），本次 additional allocation=0，空间门通过。

安装器在独立目录保存三个原始块集合和 manifest，随后写入 65 块 executable、
1 块 Info.plist、1 块 catalog。脚本逐项读回相等后，又用独立 adb dd 再读并 cmp
三个 pending 文件，全部相等：

- executable allocation SHA256：
  `6cd037fa90ae5faee6701844f518805d5631b2263316404504c857027b56f272`
- Info.plist allocation SHA256：
  `049398ce4f25fb562fb1831e8840accd515b9f125a71951c77f670c42e8bcf1f`
- catalog block SHA256：
  `926fd4d4cda32eac39dda4dcf1ae32c3ee9e58d90277ef44f9f030228849da65`

已读取 `ring-hal-capture-boot69-pre.bin` 作为旧 ring 基线，total=101520，seq=221，
是第 68 枪遗留数据，不得当作第 69 枪。冻结镜像 verify-only 再次通过。
此刻未执行新 XNU 引导，预算仍 68/120。

后续按真实引导和 PCM 结果追加，不将“文件已写入”当作 HAL 实机通过。

### 第 69 枪已引导，等待 PCM 判据

`fastboot -s 2aa26095 boot` 一次成功：发送 50084 KB，Sending/Booting 都为 OKAY，
镜像保持运行。新 XNU 实验记为 **69/120**，不再将其写成“尚未开机”。
没有 flash，没有第二次 boot，没有启用 DSP/TDM/功放/GPU。

用户已进入 iOS，并反馈启动后一段时间内：

- 设置 → 声音 → 手机铃声会闪退；
- 设置 → 通用 → 关于本机先卡住，等设置自行闪退后再进可显示设备信息；
- 用户认为可能是部分缓存出问题，要求耐心等待。

以上是用户观察与假设，不是缓存根因已确认，也不能认定由本轮 HAL 引起。
不删除缓存、不改设置数据库、不额外重启。当前没有成功的铃声预览/PCM 读数，
所以尚不能判 HAL capture 通过，更不能宣布已经可固化。

USB en14（Sweet iOS）已出现、192.168.7.1/24 已由现有配置启用；现已在该既有
接口被动监听 UDP 5140。监听启动晚于开机，不把缺早期输出当作路径未执行。
核心判据仍要从用户返回 TWRP 后的当前 SWEETLOG 有效窗口读取。

### 当前 ring 已取得，旧规则下完成恢复

用户反馈任意铃声类别预览都会闪退，拖动了音量条，随后自行返回 TWRP。
取得当前 ring total=144841、seq=252，boot-args 的 sweettime=1794493381，
不是 101520 B 的旧第 68 枪 ring。

当前窗口有两次 halbridge marker、ADSP disabled、node open=1，
5 次 record-only volume generation，以及 10 次 PCM 日志快照。
最后一条为 writes=512、frames=524288、sound=500234、peak=32768、
queued=65536、overwritten=458752。前两块为零，第三块开始非零，计数递增。
这证明 capture HAL 已在本机进入真实 PCM 接收路径；不证明实时帧率、波形正确性、
DSP 渲染或扬声器有声。高峰值只是被缓存/统计，没有发给硬件。

依据当时的 UI 门，已在 TWRP 卸载 /data 后恢复原 HAL executable/Info.plist，
脚本读回与独立 adb dd + cmp 均通过，catalog size 已恢复为1172。未追加开机。

### 用户新决策：UI 问题不再阻断 capture HAL 常驻

用户随后明确表示，用户态使用障碍可以后期完善，不应阻断音频主线。
因此新决策为：

- 不追查或清理铃声页/关于页缓存，不把这些 UI 故障当作 capture 后端失败。
- 保留已通过非静音 PCM/硬件隔离门的同版 HAL 到当前用户态；重新安装时使用
  独立 `hfs-retain-boot69/` 备份目录，保留此前的安装/恢复记录。
- 不新增 XNU 引导，预算仍69/120；不声称已能输出声音。
- 后续只先离线设计连续 feeder、格式验证和强制幅度上限，再登记下一轮硬件门。
- HAL 输入曾达到32768，禁止未经限幅/衰减原样接DSP/扬声器；增益红线不变。
