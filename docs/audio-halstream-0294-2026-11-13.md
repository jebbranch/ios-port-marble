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
用户当前设备在Android，尚未重启或引导。预算仍69/120。
必须取得0294完整新产物并冻结镜像身份后才测试，不因用户离席绕过产物门。
