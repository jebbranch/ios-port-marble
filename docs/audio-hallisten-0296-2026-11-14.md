# 0296 第72枪：300秒等待听测镜像

R1已证明旧0295在120秒内没有非静音HAL输入，功放未启动、period=0；
用户没听到不是电声负读数。0296仅延长功放关闭时的等待窗口到300秒。

完整身份：

- raw SHA：`99e47ec410b078323661e37f29c9d66896476966b7d19d54e784ea1737f5208d`
- patched kernel SHA：`5d659e984f43da91f22b5230fea113dadef378823e51e95404eaa292ee524dec`
- image：`images/hallisten-0296-20261114.img`
- image SHA：`9d7fcf7ace11936386c7437259d26e0e9f2a5c9d1229505f01c5491d10ba6843`
- sweettime：`1794624169`
- cmdline：590B、无重复key、8核KEEPALIVE。

88个workflow marker、17个bridge/stream/period/listen符号、CNTP唯一NOP、
所有kernel段/ADT/boot包通过。与0295运行逻辑的唯一差异是
wait-ms=120000→300000及循环12000→30000。

第72枪只准一次fastboot boot，预算72/120。进入iOS后五分钟内尽快触发一次声音；
不提高音量、不重复。拿到非静音PCM前功放保持关闭；之后最多30 period约0.64秒。
用户听感须明确听到/没听到/不确定，回TWRP后按0295完整日志门判决。

## 第72枪判决：有效无声，失败点在TDM时钟门

用户明确反馈“没有”。本轮身份与窗口成立：

- `sweettime=1794624169`
- raw `total=145822`、`seq=174`
- raw窗口与`logs/ring-hallisten-boot72.txt`逐字节长度一致
- image SHA仍为`9d7fcf7ace11936386c7437259d26e0e9f2a5c9d1229505f01c5491d10ba6843`

非静音HAL输入在300秒内及时到达：第3次write时已有3072帧、701个非静音帧，
峰值17372。GPIO52明确为0，只选speaker，从未切handset。两颗AW88266均读回
ID `0x2032`、安全控制值`0xf543`和原Music表音量寄存器`0x05a9`。

第一颗功放清PWDN后的`SYSST=0`，因此PLLS=0、CLKS=0。安全门随即拒绝pre-play，
`attempted/sent/done=0/0/0`；两颗功放均重新读回`ctrl=0xf543`，没有解除静音，
也没有向DSP提交任何听测period。

判决边界：

- “没有声音”是有效的人类负读数。
- 它不是PCM过小、HAL未触发或Music表未写入。
- 它证明当前图START成功仍没有让AW88266看到TDM bit clock。
- 它不测试功放输出级或扬声器振膜，因为硬件在PCM提交前已被安全关闭。

下一单变量是厂商精确的PRM硬件时钟请求：
`clock=0x204, rate=12288000, attr=4, root=0`。必须在图OPEN前请求，图
STOP/CLOSE后成对释放；request失败时不得启功放或发PCM。禁止用提高音量绕过。

归档：`verify/hallisten-boot72/result.json`、`hearing.json`和`SHA256SUMS`。
