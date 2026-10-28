# SPEC：Eyes UI 动画组件接入小智 ESP32（StopWatch / LVGL）

- **文档 ID**：SPEC-EYES-XIAOZHI-001
- **版本**：v1.0（可执行实现规范）
- **日期**：2026-10-20
- **状态**：Ready for implementation；尚未完成 ESP-IDF 真机构建与集成验证
- **目标平台**：M5Stack StopWatch（ESP32-S3 / 466×466 圆形 AMOLED）
- **软件基线**：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 当前 LVGL 显示架构（按实际 checkout 版本适配），LVGL `~9.5.0`、`esp_lvgl_port ~2.9.0`（查看 `main/idf_component.yml`）
- **输入资产**：现有 Eyes Lab 工程 `firmware/eyes_demo/eyes_demo.ino`、`firmware/eyes_demo/presets.h`、`presets.json`
- **预期产物**：可独立编译的 ESP-IDF `eyes_ui` 组件；小智 StopWatch 集成 patch；验收记录；不改变既有音频/网络链路

## 1. 背景与目标

已有 Arduino + M5Unified/M5GFX 的“两只眼睛”动画 Demo，在设备上完成流畅度体验验证。当前程序把表情状态、Spring 动画、眨眼/视线、显示坐标变换和 `M5.Display` 绘制混合在 `.ino` 中，无法直接并入小智的 LVGL UI。

**目标**：在不替换小智固件的显示驱动、LVGL 刷新任务、ASR/LLM/TTS 协议和音频管线的前提下，把 Eyes Lab 做成一个可挂载到任意 `lv_obj_t*` 父容器上的 `EyesWidget`，实现十种预设表情、连续过渡、自动眨眼、随机视线及根据小智工作状态切换动画行为。

### 1.1 MVP 范围（必须完成）

1. 纯 C++ `EyeAnimator`：表达式参数、状态更新、Spring、blink、gaze、时间步管理，不依赖 Arduino/M5GFX/LVGL。
2. `EyesWidget` 的 LVGL 9.5 渲染适配：利用已有 `esp_lvgl_port`、绘制事件和刷新流程；不独立驱动 LCD。
3. StopWatch `RoundLcdDisplay` 挂载：替换中间默认 emoji，仅保留并兼容顶部状态栏、底部字幕、通知及交互逻辑。
4. `SetEmotion` 映射小智表情；`Activity` 映射 Idle / Listening / Speaking / Connecting；Thinking 仅在有明确语义事件时触发。
5. 目标 30 FPS（可配置）；离屏/静止时省电降频；硬件性能不达标时可回退 20 FPS。
6. StopWatch 真实设备编译、刷机、对话回归及连续运行测试。

### 1.2 非目标（本期不做）

- 不接入 groki-bot DSL/VM，不支持 JSON 动态界面协议。
- 不重写 LCD、触摸、Wi-Fi、WebSocket、麦克风、音频编码/解码等已有底层。
- 不实现图片、菜单、圆环进度等新的通用组件（留给 UI Kit 后续迭代）。
- 不强制同时支持 CoreS3；设计需保留跨分辨率能力，但首个验收设备只选 StopWatch。
- 不把 Speaking 动画与 PCM 音量精确同步；留独立扩展接口。

## 2. 仓库事实与集成锚点

核对的源码（以实际检出的版本为准）：

