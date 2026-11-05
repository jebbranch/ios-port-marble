# marble / iOS-on-Android 项目工作守则

这是本工作区的**第一入口**。目的不是重复文档，而是把"每次都重新踩的坑"和
"已经付费验证过的事实"放在一处，省掉翻 `docs/*.md` 注释的时间。
细节按主题在 `work/xnu-gh/docs/` 里，本文件只给结论 + 怎么做。

---

## 0. 这是什么

XNU 8796 / iOS 16 移植到 Redmi Note 12 Turbo（`marble`，SM7475，SoC 名 **UKEE**），
纯 RAM 运行；ADSP 侧走 Qualcomm AudioReach（GPR/APM/SPF）；震动是 PMIC HAP520 走 SPMI。

---

## 1. 设备红线（不可协商）

- **只 `fastboot boot`**。禁 flash / erase / format / set-active / oem，
  不碰 GPT、persist、modemst。任何"写分区"的需求先问用户。
- 目标机 serial = **2aa26095**。同一条 USB 上可能挂着别的手机（三星 `r9q` 等）——
  **绝不在别的机器上 boot 测试镜像**。
- **不主动 collapse GPU**（GPU 线目前是 FROZEN 状态）。
- 死机只能建议用户**长按电源**；我不做"再等等看"以外的补救。
- 宿舍安静：音幅 1/16、`sweetbufs≤30`、不放长音、不热挂探针。
  **更长/更响的测试需要用户明确许可**。
- 开机预算：用户 2026-11-04 把额度从 20 提到 **120**；已用 19（2026-11-05：C=16、D=17、E=18、F=19）。
  **每一次开机前必须先完成本地验证**（产物 grep、marker、stub 编译、判据表已 commit）。
- **带外探针每轮只发一发**（2026-11-05 实测：boot 17 一发被拒 ⇒ 数据面 31 发 + 4 次 done；
  boot 18 四发被拒 ⇒ 1 发无响应。发数不是因，但**多发的代价无法归因**，所以一次一发）。
- **SMMU CB 故障寄存器不能当负证据**：六个 CB 常年 `FSR=0x400`、我们 W1C 清不掉、
  `FAR/farhi` 是无意义陈旧值，pre 与 post **逐字节相同**（boot 18/19 两次实测）。
  ⇒ "没有新故障位" **不等于** "没有发生翻译故障"。
- `fetch_kernel.sh` 取产物摘要必须**按资产名配对**：`kernel-latest` 旁边躺着一个陈旧的
  `build-error.txt` 且排在前面，"页面第一个 sha256" 取的是它的摘要 ⇒ 会误判"没出新内核"。
- 一次开机一个会话（one-device-one-session）；不要在子智能体里抢设备。
- **拉完 ring 先对身份**：`grep -m1 -o "sweettime=[0-9]*" logs/ring-<label>.txt` 必须等于本轮
  build log 里 `cmdline:` 的 `sweettime`。不等 ⇒ panic/早复位没写 ring，`pull_logs.sh` 会
  **静默返回上一轮的 ring**（2026-11-04 真的发生过，差点把 M 轮数据当成 C 轮打分）。
- **不要在持锁函数里打印或再进 IOKit**：`IORegistryEntry::copyProperty()` 的 `wrap2` 是
  `PLOCK; getProperty(...); PUNLOCK` ⇒ 在 `getProperty` 里 `getName()` + `IOLog` 会嵌套拿
  同一把锁。0251 因此连炸两次开机，已从序列删除。要打印就挂在**解锁之后**的位置。

---

## 2. 环境（Mac 侧）

- GitHub **HTTPS/网页/API 经常炸**（SSL_ERROR_SYSCALL、DNS 失败、CDN 发旧包）。
  `git push`（SSH）通常能过，失败就**重试一次**再下结论。
- 没有 `gh` CLI。**不要用循环轮询 Actions API**（60/hr 未认证额度，历史上被烧过）。
- 构建结果只看 release 资产：
  `https://github.com/<repo>/releases/expanded_assets/kernel-latest`（加 `?x=<ns>` 绕缓存）
  + `build-error.txt` 的 `commit:` 行。
