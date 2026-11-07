# 0289 描述符隔离：构建与真机对照记录（2026-11-08）

## 授权与目标

用户要求：“请你直接展开工作，推送触发构建，然后继续测试”。
本轮提交/推送 0289，验证完整构建产物，再做 **sweetdescsep=0 / 1** 两次音频 RAM-only 对照。
事前判据已经写入 `audio-closeout.md` 的“2026-11-08：0289 描述符隔离对照，第 65/66 枪事前判据”。
不启动 B1 去 AW、EOS 新实验、格式配置、GPU 或其他子系统；不增大既有音幅/长度。

预算起点 64/120，**两次开机均已完成，当前 66/120**。本轮两枪止损已到，没有再开 B1/EOS/格式第三枪。

**执行结果：两轮全部事前日志门通过，隔离组实际 off=0x4000；声音仍等待用户听感，未判为有声或无声。**

## 1. 构建状态

- 补丁：`xnu/patches/0289-sweet-separate-graph-descriptor.patch`，1 hunk / +22 行。
- 基线：chain-tree `ccb174f075f3cc328473136ee69f31bcb9a40df4`（已落 0288）。
- 新 marker：`SWEET MARK: descsep v1 on=%d off=%#x len=%u`。
- 本轮重跑：12 项 host 回归通过；`-Wall -Wextra -Wformat=2 -Werror -fsyntax-only` 通过；补丁等于 0288 完整基线 diff。
- 链树已提交为 `7d24fbc`（`audio/0289-descsep`）；本轮 `ci_replay.sh`：**applied=230 / skip=49，post-image==链树 HEAD**。证据：`verify/ci-replay-0289-2026-11-08.log`。
- 先前 ASan/UBSan 与逐字节隔离对照记录保留在 `verify/audio-descsep-2026-11-07/`，未冒充为本轮重跑。
- `fetch_kernel.sh` 的下载数据解析使用 `python3 -I`；下载目录与自写脚本隔离。
- **已推送**：`6f72bdcab4313aaf5d744b4a49bcefc0d45f1a0b` → `origin/main`；推送后左右计数 `0 0`，仅指定文件入库，没有加入历史未跟踪日志或二进制。
- **新构建产物已取回并过门**：raw SHA256 `ce969727ba39e6acb2710d124e1a1ed2cf1c8355014123ca4d22bddfa812d329` 与 release 对应资产摘要相等；17,548,216 B。
- CNTP raw 恰 1 处 `0x4aa348`，已改 NOP；修后 SHA256 `bdb90910f206c9b12c3b2bc5021825a8cc8310c93967ce234d40e7f9bb4a0fe8`，未修指令计数 0。
- `descsep v1` 完整格式串、`dcnv v2`、`rxall v1`、`noaw v1` 各 1；保存于 `out/fetch-0289-6f72bdc/`，不会误用旧的默认 out/kernel.adsp。
- 实测目标设备电量 97%，设备名 marble，sys.boot_completed=1；没有其他可达会话抢设备。
- 新增 image/ring 打分器的 **10 项合成正/负对照通过**，能拒绝缺失 marker、旧 sweettime、回落图、EOS 混入和缺失 witness。
- 下载日志：`logs/fetch-0289-6f72bdc.log`。两张镜像事前门通过后，**第 65/66 枪均已完成并正常复位**。
- 控制镜像：`images/d0-descsep-control-0289.img`，SHA256 `669bacf29732772846d4ab5591b8e6e342f035b419c0ae25ef71b80b202ca9b0`，sweettime=1794069367。
- 隔离镜像：`images/d1-descsep-isolated-0289-paired.img`，SHA256 `1cfad91ff6e01e56badceee8ed9e6b28d34d294aae9fc4136fade4e638295d5b`，sweettime=1794069467。
- 两张真实内嵌 cmdline 均 **582 B**、唯一 sweettime、所有参数值完整；忽略 sweettime/descsep 后参数逐字相同。
- 原始两次打包只有 ADT 的 64 B random-seed 在参数外不同（63 字节实际不同），不是“全包一致”。在开机前发现并排除这项混淆：隔离镜像从已验证控制镜像派生，仅替换原有 608 B cmdline 字段，复用同一 ADT seed/kernel/firmware/boot header。**最终整张镜像在 cmdline 字段以外逐字节相等。** 原始带不同 seed 的隔离镜像保留，未使用、未计入开机。
- 事前镜像验证与配对证据：`verify/d0-descsep-control-0289-image.json`、`verify/d1-descsep-isolated-0289-image.json`、`verify/descsep-pair-0289-before-boot.json`。

