# 0289 描述符隔离：构建与真机对照记录（2026-11-08）

## 授权与目标

用户要求：“请你直接展开工作，推送触发构建，然后继续测试”。
本轮提交/推送 0289，验证完整构建产物，再做 **sweetdescsep=0 / 1** 两次音频 RAM-only 对照。
事前判据已经写入 `audio-closeout.md` 的“2026-11-08：0289 描述符隔离对照，第 65/66 枪事前判据”。
不启动 B1 去 AW、EOS 新实验、格式配置、GPU 或其他子系统；不增大既有音幅/长度。

预算起点 64/120。本轮最多两次开机，成功执行两次后为 66/120；收尾/设备安全门失败即停。

## 1. 构建状态

- 补丁：`xnu/patches/0289-sweet-separate-graph-descriptor.patch`，1 hunk / +22 行。
- 基线：chain-tree `ccb174f075f3cc328473136ee69f31bcb9a40df4`（已落 0288）。
- 新 marker：`SWEET MARK: descsep v1 on=%d off=%#x len=%u`。
- 本轮重跑：12 项 host 回归通过；`-Wall -Wextra -Wformat=2 -Werror -fsyntax-only` 通过；补丁等于 0288 完整基线 diff。
- 链树已提交为 `7d24fbc`（`audio/0289-descsep`）；本轮 `ci_replay.sh`：**applied=230 / skip=49，post-image==链树 HEAD**。证据：`verify/ci-replay-0289-2026-11-08.log`。
- 先前 ASan/UBSan 与逐字节隔离对照记录保留在 `verify/audio-descsep-2026-11-07/`，未冒充为本轮重跑。
- `fetch_kernel.sh` 的下载数据解析使用 `python3 -I`；下载目录与自写脚本隔离。
- **当前尚未推送/尚未取回新产物，旧 out/kernel.adsp 是 0288，不可用于隔离测试。**

完整内核摘要、release 配对摘要、CNTP 原始位置、marker 数量与推送 SHA 在实际取得后填入，不预写。

## 2. 待执行的对照

两张镜像均为单核，采用同一 0289 内核与相同参数，只改变 descsep 值和独立身份 sweettime。
两轮均不发 EOS；预期 cmdline 582 B，必须实测。

| 项 | 第 65 枪：d0-descsep-control-0289 | 第 66 枪：d1-descsep-isolated-0289 |
|---|---|---|
| sweetdescsep | 0 | 1 |
| 图描述符 offset | 0 | 0x4000 |
| 图正文 | 856 B DEC/CNV | 相同 |
| 音幅/提交数 | sweetvol=16 / sweetbufs=120 | 相同 |
| 当前状态 | 未开机 | 未开机 |
| 听感 | 未取得，不判 | 未取得，不判 |

判读使用每张镜像自己的 cmdline/sweettime 与 ring raw total，排除回落图、旧日志及未执行分支。

## 3. 结果栏

等待构建产物与两次真实测试。此处不把 host mock 的返回值或“没有报告声音”当成 DSP/听感结果。
