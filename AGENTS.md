# marble / XNU 工作守则

> **新会话第一入口**：[`docs/STATUS.md`](docs/STATUS.md)（现在在哪、下一步是什么）。
> 按时间的编年史只有一份：[`docs/chronology.md`](docs/chronology.md)（要改日期就改那里）。
> 本文件只留操作守则、踩坑清单与已验证事实，不再堆历史结论。

## 0. 这是什么

把 Apple 开源的 **XNU 8796.141.3**（iOS 16 同期内核）和 iPhone X 的
iOS 16.7.16 用户态跑到 **Redmi Note 12 Turbo（marble / SM7475 / Snapdragon 7+ Gen 2）**上。
启动链路是 ABL → 自写裸机 shim → XNU → iPhone X userland，**纯 RAM 运行**：
不刷 boot、不写 Android 分区表，只用 `fastboot boot`。

当前主线是 Wi-Fi（0299–0303，总开机 74/120）；音频暂停在 73/120；GPU 第二优先。
详细状态以 `docs/STATUS.md` 为准。

## 1. 设备红线（不可协商）

- **只允许 `fastboot boot`**。禁止 `flash` / `erase` / `format` / `set_active` /
  `oem` 写命令；不碰 GPT、`persist`、`modemst`。
- **不主动 collapse GPU**。没有用户明确点头，不开任何 GPU 枪。
- **一次开机一个会话**（one-device-one-session）；不要在子智能体里抢设备。
- 每次开机前必须完成本地验证：产物 grep、marker、stub 编译、判据表已 commit。
- 宿舍安静：音幅 1/16、`sweetbufs≤30`、不放长音、不热挂探针。
  更长/更响的测试需要用户明确许可。
- 死机只建议用户**长按电源**；不做「再等等看」以外的补救。
- 仓库不含 Apple / Qualcomm / Xiaomi 的二进制固件、IPSW、HFS 用户态镜像。
- 不提交设备私密数据；公开文档统一用 `$SERIAL` 或 `<your-device>`。

## 2. 构建 → 验证 → 开机

```bash
export D22AP_ADT=$HOME/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt
export ADSP_FW_DIR=$HOME/Desktop/marble-ios/work/adsp-fw
cd $HOME/Desktop/marble-ios/work/xnu-gh

# 交互轮：KEEPALIVE=1 停在 iOS；日志轮：sweetreboot=1 自动复位并拉 ring
bash scripts/mkimage.sh sweethap=1 ...
bash scripts/boot_interactive.sh images/<img>
bash scripts/boot_one.sh images/<img> <label>
```

1. **CNTP_CTL 必须 NOP**：`fetch_kernel.sh` 会把 `msr CNTP_CTL_EL0` 改成 NOP；
   全 kernel 恰好 1 处，否则 assert 失败。raw kernel 不能直接启动。
2. **boot-args ≤ 590 字节**：`mkimage.sh` 在超长时直接报错退出，不许绕过。
   历史上超长会静默把 `sweetbufs=30` 切成 `sweetbufs=3`。
3. **逐条核产物**，不是只看构建日志：
   ```bash
   LC_ALL=C grep -ac "sweetbufs=30" images/<img>
   LC_ALL=C grep -ac "<本补丁的 marker 字面量>" images/<img>
   ```
   裸 grep 到 flag 名不等于它在 cmdline 里；`%#x` 打 0 时输出是 `0` 不是 `0x0`。
4. **判据表开机前 commit**，事后不改。一个语义变量；证人打印它实际用的参数。
5. **拉完 ring 先对身份**：`grep -m1 -o "sweettime=[0-9]*" logs/ring-<label>.txt`
   必须等于本轮 build log 的 `sweettime`。不等 ⇒ panic/早复位没写 ring，
   `pull_logs.sh` 会静默返回上一轮。

## 3. 补丁纪律

- 常驻重建树在 `work/chain-tree/`（`/tmp` 会被清空）。
- 生成补丁前先把重放好的基线 **commit**；然后编辑；
  用 `git diff <基线 rev> -- <file>`，**不是 `git diff`**。
- 基线 rev = 最后一个**已经落成 `.patch`** 的 rev，不是最后一个本地 commit。
- 在纯净基线副本上 `git apply --check -p1` → apply → `diff -q` 对比；
  hunk 数与改动点数一致才提交。
- 每个补丁自带唯一 marker，marker = **整条 printf 格式串**、跨补丁唯一、在镜像里连续。
- 改 printf 字符串 ⇒ 同一 push 里更新 `.github/workflows/xnu.yml` 的 marker 列表。
- 新增/改写函数后必做 stub 语法检查：
  `clang / clang++ -Wall -Wextra -Wformat=2 -Werror -fsyntax-only`。
- 插代码前检查标识符首次声明是否在使用点之前（stub 抓不到顺序问题）。
- `git add` 不许管进 `head`（SIGPIPE 会吞掉 add）；用
  `git diff --cached --numstat` 核对实际暂存内容。

## 4. 仪器与日志的坑

