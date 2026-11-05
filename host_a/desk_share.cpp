// SPDX-License-Identifier: Apache-2.0

#include "desk_share.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "jpeg_encoder.h"
#include "kvm/log.h"
#include "kvm/net.h"

#pragma comment(lib, "dwmapi.lib")

namespace kvm {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kPoll = std::chrono::milliseconds(10);
constexpr auto kLogEvery = std::chrono::seconds(5);

// The alpha of a BGRA picture as runs per row: [u8 alpha][u16 count, little-endian]. Empty if the
// picture is fully opaque (most windows: only rounded corners aren't).
void alpha_runs(const std::uint8_t* first, int width, int height, int stride, std::vector<std::uint8_t>& out) {
    out.clear();
    bool any = false;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = first + static_cast<size_t>(y) * stride;
        int x = 0;
        while (x < width) {
            const std::uint8_t a = row[x * 4 + 3];
            int n = 1;
            while (x + n < width && n < 0xFFFF && row[(x + n) * 4 + 3] == a) ++n;
            if (a != 255) any = true;
            out.push_back(a);
            out.push_back(static_cast<std::uint8_t>(n & 0xFF));
            out.push_back(static_cast<std::uint8_t>(n >> 8));
            x += n;
        }
    }
    if (!any) out.clear();
}

// The radius of a window's rounded corners as DWM draws them (Windows 11), in physical px; 0 for
// square ones (Windows 10, maximized windows, windows that opt out). The viewer clips the window to it.
int corner_radius(HWND hwnd) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_DEFAULT;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)))) return 0;
    if (IsZoomed(hwnd) || pref == DWMWCP_DONOTROUND) return 0;
    const int dp = pref == DWMWCP_ROUNDSMALL ? 4 : 8;
    return MulDiv(dp, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

}  // namespace

struct DeskShare::Window {
    std::uint64_t id = 0;
    HWND hwnd = nullptr;
    std::string title;
    display::Rect frame;                     // screen coordinates (latest from the tracker)
    std::unique_ptr<ScreenCapture> capture;  // nullptr: cut out of the whole-VDD capture
    std::mutex mutex;                        // pending
    std::optional<CapturedFrame> pending;    // newest picture not sent yet
    Clock::time_point next_send{};
    proto::FrameHeader last_header;          // for resend()
    std::vector<std::uint8_t> last_payload;
};

DeskShare::DeskShare(const VirtualDisplay::MonitorRect& vdd, desk::Region& region, SendFn send, LayoutFn layout,
                     Options options)
    : region_(region),
      send_(std::move(send)),
      layout_(std::move(layout)),
      options_(options),
      vdd_{vdd.rect.left, vdd.rect.top, vdd.rect.right, vdd.rect.bottom} {
    if (options_.codec == "h264") {
        // One stream of the whole VDD: the viewer cuts the windows out at their layout rects.
        std::string why = "no GPU with video support";
        auto gpu = GpuDevice::create();
        auto send_desk = [this](const proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
            proto::FrameHeader desk = h;
            desk.layer = "desk";
            desk.desk_w = vdd_.width();
            desk.desk_h = vdd_.height();
            desk.window = 0;  // the whole VDD, not one window
            ++sent_;
            return send_(desk, payload);
        };
        h264_ = gpu ? H264Sender::create(gpu, vdd_.width(), vdd_.height(), send_desk,
                                         {.max_fps = options_.max_fps, .bitrate = options_.h264_bitrate}, why)
                    : nullptr;
        if (h264_) {
            // The cursor isn't in the picture: the viewer shows its own, in the shape desk_cursor says.
            gpu_capture_ = ScreenCapture::for_monitor(vdd.monitor, false, gpu);
            H264Sender* sender = h264_.get();
            gpu_capture_->start_gpu([sender](GpuFrame&& f) { sender->put(std::move(f)); });
        } else {
            log::warn("desk: no H.264 ({}): JPEG per window", why);
        }
    }
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    tracker_ = std::make_unique<DeskTracker>(region_, [this](const auto& windows) { on_windows(windows); });
    log::info("desk: sharing the other screen's windows ({}x{} at {},{}) as {}", vdd_.width(), vdd_.height(), vdd_.left,
              vdd_.top, codec());
}

DeskShare::~DeskShare() {
    tracker_.reset();  // no more on_windows()
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    for (auto& [id, w] : windows_) {
        if (w->capture) w->capture->stop();
    }
    if (screen_) screen_->stop();
    if (h264_) h264_->stop();  // the sender before the capture that feeds it, and stays until it stopped
    if (gpu_capture_) gpu_capture_->stop();
    h264_.reset();
    log::info("desk: stopped after {} pictures", sent_.load());
}

