# AudioReach GRAPH_OPEN 序列化格式：外部答案 + 我们自己字节的验证（2026-11-06，零开机）

来源：用户把我们的问题转给 GPT，回答引用 AudioReach 公开仓库
（`audioreach-graphservices/spf/api/apm/apm_module_api.h`、`audioreach-engine/.../apm_gpr_cmd_parser.c`、
`modules/cmn/pcm_mf_cnv/capi/{pcm_cnv,mfc}/api/*`、`ar_osal/api/ar_osal_error.h`）。
**下面的每一条都在我们自己的 blob 上复跑过**，不是照抄。

## 1. 官方格式（GPT 给的）与我们 760 B blob 的实际走链结果

记录外框是我们早就确认的 `apm_module_param_data_t{iid, param_id, param_size, error_code}`（16 B），
记录间 `align8(param_size)`。三段内部布局：

- `0x08001002 MODULES_LIST`：`{num_modules_list}` + 每列表 `{sub_graph_id, container_id, num_modules}` + `num_modules × {module_id, instance_id}`（**每条只有 8 字节**，没有端口数组、没有 gateway 计数）
- `0x08001003 MODULE_PROP`：`{num_module_prop_cfg}` + 每块 `{instance_id, num_props}` + 每属性 `{prop_id, prop_size}` + 数据（PORT_INFO `0x08001015` = `{max_ip_port, max_op_port}`，8 字节）
- `0x08001004 MODULE_CONN`：`{num_connections}` + `num × {src_iid, src_op_port, dst_iid, dst_ip_port}`（16 字节，**数据连接是 1:1**）

我们的 `g_graph_vp_wr`（760 B）用这套布局走链，**正好消耗 760 字节、5 条记录、无残留**：

```
rec0 @0x000 iid=1 0x08001001 SG_CFG          size=48
rec1 @0x040 iid=1 0x08001000 CONTAINER       size=100
rec2 @0x0b8 iid=1 0x08001002 MODULES_LIST    size=80   num_modules_list=1
        list[0] sg=0xb0000002 container=0xe0000002 num_modules=8
          0x07001010 SAL            iid=0x4012
          0x0700101A ?              iid=0x4007
          0x07001011 ?              iid=0x418e
          0x0700100e TDM(本仓库 G_TDM_MODULE_ID) iid=0x428f
          0x07001098 ?              iid=0x4006
          0x07001015 MFC            iid=0x48ba
          0x10013d01 AW(厂商私有)     iid=0x42d3
          0x07001000 WR_SHARED_MEM_EP iid=0x40bd
rec3 @0x118 iid=1 0x08001003 MODULE_PROP     size=196  num_module_prop_cfg=8
        0x4012 {0x17,0x1}  0x4007 {0x1,0x1}  0x418e {0x1,0x1c}  0x428f {0x1,0x0}
        0x4006 {0x2,0x1}   0x48ba {0x1,0x1}  0x42d3 {0x1,0x1}   0x40bd {0x0,0x1}
rec4 @0x1f0 iid=1 0x08001004 MODULE_CONN     size=244  num_connections=15
```
⇒ **GPT 反问的那一条已经成立：三段都能连续解码**（`num_modules=8` 与 8 个 prop 块、15 条连接、
`16+align8(size)` 步进全部吻合，最后正好 760）。**我们不需要按"每模块后跟端口描述"去扩图。**

## 2. 数据路径（从 conn 表直接读，不是推测）

```
0x40bd WR :out 1  ->  0x4012 SAL  :in 2
0x4012 SAL :out 1  ->  0x48ba MFC :in 2     <-- MFC_DATA_INPUT_PORT = 0x2（与公开 API 一致）
0x48ba MFC :out 1  ->  0x42d3 AW  :in 2     <-- MFC_DATA_OUTPUT_PORT = 0x1（与公开 API 一致）
0x42d3 AW  :out 1  ->  0x418e     :in 2
0x418e     :out 1  ->  0x4006     :in 2
0x4006     :out 1  ->  0x4007     :in 2
0x4007     :out 1  ->  0x428f TDM :in 2
（其余 8 条是 0x418e 的多输出端口到 0x41e2/0x467e/0x4436/0x4199/0x468d/0x422c/0x4448/0x4333 的旁路）
```
⇒ **MFC 本来就在我们的主链上，端口号也和公开 API 对得上。** 所以 G1（配置 0x48ba 的输出媒体格式）
**不需要动 blob 一个字节** —— 这把"手改图"这一档成本直接删掉了。