- CI **只在 push 到 `main`** 且路径含 `xnu/patches/**` 或 workflow 文件时触发。
  推 feature 分支 = 静默无构建。惯例：`git push origin HEAD:main`。文档单独推不会触发。
- 下载 kernel 用**续传循环**直到完整（17.5 MB，常在第 3–8 次成功）：
  `curl -sSL -C - --max-time 200 -o out/kernel.raw "<asset-url>?cb=$i$RANDOM"`。
- `fetch_kernel.sh` 在 curl 出错时会因 `set -e` **静默退出**（只打印一行 release page 就没了），
  随后你 grep 的是**上一个构建**的 kernel ⇒ 看起来像"补丁被跳过"。
  必须自己核对：新 marker 在、旧 marker 也要在（链是累积的）。

---

## 3. 构建 → 验证 → 开机 的标准流程

```bash
export D22AP_ADT=$HOME/Desktop/marble-ios/work/xnu/research/ipsw-x/d22ap.adt
export ADSP_FW_DIR=$HOME/Desktop/marble-ios/work/adsp-fw
cd $HOME/Desktop/marble-ios/work/xnu-gh
# KEEPALIVE=1 停在 iOS（人机交互轮）；不带 = 有 sweetreboot=1，自动复位并自动拉 ring
bash scripts/mkimage.sh sweethap=1 ...           # 位置参数会被追加到 cmdline
# 交互轮： SERIAL=2aa26095 bash scripts/boot_interactive.sh images/<img>
# 日志轮： SERIAL=2aa26095 bash scripts/boot_one.sh images/<img> <label>
```

1. **CNTP_CTL 必须 NOP**（`fetch_kernel.sh` 里那段：模式 `28e21bd5` → `1f2003d5`，
   全 kernel **恰好 1 处**，否则 assert 失败；raw kernel 不启动）。
2. **boot-args 字段 = 607 字节**，`mkimage.sh` 在 >590 时**直接报错退出**（不要绕过）。
   历史上超长会静默切值（`sweetbufs=30`→`3`），把无效值当成有效读数。
3. **开机前**核产物，不是只核构建日志打印：
   ```bash
   LC_ALL=C grep -ac "sweetbufs=30" images/<img>     # 逐条 flag，值不能被切半
   LC_ALL=C grep -ac "<本补丁的 marker 字面量>" images/<img>
   ```
   两个已知假信号：
   - 裸 grep 到某 flag 名 ≠ 它在 cmdline 里
     （`PE_parse_boot_argn("sweetaopval")` 的**常量串**同样在镜像里）；
   - `%#x` 打印 0 时输出 `0`（不是 `0x0`），数故障值要用 `=[0-9a-fx]*`。
4. 判据（gate）**必须在开机前写进 `docs/audio-closeout.md` 并 commit**，事后不改。
   数字要**从已有 ring 里 grep 出真实形态**再写，不靠推。

---

## 4. 补丁（xnu/patches/NNNN-*.patch）纪律

- 常驻重建树在 **`work/chain-tree/`**（`/tmp` 会被清空）。
  它每个子树只跟踪**一个文件**：改 `bsd/dev/sweet_glink.c` /
  `iokit/Kernel/arm/hid/SweetHaptics.cpp` / `iokit/Kernel/IORegistryEntry.cpp` 之前，
  先确认该文件在树上的链位；新纳入的文件要先从 `work/ci-src/xnu-xnu-8796.141.3`
  复制并提交为基线，再编辑。
- **`work/ci-src` 是过滤过的快照**：某些文件被截断（`IOUserClient.cpp` 里搜不到 `attach`），
  `KernelConfigTables.cpp` 只到 ~0207 的链位 ⇒ 不能当 CI 基线，也不能据它数 personality 条数。
- **生成补丁的顺序**（这条错一次就烧一轮 CI）：
  1. 先把重放好的基线 **commit**；
  2. 编辑；
  3. `git diff <基线那个 rev> -- <file>` —— **不是 `git diff`**
     （HEAD 若已含上一版，你会拿到只有一行的碎片，`0251` 上真踩过）；
  4. 在一份**纯净基线副本**上 `git apply --check -p1`，再 apply，再 `diff -q` 对比你写的树；
  5. 数 hunk：和改动点数一致才提交（多出来的 hunk = 别人的补丁混进来了）。
