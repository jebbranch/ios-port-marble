# 0298 PRM专用回包绑定：离线判据

## 原因

第73枪的`0x204` request wire bytes、厂商顺序和身份均正确，但0297复用了APM控制
应答形状，只接受：

```text
opcode 0x02001005
payload {原命令opcode, status}
```

项目自己的第64枪全量RX已经证明PRM时钟请求的实际应答为：

```text
opcode 0x02001002
src=2 dst=7
payload {PARAM_ID_RSC_AUDIO_HW_CLK=0x0800102c, status}
```

因此第73枪5秒timeout不是PRM接受或拒绝的证据，而是mailbox类型错误。

## 0298单变量

把控制mailbox拆成“期望response opcode”和“期望payload key”两个字段：

- APM graph/open/prepare/start/stop/close/unmap仍严格绑定
  `0x02001005 + 原命令opcode`。
- PRM TDM request/release严格绑定
  `0x02001002 + 0x0800102c`。
- 两类仍同时要求完整包长、GPR domain、远端src、本地dst和完整32位token匹配。
- DATA_BUFFER_DONE继续走独立period owner，不受影响。

request/release正文、token、顺序、时钟参数、图、PCM、功放状态机、峰值和等待均不变。
新增v2 marker：

```text
SWEET MARK: listen tdmclk v2 request rc=%d
SWEET MARK: listen tdmclk v2 release rc=%d
```

## 离线结果

- 实际adapter 56项通过，新增错误应答opcode、错误param id均必须timeout。
- 正确PRM专用应答可继续图与listen路径。
- request status非0仍拒绝图/功放；release status非0仍不UNMAP。
- 生产period核心65536个S16值、136656断言、ASan/UBSan、`-Werror`通过。
- AW88266实际状态机与44项表10场景通过。
- 0298单文件apply/check/post-image严格相等。
- GLINK全链applied239，历史skip49不增加，post-image等于链树HEAD。

证据：`verify/hallisten-0298/`。当前仍是离线验证，预算73/120；完整构建与最终镜像
冻结前禁止第74枪。

## 第74枪逻辑判据（待完整构建后补身份）

只允许0298新镜像一次RAM-only启动。主判据：

1. PRM v2 request必须取得status0；非0或timeout时仍不OPEN、不碰功放。
2. OPEN/PREPARE/START全0后，HAL非静音才调用功放。
3. 两颗AW均需PLLS+CLKS locked；任一失败不解除静音、不发period。
4. 成功门下最多30 period，峰值上限16384、不提高`0x05a9`。
5. 功放先双关断，STOP/CLOSE均0后才PRM release；release0后才UNMAP。
6. 人类听感单独记录；未进入功放或period路径时无声不判电声链。
