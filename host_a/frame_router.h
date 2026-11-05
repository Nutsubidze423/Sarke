// SPDX-License-Identifier: Apache-2.0

// The video channel's only reader in WindowLink (Temo): desk-mode frames (layer "desk") go to the
// DeskView overlay, window-stream frames to the viewer window through a WindowStream (the newest
// JPEG, or every H.264 frame in order; latency from the owner's capture time).
//
//   kvm::FrameRouter router(*channel, viewer, &desk_view, feedback);   // WindowStream::Feedback
//   router.set_clock_offset_us(stats.clock_offset_us);
//   router.prepare("h264");                                  // proto::VideoConfig
//
// Destroying it closes the channel (the only way to wake recv_frame()) and joins its threads.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../client_b/desk_view.h"
#include "../client_b/viewer_window.h"
#include "../client_b/window_stream.h"
#include "kvm/video.h"

namespace kvm {

class FrameRouter {
public:
    // feedback: what the viewer sends back to the window owner (keyframe requests, video_stats).
    FrameRouter(net::FrameChannel& channel, ViewerWindow& viewer, DeskView* desk, WindowStream::Feedback feedback = {});
    ~FrameRouter();

    FrameRouter(const FrameRouter&) = delete;
    FrameRouter& operator=(const FrameRouter&) = delete;

    // The window owner's clock minus ours (net::LinkStats::clock_offset_us), any thread.
    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
        window_.set_clock_offset_us(offset);
        if (desk_) desk_->set_clock_offset_us(offset);
    }
    // The owner's window is coming as `codec` (proto::VideoConfig): get the decoder ready.
    void prepare(const std::string& codec) { window_.prepare(codec); }

private:
    void read_loop();

    net::FrameChannel& channel_;
    ViewerWindow& viewer_;
    DeskView* desk_;
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};
    WindowStream window_;  // window-stream frames

    std::jthread read_thread_;  // last: joined first
};

}  // namespace kvm
