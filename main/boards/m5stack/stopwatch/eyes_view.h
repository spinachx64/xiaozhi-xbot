#ifndef EYES_VIEW_H
#define EYES_VIEW_H

#include <sdkconfig.h>

#if CONFIG_EYES_UI_ENABLED

#include <lvgl.h>
#include <cstdint>
#include <functional>
#include <memory>

#include "eyes/eyes.hpp"

// eyes::Canvas backed by an A8 (alpha-only) buffer. Eye pixels become opaque,
// background pixels transparent, so the LVGL canvas object is tinted with the
// configured eye color and blends over the screen background.
class A8BufferCanvas final : public eyes::Canvas {
public:
    A8BufferCanvas(uint8_t* buf, int width, int height, int stride)
        : buf_(buf), width_(width), height_(height), stride_(stride) {}

    int width() const override { return width_; }
    int height() const override { return height_; }
    void fillScreen(uint16_t color) override;
    void fillRect(int x, int y, int w, int h, uint16_t color) override;
    void fillCircle(int x, int y, int radius, uint16_t color) override;
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) override;

    static constexpr int kMaxDirty = 4;
    // Separate bounding boxes of pixels touched since the last call (one per
    // eye in practice). Writes up to kMaxDirty areas; returns the count.
    int TakeDirty(lv_area_t* areas);

private:
    void Span(int y, int x0, int x1, uint8_t alpha);
    void MarkDirty(int x0, int y0, int x1, int y1);
    static uint8_t Alpha(uint16_t color) { return color != 0 ? 0xFF : 0x00; }

    uint8_t* buf_;
    int width_;
    int height_;
    int stride_;
    int dirty_count_ = 0;
    lv_area_t dirty_[kMaxDirty] = {};
};

// LVGL host for eyes::Engine. All methods must run with the LVGL lock held.
class EyesView {
public:
    using ActivitySource = std::function<eyes::Activity()>;

    EyesView() = default;
    ~EyesView();
    EyesView(const EyesView&) = delete;
    EyesView& operator=(const EyesView&) = delete;

    bool Create(lv_obj_t* parent, int width, int height, lv_color_t color);
    void SetExpression(const char* id);
    void SetColor(lv_color_t color);
    void SetScale(float scale) {
        if (engine_ != nullptr) {
            engine_->set_scale(scale);
        }
    }
    // Polled once per frame from the LVGL task; must be cheap and thread-safe.
    void SetActivitySource(ActivitySource source) { activity_source_ = std::move(source); }

private:
    static void TimerCb(lv_timer_t* timer);
    void Tick();

    lv_obj_t* canvas_obj_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint8_t* buf_ = nullptr;
    std::unique_ptr<A8BufferCanvas> canvas_;
    std::unique_ptr<eyes::Engine> engine_;
    ActivitySource activity_source_;
    bool was_hidden_ = false;

#if CONFIG_EYES_UI_DEBUG
    void RecordFrame(uint32_t now_ms, int64_t draw_us);
    static void RefrEventCb(lv_event_t* e);
    int64_t draw_sum_us_ = 0;
    int64_t refr_start_us_ = 0;
    int64_t refr_sum_us_ = 0;
    int64_t refr_max_us_ = 0;
    uint32_t refr_count_ = 0;
    uint32_t metrics_start_ms_ = 0;
    uint32_t last_frame_ms_ = 0;
    uint32_t frame_count_ = 0;
    uint32_t interval_sum_ms_ = 0;
    uint32_t interval_max_ms_ = 0;
    int64_t draw_max_us_ = 0;
#endif
};

#endif  // CONFIG_EYES_UI_ENABLED
#endif  // EYES_VIEW_H