- **基线 rev = 最后一个"已经变成补丁"的 rev，不是最后一个本地 commit**。
  链树里没落成 `.patch` 的 commit 在 CI 里**不存在**，据它 `git diff` 出来的上下文
  CI 树里没有 ⇒ `git apply --check` 直接拒（dxb 用的正是 `git apply --check` + `git apply`，
  所以只要基线选对，本地这一条就是 CI 的等价测试）。
  run 387（0261）就是这么红的：链树 `1a2266b` 加的 `body touched=` 观察块还没进任何补丁，
  0261 的 hunk 2 却把它当既有上下文 ⇒ `error: patch failed: bsd/dev/sweet_glink.c:5540` ⇒
  补丁被跳过 ⇒ 产物里是 K1 的 `oobcfg v8`。**marker 门把损失压成一次 CI 红，零次开机。**
- **看跳过原因不要下 44 MB 作业日志**：dxb 把 `git apply` 的报错塞进同一条
  `Skipping patch …` 行，而我们的 errors 抓取正好排除了它。
  现在 `build-error.txt` 自带 `=== skip detail ===` 段；历史轮次用
  `python3 tools/ci_joblog.py <sha>`（Keychain 里的 token，直接取作业日志里的 skip 行 + 失败步骤）。
- **每个补丁自带 marker**，且 marker = **整条 printf 格式串**、跨补丁唯一、在镜像里连续。
  作为**运行时参数**传入的 tag 不是 marker（字面量不连续）。
- **修上一版也要带可区分字面量**（如 `SWEET MARK: dtprop v3`）。`0251` v2/v3 只差代码、
  没有新字符串 ⇒ 无法用 marker 区分两个构建，只能比整包 sha。
- 改了 printf 字符串 ⇒ **同一 push 里更新 `.github/workflows/xnu.yml` 的 marker 列表**。
- **CI 侧 marker 列表是"省钱的那道门"**：它让一次被跳过的补丁表现为 CI 红，
  而不是一次白花的开机。
- 在函数里插代码前，检查它调用的标识符**首次声明是否在使用点之前**
  （`gm_rd()`、`sweet_cpu_*()` 这类晚期定义 + `extern` 会让 CI 报 ISO C90/C99 未声明；
  stub 编译**抓不到顺序问题**，要人工把标识符列一遍）。
- **新增/改写函数后必做 stub 语法检查**：把新函数原文抽出来 + 最小 stub，
  `clang / clang++ -Wall -Wextra -Wformat=2 -Werror -fsyntax-only`。
  今天两轮损失都是它没做（丢 `uint32_t i;` → CI 红；守卫顺序 → 开机 panic）。
  注意 stub 自己多余的变量也会被 `-Werror=unused-variable` 打死，
  别把 stub 的错读成代码的错。
- `python` 行手术容易吃掉括号/声明行；**用链对比抓**。生成 C 代码用**非 raw** 三引号，
  单引号串里别放撇号。
- `cmd | head -2 && echo OK` 会**吞掉 cmd 的失败**（管道取的是 head 的状态）。
  用 `cmd; st=$?` 或 `&&`。
- 只在"只动 `+` 行"时改已有补丁才安全；改了就要重算每个 hunk 的 `-N,M/+N,M` 并
  在**打补丁前**的树上 `git apply --check`。新逻辑一律开新编号补丁。

---

## 5. 仪器与日志的坑（写判据之前必读）

- 内核 console 在 `exec bluetoothd` 之后基本哑 ⇒ 只有直接调 `sweet_log_append()` 的行
  保证进 ring。早期行会出现两遍（console + 直写），那是直写通道在工作的证明。
- **行数换算**：`bsd/dev` 的 `printf` = **单写**（行数 = 次数）；
  `SWEET_HAP_LOG`（IOLog + ring 直写）= **双写**（行数 = 2×次数）。
  写判据前先从 ring grep 出真实形态（历史上我把双写行当成两次事件）。
- ring 单条记录 ~240 字节，**截断不加分隔符** ⇒ 一个"长行"其实是多条被粘起来；
  别数行、别按行算长度。长 payload 分 180 字节段打；读回用 `getLength()` 限制。
