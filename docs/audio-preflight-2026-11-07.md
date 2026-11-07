# 音频续接：零开机预检与 0289 描述符隔离

> **2026-11-08 执行补充**：0289 已推送构建并取回，第 65/66 枪隔离开/关对照的所有事前日志门均通过，预算现为 66/120，听感未知；见 [audio-descsep-live-2026-11-08.md](audio-descsep-live-2026-11-08.md)。EOS 冲突已由与设备原位 hash 相同的 GSL 二进制收窄为本机厂商 28 B，见 [audio-eos-wire-2026-11-08.md](audio-eos-wire-2026-11-08.md)。下文保留前轮的预检历史。

日期：2026-11-07。**本轮没有开机、没有设备写操作，预算仍为 64/120。**
本页补充并收窄 `STATUS-2026-11-07.md` 与 `unixnu-sheng-audio-2026-11-07.md` 的冻结状态；不修改旧实验的事前判据。

## 1. 本轮做了什么，没做什么

- 阅读项目守则、最新全局状态、音频判决链 §§1–48、根交接与历史聊天末段、音频桥与扬声器路径；对关键结论回查真实代码和 ring。
- 核验 `out/kernel.adsp` 仍为 **0288**：17,548,216 B，SHA256 `80c8e9b171fa5f7ee190449bc2e0a5e15c94a09ba7e8763c8ef3fc85e8ac9b45`；CNTP 未修指令计数 0；`noaw v1`、`dcnv v2`、`rxall v1` 各 1。
- 两个只读子任务都因 API 504 提前终止，恢复调用也遇到权限分类服务超时；**没有采用或声称存在它们的结果**。本页结论来自主线程实际读取和主机测试。
- 已制作 **0289 草案**与 workflow marker，本地语法、行为、内存检查和重放通过。**尚未 commit/push，尚无 0289 的完整 XNU 构建产物，不能拿现有 0288 内核冒充修复后内核。**
- 并未逐字读取全部固件、IPSW、多份几十 MB 历史聊天或所有上游源码；已覆盖本轮所需入口、音频实现与对应证据。三图/18504 校准资产的完整普查没有在本轮完成。

## 2. 已复现的 AP 侧缺陷：描述符反复作为 PCM 提交

0288 的真实调用顺序：

1. `g_pcm_prepare()` 在 offset 0 与 `g_pcm_bytes`（默认 1152）填两块正弦 PCM，并置 `g_tone_filled=1`。
2. `g_apm_graph_open_shm()` 把 **856 B / 808 B 图描述符**复制到同一 region 的 **offset 0**。
3. `g_audio_tone()` 再调 `g_pcm_prepare()`，但已填标志阻止重填。
4. 一次单缓冲探针及 120 次循环复用 offset 0 / 1152；不是只污染第一帧。

从生产文件直接抽取 `g_fill_tone`、`g_pcm_prepare`、`g_apm_graph_open_shm`、`g_audio_tone` 等函数编译执行，结果：

| 主机用例 | PCM A 被覆盖 | 含描述符前缀的提交 / 总提交 |
|---|---:|---:|
| 856 B 图，offset 0 | 是 | **61 / 121** |
| 808 B 图，offset 0 | 是 | **61 / 121** |
| 856 B 图，offset `0x4000` | 否 | **0 / 121** |

61 = 1 次探针 + 循环中 60 次 A 缓冲。B 缓冲仍有正弦，因此**不能据此声称这就是全部无声的根因**。
但“120 次 ACK 表示 120 块正确正弦 PCM 已发出”必须撤回：本地内存路径已经反证了“正确 PCM”这个前提。
三次用户听测“没有声音”、OPEN/START 成功和 121 个 ACK 的原始事实仍成立。

## 3. B1（去 AW）不能照旧表直接定性

`g_graph_aform_no_aw` 的模块表确实仅去掉 `(0x10013d01, 0x42d3)`；SG_CONFIG 与 CONTAINER_CONFIG 逐字节不变。
然而后续不是一个完全自洽的“无 AW 图”实验：

