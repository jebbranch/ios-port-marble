# 第71枪免费重放 R1：无声但未触发（2026-11-14）

用户明确反馈“没有声音”，设备已回TWRP。该人类观测如实保留；
R1扣除0，预算仍71/120。

当前ring身份成立：

- sweettime=1794620381；
- total=140574、seq=181，raw/decoded窗口匹配；
- guard/open/prepare/start成立，AW DSP cal两项status0；
- 功放I2C bypass只出现在ADSP启动前的旧probe位置。

但决定性时序是：

1. `halstream ready ... wait-ms=120000`；
2. 120秒内没有非静音HAL帧；
3. `no non-silent HAL frames before deadline`；
4. end为attempted/sent/done=0/0/0；
5. stop/close/unmap全部0，安全teardown结束；
6. **之后**才有volume generation及PCM write，第三块开始非静音。

因此功放pre_play从未被调用：没有safe-profile、PLL、healthy-muted、unmuted、
listen started/stopped或normal-stop powerdown行；也没有发送任何PCM period。
这轮“没听到”不是对功放/电声链的有效负读数，只证明操作晚于安全等待窗口。

安全性成立：超时前功放从未启动，图和映射正常关闭，无panic。不能把没有listen
marker误读成状态机丢日志，因为它前面的流明确记录attempted=0。

下一步只延长“功放仍关闭时等待HAL输入”的deadline到300秒；period数量、PCM上限、
功放状态机、I2C/ACF、图及所有听测安全门不变。必须产生新marker或可区分构建，
完整编译/镜像冻结后再登记；不直接重放旧镜像。