- **仪器必须能自证失败**：只在"已被证明会执行的路径"上打印；否则明确写成**非判据**
  （`bhap open` 就是——UC 连接根本不走 driver 侧 `open()`）。
- **阳性对照要在同一轮里被证明会出数**。`IOK launchd:` 当对照失败过
  （launchd 0 行而 backboardd 785 行）；换成 backboardd 自己当对照才对。
- 聚合数**按进程、按类/pid 拆开**。`IOK` 追踪的 400 行/boot 上限是**全局共享**的；
  某进程正好 400/60 行是**饱和**，不是它的总数。
- 尺寸/长度**匹配不是证据**：`u32=18504` 在 MTP ACDB 出现 53 次、Forte 98 次；
  "连续窗口求和凑到 18,504"在每个文件里能命中 9–12 次 ⇒ 一律不得据此认领记录组。
- **不要把仪器产生不了的数据写成观测**：`0x01001006` 的 OOB 包正文不在包里，
  `gprsend` 那 96 个字里 `p10..` 是**填充**，我曾把它读成"18,504 正文首字为 0"。
- 别拿错字段对比：HAP520 的 `auto_res`（`0xf009/0xf00a` 阻抗）≠ DT 的 `lra-f0-*`
  （谐振频率）。此比较已停用。
- 一次失败后的检查**不许复用**；没跑过的检查不许写进 commit message。
- Android 原厂侧（本机）：**debugfs 没挂**、`kprobe_events` EACCES、`mount(2)` EPERM、
  `setenforce` EPERM、pstore/sysfs EACCES、没有 `/dev/mem`、`clk_summary` 空
  ⇒ tracefs 类数据抓不到。可用：`/sys/class/regulator`、`/proc/asound/pcm`、regmap 列表、
  `/sys/kernel/tracing`（部分）。root 形式用 `adb shell "su 0 sh -c '...'"`
  （`su 0 cat` 会掉进交互 shell，`exec-out` 拿到的只有提示符）。
- `adb devices` 空常常是本机 adb server 而不是手机：`adb kill-server && adb start-server`。
- 后台任务不要 `| tail`（会缓冲到退出，看起来像"没在跑"）；重定向到文件再读产物。

---

## 6. 已验证的结论（不要重新证明，也不要自我推翻）

### 音频（iOS ⇄ ADSP）
- 控制面**全绿**：`GRAPH_OPEN`/`PREPARE`/`START` `status=0`，`DATA_BUFFER` 被 ADSP 消费
  （`data_buffer … done status=0`），`sweets1ap=1`（S1 叶子 AP=01）第一次拿到响应。
- **②（2026-11-05，F–L2 共九枪，已用 26/120；M1/N1 是第 27、28 枪）**：`status` 的分辨力已逐轴拆完，
  同址 `0x1000`、同 `dst=1`、同句柄、每轮只发一发：
  **iid 是分界**（`iid=1` ⇒ `0x3`；`iid=` 真实模块号 ⇒ `0x1`，其余逐字不动，0260/第 24 枪）；
  在 iid=真实 之下 **每条 size（12 vs 52）与条数（4 vs 9）都是 `0x1`** ⇒ 这两格低信息量、未再花开机；
  **传输不是分界**（同一份 288 B 带外与带内同码 `0x1`，0261/第 25 枪）；
  **长度不是分界**（降到 216 B ≤256 带内仍 `0x1`，0262/第 26 枪）；
  已排除还包括 **地址 / 地址形式 / param id 单独 / 正文字节**（F/E/I/J）。
  ⇒ 与唯一拿到 `0` 的 C 之间只剩 **param id 与每条 size** 两格，且对 C 是一起动
  （C = `iid=0x428f` + `param=0x08001017`(12)/`0x0800101b`(28)、2 条、带内 80 B）。
  下一枪 **M1** = C 的形状**只**把 param 换成 `0x08001004`；再下一枪 **N1** 把 `error_code` 预置成
  非 0 哨兵（`0xa5a5a5a5`），让 `body touched!=0` 成为正证据。