## 3. 两条对我方框架的实质更正（来自 GPT，已由本地头文件对名字核实）

1. **`PCM_CNV (0x07001003) 不做采样率转换**（公开头文件原文 "It cannot be used to convert the sampling rate"）。
   ⇒ 我上一条说"我们缺 PCM_CNV 所以不响"**方向错了**：48k→96k 的转换级是 **MFC (0x07001015)**，
   而我们**有一个 MFC，就在主链上，只是从没给它任何输出格式**。
2. **MFC 的重采样器参数是 `0x0800104D PARAM_ID_MFC_RESAMPLER_CFG`，且 IIR 只到 48 kHz、96 kHz 必须用 FIR（默认 type=0）。**
   ⇒ 我们在厂商抓包里**从没见过 0x0800104D**（本次普查：两个窗口里 `0x08001024` 出现 6 次，
   `0x0800104D` **0 次**，`0x08001061` 0 次）⇒ **不发明这一封**，按公开默认 FIR 不发送是对的。

## 4. status 语义（记下来，但不许拿它命名机制）

GPT 给的公开枚举（`ar_osal_error.h`，`ar_result_t`）：`0x1 AR_EFAILED`（通用失败）、
`0x2 AR_EBADPARAM`、`0x3 AR_EUNSUPPORTED`，另有 `0x7 AR_ENORESOURCE`、`0xA AR_ENOTREADY`、
`0xC AR_EBUSY`、`0x15 AR_ETIMEOUT`。
**我们自己的规矩不变**：这三个码过去 30 多枪都只当"不同的拒绝"，因为
(a) 公开枚举与 UKEE 量产固件可能有私有差异，(b) `0x1` 是 catch-all，本身不含原因信息，
(c) `GRAPH_START status=0` 也**不**证明物理引脚有输出（GPT 同判）。
⇒ 允许写"应答 = 0x1（对外映射为通用失败类）"，**不允许**据此命名卡点。

## 5. 厂商侧的对照事实（我们的抓包，非 GPT）

厂商设备子图里同样是 `0x4012 → 0x48ba(MFC) → 0x42d3 → …`，
但它发 `0x08001024` 的对象是**别的 MFC 实例**：一对 `{0x40ca→48000, 0x40c8→96000}`、
另一对 `{0x401d→48000, 0x40b6→96000}`（同一 dst=1 批量、payload 64、整包 104）。
⇒ **G1 是"厂商形状、厂商数值，目标换成我们自己的实例"**，不是逐字重放。这点在读数时必须挂着：
如果 `rc=0` 仍无声，结论不是"配置无用"，而是"格式在厂商那边是由**另一张子图**里的 MFC 完成的，
我们那张子图根本不在我们 blob 里"——那时才回到扩图。

## 6. GPT 给的 `param_id_mfc_output_media_fmt_t` **逐字解释了我们手上的 12 字节**（新确认）

公开结构：
```c
struct param_id_mfc_output_media_fmt_t {
    int32_t  sampling_rate;
    int16_t  bit_width;
    int16_t  num_channels;
    uint16_t channel_type[0];      /* 柔性数组 */
};
```
厂商那条被接受的记录 `size=12`，正文三个 u32 是 `{0x00017700, 0x00020020, 0x00020001}`：

| 字节 | 字段 | 值 |
|---|---|---|
| 0..3 | `sampling_rate` | `0x17700` = **96000** |
| 4..5 | `bit_width` | `0x0020` = **32** |
| 6..7 | `num_channels` | `0x0002` = **2** |
| 8..9 | `channel_type[0]` | `0x0001` |
| 10..11 | `channel_type[1]` | `0x0002` |

⇒ `12 = 8 + 2×2` 与柔性数组**正好吻合**，两个通道映射就是 `{1,2}`（左/右）。
这同时**回溯确认**了我们 0265 里 `mfc_a/mfc_b = {rate, 0x20020, 0x20001}` 的抄写是对的
（当时只是字节照抄，没人知道字段含义），也说明 `sel=3` 的两条批量在结构层面无缺项。
顺带一条读数纪律：`0x08001024` 的 `size` **必须是 8 + 2×num_channels**，
若哪天把 `num_channels` 改成 4，`size` 就得变 16——否则 DSP 读到的是截断的通道映射。

## 7. GPT 答案里我**不接受**的部分

- 他建议"先插入 MFC 并确认 UKEE 固件支持该模块"——**我们不用插**，第 2 节已经证明 MFC 在主链上、端口号也对。
- 他猜测"`0x48ba` 可能需要 `PORT_INFO` 属性"——**已有**：`rec3` 里 `0x48ba {max_ip=1, max_op=1}`，正是 MFC 的 1 入 1 出。
- 他提的"12.288 MHz / 4 slot"账算得对（`128 = 4 slot × 32 bit`），
  但厂商 `0x0800101b` 的 `slot_mask=7` 只开 3 个 slot、`active_slot_mask` 与两颗 AW882xx 的映射
  **仍未逐字核对**——这一格留给"下一轴"，不混进 G1 的判据。

## 8. 收尾快照（11-06 午间，设备/电脑都将离线；全部离线，零新开机）

**GPT 的 TDM 解码通过尺寸自检**：`param_id_tdm_intf_cfg_t` 公开字段 4+4+(2+2)+4+(2+2)+(2+2)+(2+2) = **28**，
与厂商 `0x0800101b` 的 `size=28` **精确相等**；逐字读作
`lpaif_type=0, intf_idx=2(Tertiary), sync_src=1(internal), ctrl_data_out_enable=1,
slot_mask=7, nslots_per_frame=4, slot_width=32, sync_mode=0(short), invert=0, sync_data_delay=1, reserved=0`。
⇒ 直接回答了我问的两点：**`7` 是 `slot_mask` 不是 `slot_cnt`**；`0x00200004` 低半字 4 = 每帧 slot 数、高半字 32 = slot 位宽。
算术自洽：`96000 × 4 × 32 = 12 288 000` = 厂商那票 `0x204 @ 12288000` ✓。
**遗留异常**：`slot_mask=7` 开 3 个 slot，而 MFC/WR 都是 2 声道 ⇒ 第三路从哪来仍未解（GPT 猜 MUX_DEMUX/AW DSP 加路），
这一格不进 G1 判据。

**普查更正（重要，压低 G1 的先验）**：把厂商抓包里每条 SET_CFG 的**首记录** `(dst, iid, param, size)` 全列出来，
**没有任何一条指向 `0x4006`（MUX_DEMUX）或 `0x48ba`（我们图里的那个 MFC）**；
`0x08001024` 只出现在两个 dst=1 批量里，实例对是 `{0x40ca@48k, 0x40c8@96k}` 与 `{0x401d@48k, 0x40b6@96k}`。
⇒ G1 是**"厂商形状 + 厂商数值，目标换成我们自己的实例"**，不是逐字重放；
`rc=0` 仍无声时，正确结论是"厂商的转换发生在**我们 blob 里根本不存在的另一张子图**"，而不是"MFC 无关"。

**vendor4 的容器外框已从 preamble 自证**：4 条记录，`sg = 0xb0000002 / 0xb0000081 / 0xb0000024 / 0xb0000023`，
`desc_len=36`，`blob_len` 就存在 preamble 第 12 个字 = **816 / 516 / 256 / 924**，`entry_len = 44 + blob_len`（860/560/300/968 ✓）。
求和：`8 + 4×52 + 2512 = 2728 = 0xaa8`，而数组只有 `2724 = 0xaa4` ⇒ **"短 4 字节"这条从前只能引用的结论，现在由 preamble 自己算出来了**。

**但我这次的内层走链失败，结论要写清楚**：从 `body` 开始按 16 B 线记录走，四条子图都**只走出第一条 `SG_CFG`**
（正文 `{sg, 3, 0x0800100e, 4, 2, 0x0800100f, 4, 2}`），下一字是 `pid=0x64 size=1`（第 4 条是 `0x124 size=3`）——
**那不是 param 命名空间的值**。⇒ **`vendor4`/`2960` 是 ACDB 容器格式，不是 GRAPH_OPEN 线格式**；
而我们的 `g_graph_vp_wr` 是线格式（5 条记录精确铺满 760）。两者不能混用同一种走链，
`tools/acdb_graph_rebuild.py` 才是把前者变成后者的工具。

**module id 名字的可信度分级**：`0x0700100E` 有**两条独立来源**（GPT 给的 `MODULE_ID_TDM_SINK` + 我们自己源码
`G_TDM_MODULE_ID = 0x0700100eu`（:2568））⇒ 可当事实；
`0x07001011 SPLITTER / 0x0700101A DATA_LOGGING / 0x07001019 SOFT_PAUSE / 0x0700101B VOL_CTRL /
0x07001032 SPR / 0x07001098 MUX_DEMUX / 0x10013D01 AWDSP_RX` 在本地 ASoC 头里 **grep 不到**（已核：`No matches found`）
⇒ 只算外部命名，读数据时不许当证据。注意 `0x0700101b`（模块 VOL_CTRL）与 `0x0800101b`（参数 TDM_INTF_CFG）
是两个命名空间，别写串。

## 9. 第二轮 GPT 答案：核对结果、一处自我更正、以及它没意识到的事

**（a）它引用我们仓库的那句话是真的，而且它问的问题我们已经答过自己了。**
`g_graph_vp_wr` 的头注释（链树 `:1986-1991`）原文就是：
> Vendor device graph 0xb0000002 with a WR_SHARED_MEM_EP (0x07001000, iid 0x40bd) prepended and
> wired to SAL input port 2. **The vendor does this with a second (stream) subgraph and a
> cross-graph connection; a single merged subgraph avoids having to open and bind two graphs.**

⇒ 所以"单图 vs 双图"这一格**不是新信息**：我们的图从设计上就是"把 WR 塞进设备子图"的合并方案。
GPT 的判断（可以先试合并单图）**与我们的设计意图一致**；他补的那句限制才是有价值的：
这只回答"数据通路是否自包含"，**不等于**证明单图能出正确波形。
（顺带：同一注释还写了我们的模块/端口/连接取自厂商 stream 图 **`0xb00000d6`** 的 dump——
这个 sg id 与 vendor4 里那四个（`0xb0000002/81/24/23`）**不是一个来源**，别混用。）

**（b）我自己的一处猜测必须收回。** GPT 说他在公开 `apm_api.h` 里找不到 `APM_CMD_GET_DATA`
/`apm_cmd_get_data_t`/`apm_param_id_gdr_t`，也没有 `0x01001011`。核对：我们源码里
`APM_CMD_*` 定义只有 `0x01001000/01/02/06/0C` + `0x0400100A`/`0x05001004`，**从来没有 0x01001011**；
那个"GET_DATA = 0x01001011"的标签来自**我自己某次解包脚本里写的一张 NAMES 猜测表**，
它后来被我当成"已知名字"引用过。⇒ **作废**，任何判据/文档不得再引用它。
采用的公开值是 `APM_CMD_GET_CFG = 0x01001007`、`APM_CMD_RSP_GET_CFG = 0x02001000`
（已在 `:2562` 附近定义，且 0269 的包型由宿主机 stub 实测过）。

**（c）MUX_DEMUX 的参数 id 是外部-only，且他给的一条否证很有用。**
`PARAM_ID_MUX_DEMUX_CONFIG = 0x080011BD`（`{num_connections}` + 每条 16 B
`{in_port, in_chan, out_port, out_chan}`）、`PARAM_ID_MUX_DEMUX_OUT_FORMAT = 0x080011BE`
（每配置固定 12 B：`{out_port, bits_per_sample(u16), q_factor(u16), num_channels(u32)}` + `channel_type[]`）。
本地 ASoC 头里**grep 不到这两个号** ⇒ 只当外部命名，等真发/真读回再升格。
他点出的要害：**MUX_DEMUX 的输出格式里没有采样率字段** ⇒ 它能解释声道重排，
**不能**解释 48k→96k，所以速率问题只能回到 MFC。
（我们自己的普查补充：厂商抓包里**没有任何一封 SET_CFG 指向 `0x4006`**，
所以"厂商给 MUX_DEMUX 下发过配置"目前**没有证据**，不能当既有事实引用。）

**（d）GET_CFG 的两个坑，第二个我们以前踩过同类。**
`param_size` 必须填**缓冲区容量**（容器把它当 `max_data_len` 传给模块，之后用实际长度写回），
填 0 就什么都读不到；响应里 `param_size` 会**从 72 变成实际返回长度**——这正好是
"N1 那次 `err=0` 其实是没人写过"的同一类陷阱的反向利用：**写回变化本身就是"模块真被问到过"的哨兵**。
包型由 `tools/stub_getcfg.py` 实测（真编译真跑）：
```
dst=0x48ba opcode=0x1001007 record{iid=0x48ba,param=0x8001024,size=72(CAPACITY),err=0}
apm_payload=88 (=0x58)  whole gpr packet=128
假响应回放：w5=0x2001000 w6=0 w7=0x48ba w8=0x8001024 w9=0xc w11=0x17700(96000) len=56
短响应只打印放得下的字；无响应打印 "no response"（不会 dump 零）
```
stub 自己也错过一次（把记录字段按 16 字节步进去读，`param` 显示成 0）——那是 stub 的错，不是代码的错。

## 10. 落库：0269（GET_CFG 读回）

链树 `c34824b`，补丁 `xnu/patches/0269-sweet-apm-get-cfg-mfc-readback.patch`，
标记 `SWEET MARK: mfcget v1 `；`tools/ci_replay.sh`：`applied=210 / skipped=49`，
`OK: CI post-image == chain-tree HEAD`。
`sweetmfcget=1` 在 set 之前与之后各读一次；`=2` 再加一次**在 PREPARE/START 之后**（格式传播完）；
默认 0 一个字节都不发。
**注意编号**：0269 已被音频占用 ⇒ GPU PR 若要落库，重编号目标改为 **0270/0271/0272**（任务 #37 已相应改写）。

## 11. 第三轮外部答案：核对结果与 0270

**（a）最重要的一条自我更正：0269 的应答解析是错的，且错在"会把好答案读成失败"。**
0269 让 `g_apm_cmd()` 代发 GET_CFG，而那个 helper **把每一封应答都当 basic response 解析**
（`status = g_le32(g_rsp+28)`）。对结构化应答 `0x02001000` 来说 `+28` 是**回显的 module iid**（=0x48ba≠0），
于是**一次完全成功的读回会被判成 rc=-1**；对 basic result `0x02001005` 来说 `+24` 是**原命令 opcode**，
会被当成 status 打印。⇒ **0270 改成自己发包 + 按应答自己的 opcode 分支**，
marker 由 `mfcget v1 ` 升为 `mfcget v2 `（workflow 同步）。

**（b）他还给了一层我没写进去的区别，值得记：模块"不支持 Get" ≠ 一定收到 Basic Result。**
公开容器实现对 GET_CFG 会调模块 `get_param`，把结果写进该记录的 `error_code`、
把累计结果写进 `0x02001000` 的整体 `status`；只有请求在**进入结构化处理之前**失败
（路由/命令头/映射/应答分配）才走 `__gpr_end_command()` 发 Basic Result。
⇒ 我们的分支必须区分四件事：应答 opcode、整体 status、记录 `error_code`、实际应答长度——
**0270 四件都打印**。

**（c）in-band 与 OOB 的读偏移不同（我们只用 in-band）。**
公开容器对 OOB GET_CFG 只回 4 字节的 `status`，参数记录留在共享内存里；
in-band 才是 `status + 记录 + 数据`。我们的 GET 是 in-band（记录 16 + 容量 72，`payload_size=88`），
所以 `sampling_rate` 在绝对 `+44`。**若哪天改 OOB，绝不能在应答包 `+4` 处找 iid。**

**（d）输入媒体格式：这一格关掉，别再发。**
公开 MFC 实现里输入格式是经 **SPF 的媒体格式传播 → `CAPI_INPUT_MEDIA_FORMAT_V2`**（CAPI 属性，**不是 APM param_id**）
进入模块的，客户端不需要也不应该再对 MFC 发一封 `0x0800100C`；
`0x08001023` 在他核的两个公开版本里**根本不存在**（我上一轮把它当候选问出去，是错的，撤回）。
他的 `handle_input_media_fmt()` 片段也把默认语义钉死了：
`if (PARAM_VAL_NATIVE == configured_media_fmt->sampling_rate) out->sampling_rate = in->sampling_rate;`
⇒ "从没下发输出格式 ⇒ MFC 输出就是 Native（跟输入 48k）"这条从推测变成有源码路径的推断。
**但注意他给的反向提醒**：我们观察到 SAL 对 `0x0800100C` 直接 SET_CFG 被拒，
那**只说明 SAL 不吃这个 APM 参数**，**不能**推出 SAL 不通过 CAPI 框架向下传播格式——
所以"MFC 到底收到没有 48k 输入"仍未测，只能由 J1 的读回去回答。

**（e）channel_type 枚举：可用，但两代公开定义不同。**
`PCM_CHANNEL_L=1 … PCM_CHANNEL_R=2 … PCM_MAX_CHANNEL_MAP=63`（GraphServices `558ddfc2`，35–47 未定义，48–63 自定义）；
Engine `5773bf8` 扩到 `…_V2 = 128`。⇒ **不得**用公开头断言 SM7475 固件用哪一版；
我们那条 MFC 输出记录里的 `cmap={1,2}` 与 `nch=2` 自洽（L,R），这就够了。
另外他纠正的用法要记：`MUX_DEMUX_CONFIG` 里的 `input_channel_index/output_channel_index`
是**位置序号**，不是 `PCM_CHANNEL_*`；只有 `OUT_FORMAT` 的 `channel_type[]` 才是声道语义。
⇒ 想解释 TDM 那 3 个 active slot，必须同时看 `0x4006` 的两段 + TDM 自己的格式与 slot_mask，
单看 `slot_mask=7` 推不出对应哪三个通道。
（我们自己的普查仍然成立：**厂商没有任何一封 SET_CFG 指向 `0x4006`**，
所以"厂商给 MUX_DEMUX 下发过配置"目前**无证据**，别当既有事实。）

## 12. 0270 的宿主机证据（真编译真跑，`tools/stub_getcfg.py`）

stub 特意把应答做成 **pending → `g_drain()` 投递**，这样能测到函数自己的
"发之前先清 `g_rsp_ready`"——旧写法若在 `fake_reply` 里直接置 ready，
就会被函数清掉，**五次读回全部打印 no response**，看着像代码坏了，其实先错的是 stub。
（另一次 stub 自己的错：把记录字段按 16 字节步进读，`param` 显示成 0。）

```
sent: pkt_size=128 hdr{ver+len=0x8060 dom=0x302 src=0x2010 dst=0x48ba tok=0x1001107 op=0x1001007}
      record{iid=0x48ba param=0x8001024 size=72(CAPACITY) err=0}
