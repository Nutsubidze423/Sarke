// SPDX-License-Identifier: Apache-2.0

#include "h264_stream.h"

#include <string>
#include <utility>

#include "h264_decoder.h"
#include "kvm/annexb.h"
#include "kvm/log.h"
#include "kvm/video_stats.h"
#include "picture_converter.h"

namespace kvm {

H264Stream::H264Stream(Target& target, std::string layer, Feedback feedback)
    : target_(target), layer_(std::move(layer)), feedback_(std::move(feedback)) {
    thread_ = std::jthread([this] { run(); });
}

H264Stream::~H264Stream() {
    close();
    // thread_ joins here.
}

void H264Stream::push(net::Frame frame) {
    queue_.push(std::move(frame));  // full: the oldest goes, and the gap waits for a keyframe
}

std::optional<std::int64_t> H264Stream::local_time(std::int64_t t_capture_us) const {
    // t_capture_us is the owner's clock; subtracting the offset maps it to ours (the same as
    // Connection::peer_to_local_us).
    if (!has_clock_offset_) return std::nullopt;
    return t_capture_us - clock_offset_us_;
}

void H264Stream::run() {
    std::shared_ptr<GpuDevice> gpu;  // the target's: the decoder and converter work on it
    std::unique_ptr<H264Decoder> decoder;
    std::unique_ptr<PictureConverter> converter;
    BgraImage spare;
    std::optional<std::uint32_t> last_seq;
    bool need_keyframe = true;
    bool decoder_missing = false;
    std::uint64_t decoded = 0, waited = 0;
    std::chrono::steady_clock::time_point last_request{};

    // The decoder on the target's current device (a new one after the device was lost).
    const auto ensure_decoder = [&] {
        const auto now = target_.gpu();
        if (now == gpu && (decoder || decoder_missing)) return false;
        gpu = now;
        converter.reset();
        std::string why;
        decoder = H264Decoder::create(gpu && gpu->has_video() ? gpu : nullptr, why);
        decoder_missing = !decoder;
        if (!decoder) log::error("video: {}: h264 frames are not shown", why);
        converter = std::make_unique<PictureConverter>(gpu);
        return decoder != nullptr;  // a new decoder: it starts at a keyframe
    };
    // Waiting for a keyframe: ask the owner for one, now and then.
    const auto ask_for_keyframe = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (!feedback_.keyframe || now - last_request < kKeyframeRequestGap) return;
        last_request = now;
        feedback_.keyframe();
    };
    // How the frames arrive, for the owner's bitrate: once a second while some are shown.
    auto next_stats = std::chrono::steady_clock::now() + kStatsEvery;
    const auto report = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (!feedback_.stats || now < next_stats) return;
        next_stats = now + kStatsEvery;
        const StatsSummary s = video_stats().summary(layer_);
        if (s.frames == 0 || !s.latency_p50_ms || decoded == 0) return;
        feedback_.stats(static_cast<int>(*s.latency_p50_ms), static_cast<int>(s.fps + 0.5));
    };

    for (;;) {
        auto frame = queue_.pop_for(std::chrono::milliseconds(100));
        report();
        if (!frame) {
            if (queue_.closed()) break;
            if (prepare_.exchange(false)) ensure_decoder();  // proto::VideoConfig: be ready
            continue;
        }
        const proto::FrameHeader& h = frame->header;
        // A missing frame: the ones after it refer to a picture that isn't there.
        if (last_seq && h.seq != *last_seq + 1 && !need_keyframe) {
            log::info("video: {} h264 frame {} after {}: waiting for a keyframe", layer_, h.seq, *last_seq);
            need_keyframe = true;
        }
        last_seq = h.seq;
        if (const auto why = annexb::check(frame->payload, h.keyframe); !why.empty()) {
            log::warn("video: {} h264 frame {} dropped: {}", layer_, h.seq, why);
            need_keyframe = true;
            ask_for_keyframe();
            continue;
        }
        if (need_keyframe && !h.keyframe) {
            ++waited;
            ask_for_keyframe();
            continue;
        }
        if (ensure_decoder() && !h.keyframe) {  // a new decoder starts at a keyframe
            need_keyframe = true;
            ++waited;
            ask_for_keyframe();
            continue;
        }
        if (!decoder) continue;
        need_keyframe = false;

        const std::int64_t started = steady_now_us();
        const auto errors = decoder->errors();
        auto picture = decoder->decode(frame->payload, h.t_capture_us);
        if (decoder->errors() > errors) {  // the decoder refused it: what follows can't be trusted
            need_keyframe = true;
            ask_for_keyframe();
            continue;
        }
        if (!picture) continue;
        ++decoded;
        if (queue_.size() > 0) continue;  // more are waiting: show the newest picture, not this one

        FrameInfo info{.seq = h.seq,
                       .t_capture_local_us = local_time(h.t_capture_us),
                       .bytes = frame->payload.size(),
                       .codec = "h264"};
        if (picture->texture) {
            auto bgra = converter->to_texture(*picture);
            const int w = picture->width, ht = picture->height;
            picture.reset();  // the decoder has its texture back
            if (!bgra) continue;
            info.decoded_at_us = steady_now_us();
            info.decode_us = info.decoded_at_us - started;
            target_.show(std::move(bgra), w, ht, info);
        } else if (PictureConverter::to_bgra(*picture, spare)) {
            info.decoded_at_us = steady_now_us();
            info.decode_us = info.decoded_at_us - started;
            target_.show(spare, info);
        }
    }
    if (decoded > 0 || waited > 0) {
        log::info("video: {} h264 stopped after {} decoded frames ({} skipped waiting for a keyframe)", layer_, decoded,
                  waited);
    }
}

}  // namespace kvm
