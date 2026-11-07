# 本机 EOS wire 格式复核（2026-11-08，零额外开机）

## 结论与边界

**本机厂商 AudioReach GSL 的 EOS 是 GPR 头后直接放 4 B policy，不加 16 B APM 命令头。**
当前 XNU 的 `g_apm_cmd()` 则为 EOS 生成 44 B 包，GPR payload 为 `{0,0,0,4,1}`；两者不一致。
因此此前“发出 EOS 但没有 EOS_RENDERED”的读数不能用于排除 AP 请求封装错误。

这项是二进制/协议复核，**不是 DSP 真机接受正确 EOS 的验证，更不是无声根因判决**。
本轮两次 descsep 测试都不发 EOS；没有第三枪，没有改 EOS 内核代码。

## 1. 库身份（已回设备核对）

- 本地副本：`work/phone-libs/libar-gsl.so`，224,440 B。
- Android 原位：`/vendor/lib64/libar-gsl.so`。
- 两者 SHA256 **完全相同**：`d46402219e582856b03d84972cef5ebb1687193c916d8ca7cc697a2b3b2120ad`。
- 原位只读命令为 `adb -s <项目目标 serial> shell "su 0 sh -c 'sha256sum /vendor/lib64/libar-gsl.so'"`。
- 没有用 Adreno 的 `libgsl.so`，没有用不匹配的陈旧副本，也没有执行 `.so`。

## 2. 逐条确认的二进制证据

归档：`verify/gsl-eos-wire-0289-2026-11-08.txt`。
使用 NDK 的 llvm-readelf / llvm-objdump，只反汇编相关范围。

| 地址 | 实测指令/行为 | 含义 |
|---|---|---|
| `0x33808` / `0x3380c` | 导出 `gsl_dp_write_send_eos`，跳转 `0x3160c` | 不是凭字符串搜索猜函数 |
| `0x3165c` + `0x31668` | w0 合成 `0x04001002` | 正是 WR EOS opcode |
| `0x3166c` | `mov w3, #4` | gsl_msg_alloc_ext 的 GPR payload_size=4 |
| `0x31678` | `mov w6, wzr` | OOB payload_size=0 |
| `0x31670` | `mov w7, #1` | force_inband=true |
| `0x3168c` | 调 `gsl_msg_alloc_ext@plt` | 参数真正用于分配 |
| `0x32a58–0x32a94` | 强制带内分支把 w3+w6=4 交给 `__gpr_cmd_alloc_ext` | 分配器没有加 APM 命令头 |
| `0x316cc–0x316dc` | 从 GPR 首字取 header_size，直接在包+header_size 写入 1 | policy 在 GPR payload 起点，而不是再加16 |
| `0x3180c` | `mov w2, #0x1c`，随后 `ar_fwrite` | 实际日志按 28 B 记录该包（24+4） |
| `0x31850–0x31854` | 原 packet 指针直接调 `__gpr_cmd_async_send` | 发送前没有额外包装步骤 |

额外核过 msg allocator 的 payload 指针初始化（`0x32b78–0x32bb8`）及通用发送函数的直接 async-send 分支；不是只看一次 size 常量就认领格式。

## 3. 参考实现冲突如何收窄

本地 Qualcomm GSL 源码 `gsl_dp_write_send_eos` 是 24+4 B；本地 Linux qdsp6 EOS helper 是 24+16+4 B。
此前两者冲突，所以没有选一边冒充这台设备的事实。
现在有与设备原位 hash 相同的 GSL 二进制：**对本机厂商路径，28 B 这一边已经确认**。
Linux helper 的存在不再能证明本机 GSL 也发 44 B；也不能据此推广其他固件一定不接受 44 B。

## 4. 对旧记录的影响

- 第 62–64 枪收包窗为 5 秒、收包仪器有效、实际没有 EOS_RENDERED，这些原始观测保留。
- 但旧代码发的是比本机厂商多 16 B 头的 EOS；“所有 AP 侧原因已排除，只能是对端不发/发往别处”这一排他性解释撤回。
- 正确格式是否得到事件、事件是否表示 rendered，以及扬声器是否有声，均尚未真机验证。
- 本轮按登记止损停在第 66 枪，不借这个新发现自动增加 EOS 开机。