void DeskShare::resend() {
    {
        std::lock_guard lock(mutex_);
        resend_ = true;
    }
    cv_.notify_all();
}

void DeskShare::request_keyframe() {
    if (h264_) h264_->request_keyframe();
}

void DeskShare::on_viewer_stats(int latency_ms) {
    if (h264_) h264_->on_viewer_stats(latency_ms);
}

void DeskShare::notify_frames() {
    {
        std::lock_guard lock(mutex_);
        frames_ready_ = true;
    }
    cv_.notify_all();
}

void DeskShare::on_windows(const std::vector<DeskTracker::Window>& windows) {
    display::Rect vdd = vdd_;
    bool live = false;
    region_.vdd_rect(vdd, live);
    // The active window is in front over there too: is the owner's foreground one of the VDD windows?
    const HWND foreground = GetForegroundWindow();
    const bool active =
        std::any_of(windows.begin(), windows.end(), [&](const auto& w) { return w.hwnd == foreground; });
    proto::DeskLayout layout{.desk_w = vdd.width(), .desk_h = vdd.height(), .active = active};
    for (const auto& w : windows) {
        layout.windows.push_back({.id = reinterpret_cast<std::uint64_t>(w.hwnd),
                                  .x = w.frame.left - vdd.left,
                                  .y = w.frame.top - vdd.top,
                                  .w = w.frame.width(),
                                  .h = w.frame.height(),
                                  .radius = corner_radius(w.hwnd)});
    }
    layout_(layout);  // right away: a moved window is shown at its new place without a new picture
    {
        std::lock_guard lock(mutex_);
        last_layout_ = std::move(layout);
        new_windows_ = windows;
    }
    cv_.notify_all();
}

void DeskShare::sync(const std::vector<DeskTracker::Window>& windows) {
    std::set<std::uint64_t> wanted;
    bool need_screen = false;
    for (const auto& tw : windows) {
        const auto id = reinterpret_cast<std::uint64_t>(tw.hwnd);
        wanted.insert(id);
        auto& w = windows_[id];
        if (!w) {
            w = std::make_unique<Window>();
            w->id = id;
            w->hwnd = tw.hwnd;
            w->title = tw.title;
            try {
                auto capture = ScreenCapture::for_window(tw.hwnd, /*capture_cursor=*/false);
                Window* raw = w.get();
                capture->start([this, raw](CapturedFrame&& f) {
                    {
                        std::lock_guard lock(raw->mutex);
                        raw->pending = std::move(f);
                    }
                    notify_frames();
                });
                w->capture = std::move(capture);
            } catch (const std::exception& e) {
                log::info("desk: \"{}\" is cut out of the screen picture ({})", tw.title, e.what());
            }
        }
        w->frame = tw.frame;
        if (!w->capture) need_screen = true;
    }
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second->capture) it->second->capture->stop();  // no callback touches it after this
        it = windows_.erase(it);
    }
    if (need_screen && !screen_) {
        try {
            const POINT center{vdd_.left + vdd_.width() / 2, vdd_.top + vdd_.height() / 2};
            screen_ =
                ScreenCapture::for_monitor(MonitorFromPoint(center, MONITOR_DEFAULTTONULL), /*capture_cursor=*/false);
            screen_->start([this](CapturedFrame&& f) {
                {
                    std::lock_guard lock(mutex_);
                    screen_frame_ = std::move(f);
                }
                notify_frames();
            });
        } catch (const std::exception& e) {
            log::warn("desk: no screen picture either: {}", e.what());
            screen_.reset();
        }
    } else if (!need_screen && screen_) {
        screen_->stop();
        screen_.reset();
    }
}

