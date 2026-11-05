// SPDX-License-Identifier: Apache-2.0

#include "window_stream.h"

#include <optional>
#include <string>
#include <utility>

#include "jpeg_decoder.h"
#include "kvm/log.h"

namespace kvm {

WindowStream::WindowStream(ViewerWindow& viewer, Feedback feedback)
    : viewer_(viewer), target_(viewer), h264_(target_, "window", std::move(feedback)) {
    jpeg_thread_ = std::jthread([this] { jpeg_loop(); });
}

WindowStream::~WindowStream() {
    close();
    // jthread members join here.
}

void WindowStream::push(net::Frame frame) {
    if (frame.header.codec == "h264") {
        h264_.push(std::move(frame));
    } else {
        latest_.put(std::move(frame));
    }
}

void WindowStream::prepare(const std::string& codec) {
    if (codec == "h264") h264_.prepare();
}

void WindowStream::close() {
    latest_.close();
    h264_.close();
}

std::optional<std::int64_t> WindowStream::local_time(std::int64_t t_capture_us) const {
    // t_capture_us is the owner's clock; subtracting the offset maps it to ours (the same as
    // Connection::peer_to_local_us).
    if (!has_clock_offset_) return std::nullopt;
    return t_capture_us - clock_offset_us_;
}

void WindowStream::jpeg_loop() {
    JpegDecoder decoder;
    BgraImage spare;  // reused: present() hands back a spare buffer
    bool codec_warned = false;
    std::uint64_t decoded_frames = 0;

    while (auto frame = latest_.take()) {
        if (frame->header.codec != "jpeg") {
            if (!codec_warned) {
                log::info("video: codec '{}' is not shown (counted for MBPS only)", frame->header.codec);
                codec_warned = true;
            }
            continue;
        }
        const std::int64_t started = steady_now_us();
        if (!decoder.decode(frame->payload, spare)) {
            log::warn("video: frame {} dropped: {}", frame->header.seq, decoder.last_error());
            continue;
        }
        const std::int64_t decoded = steady_now_us();
        FrameInfo info{.seq = frame->header.seq,
                       .t_capture_local_us = local_time(frame->header.t_capture_us),
                       .bytes = frame->payload.size(),
                       .decode_us = decoded - started,
                       .decoded_at_us = decoded};
        viewer_.present(spare, info);
        ++decoded_frames;
    }
    if (decoded_frames > 0) log::info("video: jpeg stopped after {} decoded frames", decoded_frames);
}

}  // namespace kvm
