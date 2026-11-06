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