- 内核 console 在 `exec bluetoothd` 之后基本哑 ⇒ 关键行必须直写 ring。
- `bsd/dev` 的 `printf` = 单写；`SWEET_HAP_LOG` = 双写（IOLog + ring）。
- ring 单条记录约 240 字节，截断不加分隔符；长 payload 分 180 字节段打。
- **仪器必须能自证失败**：只在已被证明会执行的路径上打印，否则标注「非判据」。
- 阳性对照要在同一轮里被证明会出数；聚合数按进程/类/pid 拆开。
- 尺寸/长度匹配不是证据；同一个 `u32=18504` 在 ACDB 里能出现几十次。
- `status=0` + 静音是**移除**，不是成功；`no response` 要先做 liveness probe。
- 新 checker 先在已知-good 产物上测；两个解释都能拟合一个 refusal 时，一次只动一个轴。
- 导出/读回的 body shape 必须从对象自己的 walk 推导，不能猜。

## 5. 已验证事实 / 不要重走

### 音频
- 控制面全绿：`GRAPH_OPEN` / `PREPARE` / `START`、TDM `SET_CFG` 全 `status=0`。
- 数据面已通：S1 叶子 `AP` 位修正后 `DATA_BUFFER done status=0`，1200 buffer 正常复位。
- 856 B A 形式图（含 `PCM_DEC` + `PCM_CNV`）能开、能起、能推 120 帧；三次有效听测均无声。
- `EOS_RENDERED` 从未到达；收包仪器已被证明可用（137 包、预期 opcode mix）。
- 第 73 轮是 **PRM 回包解析失配**，不是 PRM 拒绝：正确应答是
  `0x02001002` + `0x0800102c`；0298 已离线修好绑定，尚未完整构建/开机。
- 不要再用 `DATA_BUFFER` 当探针；DONE 只证明缓冲归还，不证明最终渲染。

### 触感
- AP 侧 SPMI 直驱 HAP520 真的能震（44/44 次有震感）⇒ 输出侧已解决。
- 按键与开机震动已解决；UIKit / CoreHaptics 的请求仍被 `backboardd` 的用户态 gate 拦下。
- 不得用坐标 / selector / 内核定时器模拟脉冲。

### GPU / Wi-Fi / 基带
- 旧 XNU 8796 GPU 线仍冻结：kgsl-smmu 的 CB 页一碰整机挂死，CX 域电源未真正打通。
- FoXLinne 的上游 iOS 18 GPU 实现可参考，但 iOS 16 适配未开始；不得整仓覆盖。
- Wi-Fi 当前主线：第 74 轮只读 handoff 通过，确认 QCA6490/PCIe0 交接后保持关闭；端点识别尚未实机验证。0302/0303 尚未进入实际启动路径。
- Wi-Fi 失败关闭：没有端点 marker 不进 DMA；没有 owner 授权不放 BME；
  不能照抄参考机 bank/slot，不能因 MMIO 回读成功就放开 DMA。
- 基带未开实验；先决门是 306 MB `mpss_region` 能否在本内核启动时持有。

### 存储
- root 镜像位于 userdata 尾部；**f2fs 没有缩小，GPT 没有改**。
- 研究机日常仍开 Android ⇒ f2fs 写满会覆盖尾部 root，这是活的风险。
- 当前只能事后发现：开机前重读 HFS+ 卷头签名与 free 块数，发现被覆盖就重新分块写尾部。

## 6. 环境与工具

- GitHub HTTPS/网页/API 经常炸；SSH 推通常能过。没有 `gh` CLI。
- CI 只在 push 到 `main` 且路径含 `xnu/patches/**` 或 workflow 时触发。
- runner 类失败（`Job is waiting for a hosted runner to come online`）表示作业根本没进 Build；
  这时 release 摘要和 `build-error.txt` 都不会变，摘要本身不能定位失败，必须看日志。
  本地 `ci_replay.sh` / stub 编译 / marker 回查都通过时，重跑即可，不要改代码。
- 构建结果只看 release 资产；`fetch_kernel.sh` 取摘要必须按资产名配对。
- 下载 kernel 用续传循环直到完整；不要用循环轮询 Actions API。
- Android 侧 debugfs 没挂、kprobe 不可用、`/dev/mem` 不存在、`clk_summary` 为空；
  可用的是 regulator、`/proc/asound/pcm`、regmap 列表等只读接口。

## 7. 目录与产物

```text
.github/workflows/xnu.yml       CI：构建 XNU、marker 门、发布 kernel-latest
xnu/patches/                    XNU 的全部移植补丁（当前到 0303）
shim/                           RAM-only 引导 shim（AArch64 裸机 + boot.img v4）
scripts/                        取内核 / 打包 / 启动 / 拉日志 / Wi-Fi 构建
docs/chronology.md              唯一编年史
docs/STATUS.md                  当前状态
docs/START-HERE.md              当前验证过的配方
docs/audio-closeout.md          音频当前判决 + 证据索引
docs/audio-hal-listen.md        0293→0298 / R1 合并记录
docs/gpu-status.md              GPU 当前判决 + 原始日志索引
docs/wifi-foxlinne-port.md      当前 Wi-Fi 移植
docs/haptics-abi-2026-10-30.md  触感 ABI 与当前 gate
docs/smmu-dead-ends.md          已证伪的 SMMU 路线
docs/required-assets.md        自备文件与许可
docs/traces/                    原始抓包与解析方法
work/chain-tree/                常驻重建树
work/archive/                   本地档案（不进公开仓库）
```
