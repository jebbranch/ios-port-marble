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
