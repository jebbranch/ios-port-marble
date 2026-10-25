# kprobe 抓取厂商 GPR 报文

前置：手机在 Android、已 root（SukiKernelSU）。

```sh
# 24 字探针（覆盖 96 字节包）
FIELDS=""; for i in $(seq 0 23); do FIELDS="$FIELDS p$i=+$((i*4))(%x1):u32"; done
adb shell su -c "cd /sys/kernel/tracing && echo 0 > events/enable && echo > kprobe_events && \
  echo \"p:gprpkt gpr_send_pkt a0=%x0 a1=%x1$FIELDS\" >> kprobe_events"

# 触发真实播放：打开系统音效后点击/音量键
adb shell su -c 'settings put system sound_effects_enabled 1'
adb shell input keyevent 224
adb shell su -c 'wm dismiss-keyguard'
adb shell su -c 'cd /sys/kernel/tracing && echo > trace && echo 1 > events/kprobes/enable && echo 1 > tracing_on'
adb shell input tap 540 1200
adb shell su -c 'echo 0 > tracing_on; echo 0 > events/kprobes/enable'
adb shell su -c 'cat /sys/kernel/tracing/trace' > trace_ui.txt
```

解析：每行 `p0..p23` 是 GPR 包的前 96 字节。
- `p5` = opcode（0x01001006 = SET_CFG，0x01001000 = GRAPH_OPEN，
  0x0400100A = DATA_BUFFER_V2）
- SET_CFG：`p10`=iid，`p11`=param_id，`p12`=param_size，`p14..`=数据

`trace_ui.txt` 里包含厂商的：
- `0x08001017` 媒体格式：96000/32/2/data_format=1
- `0x0800101B` 接口配置：28 字节 `{0,2,0x00010001,3,0x00200004,0,1}`
- `0x0800100C` SAL PCM 格式：48000/24-bit/2ch