void DeskShare::run(std::stop_token stop) {
    JpegEncoder jpeg(options_.jpeg_quality, /*full_chroma=*/true);
    std::vector<std::uint8_t> encoded, alpha;
    const auto min_interval =
        options_.max_fps > 0 ? std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)) / options_.max_fps
                             : Clock::duration::zero();
    std::map<std::uint64_t, std::uint32_t> seq;  // per window: each is a stream, numbered without gaps
    auto period_start = Clock::now();
    std::uint64_t period_frames = 0, period_bytes = 0, period_pixels = 0;
    double period_encode_ms = 0;

    // Encodes one picture of window `w` (x, y = where it sits inside the window's frame) and sends it.
    const auto send_picture = [&](Window& w, const std::uint8_t* first, int width, int height, int stride, int x, int y,
                                  bool has_alpha, std::int64_t t_capture_us) {
        if (width <= 0 || height <= 0 || !jpeg.encode(first, width, height, stride, encoded)) return;
        if (has_alpha) {
            alpha_runs(first, width, height, stride, alpha);
        } else {
            alpha.clear();
        }
        proto::FrameHeader header{.seq = seq[w.id]++,
                                  .t_capture_us = t_capture_us,
                                  .width = width,
                                  .height = height,
                                  .layer = "desk",
                                  .x = x,
                                  .y = y,
                                  .desk_w = vdd_.width(),
                                  .desk_h = vdd_.height()};
        header.window = w.id;
        header.alpha_bytes = static_cast<std::uint32_t>(alpha.size());
        encoded.insert(encoded.end(), alpha.begin(), alpha.end());  // payload = JPEG + alpha runs
        send_(header, encoded);
        w.last_header = header;
        w.last_payload = encoded;
        ++sent_;
        ++period_frames;
        period_bytes += encoded.size();
        period_pixels += static_cast<std::uint64_t>(width) * height;
        period_encode_ms += jpeg.last_encode_ms();
    };

    while (!stop.stop_requested()) {
        std::optional<std::vector<DeskTracker::Window>> changed;
        std::optional<CapturedFrame> screen;
        bool resend = false;
        proto::DeskLayout layout;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, kPoll,
                         [&] { return stop.stop_requested() || new_windows_ || frames_ready_ || resend_; });
            changed = std::exchange(new_windows_, std::nullopt);
            screen = std::exchange(screen_frame_, std::nullopt);
            frames_ready_ = false;
            resend = std::exchange(resend_, false);
            if (resend) layout = last_layout_;
        }
        if (stop.stop_requested()) break;
        if (h264_) {  // one stream of the whole VDD: H264Sender sends it; here only the layout
            if (resend) {
                layout_(layout);
                h264_->request_keyframe();  // the viewer starts over from nothing
            }
            continue;
        }
        if (changed) sync(*changed);
        if (resend) {
            layout_(layout);
            for (auto& [id, w] : windows_) {
                if (!w->last_payload.empty()) send_(w->last_header, w->last_payload);
            }
        }

        const auto now = Clock::now();
        for (auto& [id, w] : windows_) {
            if (!w->capture || now < w->next_send) continue;  // too soon: the newest one goes out next time
            std::optional<CapturedFrame> f;
            {
                std::lock_guard lock(w->mutex);
                f = std::exchange(w->pending, std::nullopt);
            }
            if (!f) continue;
            send_picture(*w, f->bgra.data(), f->width, f->height, f->stride, 0, 0, /*has_alpha=*/true, f->t_capture_us);
            w->next_send = std::max(w->next_send + min_interval, now);
        }
        if (screen) {
            // Windows without their own capture: their part of the VDD picture, opaque.
            for (auto& [id, w] : windows_) {
                if (w->capture || now < w->next_send) continue;
                w->next_send = std::max(w->next_send + min_interval, now);
                const display::Rect part{std::max(w->frame.left, vdd_.left), std::max(w->frame.top, vdd_.top),
                                         std::min(w->frame.right, vdd_.left + screen->width),
                                         std::min(w->frame.bottom, vdd_.top + screen->height)};
                if (part.width() <= 0 || part.height() <= 0) continue;
                const std::uint8_t* first = screen->bgra.data() +
                                            static_cast<size_t>(part.top - vdd_.top) * screen->stride +
                                            static_cast<size_t>(part.left - vdd_.left) * 4;
                send_picture(*w, first, part.width(), part.height(), screen->stride, part.left - w->frame.left,
                             part.top - w->frame.top, /*has_alpha=*/false, screen->t_capture_us);
            }
        }

        const auto elapsed = Clock::now() - period_start;
        if (elapsed >= kLogEvery) {
            if (period_frames > 0) {
                const double seconds = std::chrono::duration<double>(elapsed).count();
                const double n = static_cast<double>(period_frames);
                log::info("desk video: {:.1f} pictures/s, {:.0f}k px each, encode {:.1f} ms, {:.1f} Mbit/s",
                          n / seconds, static_cast<double>(period_pixels) / n / 1000.0, period_encode_ms / n,
                          static_cast<double>(period_bytes) * 8.0 / seconds / 1e6);
            }
            period_start = Clock::now();
            period_frames = period_bytes = period_pixels = 0;
            period_encode_ms = 0;
        }
    }
}

}  // namespace kvm
