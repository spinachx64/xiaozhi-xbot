#include "eyes_view.h"

#if CONFIG_EYES_UI_ENABLED

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#define TAG "EyesView"

// ---------------------------------------------------------------------------
// A8BufferCanvas: minimal software rasterizer for the eyes engine primitives
// ---------------------------------------------------------------------------

// Grow the area that already touches this span (within kGap px), otherwise
// start a new one. Keeps the two eyes as separate refresh areas so the empty
// gap between them is not re-rendered and re-sent every frame.
void A8BufferCanvas::MarkDirty(int x0, int y0, int x1, int y1) {
    constexpr int kGap = 8;
    auto grow = [&](lv_area_t& a) {
        a.x1 = std::min<int32_t>(a.x1, x0);
        a.y1 = std::min<int32_t>(a.y1, y0);
        a.x2 = std::max<int32_t>(a.x2, x1);
        a.y2 = std::max<int32_t>(a.y2, y1);
    };
    for (int i = 0; i < dirty_count_; i++) {
        lv_area_t& a = dirty_[i];
        if (x0 <= a.x2 + kGap && x1 >= a.x1 - kGap && y0 <= a.y2 + kGap && y1 >= a.y1 - kGap) {
            grow(a);
            return;
        }
    }
    if (dirty_count_ < kMaxDirty) {
        dirty_[dirty_count_++] = {x0, y0, x1, y1};
        return;
    }
    grow(dirty_[kMaxDirty - 1]);
}

int A8BufferCanvas::TakeDirty(lv_area_t* areas) {
    const int count = dirty_count_;
    for (int i = 0; i < count; i++) {
        areas[i] = dirty_[i];
    }
    dirty_count_ = 0;
    return count;
}

// Fill [x0, x1] inclusive on row y, clipped to the buffer.
void A8BufferCanvas::Span(int y, int x0, int x1, uint8_t alpha) {
    if (y < 0 || y >= height_) {
        return;
    }
    if (x0 > x1) {
        std::swap(x0, x1);
    }
    x0 = std::max(x0, 0);
    x1 = std::min(x1, width_ - 1);
    if (x0 > x1) {
        return;
    }
    memset(buf_ + y * stride_ + x0, alpha, x1 - x0 + 1);
    MarkDirty(x0, y, x1, y);
}

void A8BufferCanvas::fillScreen(uint16_t color) {
    const uint8_t alpha = Alpha(color);
    for (int y = 0; y < height_; y++) {
        memset(buf_ + y * stride_, alpha, width_);
    }
    MarkDirty(0, 0, width_ - 1, height_ - 1);
}

void A8BufferCanvas::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const uint8_t alpha = Alpha(color);
    for (int row = y; row < y + h; row++) {
        Span(row, x, x + w - 1, alpha);
    }
}

void A8BufferCanvas::fillCircle(int x, int y, int radius, uint16_t color) {
    if (radius < 0) {
        return;
    }
    const uint8_t alpha = Alpha(color);
    const float r2 = (radius + 0.5f) * (radius + 0.5f);
    for (int dy = -radius; dy <= radius; dy++) {
        const int half = static_cast<int>(sqrtf(std::max(0.0f, r2 - dy * dy)));
        Span(y + dy, x - half, x + half, alpha);
    }
}

void A8BufferCanvas::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
    const uint8_t alpha = Alpha(color);
    // Sort vertices by y (y0 <= y1 <= y2)
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
    if (y1 > y2) { std::swap(y1, y2); std::swap(x1, x2); }
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }

    if (y0 == y2) {
        Span(y0, std::min({x0, x1, x2}), std::max({x0, x1, x2}), alpha);
        return;
    }

    const int y_start = std::max(y0, 0);
    const int y_end = std::min(y2, height_ - 1);
    for (int y = y_start; y <= y_end; y++) {
        // Long edge 0->2 always spans this row
        const float xa = x0 + static_cast<float>(x2 - x0) * (y - y0) / (y2 - y0);
        // Short edges: 0->1 above the middle vertex, 1->2 from it downward
        float xb;
        if (y < y1) {
            xb = x0 + static_cast<float>(x1 - x0) * (y - y0) / (y1 - y0);
        } else {
            xb = (y2 == y1) ? x1 : x1 + static_cast<float>(x2 - x1) * (y - y1) / (y2 - y1);
        }
        Span(y, lroundf(xa), lroundf(xb), alpha);
    }
}

