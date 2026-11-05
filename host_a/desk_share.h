// SPDX-License-Identifier: Apache-2.0

// Desk mode, window owner (Temo): sends the windows on the virtual display (VDD) to the other PC,
// which draws them at the same place on its screen (client_b/desk_view).
//
//   kvm::DeskShare share(*vdd_rect, region, send_frame, send_layout, {.jpeg_quality = 80, .max_fps = 30});
//   share.resend();   // after a reconnect: the peer starts from nothing
//
// Pixel-exact, without anything of this PC's background: every window is captured on its own (WGC
// window capture), so the picture is the window and nothing else, with the transparency of its
// rounded corners. Where each window is goes separately as desk_layout (control channel) whenever
// one moves, opens, closes or changes order, so dragging a window costs a tiny message, not a new
// picture, and the picture never lags behind its frame. A window that can't be captured on its own
// is cut out of a capture of the whole VDD instead.
//
// Pictures: JPEG 4:4:4 (sharp text) + run-length alpha, at most max_fps per window, only when the
// window's content changed. Every 5 s the sender logs fps, size and bitrate.
//
// With Options::codec "h264" (roadmap P2.7, when the viewer lists "h264-desk"): one H.264 stream of
// the whole VDD instead (h264_sender.h; frames with layer "desk", window 0, the VDD's size). The
// viewer cuts each window out of it at its desk_layout rect, rounded by its `radius`. JPEG stays the
// fallback when this PC has no hardware encoder.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "desk_tracker.h"
#include "h264_sender.h"
#include "kvm/desk_region.h"
#include "kvm/protocol.h"
#include "video_sender.h"
#include "virtual_display.h"

namespace kvm {

class DeskShare {
public:
    using SendFn = VideoSender::SendFn;
    using LayoutFn = std::function<void(const proto::DeskLayout&)>;

    struct Options {
        int jpeg_quality = 80;
        int max_fps = 30;            // per window (JPEG), or for the whole VDD (H.264)
        std::string codec = "jpeg";  // "h264": one stream of the whole VDD (kvm/codec.h choose_desk)
        int h264_bitrate = 8'000'000;
    };

    // `vdd` must be attached; `region` must already hold it and outlive the share.
    DeskShare(const VirtualDisplay::MonitorRect& vdd, desk::Region& region, SendFn send, LayoutFn layout,
              Options options);
    ~DeskShare();

    DeskShare(const DeskShare&) = delete;
    DeskShare& operator=(const DeskShare&) = delete;

    // The peer lost what it showed (e.g. reconnected): send the layout and every picture again.
    void resend();
    // From the viewer (proto::VideoKeyframe / VideoStats): H.264 only.
    void request_keyframe();
    void on_viewer_stats(int latency_ms);
    // "h264" or "jpeg": what the pictures go as.
    const char* codec() const { return h264_ ? "h264" : "jpeg"; }

    std::uint64_t frames_sent() const { return sent_; }
    const DeskTracker& tracker() const { return *tracker_; }

private:
    struct Window;

    void on_windows(const std::vector<DeskTracker::Window>& windows);  // tracker thread
    void run(std::stop_token stop);
    void sync(const std::vector<DeskTracker::Window>& windows);  // sender thread: captures follow the windows
    void notify_frames();

    desk::Region& region_;
    SendFn send_;
    LayoutFn layout_;
    Options options_;
    display::Rect vdd_;
    std::atomic<std::uint64_t> sent_{0};

    std::mutex mutex_;  // everything below up to windows_
    std::condition_variable cv_;
    std::optional<std::vector<DeskTracker::Window>> new_windows_;
    proto::DeskLayout last_layout_;
    bool frames_ready_ = false;
    bool resend_ = false;
    std::optional<CapturedFrame> screen_frame_;  // the whole VDD, for windows that can't be captured alone

    std::map<std::uint64_t, std::unique_ptr<Window>> windows_;  // sender thread only
    std::unique_ptr<ScreenCapture> screen_;                       // sender thread only

    // H.264: the whole VDD on the GPU, one encoder.
    std::unique_ptr<H264Sender> h264_;
    std::unique_ptr<ScreenCapture> gpu_capture_;

    std::jthread thread_;
    std::unique_ptr<DeskTracker> tracker_;  // last: its callbacks need everything above
};

}  // namespace kvm
