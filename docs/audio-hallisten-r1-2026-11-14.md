# 第71枪免费重放 R1：事前身份（2026-11-14）

用户明确说明上一轮因睡着未能听测，要求再启动一次且不计预算。
因此本次记为第71枪的免费重放R1，扣除0，预算保持71/120。

上一轮没有取得用户听感，也尚未在TWRP拉取ring，听感记为**未知**，
不得改写为“没听到”。设备当前直接在目标机fastboot，不能在重放前取ring；
R1完成后必须回TWRP拉取，以R1唯一sweettime判日志。

固定策略：

- 同一个0295冻结镜像、同一kernel/ADT/ADSP/功放状态机/PCM ceiling/cap；
- 只原位替换唯一10位`sweettime`，不重建ADT，不重新编译；
- 对比必须证明变化仅落在sweettime的十个数字范围；
- 只fastboot boot一次，禁flash，扣除0；
- 不调高音量、不连续触发；约0.64秒，用户明确反馈听到/没听到/不确定；
- 回TWRP后拉ring并核功放/DSP/双关断门，不追加第二次免费重放。

R1身份：

- source image SHA：`693ee73e672f502cbba24b398227ad8a2f16ebbcb442f0d3d45361f4c077cbd9`
- sweettime：`1794620381`
- R1 image SHA：`530b750fd457ebcfedbec9b214d09bea8bc84e4ce10e70cd3614d8ee77c0324c`；
- 预算：71/120，本轮扣除0。

原镜像与R1只有sweettime十位字段中的6个实际变化字节，偏移均位于
`11637392..11637397`；总大小、590B cmdline和其他字节相同。
当前尚未启动。