// ---------------------------------------------------------------------------
// EyesView
// ---------------------------------------------------------------------------

static const char* ActivityName(eyes::Activity activity) {
    switch (activity) {
        case eyes::Activity::Listening: return "listening";
        case eyes::Activity::Speaking: return "speaking";
        default: return "idle";
    }
}

EyesView::~EyesView() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
    }
    if (canvas_obj_ != nullptr) {
        lv_obj_delete(canvas_obj_);
    }
    engine_.reset();
    canvas_.reset();
    if (buf_ != nullptr) {
        heap_caps_free(buf_);
    }
}

bool EyesView::Create(lv_obj_t* parent, int width, int height, lv_color_t color) {
    if (canvas_obj_ != nullptr) {
        return true;  // SetupUI may run more than once
    }

    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_A8);
    const size_t size = stride * height;
    buf_ = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf_ == nullptr) {
        buf_ = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    if (buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u byte eyes buffer", static_cast<unsigned>(size));
        return false;
    }
    memset(buf_, 0, size);

    canvas_obj_ = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_obj_, buf_, width, height, LV_COLOR_FORMAT_A8);
    lv_obj_center(canvas_obj_);
    lv_obj_remove_flag(canvas_obj_, LV_OBJ_FLAG_CLICKABLE);
    SetColor(color);

    canvas_ = std::make_unique<A8BufferCanvas>(buf_, width, height, stride);
    engine_ = std::make_unique<eyes::Engine>(*canvas_);
    engine_->begin(lv_tick_get(), esp_random());

    // The LVGL task wakes on FreeRTOS tick boundaries, so a 33 ms period only
    // fires every 40 ms at 100 Hz. Round down to a tick multiple, minus 1 ms so
    // the timer is already due when the task wakes on that tick.
    uint32_t period = 1000 / CONFIG_EYES_UI_FPS;
    if (period > portTICK_PERIOD_MS) {
        period = period / portTICK_PERIOD_MS * portTICK_PERIOD_MS - 1;
    }
    timer_ = lv_timer_create(TimerCb, period, this);
#if CONFIG_EYES_UI_DEBUG
    // Measure the whole LVGL refresh (render + flush), not just the engine.
    lv_display_t* disp = lv_obj_get_display(canvas_obj_);
    lv_display_add_event_cb(disp, RefrEventCb, LV_EVENT_REFR_START, this);
    lv_display_add_event_cb(disp, RefrEventCb, LV_EVENT_REFR_READY, this);
#endif
    ESP_LOGI(TAG, "Eyes view %dx%d @%d FPS, buffer %u bytes", width, height, CONFIG_EYES_UI_FPS,
             static_cast<unsigned>(size));
    return true;
}

void EyesView::SetExpression(const char* id) {
    if (engine_ == nullptr || id == nullptr) {
        return;
    }
    if (strcmp(engine_->expression(), id) == 0) {
        return;
    }
    if (!engine_->set_expression(id)) {
        ESP_LOGW(TAG, "Unknown expression: %s", id);
        return;
    }
    ESP_LOGI(TAG, "Expression -> %s", id);
}

void EyesView::SetColor(lv_color_t color) {
    if (canvas_obj_ == nullptr) {
        return;
    }
    // A8 images are drawn as an alpha mask filled with the recolor color.
    lv_obj_set_style_image_recolor(canvas_obj_, color, 0);
    lv_obj_set_style_image_recolor_opa(canvas_obj_, LV_OPA_COVER, 0);
}

void EyesView::TimerCb(lv_timer_t* timer) {
    static_cast<EyesView*>(lv_timer_get_user_data(timer))->Tick();
}

