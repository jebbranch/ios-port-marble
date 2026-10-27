# marble 的扬声器到底接在哪（从 DTB 读出来的）

`refs/marble/marble-fdt.dtb` 解析结果，全部是 `status = "ok"` 的节点：

```
/soc/i2c@880000/aw882xx_smartpa@34   compatible = awinic,aw882xx_smartpa   reg = 0x34
/soc/i2c@880000/aw882xx_smartpa@35   compatible = awinic,aw882xx_smartpa   reg = 0x35
/soc/qcom,msm-dai-tdm-tert-rx        compatible = qcom,msm-dai-tdm
/soc/qcom,msm-dai-tdm-tert-tx        compatible = qcom,msm-dai-tdm
/soc/spf_core_platform/tert_tdm_pinctrl     status = ok
/soc/spf_core_platform/tert_mi2s_pinctrl    status = disabled
```

对照**全部 disabled** 的 WSA 路径：

```
lpass-cdc/wsa-macro@3240000    status = disabled   (wsa883x SpkrLeft/Right)
lpass-cdc/wsa2-macro@31E0000   status = disabled
wsa_spkr_en1/2_pinctrl         status = disabled
```

⇒ **这台机器没有 WSA。扬声器 = TERTIARY TDM 上的两颗 AW882xx（I2C 0x34/0x35）。**

和 Android 侧观察一致：内核 PCM 列表里 `MI2S/TDM-...-WSA-*` 全是 `msm-stub`
（假口），唯一真实的 codec DMA 是 `TDM-LPAIF-RX-TERTIARY`（`multicodec-31`）。

## AP 侧能看到的 LPASS 寄存器

| 节点 | 地址 |
|---|---|
| lpass-cdc rx-macro | `0x03200000` |
| lpass-cdc tx-macro | `0x03220000` |
| lpass-cdc wsa-macro | `0x03240000`（disabled） |
| lpass-cdc va-macro | `0x033F0000` |
| lpass_tbu | `0x151e9000`（0x1000）+ `0x151ce230`（8） |

⚠️ 这些是**模拟前端（codec macro）**，不是数据 DMA。TDM/LPAIF 的
codec-DMA 在 ADSP 的 AudioReach 里，由 DSP 固件驱动。

## 所以「AP 直驱 TDM」并不省事

要在不经过 DSP 的情况下把 PCM 送到 TDT，必须自己驱动 **LPASS 的
codec-DMA / LPAIF**——那块寄存器不在 AP 的 DT 里，而且平时由 ADSP 固件
持有。可行的"直驱"实际上仍然是让 DSP 搬数据，只是不用 AudioReach 图，
那还是绕不开 DSP 读主存（也就是同一个 SMMU 问题）。

结论：sheng 的做法是 **SMMU pass-through**，而 marble 上 `S2CR.BYPASS`
被改写成 FAULT（run71/73）——那一半拿不到，AP 直驱那一半也不成立。
