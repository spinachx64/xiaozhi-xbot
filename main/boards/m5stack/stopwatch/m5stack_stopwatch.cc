#include "wifi_board.h"
#include "backlight.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "M5IOE1.h"
#include "M5PM1.h"
#include "config.h"
#include "eyes_view.h"
#include "assets/lang_config.h"
#include "lvgl_theme.h"
#include <esp_log.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <wifi_manager.h>

#define TAG "M5StackStopwatch"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

// CO5300 AMOLED: initialize at full brightness, then restore the saved setting.
static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 0, 10}, // RGB565
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Column address: 0-465
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Row address: 0-465
    {0x11, (uint8_t []){0x00}, 0, 120}, // Exit sleep
    {0x29, (uint8_t []){0x00}, 0, 20},  // Display on
};

class RoundLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
        area->x1 = (area->x1 >> 1) << 1;
        area->y1 = (area->y1 >> 1) << 1;
        area->x2 = ((area->x2 >> 1) << 1) + 1;
        area->y2 = ((area->y2 >> 1) << 1) + 1;
    }

    RoundLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override {
#if CONFIG_EYES_UI_ENABLED
        // The eyes face is designed for a pure black AMOLED background.
        current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");
#endif
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);

        // Horizontal inset so labels stay inside the round mask
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.2, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.2, 0);

        // Status bar: upper half, below top icon bar (not at screen bottom)
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_STATUS_BAR_TOP_OFF);
        lv_obj_set_width(status_label_, LV_HOR_RES * 0.6);
        lv_obj_set_width(notification_label_, LV_HOR_RES * 0.6);
        lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);

        // Chat/subtitle text: a bit higher, away from bottom arc
        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CHAT_BAR_BOTTOM_OFF);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75);
        }

        // emoji_box_: keep default LV_ALIGN_CENTER from SpiLcdDisplay::SetupUI()

        // Top icons: move the top bar down inside the round display safe area
        if (top_bar_ != nullptr) {
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_ROUND_EDGE_INSET / 2);
            lv_obj_set_style_pad_top(top_bar_, DISPLAY_ROUND_EDGE_INSET / 4, 0);
        }

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

#if CONFIG_EYES_UI_ENABLED
        // Replace the center emoji with the eyes. emoji_box_ stays in place so
        // the preview image logic that hides/shows it keeps working.
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(emoji_box_, kEyesWidth, kEyesHeight);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, kEyesOffsetY);
        eyes_.Create(emoji_box_, kEyesWidth, kEyesHeight, kEyeColor);
        eyes_.SetScale(kEyesScale);
        eyes_.SetActivitySource([]() {
            switch (Application::GetInstance().GetDeviceState()) {
                case kDeviceStateListening: return eyes::Activity::Listening;
                case kDeviceStateSpeaking: return eyes::Activity::Speaking;
                default: return eyes::Activity::Idle;
            }
        });
        ApplyFaceStyle();
        CreateStateMark();
#if CONFIG_EYES_UI_DEBUG
        xTaskCreate([](void* arg) { static_cast<RoundLcdDisplay*>(arg)->SnapshotLoop(); },
                    "eyes_snap", 6144, this, 1, nullptr);
#endif
#endif
    }

