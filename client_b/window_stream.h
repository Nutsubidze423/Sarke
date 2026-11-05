// SPDX-License-Identifier: Apache-2.0

// The viewer's frames (a window stream, not desk mode), decoded and shown in a ViewerWindow. Fed by
// whichever thread reads the video channel: VideoReceiver, or WindowLink's FrameRouter.
//   - "jpeg" frames go into a LatestSlot; a thread decodes the newest one and presents it. A slow
//     decoder drops old frames instead of queueing them.
//   - "h264" frames go to an H264Stream (h264_stream.h): every one decoded in order on the viewer's
//     GPU, the newest picture shown, keyframe requests and stats back to the owner (Feedback).
//   - Other codecs (e.g. kvm_host --test-video) count toward the HUD's MBPS but are not shown.
//
//   kvm::WindowStream stream(viewer, {.keyframe = [&] { send(proto::VideoKeyframe{}); },
//                                     .stats = [&](int ms, int fps) { send(proto::VideoStats{ms, fps}); }});
//   while (auto frame = channel.recv_frame()) stream.push(std::move(*frame));   // one thread
//   stream.set_clock_offset_us(stats.clock_offset_us);                         // any thread
//   stream.prepare("h264");                                                    // proto::VideoConfig
//
// Destroying it stops and joins its threads (frames still waiting are dropped).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "h264_stream.h"
#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_window.h"

namespace kvm {

class WindowStream {
public:
    using Feedback = H264Stream::Feedback;

    explicit WindowStream(ViewerWindow& viewer, Feedback feedback = {});
    ~WindowStream();
    WindowStream(const WindowStream&) = delete;
    WindowStream& operator=(const WindowStream&) = delete;

    // A frame from the video channel (window layer). Call from one thread.
    void push(net::Frame frame);
    // No more frames: the threads finish what is waiting, then stop. Idempotent.
    void close();

    // Frames of `codec` are coming (proto::VideoConfig): for "h264" the decoder starts now, not with
    // the first frame. Any thread.
    void prepare(const std::string& codec);

    // The window owner's clock minus ours (net::LinkStats::clock_offset_us), any thread. Until it is
    // set the viewer shows latency as unknown.
    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
        h264_.set_clock_offset_us(offset);
    }

private:
    // H.264 pictures go to the viewer.
    class ViewerTarget : public H264Stream::Target {
    public:
        explicit ViewerTarget(ViewerWindow& viewer) : viewer_(viewer) {}
        std::shared_ptr<GpuDevice> gpu() override { return viewer_.gpu(); }
        void show(Microsoft::WRL::ComPtr<ID3D11Texture2D> bgra, int width, int height, const FrameInfo& info) override {
            viewer_.present_texture(std::move(bgra), width, height, info);
        }
        void show(BgraImage& image, const FrameInfo& info) override { viewer_.present(image, info); }

    private:
        ViewerWindow& viewer_;
    };

    void jpeg_loop();
    std::optional<std::int64_t> local_time(std::int64_t t_capture_us) const;

    ViewerWindow& viewer_;
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};
    LatestSlot<net::Frame> latest_;
    ViewerTarget target_;
    H264Stream h264_;  // after target_: its thread starts at once

    // Declared last: joined first on destruction, while everything above is still alive.
    std::jthread jpeg_thread_;
};

}  // namespace kvm