- **② 的新离线结论（2026-11-05，零开机）**：子图 `0xb0000023` 的逐模块 cal 表 = **10 条**、
  全部 `param=0x08001015 size=8` ⇒ 上线正文 **240 B ≤256** ⇒ **一发带内 SET_CFG 就装得下整张表**，
  既不需要带外、也不需要切块、更与那 18 KB 无关（常量与几何见
  `docs/data/acdb-m2-subgraph23-records.txt`）。厂商 672 B OOB 正文**不等于**任何子图的记录长度和
  （最近 480）⇒ 672 仍是未解释项，不得据此认领内容组。
  三个码**除 `0` 外都是拒绝**、语义不命名；逐记录 `error_code` 已**八轮**全 0 ⇒ 记录级仍盲；
  `body touched=` 只作观察（预置 0 ⇒ 分不清"没写回"与"写了 0"）。
  另：`GRAPH_OPEN` 的 `status=0x2` 语义仍未查；18 KB 正文**一个字节都没拿到过**。
- **AudioReach 线上格式**（官方实现 + 我在本机 `libar-gsl.so` 上逐条反汇编复验）：
  - 记录头 = **`apm_module_param_data_t{module_instance_id, param_id, param_size, error_code}`
    = 16 B**，记录间 `GSL_ALIGN_8BYTE`。**没有** GSL 外层包装头。
  - OOB 包 = 40 B gsl/gpr 头 + **`apm_cmd_header_t{payload_address_lsw, msw, mem_map_handle,
    payload_size}`**（抓包里就是 `p6..p9`；**正文在映射区，包里取不到**），
    实测区内偏移可以不是 0（`p6=0x800`）。
  - 路由：`payload_size ≤ 256` 走 inband，`>256` 走 OOB（比较的是**调用者正文长度本身**，
    不含任何头）；单条记录 ⇒ `dst = 该记录的 module_instance_id`，多记录 ⇒
    `dst = APM = 1`（`APM_MODULE_INSTANCE_ID = 1`）。
  - **`property_flag = 1 << (mem_pool_id − 1)`**（`libar-gsl.so:0x186fc→0x18710`）
    ⇒ pool=3 必然 flag=4。所以 `sweetshmflag=4` **不是** "offset 模式"
    （此说法已在 `docs/START-HERE.md` 更正）。
  - `APM_CMD_SET_CFG = 0x01001006`；我们实测的 `GRAPH_START` op 是 `0x01001002`；
    `0x08001005` = `APM_PARAM_ID_SUB_GRAPH_LIST`（**不是** GRAPH_START 的 opcode）。
- 我们那个 **2,960 容器**的构造方法被反向验证：
  `Σ(52 + align8(blob_len))` 对 `{b0000002:808, b0000025:756, b0000024:256/924, b0000023:944}`
  精确等于 2,960。注意另一种框法 `8 + align8(44+blob_len)` 给出同一个数
  ⇒ **数字本身不能区分两种格式**。
- ACDB 结构（本机 6 份文件，9,039/9,039 条目验证）：条目头 **44 B**，不变式
  `entry_len = 44 + blob_len`；字段序
  `entry_len, desc_len, subgraph_id, num_desc_params, {param_id,size,data}…, blob_len, blob`；
  blob 起点恒满足 `file_offset % 4 == 2`。blob 内部是 `{iid, param_id, size} + align8` 的 TLV 链。
- **mediaserverd 现状**（J/K/L/M 四轮实测）：open 4 台 IOAudio2 设备、`map`、`notify port`
  都有，**一个 `IOAudio2DeviceUserClient` 的方法都不调**；它落空的查询只有
  `AppleEFINVRAM`/`IODTNVRAM`/`bluetooth`，且在那之后还继续跑了 180+ 行
  ⇒ "缺 NVRAM 所以停住"**没有证据**。
  `path IODeviceTree:/product` 的上百次查询发生在**成功 open 之后**。
  `GetControlValue`（selector 4）的回答从 0 改成 1 **什么都没变**（M 轮）。
- `sel=4`（GetControlValue）**跟音频 flag 走**，与探针/追踪无关；`sweetaopval`、`sweetnobb`、
  `sweetampverify` 这些已死仪器不再带。
- `crashed: mediaserverd` 只出现在 **24/181** 个 ring，且与"能 open 音频设备"的 12 个 ring
  **完全不重叠** ⇒ 崩溃风暴是另一批 boot 的问题，**不能**解释"开壳不出流"。