## 2. 已执行的对照

两张镜像均为单核，采用同一 0289 内核与相同参数，只改变 descsep 值和独立身份 sweettime。
两轮均不发 EOS；真实 cmdline 均实测 582 B。

| 项 | 第 65 枪：d0-descsep-control-0289 | 第 66 枪：d1-descsep-isolated-0289 |
|---|---|---|
| sweetdescsep | 0 | 1 |
| 图描述符 offset | 0 | 0x4000 |
| 图正文 | 856 B DEC/CNV | 相同 |
| 音幅/提交数 | sweetvol=16 / sweetbufs=120 | 相同 |
| 当前状态 | **已开，全部日志门通过** | **已开，全部日志门通过** |
| 听感 | 未取得，不判 | 未取得，不判 |

判读使用每张镜像自己的 cmdline/sweettime 与 ring raw total，排除回落图、旧日志及未执行分支。

## 3. 配对结果（真实 ring，不是 mock）

| 观察 | 控制组（65） | 隔离组（66） |
|---|---|---|
| sweettime | 1794069367 | 1794069467 |
| 有效窗口 total / 解码字节 | 99545 / 99545 | 99202 / 99202 |
| graph_open/dec-cnv | 1 次，status=0 | 1 次，status=0 |
| graph_start | status=0 | status=0 |
| 描述符 witness | 门=0，0 行（符合事前定义） | 恰 1 行 on=1 off=0x4000 len=856 |
| tone / DATA_BUFFER_DONE | 120 / 121 | 120 / 121 |
| dblat min/avg/max ms | 5 / 8 / 10 | 5 / 8 / 10 |
| timeout | 0 | 0 |
| rxall op 分布 | 121 DONE + 9 basic + 5 PRM + 1 SPF + 1 MAP | 完全相同 |
| 回落图 / EOS 发送 | 0 / 0 | 0 / 0 |
| 正常收尾 / panic | done + PSCI_RESET / 0 | done + PSCI_RESET / 0 |
| 听感 | 用户未报告，未知 | 用户未报告，未知 |

**判决：隔离在本机实际执行、OOB open 接受新 offset，并没有破坏已知控制面或提交/ACK 路径。**
结合真实函数 host 字节测试，AP 侧“图描述符覆盖 PCM A”的写入路径已得到隔离；没有真机逐 buffer 内存读回或渲染证据，不能将其扩大为“所有样本实际到了功放”。
更不能因为未收到听感就写“仍无声”。

原始文本：`logs/ring-d0-descsep-control-0289.txt`、`logs/ring-d1-descsep-isolated-0289.txt`；raw 保留在本地 `logs/raw/`。
独立打分：`verify/d0-descsep-control-0289-result.json`、`verify/d1-descsep-isolated-0289-result.json`；配对摘要 `verify/descsep-pair-0289-verdict.json`。
发布前另查到全局 Git autocrlf=input 会把两个原始窗口各删去 245 个 CR 字节，导致仓库文本不再等于 raw total。已给这两个文件单独设置 `.gitattributes -text` 保留原字节，重新入库并比较 Git blob 与本地窗口完全相同；没有修剪日志内容或改变判读。

设备两次都自己复位回 Android，最后 sys.boot_completed=1、电量 98%；没有软件救机操作。
默认 `out/kernel.adsp` 已更新为验证过的 0289（bdb90910…）；旧 0288 保留在 `out/kernel.0288-before-descsep.adsp`，没有丢失原对照产物。

## 4. 等待期间的零开机新事实：EOS 厂商包确认 28 B

见 [audio-eos-wire-2026-11-08.md](audio-eos-wire-2026-11-08.md)。本地 `libar-gsl.so` 与设备原位 SHA256 相同，真实 EOS 分配 payload_size=4、OOB=0、force_inband=1，policy 在 GPR payload 首字；日志固定记录 28 B，原 packet 直接 async_send。
这收窄了先前 GSL 28 B / Linux 44 B 的冲突：**本机厂商路径确认是 28 B，旧 XNU EOS 发 44 B 与其不一致**。
未改 EOS 代码、未开第三枪；旧“无 EOS 事件意味着 AP 原因全排除”的解释撤回，原始零事件观测仍保留。