// Runs in the LVGL task, which already holds the LVGL lock.
void EyesView::Tick() {
    // Skip work while hidden (e.g. preview image shown); the engine clamps
    // its time step, so resuming does not make the animation jump.
    if (!lv_obj_is_visible(canvas_obj_)) {
        was_hidden_ = true;
        return;
    }
    if (was_hidden_) {
        was_hidden_ = false;
        engine_->invalidate();
    }

    if (activity_source_) {
        const eyes::Activity activity = activity_source_();
        if (activity != engine_->activity()) {
            ESP_LOGI(TAG, "Activity %s -> %s", ActivityName(engine_->activity()),
                     ActivityName(activity));
            engine_->set_activity(activity);
        }
    }

    const uint32_t now = lv_tick_get();
#if CONFIG_EYES_UI_DEBUG
    const int64_t t0 = esp_timer_get_time();
#endif
    if (!engine_->update(now)) {
        return;
    }
#if CONFIG_EYES_UI_DEBUG
    RecordFrame(now, esp_timer_get_time() - t0);
#endif

    lv_area_t dirty[A8BufferCanvas::kMaxDirty];
    const int count = canvas_->TakeDirty(dirty);
    if (count == 0) {
        return;
    }
    // Convert buffer-local coordinates to absolute screen coordinates.
    lv_area_t coords;
    lv_obj_get_coords(canvas_obj_, &coords);
    for (int i = 0; i < count; i++) {
        lv_area_move(&dirty[i], coords.x1, coords.y1);
        lv_obj_invalidate_area(canvas_obj_, &dirty[i]);
    }
}

#if CONFIG_EYES_UI_DEBUG
void EyesView::RefrEventCb(lv_event_t* e) {
    auto* self = static_cast<EyesView*>(lv_event_get_user_data(e));
    const int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        self->refr_start_us_ = now;
        return;
    }
    if (self->refr_start_us_ == 0) {
        return;
    }
    const int64_t dt = now - self->refr_start_us_;
    self->refr_start_us_ = 0;
    // Refresh cycles with nothing to redraw finish in a few us; skip them.
    if (dt < 200) {
        return;
    }
    self->refr_sum_us_ += dt;
    self->refr_max_us_ = std::max(self->refr_max_us_, dt);
    self->refr_count_++;
}

// Logs frame interval, engine draw time, LVGL refresh time and running
// animations every 5 seconds.
void EyesView::RecordFrame(uint32_t now_ms, int64_t draw_us) {
    if (frame_count_ == 0 && metrics_start_ms_ == 0) {
        metrics_start_ms_ = now_ms;
    } else {
        const uint32_t interval = now_ms - last_frame_ms_;
        interval_sum_ms_ += interval;
        interval_max_ms_ = std::max(interval_max_ms_, interval);
        frame_count_++;
    }
    last_frame_ms_ = now_ms;
    draw_max_us_ = std::max(draw_max_us_, draw_us);
    draw_sum_us_ += draw_us;

    if (now_ms - metrics_start_ms_ >= 5000 && frame_count_ > 0) {
        ESP_LOGI(TAG,
                 "metrics: %lu frames, avg %.1f ms (%.1f fps), worst %lu ms | draw avg %lld max "
                 "%lld us | refr %lu avg %lld max %lld us | anims %u",
                 static_cast<unsigned long>(frame_count_),
                 static_cast<double>(interval_sum_ms_) / frame_count_,
                 1000.0 * frame_count_ / interval_sum_ms_,
                 static_cast<unsigned long>(interval_max_ms_), draw_sum_us_ / (frame_count_ + 1),
                 draw_max_us_, static_cast<unsigned long>(refr_count_),
                 refr_count_ ? refr_sum_us_ / refr_count_ : 0, refr_max_us_,
                 static_cast<unsigned>(lv_anim_count_running()));
        metrics_start_ms_ = now_ms;
        frame_count_ = 0;
        interval_sum_ms_ = 0;
        interval_max_ms_ = 0;
        draw_max_us_ = 0;
        draw_sum_us_ = 0;
        refr_sum_us_ = 0;
        refr_max_us_ = 0;
        refr_count_ = 0;
    }
}
#endif

#endif  // CONFIG_EYES_UI_ENABLED