mfcget v2 got96k   get_cfg len=56 st=0 iid=0x48ba param=0x8001024 size=12 err=0 rate=96000 bw=32 nch=2 cmap=0x20001
mfcget v2 got48k   get_cfg … size=12 err=0 rate=48000 bw=32 nch=2 cmap=0x20001
mfcget v2 size-not-written-back get_cfg len=44 … size=72 err=0 rate=0 bw=0 nch=0 cmap=0
mfcget v2 basic-result basic len=32 echoed_op=0x1001007 status=0x3
mfcget v2 no-reply no response pkt=128
```
`tools/ci_replay.sh`：`applied=211 / skipped=49`，`OK: CI post-image == chain-tree HEAD`。
**编号提醒**：0270 已用于音频 ⇒ GPU PR 的重编号目标再顺延为 **0271/0272/0273**。

## 13. 更正：第 11(d) 节里"SAL 对 0x0800100C 返回 EUNSUPPORTED"**不是我们的观测**

我写那一节时把它当作"我们此前观察到"的事来转述——**错了，那是外部答案里的前提**。
回我们自己的证据查：

- 归档 ring 里**没有任何** `cdma/mf_cfg` / `sal/output_cfg` / `sal/mf` 行
  （`grep -l 'sal/mf' logs/ring-*.txt` 与 `grep -rl 'apm/mf48|sal/mf|mf_cfg' logs/ring-*.txt` 都是 0 命中）。
- 原因在代码里：那些发送在 `g_audio_graph_cfg()`（`sweet_glink.c:6662`），
  只有 **CODEC_DMA 那两条图路径**才会调用它（`:7619`、`:7639`）；
  我们实际开的 `vp-wr` 走的是 `g_audio_graph_cfg_vp()`，两回事。
- 我们在带内被拒过的记录只有 `vp/cfg_40bf / 40c0 / 40c8 / 4165`（`status=0x1`，见 `ring-vcfg2.txt`），
  那是**厂商的实例 id 不在我们图里**，与 SAL 无关。

⇒ 结论：**"SAL 不吃 `0x0800100C`" 在我们这里既没有被证实也没有被否证**，它不该出现在判据或推理链里。
对 J1 没有影响（J1 读的是 MFC `0x48ba`，与 SAL 无关），但这条要立个规矩：
**外部答案里的前提，凡我没在我们 ring/源码里核过的，一律先标"外部前提"再引用**（这次是核过之后发现自己转述成了"我们观察到"）。

## 14. 第四轮外部答案 + 0271（把时间轴补成三个点）

**收下并照做的两条：**
1. `PARAM_ID_HW_EP_MF_CFG (0x08001017)` 对 **TDM_SINK** 描述的是**硬件端点接收的 PCM 格式**
   （`sample_rate/bit_width/num_channels/data_format`），**不是**时钟/slot——时钟/slot 属 `0x0800101b`。
   ⇒ 我们两封都发，分工与他说的**完全一致**，这一格没有缺口。
2. "中间模块没被逐一发格式"**不等于**它们没有格式：框架沿连接把上游输出格式转成下游
   `CAPI_INPUT_MEDIA_FORMAT_V2` 属性。⇒ **撤掉"要给每个中间模块补 PID"这条候选**，
   要查的是**传播有没有走通**，而不是多发几封 SET_CFG。

**他给的三个更值得查的点，我按可测性排序：**
- **SAL 的输入格式约束**（公开声明支持 Fixed Point、16/32 bit、Q15/Q27/Q31，交错方式
  `deinterleaved unpacked`）。若 WR 报出的格式与 SAL 接受的输入不符，**传播断在 SAL**——
  这比"少发 PID"合理得多，且**J1 的三个读回点正好能量出断在哪一段**（见下）。
- **MUX/DEMUX 的输出格式事件要求输出端口 STARTED** ⇒ 就算 MFC 在 PREPARE 后已有效，
  **也不能据此推断最末端 TDM 收到了格式**。所以读回必须至少两点（PREPARE 后 + START 后）。
- **`cu_handle_prepare()` 里就有 `propagate_media_fmt(base_ptr, FALSE /* is_data_path */)`**
  ⇒ 控制路径传播发生在 **PREPARE**，不必等 START ⇒ **PREPARE 之后的读回是最干净的观察点**
  （此时一帧 PCM 都还没进）。

⇒ **0271 落库**（链树 `7a5333a`，补丁 `0271-sweet-mfc-format-timeline-prepare-point.patch`，
marker `mfcget v3 `）：在 `g_apm_graph_start()` 里 **PREPARE 返回之后、START 之前**加一次读回，
仍只在 `sweetmfcget=2` 触发。于是 J1（不发 set）实际打印**三行**：
`pre-set`（cfg 阶段）→ `post-prepare` → `post-start`。
`ci_replay`：`applied=212 / skipped=49`，`OK: post-image == chain-tree HEAD`。
**编号提醒**：0271 已用于音频 ⇒ GPU PR 的重编号目标再顺延为 **0272/0273/0274**。

**替他补上他要的那个数：AW 的真实 module id 我们手里有。**
他列"最想补齐 AW 的 module ID"——我们第 11 节那次走链已经读出来了：
`g_graph_vp_wr` 的模块表里 **iid 0x42d3 → module_id 0x10013D01**，
正是他自己给的 `AFE_MODULE_ID_AWDSP_RX`（配对 TX `0x10013D00`），与其余 `0x070010xx` 前缀不同。
⇒ 下一问应该问**这个 AFE 命名系的模块**要不要显式格式参数、以及它的
`0x10013d2a` 是什么族的参数（我们照厂商发了两封，正文含 25387 / 28512）。

**他两处我们仍标"外部前提、不当我们证据"的**：`SAL 支持 deinterleaved unpacked` 等能力清单、
以及 `0x080011BE` 可省（"引擎会从连接推导"）——都来自公开源码版本，**UKEE 固件可能不同**；
读回三行才是我们的读数。另外他提醒 GPR 头有 header-size 字段（我们用固定 24），
若某次应答长度异常，`mfcget` 行的 `len=` 会先暴露它，这条已经写进判据。