### 震动（B4）
- **输出侧已解决**：AP 侧 SPMI 直驱真的能震（44/44 次有震感、震感与设置一致）
  ⇒ "B4 和 ② 是同一堵墙"作废。
- `fault during play=0x2` 由**本次脉冲**产生：`0xf066<-0x07` 清除后立即读 `0xf00c` **恒为 0**，
  播放中恒 `0x2`（44/44）。所以 `0x2` **不代表"没动"**，位语义未查。
- **4 个同名 bhap 实例是我们自己造的**：4 个不同 `self`、`provider` 全是同一个
  `IOResources` 节点 ⇒ 同一 provider 被同一条 personality 匹配 4 次，**不是**实例互相匹配；
  iOS（pid 57）attach 的是**最后注册的那个**，前 3 个是孤儿。机制候选是
  `SweetHaptics.cpp:593` 把实例 `IOMatchCategory` 改写成 `IODefaultMatchCategory`
  （XNU 靠它去重：`IOService.cpp:3887 copyClientWithCategory` + `:4033` 赋值），
  **但 Callan 同样改写却只有 1 个实例 ⇒ 仍是候选，不是结论**。
- UC 连接**不调用 driver 侧 `open()`** ⇒ 想问"谁 attach 了哪个实例"，只能挂在 selector 路径上。
- DT 里只有 **1 个** `qcom,hv-haptics@f000`；`swr_haptics`（SoundWire）并存但不是我们的必要条件。

---

## 7. 已关闭 / 不要重走

- 追那 18 KB 的**内容**（机内 6 份 ACDB 已量完：没有任何 blob 落在 18,000–19,000；
  所谓"超限校准记录"是 1,524 条目表之后的**文件级尾块**，不是子图校准）。
- 用"凑总长"认领 ACDB 记录组。
- `auto_res` vs `lra-f0-*` 的比较。
- 用 `tinyplay` 当扰动去测时钟（该格已因"无法测量"关闭）。
- `matchPropertyTable` 探针（观测不到类查询，别再挂回来）。
- GPU 线：FROZEN。重开门槛 = 真自旋心跳 + 能扛住停滞的输出通道。
- 按文件名找库：`/vendor/lib64/libgsl.so` 是 **Adreno GPU** 的 GSL，
  音频的是 **`/vendor/lib64/libar-gsl.so`**（本地副本 `work/phone-libs/`）。

---

## 8. 目录与产物

```
work/xnu-gh/                     补丁、脚本、docs、logs（git 仓库）
  xnu/patches/NNNN-*.patch       补丁序列（当前到 0251）
  .github/workflows/xnu.yml      CI，含 "Verify our patches applied" marker 列表
  docs/status-log.md             按时间的实验日志（最新看这里）
  docs/audio-closeout.md         ② 与音频收尾 + 每轮判据/判决（H/I/J/K/L/M/N、②攻坚 1-6）
  docs/audio-hal-bridge.md       iOS 侧出声还差什么（路线排序、ep 序列）
  docs/START-HERE.md             已知-good 配置 + 复现命令
  docs/gpu-probe.md / smmu-dead-ends.md / speaker-path.md / handoff.md / required-assets.md
  tools/acdb_graph_rebuild.py    ACDB TLV 走链 + 容器对表
  tools/acdb_size_scan.py        整文件 u32 尺寸扫描
work/chain-tree/                 常驻重建树（git，按文件跟踪；见第 4 节）
work/chain-tree-i2c/             I2C 子树的另一链位
work/ci-src/xnu-xnu-8796.141.3/  过滤过的源码快照（不可当基线）
work/phone-acdb/                 本机 6 份 ACDB（已从 /tmp 挪出）
work/phone-libs/libar-gsl.so     AudioReach GSL
work/iphone-x-recon/             真机取证（dyld/ioservice.txt 等；
                                 hfs 本地 iosroot16.img 只是 ~2.68 GB 前缀，用户态结论必须回机读）
logs/ring-*.txt                  ring 日志（新一轮之前先把上一份按 run 名复制出来）
```

判日志位置只认**最后一条 `sweet_log_sync()`**；文档引用 ring 要用归档文件名，
不是会被覆盖的 `logs/ring-gpu-pwr.txt` 那类默认名。
