// SPDX-License-Identifier: Apache-2.0

// The video threads for H.264 (roadmap P2.4c): the newest captured GPU frame -> NV12 -> the hardware
// encoder -> the video channel. The H.264 counterpart of VideoSender (JPEG), with the same pacing:
// at most max_fps leave, and the newest capture wins. A still window is sent again every 500 ms, as a
// new (almost empty) picture: the same access unit twice would break the viewer's decoder.
//
//   auto gpu = kvm::GpuDevice::create();
//   auto sender = kvm::H264Sender::create(gpu, w, h, send, {.max_fps = 30, .bitrate = 8'000'000}, why);
//   auto capture = kvm::ScreenCapture::for_monitor(monitor, true, gpu);
//   capture->start_gpu([&](kvm::GpuFrame&& f) { sender->put(std::move(f)); });
//
// A frame of another size starts a new encoder (its stream begins with a keyframe). Keyframes go out
// when the viewer asks (request_keyframe), and every keyframe_interval_s as a safety net.
//
// The bitrate follows the link (roadmap P2.6, kvm/bitrate_control.h): down while encoded pictures
// wait to be sent or the viewer's latency rises (on_viewer_stats), back up when the link keeps up.
// Where the encoder can't change its bitrate while running (Intel's can't), a new encoder at the new
// bitrate is made on a thread of its own while the old one goes on, then takes over (its stream
// starts with a keyframe); a step up at most once per kSwitchGap.
// A picture that waited more than kSkipAfter is not sent, nor anything after it until a keyframe:
// the viewer sees the gap in seq and waits for that keyframe, so it never shows a broken picture
// and the delay doesn't pile up.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "capture.h"
#include "h264_encoder.h"
#include "kvm/gpu_device.h"
#include "kvm/queue.h"
#include "nv12_converter.h"
#include "video_sender.h"

namespace kvm {

class H264Sender {
public:
    struct Options {
        int max_fps = 30;
        int bitrate = 8'000'000;  // the most it uses; BitrateControl goes lower on a full link
        int min_bitrate = 1'000'000;
        int keyframe_interval_s = 10;
    };

    // Starts the threads, with an encoder for width x height frames on `gpu`. nullptr with `why` set
    // if there is no hardware H.264 encoder.
    static std::unique_ptr<H264Sender> create(std::shared_ptr<GpuDevice> gpu, int width, int height,
                                              VideoSender::SendFn send, Options options, std::string& why);
    ~H264Sender();
    H264Sender(const H264Sender&) = delete;
    H264Sender& operator=(const H264Sender&) = delete;

    // A captured frame (any thread; the capture's).
    void put(GpuFrame&& frame) { frames_.put(std::move(frame)); }
    // The next picture becomes a keyframe (the viewer asked, proto::VideoKeyframe). At most one in
    // kMinKeyframeGap: a viewer can't make every picture a keyframe.
    void request_keyframe();
    // The viewer's capture-to-screen latency, p50 of the last second (proto::VideoStats). Any thread.
    void on_viewer_stats(int latency_ms) { viewer_latency_ms_ = latency_ms; }
    // Stops both threads. Also done by the destructor.
    void stop();

    static constexpr std::chrono::milliseconds kMinKeyframeGap{250};
    static constexpr std::chrono::milliseconds kSkipAfter{1000};
    static constexpr std::chrono::milliseconds kSwitchGap{1000};

    std::uint64_t frames_sent() const { return sent_; }
    // Pictures not sent because they waited more than kSkipAfter (and the ones after them).
    std::uint64_t frames_skipped() const { return skipped_; }
    // The bitrate the encoder runs at now (BitrateControl's wish is in the log).
    int bitrate() const { return bitrate_; }

private:
    H264Sender() = default;
    void feed_loop(std::stop_token stop);  // frames -> encoder
    void send_loop(std::stop_token stop);  // encoder packets -> channel
    std::shared_ptr<H264Encoder> encoder_for(int width, int height);
    // Toward BitrateControl's bitrate: set on the encoder, or a new encoder made in the background.
    void follow_bitrate(int target);

    std::future<std::shared_ptr<H264Encoder>> next_encoder_;  // feed thread: being made at next_bitrate_
    int next_bitrate_ = 0;
    std::chrono::steady_clock::time_point last_switch_{};

    std::shared_ptr<GpuDevice> gpu_;
    VideoSender::SendFn send_;
    Options options_;
    std::unique_ptr<Nv12Converter> nv12_;
    LatestSlot<GpuFrame> frames_;

    std::mutex mutex_;  // encoder_ (replaced on a size change), read by the send thread
    std::condition_variable encoder_changed_;
    std::shared_ptr<H264Encoder> encoder_;
    int refused_w_ = 0;  // the size an encoder couldn't be made for
    int refused_h_ = 0;
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<bool> channel_closed_{false};
    std::atomic<int> bitrate_{0};
    std::atomic<int> target_{0};
    std::atomic<int> longest_wait_ms_{0};      // the send thread's, since the last tick
    std::atomic<std::int64_t> send_bytes_{0};  // sent since the last tick, and the time it took
    std::atomic<std::int64_t> send_us_{0};
    std::atomic<int> viewer_latency_ms_{-1};   // -1: none since the last tick
    std::atomic<std::uint64_t> skipped_{0};
    std::chrono::steady_clock::time_point last_keyframe_request_{};  // mutex_

    std::jthread feed_thread_;  // last: they start after the members above exist
    std::jthread send_thread_;
};

}  // namespace kvm