- B1 仍调用 `g_audio_graph_cfg_vp()`；该函数无条件向 **0x42d3** 发 `vp/aw_cfg` 与 `vp/aw_cfg2` 两条 SET_CFG，且丢弃返回值。
- 主机记录器验证：B1 open 成功的模拟路径确实产生 **2 条指向已移除 iid 的配置**，之后仍进入 start/tone。模拟拒绝不是 DSP 的实测错误码。
- B1 open 被拒时只退出内层 if，**整个 bringup 会继续回落**；即使不设置 `sweetvgraph7=1`，仍有默认 760 B `graph_open/vp-wr` 路径。host 的 fallthrough 哨兵实测为 1，生产代码也有默认 open。
- 所以 start 失败不能被单独解释成“该模块是合法/必要模块”，听到声音也必须先确认到底来自 no-aw 图还是回落图。

**处理：第 65 枪未开，旧 §47 表保留作为历史预登记，本页在开机前暂停该候选。**
未来要重新安排 B1，必须先让配置集合跟模块表一致，并阻止回落混入主判据；不能事后拿旧表挑结果。

## 4. 新增 DEC/CNV 不等于配置过它们

第 60/62/64 枪的参数没有 `sweetvcfg2=1`。
直接执行真实 `g_audio_graph_cfg_vp()` 并记录发送对象得到：

| 开关 | PCM_DEC 0x40bf 配置数 | PCM_CNV 0x40c0 配置数 | 另外向图中不存在的 iid 发配置 |
|---|---:|---:|---:|
| 默认 `sweetvcfg2=0` | **0** | **0** | 0 |
| 旧 `sweetvcfg2=1` | 1 | 1 | **0x40c8 与 0x4165，各 1** |

这只是确认配置未发送，**不证明 0x08001008 必须显式设置或一定能修复无声**。
旧四包开关不能不加筛选地当作新 DEC/CNV 图的配置测试。下一项格式实验应只针对图中存在的两个模块，独立于本轮描述符隔离。
本轮没有修改任何模块、参数正文或格式配置。

## 5. EOS 封装还有未决冲突，不能宣称 AP 侧已全部排除

真实 `g_apm_cmd()` 不区分命令族，统一插入 16 B `apm_cmd_header_t`。
本轮 host 从该函数实际生成的 EOS 包为 **44 B**：GPR 头后五个字 **`{0, 0, 0, 4, 1}`**，policy=1 在包 offset 40，而非 24。

两个本地参考实现相互矛盾，必须一起保留：

- Qualcomm GSL：`gsl/src/gsl_datapath.c:2145–2168` 为 EOS 申请 `sizeof(*eos_cmd)=4`、`oob_payload_size=0`、`force_inband=true`，直接 `GPR_PKT_GET_PAYLOAD` 后写 policy；`gsl_msg_builder.c:68–84` 没有额外 APM 头。由 API/实现算得 **28 B**。
- 本地 Linux 参考：`refs/qdsp6/audioreach.c:1365–1384` 与 `x-sound_soc_qcom_qdsp6_audioreach.c:1599–1612` 使用 `audioreach_alloc_cmd_pkt`，该 helper 额外加 APM 头；policy 在 GPR+APM 头之后，**44 B**，与现有发送一致。

因此现在**不把“应改成 28 B”写成这颗固件的已验证修复**。需要先匹配本机 `libar-gsl.so` 的 EOS 实现或获取同固件的真实 EOS 包，不能只选一个参考。
第 64 枪“137 个 RX、EOS 后零 RX、没有 EOS_RENDERED”的观测仍然成立；但仅由这些零推导“请求一定正确，因此 AP 侧不存在其他原因”不成立。
这项是离线封装核查，不擅自重开已关闭的 EOS 真机路线。

## 6. 0289：只隔离图描述符，默认关闭

补丁：`xnu/patches/0289-sweet-separate-graph-descriptor.patch`，从最后已落补丁的链树基线 **`ccb174f075f3cc328473136ee69f31bcb9a40df4`（0288）** 生成。

