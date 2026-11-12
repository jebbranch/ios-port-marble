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

尚未安装或引导；后续按真实结果追加。
