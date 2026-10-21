# 主动播报没有声音：排查手册

适用场景：后台任务完成后，服务端（xiaozhi-esp32-server-golang）通过 `speak_request` 让设备播报结果，但手表没出声。

## 链路

```
agent-bridge 完成 → 服务端等设备空闲 → speak_request（MQTT）
→ 设备 hello + 建 UDP → 发 2 个只有包头的 UDP 打洞包 → speak_ready
→ 服务端 tts start → TTS 音频（UDP）→ tts stop → goodbye → 设备回 Idle
→ 服务端确认播完才向 bridge ack
```

没声音一定是其中某一段断了。按下面顺序从服务端查到设备端。

## 1. 服务端日志

日志在 `/tmp/xz-run/server.log`（本机调试环境）。先找到这次任务：

```bash
grep -n "task=<task_id>" /tmp/xz-run/server.log
```

再看这段时间的关键行：

```bash
grep -nE "speak_request|speak_ready|首帧解码|音频被丢弃|TTS资源失败|千问流式请求失败|announce|interrupted|SendTtsStop" /tmp/xz-run/server.log | tail -40
```

| 现象 | 含义 | 已知原因 / 处理 |
|---|---|---|
| 发了 `speak_request`，一直没有 `speak_ready`，5 秒超时 | 设备没收到 MQTT 消息 | 设备快速重连时旧连接把新连接的订阅退掉了。已在 `device_hook.go` 的 `OnDisconnect` 修复。若再出现，看日志里是否有"已被同ID新连接接管"或"已有新连接，跳过" |
| `UDP远端地址未建立，TTS音频被丢弃` | 服务端不知道设备的 UDP 地址 | 设备没发打洞包。检查固件 `HandleSpeakRequest` 是否发了空包，`SendAudio` 对空 payload 是否跳过加密 |
| `不支持的TTS提供者:`（空） | 设备配置没拉到 | 面板里 VAD/ASR/LLM/TTS 必须各有一条默认配置，设备要绑定智能体。用 `/api/configs?device_id=` 确认返回 200 |
| `发送千问流式请求失败 ... deadline exceeded` | TTS 云服务超时 | 网络或 DashScope 问题。服务端会判为没播成，不 ack，稍后重试 |
| 有 `首帧解码完成`、无丢弃告警、`announced` | 服务端这一侧全部正常 | 问题在设备端，看第 2 节 |

`announced` 之后结果已经 ack，bridge 上删掉了，无法补播。`interrupted` 或 `announce failed` 表示结果还在队列里，会重试。

## 2. 设备端

### 注意：开串口会复位设备

用 `idf.py monitor` 或普通串口工具打开 USB 串口时，DTR/RTS 会让 ESP32-S3 复位。复位会改变要测的条件（例如 15:34 那次没声音时设备已运行约 20 分钟，而开串口后的两次都是刚启动、都有声音）。

所以优先在问题出现**之前**就挂着串口，或者用不拉 DTR/RTS 的方式打开串口。

### 看什么

```
Application: Speak request, session_id: ...       收到请求
MQTT: Session ID: ...                              hello 完成
StateMachine: State: idle -> speaking              收到 tts start
Application: << <字幕>                              收到 sentence_start
StateMachine: State: speaking -> idle              收到 tts stop
MQTT: Received goodbye message                     服务端关通道
```

- 停在 `Speak request` 之后：`OpenAudioChannel` 失败，看前后是否有 MQTT 或 UDP 报错。
- 有 `speaking`、有字幕，但没声音、也没有解密或序列号告警：音频包是否到达、是否解码、是否写进扬声器，目前固件**没有日志能证明**。这正是 15:34 那次的情况，原因未查明。
- 先排除最简单的：音量是否太小、手表是否静音。

## 3. 未解决：服务端正常、设备有字幕但没声音

已观察到的对比：

| 时间 | 串口 | 播报前设备状态 | 声音 |
|---|---|---|---|
| 14:56 | 连着 | 刚复位启动 | 有 |
| 15:34 | 没连 | 已运行约 20 分钟，只是重连了 MQTT | 无 |
| 15:41 | 连着 | 刚复位启动 | 有 |

两个待验证的假设：

1. 和 USB / 串口供电或省电状态有关。
2. 设备长时间待机后，音频输出或解码器处于某种状态，第一次主动播报没能唤醒。

## 4. 需要时再加的诊断（尚未实现）

如果再次遇到第 3 节的情况，计划在固件里加一组播放统计，放在 debug 开关后面，正式固件不编译：

- Speaking 期间统计：收到的 UDP 音频包数、解密失败数、解码出的帧数、写入扬声器的帧数、输出是否使能。
- Speaking 结束时作为一条 debug JSON 经 MQTT 发给服务端，服务端写进日志。这样不连串口也能看到设备端情况。

判断方式：

| 统计结果 | 结论 |
|---|---|
| 收到 0 个包 | 网络 / UDP 地址问题 |
| 收到了包，解码 0 帧 | 解密或解码问题 |
| 写入扬声器帧数正常，仍无声 | 输出被关闭或硬件问题 |

建议的对照测试：刷入 debug 固件后断开串口，设备待机 2 到 3 分钟，再提交任务，听有没有声音并对照统计。
