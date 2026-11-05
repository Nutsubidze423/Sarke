// SPDX-License-Identifier: Apache-2.0

// An H.264 stream from the other PC, decoded on this PC and handed to whatever shows it: the window
// viewer (window_stream.h) or desk mode's overlay (desk_view.h, roadmap P2.7c).
//   - Frames go into a short queue; a thread of their own decodes every one in order (a picture refers
//     to the one before it) on the target's GPU (h264_decoder.h), and shows the newest picture.
//   - After a gap in `seq` (the queue overflowed, or the sender dropped one) or a frame that fails the
//     framing check (kvm/annexb.h) or the decoder, it waits for the next keyframe, and asks the owner
//     for one (Feedback::keyframe, at most every 500 ms).
//   - Once a second while it shows pictures, it reports the latency and fps of its layer to the owner
//     (Feedback::stats), which adapts its bitrate.
//
//   kvm::H264Stream stream(target, "window", {.keyframe = ..., .stats = ...});
//   stream.push(std::move(frame));                       // one thread (the channel's reader)
//
// Destroying it stops and joins its thread (frames still waiting are dropped).
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "jpeg_decoder.h"
#include "kvm/gpu_device.h"
#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_window.h"

namespace kvm {

class H264Stream {
public:
    // Where the pictures go; called on the stream's thread.
    class Target {
    public:
        virtual ~Target() = default;
        // The device to decode onto (when it has video support), or nullptr. A new pointer after the
        // old device was lost: the decoder follows, from a keyframe.
        virtual std::shared_ptr<GpuDevice> gpu() = 0;
        // A picture as a BGRA texture of gpu()'s device, `width` x `height` of it shown.
        virtual void show(Microsoft::WRL::ComPtr<ID3D11Texture2D> bgra, int width, int height,
                          const FrameInfo& info) = 0;
        // A picture decoded in software. Swap it out of `image`; what comes back is a spare buffer.
        virtual void show(BgraImage& image, const FrameInfo& info) = 0;
    };

    // What goes back to the window owner, called on the stream's thread. Either may be empty.
    struct Feedback {
        std::function<void()> keyframe;                        // the decoder needs a keyframe
        std::function<void(int latency_ms, int fps)> stats;  // once a second while pictures are shown
    };

    // `layer`: the kvm::video_stats() layer the stats report reads ("window" or "desk").
    H264Stream(Target& target, std::string layer, Feedback feedback = {});
    ~H264Stream();
    H264Stream(const H264Stream&) = delete;
    H264Stream& operator=(const H264Stream&) = delete;

    void push(net::Frame frame);
    // Frames are coming (proto::VideoConfig): the decoder starts now, not with the first frame.
    void prepare() { prepare_ = true; }
    // No more frames: the thread finishes what is waiting, then stops. Idempotent.
    void close() { queue_.close(); }

    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
    }

    static constexpr std::size_t kQueue = 30;  // a second at 30 fps; beyond that, wait for a keyframe

private:
    void run();
    std::optional<std::int64_t> local_time(std::int64_t t_capture_us) const;

    static constexpr auto kKeyframeRequestGap = std::chrono::milliseconds(500);
    static constexpr auto kStatsEvery = std::chrono::seconds(1);

    Target& target_;
    std::string layer_;
    Feedback feedback_;
    std::atomic<bool> prepare_{false};
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};
    BlockingQueue<net::Frame> queue_{kQueue};
    std::jthread thread_;  // last: joined first
};

}  // namespace kvm
