# 0294：开机前恢复已验证的 S1 权限基线

0293已发布raw资产SHA为
fb0644b585d29e7e2461730adb1091f36d9e6897f86f70e553ed515c2a1d5509。
开机前对照第68枪已冻结镜像/设备结果及START-HERE发现：
0293新guard把sweets1ap列入必须为1的参数；而已验证当前数据面配置是
`sweets1ap=3`，S1 leaf AP=11。此前“1即可”的历史总结不能替代该实际配置。

因此不引导0293。新0294只改guard，将sweets1ap单独要求3，
marker升级为 `SWEET MARK: halstream guard v2 pass=%d s1ap=%d`，
workflow同步更新。不改SMMU配置实现、不新增权限实验、不改图/PCM/功放/增益。
新35项实际adapter主机测试含1/0拒绝、3通过；已有核心全S16测试不变。

第70枪候选其余逻辑判据不变，只采用v2 guard/已验证s1ap3。

## 0294 完整产物与镜像身份

GitHub release新raw配对SHA：
`53923310ce90c6b5b90bc778512644a838ef9c11a9a87f1e2e214e5305c8b02e`。
CNTP唯一`0x4aa348` NOP后kernel SHA：
`4899c24cd04ee8d7f9067f646b4d01eac364314a200304f59c4a73984ce8897b`。

最终候选镜像：
`images/halstream-0294-20261113.img`，51302400 B，
SHA=`931181d6b3e0b327b2a190f4efad9b693e77fbafcce6bfdc98b89d5f774fe19f`，
sweettime=`1794581323`。

实际cmdline=576B、无重复key，8核KEEPALIVE。`sweetbatt=0`在基线位置，
无sweetreboot。关键参数：

```text
sweethalbridge=1 sweethalstream=1 sweetpcm=1 sweetdescsep=1
sweets1=1 sweets1ap=3 sweetiova=1 sweets1va=0x1FFE8000 sweetshmhi=1
sweettdmvendor=7 sweeti2c=0 sweetamp=0 sweetgpu=0 sweetbatt=0
```

`sweetshmflag`故意省略，代码及guard默认均为4，以满足590B预算；不以重复key覆盖。
84个workflow累计marker、新7个stream marker、14个bridge/stream/period符号、
所有kernel segment、ADSP blob、boot v4包体、8个MPIDR逐项通过。
GPU shim probe level=0。默认旧kernel及capture镜像没有覆盖。

本地`build_halstream_image.py --verify-only`必须在开机前再次通过。

## 第70枪冻结判据

设备当前离线，尚未启动，预算仍69/120。设备回来后只允许一次：

1. 核serial=2aa26095、镜像SHA和verify-only；只能fastboot boot，禁flash。
2. 有效ring唯一sweettime=1794581323，raw窗口/decoded长度成立。
3. `halstream guard v2 pass=1 s1ap=3`，amp-bypass和awcfg-bypass各出现；
   禁旧tone、ampwrite/ampguard、GPU/GMU及fallback数据路径。
4. open/prepare/start的v1控制reply均status0，graph bytes=856/off=0x4000。
5. 必须有非静音HAL输入和8条以内raw witness；离线核其是否合理S16。
6. end必须`result=0 attempted=30 sent=30 done=30 reject=0 fault=0`，
   `source>0`且`0<peak-out<=1024`。
7. stop/close/unmap控制reply均status0，teardown open=1 stop=0 close=0 unmap=0；
   无panic。任何缺项即停，不追加第二枪。
8. 本版功放绕过、不会出声；人耳不是判据。用户若暂时离席且需要手动长按，
   停在运行状态等待用户，不擅自用别的设备或额外引导。

## 第70枪判决：通过（70/120）

用户完成操作并自行返回TWRP；只执行一次冻结镜像。当前ring：
sweettime=1794581323、total=142193、seq=167，raw窗口与decoded逐字节匹配，
与开机前旧ring（sweettime=1794493381、seq=252）明确不同。

所有冻结门通过：

- guard v2 pass=1 s1ap=3；
- amp-bypass/awcfg-bypass各1，ampwrite/ampguard/旧tone/GPU/GMU均0；
- graph bytes=856 off=0x4000；
- open/prepare/start/stop/close/unmap六项均匹配status=0；
- end为`result=0 attempted=30 sent=30 done=30 reject=0 fault=0`；
- source=30720、silence=0、peak-in=700、peak-out=21；
- teardown open=1 stop=0 close=0 unmap=0，无kernel panic。

原始witness从首次非静音附近frame729开始，共512B/128个双声道S16帧。
左右峰值432/489，256个样本全非零，128帧中121帧左右同号；
它不是单声道复制，也不像float字节误读，幅值不超过整轮peak-in=700。
这足以支持“HAL送来合理的小幅双声道S16片段”，仍不是音乐内容鉴定。

独立评分器11门全部通过；5项正负对照能拒绝旧身份、text/raw不一致、
done=29和ampwrite污染。原始证据SHA见`verify/halstream-boot70/`。

结论范围：

**已证明iOS HAL真实PCM经过固定衰减，以四个共享period连续送给AudioReach；
DSP严格归还30个buffer，且图和映射安全停止。**

没有证明：功放开启、PLL锁定、扬声器振膜输出、人耳听到声音或30块以后长期稳定。
下一阶段才允许进入本机AW88266（ID 0x2032）短时功放状态机与听测设计；
不得把mayfly AW88261 profile、正增益或听筒配置套进来。