#if CONFIG_EYES_UI_ENABLED
    // The eyes already show listening/speaking; keep the status line for the
    // clock, connection and other messages only.
    void SetStatus(const char* status) override {
        if (status != nullptr && (strcmp(status, Lang::Strings::LISTENING) == 0 ||
                                  strcmp(status, Lang::Strings::SPEAKING) == 0)) {
            status = "";
        }
        SpiLcdDisplay::SetStatus(status);
    }

    void SetEmotion(const char* emotion) override {
        if (emotion == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        // Base class code paths (ClearChatMessages etc.) may unhide the label.
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        eyes_.SetExpression(MapEmotion(emotion));
    }

    void SetChatMessage(const char* role, const char* content) override {
        // While listening only the user's live transcript is shown; a late
        // assistant subtitle would make listening look like speaking.
        if (role != nullptr && strcmp(role, "assistant") == 0 &&
            Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
            return;
        }
        SpiLcdDisplay::SetChatMessage(role, content);
        HideEmoji();
        DisplayLockGuard lock(this);
        StartSubtitleScroll();
    }

    void ClearChatMessages() override {
        SpiLcdDisplay::ClearChatMessages();
        HideEmoji();
    }

    // Always stay on the dark theme; the face style is reapplied on top of it.
    void SetTheme(Theme* theme) override {
        (void)theme;
        SpiLcdDisplay::SetTheme(LvglThemeManager::GetInstance().GetTheme("dark"));
        DisplayLockGuard lock(this);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        ApplyFaceStyle();
    }

private:
    // Canvas size chosen so the engine renders eyes at ~1:1 with the 466px
    // reference; the canvas is transparent, so it can overlap the text areas.
    static constexpr int kEyesWidth = 400;
    static constexpr int kEyesHeight = 300;
    static constexpr int kTopBarWidth = 200;   // icons stay inside the top arc
    static constexpr int kStatusTopOffset = 80; // clock/status below the icons
    static constexpr float kEyesScale = 1.3f;   // eyes are the focal point
    static constexpr int kSubtitleWidth = 320;  // fits the round chord near the bottom
    static constexpr int kSubtitleLines = 2;
    static constexpr uint32_t kSubtitleScrollDelayMs = 1500;  // read the first lines first
    static constexpr uint32_t kSubtitleLineHoldMs = 1500;     // reading time per line
    static constexpr uint32_t kSubtitleLineSlideMs = 200;     // short slide to the next line
    static constexpr uint32_t kSubtitleLinePeriodMs = kSubtitleLineHoldMs + kSubtitleLineSlideMs;
    static constexpr int kEyesOffsetY = -20;    // leave a gap above the subtitle
    static constexpr lv_color_t kEyeColor = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);

    // A small dot below the subtitle marks speaking: slowly breathing amber,
    // muted so it stays in the background. Listening is shown by the live
    // transcript instead, so the dot is hidden then.
    static constexpr int kStateMarkSize = 8;
    static constexpr int kStateMarkBottomOff = 40;
    static constexpr uint32_t kStateMarkTickMs = 100;
    static constexpr uint32_t kStateMarkBreathMs = 1600;
    static constexpr lv_color_t kSpeakMarkColor = LV_COLOR_MAKE(0xB0, 0x7A, 0x2A);

    EyesView eyes_;
    int32_t subtitle_view_height_ = 0;
    lv_obj_t* state_mark_ = nullptr;
    DeviceState mark_state_ = kDeviceStateUnknown;
    uint32_t mark_phase_ms_ = 0;

    // Call with the display lock held.
    void CreateStateMark() {
        state_mark_ = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(state_mark_);
        lv_obj_set_size(state_mark_, kStateMarkSize, kStateMarkSize);
        lv_obj_set_style_radius(state_mark_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(state_mark_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(state_mark_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(state_mark_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(state_mark_, LV_ALIGN_BOTTOM_MID, 0, -kStateMarkBottomOff);
        // Runs in the LVGL task, so it may touch objects without extra locking.
        lv_timer_create([](lv_timer_t* t) {
            static_cast<RoundLcdDisplay*>(lv_timer_get_user_data(t))->UpdateStateMark();
        }, kStateMarkTickMs, this);
    }

    void UpdateStateMark() {
        const DeviceState state = Application::GetInstance().GetDeviceState();
        if (state != mark_state_) {
            mark_state_ = state;
            mark_phase_ms_ = 0;
            if (state == kDeviceStateListening) {
                // The live transcript shows listening; no dot needed.
                lv_obj_add_flag(state_mark_, LV_OBJ_FLAG_HIDDEN);
                // The last reply's subtitle made listening look like speaking.
                if (chat_message_label_ != nullptr) {
                    lv_anim_delete(chat_message_label_, nullptr);
                    lv_label_set_text(chat_message_label_, "");
                }
            } else if (state == kDeviceStateSpeaking) {
                lv_obj_set_style_bg_color(state_mark_, kSpeakMarkColor, 0);
                lv_obj_remove_flag(state_mark_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(state_mark_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (state == kDeviceStateSpeaking) {
            // Triangle wave between 30% and 90% opacity.
            mark_phase_ms_ = (mark_phase_ms_ + kStateMarkTickMs) % kStateMarkBreathMs;
            const uint32_t half = kStateMarkBreathMs / 2;
            const uint32_t d = mark_phase_ms_ < half ? mark_phase_ms_ : kStateMarkBreathMs - mark_phase_ms_;
            lv_obj_set_style_bg_opa(state_mark_, LV_OPA_30 + (LV_OPA_90 - LV_OPA_30) * d / half, 0);
        }
    }

    // Short text sits centered in the viewport; text taller than the viewport
    // waits briefly, then scrolls up once until its last lines are visible.
    // Call with the display lock held.
    void StartSubtitleScroll() {
        if (bottom_bar_ == nullptr || chat_message_label_ == nullptr ||
            subtitle_view_height_ <= 0) {
            return;
        }
        lv_anim_delete(chat_message_label_, nullptr);
        lv_obj_update_layout(chat_message_label_);
        const int32_t overflow = lv_obj_get_height(chat_message_label_) - subtitle_view_height_;
        if (overflow <= 0) {
            lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
            return;
        }
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
        // Step one line at a time: hold, then a short slide. The label is only
        // redrawn during the slides, not every frame as with a smooth scroll.
        const lv_font_t* font = lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
        const int32_t line_h = font->line_height;
        const int32_t steps = (overflow + line_h - 1) / line_h;
        const uint32_t total_ms = steps * kSubtitleLinePeriodMs;
#if CONFIG_EYES_UI_DEBUG
        ESP_LOGI(TAG, "Subtitle scroll: overflow %ld px, %ld lines, %lu ms",
                 static_cast<long>(overflow), static_cast<long>(steps),
                 static_cast<unsigned long>(total_ms));
#endif
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, chat_message_label_);
        lv_anim_set_custom_exec_cb(&a, [](lv_anim_t* anim, int32_t t) {
            auto* label = static_cast<lv_obj_t*>(anim->var);
            const lv_font_t* f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
            const int32_t lh = f->line_height;
            const int32_t max_shift = lv_obj_get_height(label) - lh * kSubtitleLines;
            // Each period: hold for kSubtitleLineHoldMs, then slide one line.
            const int32_t line = t / static_cast<int32_t>(kSubtitleLinePeriodMs);
            const int32_t in_period = t % static_cast<int32_t>(kSubtitleLinePeriodMs);
            int32_t shift = line * lh;
            if (in_period > static_cast<int32_t>(kSubtitleLineHoldMs)) {
                const int32_t p = (in_period - kSubtitleLineHoldMs) * LV_BEZIER_VAL_MAX /
                                  kSubtitleLineSlideMs;
                shift += lv_cubic_bezier(p, 341, 0, 683, 1024) * lh / LV_BEZIER_VAL_MAX;
            }
            shift = std::min(shift, max_shift);
            if (lv_obj_get_y(label) != -shift) {
                lv_obj_set_y(label, -shift);
            }
        });
        lv_anim_set_values(&a, 0, total_ms);
        lv_anim_set_duration(&a, total_ms);
        lv_anim_set_delay(&a, kSubtitleScrollDelayMs - kSubtitleLineHoldMs);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    }

    // Eyes are the focus; text is secondary: no bar backgrounds, muted grays.
    void ApplyFaceStyle() {
        const lv_color_t dim = lv_color_hex(0x6E6E6E);
        const lv_color_t soft = lv_color_hex(0xB4B4B4);
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            lv_obj_set_width(top_bar_, kTopBarWidth);
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_ROUND_EDGE_INSET / 2);
        }
        lv_obj_set_style_text_color(network_label_, dim, 0);
        lv_obj_set_style_text_color(mute_label_, dim, 0);
        lv_obj_set_style_text_color(battery_label_, dim, 0);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, kStatusTopOffset);
        lv_obj_set_style_text_color(status_label_, dim, 0);
        lv_obj_set_style_text_color(notification_label_, soft, 0);
        if (bottom_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(chat_message_label_, soft, 0);
            // A fixed two-line viewport that clips a fully wrapped label. Longer
            // text scrolls upward slowly (see StartSubtitleScroll) instead of
            // scrolling sideways, which got cut off by the round edge.
            const lv_font_t* font = lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
            subtitle_view_height_ = font->line_height * kSubtitleLines;
            lv_obj_set_size(bottom_bar_, kSubtitleWidth, subtitle_view_height_);
            lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
            lv_anim_delete(chat_message_label_, nullptr);
            lv_obj_set_style_anim(chat_message_label_, nullptr, LV_PART_MAIN);
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_width(chat_message_label_, kSubtitleWidth);
            lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(chat_message_label_, LV_COORD_MAX, 0);
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CHAT_BAR_BOTTOM_OFF);
            StartSubtitleScroll();
        }
        eyes_.SetColor(kEyeColor);
    }

#if CONFIG_EYES_UI_DEBUG
    // Dumps a base64 JPEG of the screen to the console 1.5 s after each switch
    // to idle/listening/speaking. Read with scripts/eyes_snap.py.
    void SnapshotLoop() {
        DeviceState last = kDeviceStateUnknown;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(200));
            const DeviceState state = Application::GetInstance().GetDeviceState();
            if (state == last) {
                continue;
            }
            last = state;
            if (state != kDeviceStateIdle && state != kDeviceStateListening &&
                state != kDeviceStateSpeaking) {
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
            DumpSnapshot(DeviceStateMachine::GetStateName(state));
        }
    }

    void DumpSnapshot(const char* label) {
        std::string jpeg;
        if (!SnapshotToJpeg(jpeg, 70)) {
            return;
        }
        printf("\n===SNAP-BEGIN %s %u===\n", label, static_cast<unsigned>(jpeg.size()));
        unsigned char line[80];
        for (size_t i = 0; i < jpeg.size(); i += 57) {  // 57 bytes -> 76 base64 chars
            const size_t n = std::min<size_t>(57, jpeg.size() - i);
            size_t olen = 0;
            mbedtls_base64_encode(line, sizeof(line), &olen,
                                  reinterpret_cast<const unsigned char*>(jpeg.data()) + i, n);
            printf("SNAP:%.*s\n", static_cast<int>(olen), line);
        }
        printf("===SNAP-END===\n");
    }
#endif

    void HideEmoji() {
        DisplayLockGuard lock(this);
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Map xiaozhi emotion strings to eyes preset IDs; unknown values fall back to idle.
    static const char* MapEmotion(const char* emotion) {
        static const struct { const char* from; const char* to; } kMap[] = {
            {"neutral", "idle"},       {"robot_2", "idle"},      {"relaxed", "idle"},
            {"happy", "happy"},        {"funny", "happy"},       {"loving", "happy"},
            {"confident", "happy"},    {"delicious", "happy"},   {"cool", "happy"},
            {"laughing", "excited"},   {"kissy", "excited"},
            {"thinking", "thinking"},
            {"angry", "angry"},
            {"sad", "sad"},            {"crying", "sad"},
            {"surprised", "surprised"}, {"shocked", "surprised"}, {"embarrassed", "surprised"},
            {"confused", "confused"},  {"doubtful", "confused"},
            {"sleepy", "sleepy"},
            {"winking", "wink"},       {"silly", "wink"},
        };
        for (const auto& m : kMap) {
            if (strcmp(emotion, m.from) == 0) {
                return m.to;
            }
        }
        ESP_LOGD(TAG, "Unmapped emotion '%s', using idle", emotion);
        return "idle";
    }
#endif
};

class StopwatchBacklight : public Backlight {
public:
    StopwatchBacklight(esp_lcd_panel_io_handle_t panel_io, Display* display)
        : panel_io_(panel_io), display_(display) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        DisplayLockGuard lock(display_);
        uint8_t data[] = {static_cast<uint8_t>((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;

        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, sizeof(data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set display brightness: %s", esp_err_to_name(err));
        }
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
    Display* display_;
};

class M5StackStopwatchBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    M5PM1 pmic_;
    M5IOE1 ioe_;
    Button button1_;
    Button button2_;
    RoundLcdDisplay* display_;
    StopwatchBacklight* backlight_;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (ioe_.begin(i2c_bus_, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ_100K, M5IOE1_INT_MODE_POLLING) != M5IOE1_OK) {
            ESP_LOGE(TAG, "M5IOE1 begin failed");
            return;
        }

        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
            ESP_LOGE(TAG, "M5PM1 begin failed");
            return;
        }

        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(true);
        pmic_.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
        pmic_.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
        pmic_.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW); // Set charge current to 425mA

        ioe_.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_LCD_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);

        ioe_.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_PA_EN, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);

        ioe_.pinMode(IOE_PIN_MOTOR, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_SCK;
        buscfg.data0_io_num = DISPLAY_QSPI_D0;
        buscfg.data1_io_num = DISPLAY_QSPI_D1;
        buscfg.data2_io_num = DISPLAY_QSPI_D2;
        buscfg.data3_io_num = DISPLAY_QSPI_D3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        ioe_.digitalWrite(IOE_PIN_LCD_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_QSPI_HOST, &io_config, &panel_io));

        co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));

        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new RoundLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                       DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons() {
        // Button1: wake / toggle conversation
        button1_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Button2: volume 0 -> 10 -> ... -> 100 -> 0
        button2_.OnClick([this]() {
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ":" + std::to_string(volume) + "%");
        });
    }

public:
    M5StackStopwatchBoard()
        : i2c_bus_(nullptr),
          button1_(BUTTON1_GPIO),
          button2_(BUTTON2_GPIO),
          display_(nullptr),
          backlight_(nullptr) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA,
            AUDIO_CODEC_ES8311_ADDR,
            false);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        return backlight_;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }

        int charge_state_level = pmic_.digitalRead(PMIC_PIN_CHARGE_STATE);
        if (charge_state_level < 0) {
            // Charge status read failed; leave charging/discharging unknown
            charging = false;
            discharging = false;
        } else {
            // M5PM1 charge status is active low: 0 means charging, 1 means not charging
            charging = (charge_state_level == 0);
            discharging = !charging;
        }

        const int BATTERY_MIN_VOLTAGE = 3400;
        const int BATTERY_MAX_VOLTAGE = 4200;
        if (voltage_mv < BATTERY_MIN_VOLTAGE) {
            level = 0;
        } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
            level = 100;
        } else {
            level = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
        }
        return true;
    }
};

DECLARE_BOARD(M5StackStopwatchBoard);