| 功能 | 文件 / 现状 | 我们的操作 |
|---|---|---|
| StopWatch 板级入口 | [`main/boards/m5stack/stopwatch/m5stack_stopwatch.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/boards/m5stack/stopwatch/m5stack_stopwatch.cc)：`RoundLcdDisplay : SpiLcdDisplay`，有 `SetupUI()`，并进行圆屏安全边距调整 | 在该类中挂载 EyesWidget，保留已有布局和 `rounder_event_cb` |
| LVGL 界面根对象 | [`main/display/lcd_display.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/display/lcd_display.cc)：`LcdDisplay::SetupUI()` 创建 `emoji_box_`、`emoji_label_`、`emoji_image_`、status、subtitle | 优先将 EyesWidget 作为 `emoji_box_` 子对象，不删除其它 UI |
| 情绪渲染 | `LcdDisplay::SetEmotion(const char*)`：根据主题表情资源展示文本/图片/GIF | 在板级子类覆写 `SetEmotion`，不调用会重新显露 emoji 的默认实现 |
| 设备状态 | [`main/application.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/application.cc) `Application::HandleStateChangedEvent()` | 增加非破坏性的 Activity 通知 |
| Display 可扩展 API | [`main/display/display.h`](https://github.com/78/xiaozhi-esp32/blob/main/main/display/display.h) `Display` 抽象基类 | 新增默认 no-op 的 `SetActivity()`（建议） |
| LVGL 锁 | `LcdDisplay::Lock()` / `Unlock()` 内部封装 `lvgl_port_lock()` / `lvgl_port_unlock()` | 从应用任务更新时使用同一把锁；LVGL timer 内不要跨任务访问 UI |
| 依赖版本 | `main/idf_component.yml` 配置 LVGL 9.5 与 esp_lvgl_port | 组件使用项目现成依赖，不引入第二套 LVGL |

**重要事实**：小智的 `kDeviceStateListening` 会调用 `SetEmotion("neutral")`；`kDeviceStateSpeaking` 主要调用 `SetStatus("SPEAKING")`，不能指望 `SetEmotion()` 携带所有活动状态。因此 `Emotion` 与 `Activity` 必须正交。

## 3. 目标架构

```text
小智 Application / WebSocket / 音频状态
        | SetEmotion       | SetActivity
        v                  v
     RoundLcdDisplay (板级适配，拥有 EyesWidget)
                    |
           LVGL UI 任务 / 锁
                    |
        EyesWidget (LVGL 自定义控件)
        |     ↑ lv_timer / DRAW_MAIN
        |     |
        v     |
      EyeAnimator (纯 C++ 数学计算)
      + PresetData (10 种 EyePose)
                    |
           LVGL draw pipeline
                    |
          esp_lvgl_port / esp_lcd
                    |
         StopWatch CO5300 AMOLED
```

**三条边界**：

1. `EyeAnimator` 只读写姿态数值；不 `#include <Arduino.h>`、`<M5Unified.h>`、`<lvgl.h>`，不调用 `M5.Display`。
2. `EyesWidget` 只在 LVGL 的绘图层提交几何图元和无状态的绘制描述；**不**创建新 `lv_display_t`、`esp_lcd_panel_*`、渲染 FreeRTOS 任务或第二个 LVGL tick。
3. Board/Application 只调用公开的 UI API；不直接操作眼睛的几何点、Timer、Draw Descriptor。

### 3.1 文件布局

```text
xiaozhi-esp32/
├── components/
│   └── eyes_ui/
│       ├── CMakeLists.txt
│       ├── include/
│       │   ├── eyes_ui/eye_types.h
│       │   ├── eyes_ui/eye_animator.h
│       │   └── eyes_ui/eyes_widget.h
│       ├── eye_animator.cpp
│       ├── eye_presets.cpp
│       ├── eyes_widget_lvgl.cpp
│       └── Kconfig                      # 可选：默认开启/帧率/自动眨眼
├── main/
│   ├── display/display.h               # 可选的通用 Activity 扩展
│   ├── application.cc                  # Activity 状态通知
│   └── boards/m5stack/stopwatch/
│       └── m5stack_stopwatch.cc        # 挂载点与情绪适配
└── tests/                              # 纯数学单测或 host 测试
```

如后续需要完全跨框架复用，再把一个组件拆成 `eyes_core` 与 `eyes_lvgl` 两个组件；**首版不要求过度拆包**。

## 4. 公开接口（建议约定）

以下为目标 API 设计，不是已经实现的代码。

```cpp
// eyes_ui/eye_types.h
namespace eyes {
enum class Emotion : uint8_t {
    Idle, Happy, Excited, Thinking, Angry,
    Sad, Surprised, Confused, Sleepy, Wink
};

enum class Activity : uint8_t {
    Idle, Connecting, Listening, Speaking, Thinking
};

struct EyePose {
    float x, y, w, h, angle, lid, tilt, curve;
};
struct EyeFrame {
    EyePose left;
    EyePose right;
    float gaze_x, gaze_y;
    float blink;
};
}

// eyes_ui/eye_animator.h  (纯 C++)
class EyeAnimator {
public:
    void Reset(uint32_t now_ms);
    void SetEmotion(eyes::Emotion emotion);
    void SetActivity(eyes::Activity activity);
    void SetGaze(float normalized_x, float normalized_y);
    void TriggerBlink(uint32_t now_ms);
    void Update(uint32_t now_ms);
    const eyes::EyeFrame& Frame() const;
};

// eyes_ui/eyes_widget.h (LVGL wrapper; 仅在 LVGL 任务/锁内调用)
class EyesWidget {
public:
    bool Create(lv_obj_t* parent);
    void SetEmotion(eyes::Emotion emotion);
    void SetActivity(eyes::Activity activity);
    void SetVisible(bool visible);
    void SetFrameRate(uint8_t fps);
    void Destroy();
};
```

- `EyeAnimator` 负责生成“当前帧”，**不持有屏幕坐标和显示句柄**。
- `EyesWidget` 基于 widget 的大小将 466×466 逻辑基准（当前 `.ino` 的参考尺寸）转换为本地坐标；保持长宽比、居中，防止圆屏外切。
- 公开 `SetEmotion()` / `SetActivity()` 不直接画屏，只更新目标状态；下一次 LVGL timer 决定更新。
- 销毁流程要取消 `lv_timer_t`，释放 C++ 实例并解绑事件，防止切页后悬空指针。禁止在 `LV_EVENT_DRAW_MAIN` 中创建、删除或改变其它 LVGL 对象。

### 4.1 动画核心迁移规则

从 `.ino` 迁移：

| 当前函数或数据 | 迁移目标 | 要求 |
|---|---|---|
| `EyePose`、`kPresets` | `eye_types.h` / `eye_presets.cpp` | 10 种预设数值与原 Demo 一致，`presets.json` 继续作为单一数据源 |
| `springEye()` | `EyeAnimator::Update()` | 沿用当前二阶 Spring 参数，固定/限制最大时间步；不可依赖 M5 库 |
| `triggerBlink()`、`blinkFactor()` | `EyeAnimator` | 保持自动眨眼随机间隔；支持手动触发 |
| gaze target 与平滑 | `EyeAnimator` | 默认随机扫视；预留外部视线优先级 |
| `makeFrame()` | core 输出 + LVGL 本地坐标变换 | 将 M5.Display 宽高查询移出 core；保留 bob/sine 微动 |
| `drawEye()`、`capsule()` | `eyes_widget_lvgl.cpp` | 与 `.ino` 类似的胶囊 / 弯曲微胶囊 / 眼皮遮挡外观 |
| `drawFrame()` | LVGL draw callback + `lv_obj_invalidate()` | 不调用 `M5.Display` 和 `fillScreen` |
| `setup()` / `loop()`、串口、按钮 | 不进入组件 | 由小智启动/事件生命周期接管 |

*注*：当前 `.ino` 的倾斜眼皮遮挡比浏览器 Demo 简化；本阶段验收以 **真机 `.ino` 的形态** 为首要对齐目标，不把浏览器未移植的效果算作 regression。

### 4.2 定时/动画稳定性

- 使用 LVGL `lv_timer_create()` 更新帧，每帧只更新数学状态与 `lv_obj_invalidate()`；真正绘图由 `LV_EVENT_DRAW_MAIN` 触发。
- 采用 LVGL tick / `esp_timer` 获取单调时间；处理 `uint32_t` 毫秒回绕和临界帧率波动。
- 对 `dt` 做限幅/子步进，避免 Wi-Fi 或 TTS 抢占 CPU 后 Spring 猛跳。
- `SetVisible(false)`、屏幕休眠时暂停或降频，恢复时重置时间基准，不能突然闪跳。
- Timer 回调所在的 LVGL 任务可以直接调用 LVGL API；来自其它任务的调用必须由主显示锁保护，或投递到 UI 队列。

## 5. LVGL Renderer 实现约束

### 5.1 首版渲染方案：自定义 LVGL Widget

1. `EyesWidget::Create(parent)` 创建 `lv_obj_t`，设透明背景、禁滚动、确定实际宽高；注册 `LV_EVENT_DRAW_MAIN`、`LV_EVENT_DELETE`（或采用等效确定性销毁管理）。
2. Draw callback 中通过 `lv_event_get_layer(e)` 获得当前图层，按 `EyeFrame` 构造胶囊、曲线、遮眼矩形等 LVGL draw task。
3. 先探索 `lv_draw_line` 的线宽及圆头作为胶囊图元，再按需要使用 `lv_draw_rect`、`lv_draw_arc`、可用的多边形/离屏子缓冲实现不规则遮挡。以 LVGL **9.5 的实际头文件/API** 为准，不照搬 v8 `lv_draw_ctx_t` 示例。
4. 动画帧仅 `lv_obj_invalidate(eyes_obj)`，由 LVGL 裁剪/脏区系统统一管理。禁止照搬 `.ino` 中对**LCD**执行的 `clearDirtyRect`，否则会擦掉状态栏/字幕/其它界面层。
5. 如果极端情况下需位图栅格化，最多分配 EyesWidget 局部缓冲，并评估 PSRAM/拷贝带宽；不在 MVP 默认申请一张 466×466 RGB565 全屏缓冲（单帧约 424 KiB）。

### 5.2 图层及布局

```text
Screen（小智已有）
├── container_（背景）
├── emoji_box_（中心）
│   ├── emoji_label_（隐藏）
│   ├── emoji_image_（隐藏）
│   └── EyesWidget（新建，透明背景，局部刷新）
├── top_bar_（Wi-Fi / 电量等，保留）
├── status_bar_（Listening 等，保留）
├── preview_image_（图片预览，保留）
└── bottom_bar_（字幕/消息，保留）
```

- `emoji_box_` 原来是 `LV_SIZE_CONTENT`；挂载后需要显式校验并设置 EyesWidget 的实际尺寸，避免父对象为零尺寸或裁剪。
- 圆屏外缘和顶部/底部字幕有安全区限制。建议默认 `EyesWidget` 使用中部约 75% 宽、45%～55% 高的区域，实际数值以截图回归调整；**不要覆盖或删除默认字幕**。
- `SetPreviewImage()`、主题变化及通知需继续工作；遇到临时大图可隐藏/降频 EyesWidget，结束后恢复。

## 6. 小智接入改动

### 6.1 `RoundLcdDisplay::SetupUI()`

在现有函数 **末尾保留全部原有圆屏边距和 invalidate-area 回调**，追加以下逻辑（伪代码，非直接可编译补丁）：

```cpp
SpiLcdDisplay::SetupUI();
DisplayLockGuard lock(this);
// ... 原有圆形屏布局逻辑保持不变 ...

lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
// 配置 emoji_box_ 的大小、位置与不裁剪等属性
// eyes_widget_.Create(emoji_box_);
// eyes_widget_.SetFrameRate(CONFIG_EYES_UI_FPS);
```

注意 `SetupUI()` 可能被调用两次，遵循小智现有幂等判断；`EyesWidget` 不能重复创建 Timer 或事件回调。外层已持有 `DisplayLockGuard` 时，不必再次从 `EyesWidget` 内申请显示锁。

### 6.2 `RoundLcdDisplay::SetEmotion(const char*)`

覆盖基类处理并将字符串映射为十种预设；保持未知情绪安全回退，不显露原始 emoji。

| 小智字符串 | EyeEmotion | 说明 |
|---|---|---|
| `neutral`, `robot_2` | Idle | 默认 |
| `happy` | Happy | 开心 |
| `laughing` | Excited | 兴奋 |
| `angry` | Angry | 生气 |
| `sad`, `crying` | Sad | 难过 |
| `sleepy` | Sleepy | 困倦 |
| `doubtful` | Confused | 疑惑 |
| 其它未识别值 | Idle / 保留原姿态 | 明确策略，限频记录日志 |

预设中 `Surprised / Thinking / Wink` 仍可由测试界面、设备活动状态或以后扩展事件触发。

**优先级规则**：Emotion 决定形状，Activity 决定动效强度与视线/节奏；Listening 时小智发来的 neutral **不覆盖** Listening 的活动层。后到的 Emotion 可覆盖形状，但不能重置 Activity。

### 6.3 `Application::HandleStateChangedEvent()` → Activity

推荐在小智 [`Display`](https://github.com/78/xiaozhi-esp32/blob/main/main/display/display.h) 增加兼容现有硬件的默认 no-op 方法：

```cpp
virtual void SetActivity(const char* activity) { (void)activity; }
```

板级 `RoundLcdDisplay` override，内部在 `DisplayLockGuard` 保护下调用 `EyesWidget` 的枚举 API。`HandleStateChangedEvent()` 在原逻辑之后补充（示意）：

| 小智设备状态 | 目标 Activity | 视觉表现 |
|---|---|---|
| `kDeviceStateIdle` | Idle | 常态微呼吸、随机眨眼 |
| `kDeviceStateConnecting` | Connecting | 低频左右探视，**不等于 LLM Thinking** |
| `kDeviceStateListening` | Listening | 集中注视、稍微睁大、降低随机扫视 |
| `kDeviceStateSpeaking` / `kDeviceStateNotifying` | Speaking | 较活跃的弹跳和眨眼（不伪称 PCM 同步） |
| 其它状态 | 保持/Idle | 明确处理恢复与休眠 |

`Thinking` 仅当未来接入明确的 **模型处理中 / 工具执行中** 事件时触发；不要直接把网络连接阶段当作思考。

**状态优先级建议**：`Speaking > Listening > Thinking > Connecting > Idle`；当系统状态切换时以小智状态机为准，避免长任务状态阻塞实时语音状态。

### 6.4 既有能力不回归

- `SetStatus`、`SetChatMessage`、`ShowNotification`、`SetPreviewImage` 不修改原行为。
- 不修改服务器 WebSocket 消息格式，不需要更换小智服务端。
- 不接管按钮/触摸中已有的配网、录音和控制逻辑；调试表情切换优先用本地开发模式或临时调试函数，并保证默认构建关闭。
- 停用 EyesUI（编译宏/Kconfig）时，原来的 `LcdDisplay::SetEmotion()` 及 emoji 显示行为可恢复。

## 7. 构建和配置

组件 CMake 参考：

```cmake
idf_component_register(
    SRCS
        "eye_animator.cpp"
        "eye_presets.cpp"
        "eyes_widget_lvgl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
)
```

板级源码应能解析 `eyes_ui/eyes_widget.h`。若实际 `main` 组件依赖不可见，按 IDF 版本给 `main/CMakeLists.txt` 增加 `REQUIRES eyes_ui` 或 `PRIV_REQUIRES eyes_ui`；不要凭空假设工程已自动链接所有组件。

建议配置项：

```text
CONFIG_EYES_UI_ENABLED=y
CONFIG_EYES_UI_FPS=30            # 可选 20/30/60，先测 30
CONFIG_EYES_UI_AUTO_BLINK=y
CONFIG_EYES_UI_AUTO_GAZE=y
CONFIG_EYES_UI_DEBUG_METRICS=n
```

以上为待新增 Kconfig 选项，**不是小智原有的 `sdkconfig` 项**。ESP-IDF 组件依赖现有 LVGL 包，不再安装 M5Unified/M5GFX。

构建流程：

```bash
# 在已配置好 ESP-IDF 的 shell 中
cd xiaozhi-esp32
idf.py menuconfig               # 选择官方 StopWatch 板型（按当前菜单位置）
idf.py build
idf.py -p <SERIAL_PORT> flash monitor
```

推荐先完整备份现有小智固件/NVS，确认 USB 端口与 Flash 布局；**刷机会覆盖当前运行的应用固件**。初次集成不主动修改分区表或 NVS 配对信息。

## 8. 验收标准（Definition of Done）

### 8.1 编译与功能

- [ ] 在用户当前小智源码提交/分支上通过 `idf.py build`，无组件符号冲突、LVGL 版本 API 冲突。
- [ ] 真机启动后替代中间默认 emoji；两只眼睛完整、位置居中、无残影/明显撕裂。
- [ ] 10 种预设通过本地调试入口依次展示，形态与现有 `.ino` Demo 对齐。
- [ ] 可连续触发自动眨眼、视线移动、Spring 表情转场；离屏后停止不必要刷新。
- [ ] 语音唤醒/聆听时视线集中；TTS 说话时动态活跃；返回 Idle 后恢复自然微动。
- [ ] 小智服务端发来的 `happy`、`sad`、`angry` 等情绪变化可驱动表情。
- [ ] 顶部电量/Wi-Fi、状态文本、字幕、通知、图片预览仍然可用且不被画面擦除。
- [ ] 网络断开/重连、配网、睡眠唤醒/切换页面无空指针、悬空 Timer、屏幕锁死。
- [ ] 通过关闭 Kconfig 开关恢复原始 emoji 方式，且无其他行为改变。

### 8.2 性能与稳定性

- [ ] 默认目标 30 FPS，记录真机 5 分钟平均/最差帧间隔；若无法达到，报告原因与 20 FPS 降级方案。
- [ ] 朗读/TTS 并行时连续测试 10 分钟，无音频卡顿、WDT、丢包显著恶化。
- [ ] 不额外申请整个 466×466 全屏 RGB565 帧缓冲（约 424 KiB）；记录组件初始化前后内部 RAM / PSRAM 差异。
- [ ] LVGL 更新只通过现有任务/锁；无其它任务直接写 LCD；无高频全屏 `fillScreen`。
- [ ] 记录 UI 动画实际占用的 CPU、内存与 draw 耗时，用于后续 CoreS3 适配。

### 8.3 测试用例（最小集）

| ID | 操作 | 预期 |
|---|---|---|
| EYES-01 | 上电待机 1 分钟 | 两眼平静，随机眨眼、慢速视线，无残影 |
| EYES-02 | 连续切换 10 种表情 | Spring 连续过渡，无眼睛瞬移或明显异常 |
| EYES-03 | 进入 Listening | 视线聚焦，底层 neutral 不重置活动特征 |
| EYES-04 | 接收情绪 happy 后播放 TTS | 开心表情 + Speaking 动效，字幕正常 |
| EYES-05 | TTS 停止并返回 Idle | 平滑回到平静状态，Timer 没有泄漏 |
| EYES-06 | Wi-Fi 掉线/重连 | 无花屏、崩溃或状态卡死 |
| EYES-07 | 启用通知/图片预览 | 显示层级正确，结束后眼睛正常恢复 |
| EYES-08 | 连续运行 10 分钟语音对话 | 无 watchdog、明显音频卡顿、不可恢复 UI 停顿 |
| EYES-09 | 关闭 EyesUI 功能开关 | 恢复原有 emoji 显示能力 |

## 9. 执行任务拆解与推荐顺序

### PR-1 / 提取纯 C++ 动画核心

- 从现有 `.ino` 提取 `EyePose`、`EyeFrame`、Spring、blink、gaze、时间计算。
- 不带入 Arduino 和 M5GFX；构建 Host 小测试，至少覆盖 10 种 Emotion 目标与有限值（非 NaN）。
- 用 `presets.json` 生成 C++ 配置，避免硬编码两个版本不一致。

**验收**：动画核心能独立构建；输出姿态连续且所有维度合法。

### PR-2 / 实现 LVGL EyesWidget

- 挂载控件，注册 LVGL timer / draw event；确保 `Destroy` 完整销毁。
- 复刻胶囊眼、弯曲眼和简化眼皮遮挡；本地坐标与裁剪正确。
- 在 LVGL Demo 或 PC Simulator 上逐帧预览，再进 StopWatch。

**验收**：纯 LVGL 绘图，无 M5GFX 依赖；动态更新不覆盖旁边 UI。

### PR-3 / 接入小智 StopWatch

- 给 `RoundLcdDisplay` 新增成员和 `SetupUI` 挂载。
- 增加 `SetEmotion` 映射，保留状态栏、字幕、预览图。
- 新增可选的 `Display::SetActivity` 默认空实现和板级覆写，接通 `Application::HandleStateChangedEvent`。
- 添加组件注册及配置开关。

**验收**：小智的语音交互和 UI 同时正确运行；关闭开关可回退。

### PR-4 / 真机性能与回归

- 记录帧率/延时、内存和音频负载。
- 增加暂停、休眠/重连等恢复路径；优化渲染频率与无变化帧。
- 输出前后对比截图/视频与 `idf.py monitor` 关键性能日志。

**验收**：通过 §8 所有必选项并留下可复现测试记录。

## 10. 风险与应对

| 风险 | 后果 | 缓解 |
|---|---|---|
| 直接沿用 `M5.Display` 绘图 | LVGL 与 M5GFX 争用同一 LCD，产生闪烁/覆盖 | 完全移除组件内的 M5GFX 绘图，统一使用 LVGL |
| 把 `.ino` 的 Dirty Rect 擦黑逻辑直接搬到屏幕 | 擦除状态栏和字幕 | 只 `lv_obj_invalidate`，由 LVGL 控制裁剪/刷新 |
| LVGL 9.5 自定义绘图 API 与 v8 示例不一致 | 编译失败、内存错误 | 编译锁定 LVGL 9.5，优先查 9.5 文档与头文件 |
| 自定义对象重建未停止 timer | 悬空指针、偶发崩溃 | 明确成员所有权与清理顺序，销毁/重复 SetupUI 单测 |
| 466×466 过大或不正确缩放 | 眼睛遮挡字幕或裁剪 | 为 EyesWidget 独立定义视口与比例/安全区 |
| 用 Emotion 代替 Activity | Listening 被 neutral 重置，Speaking 没有专属动画 | 明确两套状态与组合优先级 |
| 共享总线与动画高频刷新 | TTS 卡顿、CPU 占用过高 | 默认 30FPS、暂停隐形组件、性能测量后再提高 |
| 在第三方代码中复制 Hermes Watch AvatarEngine | AGPL 开源义务 | 本组件仅移植我们自己 MIT Demo 的代码，不复制 AGPL 源码 |

## 11. 后续版本（不阻塞 MVP）

- **v1.1**：CoreS3 320×240 / Stack-chan 设备适配，增加方屏 Layout Profile。
- **v1.2**：PCM RMS → `speaking_energy` 参数，语音驱动眼睛微动。
- **v1.3**：将 `EyesWidget` 作为通用 UI Kit 的第一个 Widget，增加菜单、图片、Arc/Progress。
- **v2.0**：引入 Scene JSON / Agent 控制 UI；独立桌面 LVGL 模拟器，实现 Linux/macOS 与设备共用 C++ core。

## 12. Coding Agent 执行指令（可直接交给 Codex）

> 请严格按 `SPEC-XIAOZHI.md` 实现，先读取当前 `eyes_demo.ino`、`presets.h`、`presets.json` 以及小智仓库的 `m5stack_stopwatch.cc`、`lcd_display.cc/.h`、`display.h`、`application.cc`、`main/CMakeLists.txt`。先产出最小影响的修改计划，再按 PR-1 至 PR-4 依次落地。保持动画数学核心无 Arduino/M5GFX/LVGL 依赖，所有 LCD 输出由 LVGL 9.5 管理，保留小智原有语音、状态栏、字幕及通知行为。对所修改工程运行编译与相关测试；如环境没有 ESP-IDF 或真实 StopWatch，明确标注“未验证”，不能声称已刷机验收。不得复制 Hermes Watch AGPL 动画源码。

## 13. 参考资料

- 原有 Eyes Lab Demo：本工作目录 `firmware/eyes_demo/eyes_demo.ino`、`presets.json`
- 小智板级 StopWatch：https://github.com/78/xiaozhi-esp32/blob/main/main/boards/m5stack/stopwatch/m5stack_stopwatch.cc
- 小智 Display：https://github.com/78/xiaozhi-esp32/blob/main/main/display/display.h
- 小智 LCD Display：https://github.com/78/xiaozhi-esp32/blob/main/main/display/lcd_display.cc
- 小智 Application：https://github.com/78/xiaozhi-esp32/blob/main/main/application.cc
- LVGL 9.5 Draw API：https://lvgl.io/docs/open/9.5/main-modules/draw/draw_api
- LVGL 线程安全：https://lvgl.io/docs/open/9.5/integration/overview
- ESP LVGL Port：https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port
