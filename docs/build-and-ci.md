
## 一次"红色 run"其实不是代码问题（2026-11-06，run #404 前后）

0277 推上去后 release 摘要长时间不变，`build-error.txt` 也不变。用户贴出运行日志，
决定性的一行是：

> Job is waiting for a hosted runner to come online.

也就是 **macos-15 托管 runner 没起来**，作业根本没进入 Build——所以既不是补丁链，
也不是 marker 门，也不是代码。判别这三类只需要两个本地事实（都当场可查）：

| 观察 | 结论 |
|---|---|
| `Verify our patches applied`(:120) 在 `Publish kernel to rolling release`(:316) **之前** | 资产摘要不变，与"构建失败/marker 门失败/runner 没起来"三者都相容 ⇒ **摘要本身不能定位**，必须有日志 |
| `build-error.txt` 内容不变（digest `7d85b30e…`、里面 commit 仍是旧的 `11ac917…`） | `if: failure()` 的那一步没跑到 ⇒ 作业连失败收尾都没进入，倾向"根本没开始/被取消"，而非编译错误 |

本地能做的排除（这次全做了，都不需要开机也不需要日志）：
- `tools/ci_replay.sh`：base e33e15e + 218 patch，post-image == chain-tree HEAD ⇒ **不是 run-387 那类 hunk 被跳过**；
- `tools/stub_dblat.py`：把新代码原样抽出来 `-Wall -Wextra -Wformat=2 -Werror` 编过并跑出预测读数 ⇒ **新增代码不是编译错误**（也只证明到这一层，整棵树仍只有 CI 能证）；
- gate 里 63 个 marker 字符串逐个回查源码：`dblat v2` 在；另有 11 个按字面搜不到，
  **那是我的搜法的过滤器问题**（跨行拼接 / 在别的文件里），不是缺失——这条要标出来，
  否则"搜不到"很容易被当成发现。

处置：runner 类失败**什么都不用改**，重跑即可；但要触发新 run 必须动
`xnu/patches/**` 或 workflow（paths 过滤把 docs/tools 的推送排除在外），
所以用户手动 Rerun 是对的工具。顺带一条纪律也被这次验证了：**等待器要能自证活着**——
我看到它在反复重下同一个已被自己拒绝的 0276 资产后主动停掉了它，
而不是让一个 0 字节的输出文件冒充"还在跑"。