- 只改 `g_apm_graph_open_shm()` 一处，**1 hunk / +22 行**。
- `sweetdescsep=1` 把描述符的复制地址与 wire address_lsw **同时**改到 region offset **`0x4000`**。
- 默认没有该开关时行为保持不变。图正文、句柄、src 端口、模块表、配置、缓冲数量与幅度均不修改。
- 新 witness：`SWEET MARK: descsep v1 on=%d off=%#x len=%u\n`；已加入 workflow marker 列表。
- 只允许默认双缓冲：`sweetnowait != 0` 或显式 `sweetdbb` 不是 0/1152 时，在写描述符前拒绝；原有 region 边界守卫仍生效。
- 对照测试证明：开关开启后实际发出的 address_lsw = **16384**，PCM A 保持完整；nowait、大缓冲、4097 B 描述符越界三类输入都在复制/发送前返回 -1。

### 本地验证结果

- `python3 -I tools/test_audio_preflight.py`：**12 项通过**。
- 抽出的真实函数：`clang -std=gnu11 -Wall -Wextra -Wformat=2 -Werror` 编译干净。
- ASan + UBSan：执行成功，无内存/未定义行为报告。
- 独立 0288 基线 `git apply --check` → apply：结果逐字节等于本地工作源码。
- 全链对照：旧链 **applied=229 / 已有 skip=49**，加 0289 后 **applied=230 / 同样 skip=49**；**零新增 skip**，post-image 等于工作源码。49 个旧 skip 是 e33e15e 重放基线已包含早期链位造成的历史现象，未把它们藏掉或误报为零。
- 重放第一版被外围 git 仓库的子目录规则静默跳过，post-image 比较立即失败；第二版查到基线已有 skip；最终在独立初始化的测试仓库中对照原链，按“零新增 skip + 字节相等”通过。没有把前两次失败算作通过。

证据目录：`verify/audio-preflight-2026-11-07/`（改补丁前的 8 个用例）与 `verify/audio-descsep-2026-11-07/`（新补丁 12 个用例、replay.json、sanitized.log）。
测试临时树及 executable 均在 `trees/`，已由既有 `.gitignore` 排除。

**测试范围**：transport、时钟、MMIO、DSP 接受均为 mock；内存屏障在 host 中被消去。
生产函数已有未使用的 `tdm_intf` / `src_port` 在 host 里仅添加 sizeof / void 引用，避免把既有 unused warning 当成新增代码错误。
本地测试**不代替完整 XNU 构建、产物 marker 验证或实际听测**。

## 7. 下一次设备实验的候选门（尚未开，不冒充旧 §47）

先构建含 0289 marker 的内核，再考虑 **856 B DEC/CNV 图 + 描述符隔离**；不要同时去 AW、加格式配置或更改 EOS。
对照参数沿用第 64 枪，把 `sweeteos=1` 改为 `sweeteos=0`（不发存在封装争议的命令），额外 `sweetdescsep=1`；总长离线复算 **593 B**，超 590，不能直接使用。
若后续获准安排这个单变量实验，应先用不发送 EOS 的同配置作控制，或删除 EOS token 后分别做隔离开/关的两张镜像；删除 token 后长度为 **582 B / 567 B**（包含脚本自动生成的唯一 sweettime）。
是否接受对照成本、是否调整原 B 线三枪止损，**仍需用户决定，不能借本轮“继续处理”擅自加开机**。

候选的有效性门：产物 descsep marker；cmdline 唯一 sweettime、≤590；witness `on=1 off=0x4000 len=856`；OPEN/START=0；主路径无 fallback；120 次 tone + 121 次 DONE；正常收尾、ring 身份/窗口核验。
听到/仍没听到只在这些门全部成立后记录。无声不能再次被写成“PCM 一定正确、已到功放”。
判据在完整构建与参数实测后、开机前单独 commit；本页不是已提交的开机许可。
