// SPDX-License-Identifier: Apache-2.0

// Which codec a window stream goes out in (roadmap P2.5). Each PC's hello lists what it can decode
// and encode (proto::Hello::video_decode / video_encode); the PC that sends a window picks:
//   - "h264" when it can encode H.264, the viewer can decode it, and --codec doesn't say jpeg;
//   - else "jpeg", which every Sarke can decode and encode. A viewer whose hello has no list (an
//     older Sarke) gets JPEG.
// Desk mode is the same with "h264-desk": one H.264 picture of the whole virtual display, which the
// viewer cuts the windows out of (roadmap P2.7). A viewer lists it once it can draw that.
//
//   const auto choice = kvm::codec::parse(arg);                       // --codec auto|h264|jpeg
//   const auto d = kvm::codec::choose(*choice, my_encode, peer_hello.video_decode);
//   log::info("video: windows go as {} ({})", d.codec, d.why);
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kvm::codec {

inline constexpr std::string_view kJpeg = "jpeg";
inline constexpr std::string_view kH264 = "h264";
inline constexpr std::string_view kH264Desk = "h264-desk";

// --codec: "auto" (the default) and "h264" both mean H.264 when both PCs can; "jpeg" forces JPEG.
enum class Choice { Auto, Jpeg };

inline std::optional<Choice> parse(std::string_view arg) {
    if (arg == "auto" || arg == kH264) return Choice::Auto;
    if (arg == kJpeg) return Choice::Jpeg;
    return std::nullopt;
}

struct Decision {
    std::string codec;  // "h264" or "jpeg"
    std::string why;    // for the log
};

inline bool has(const std::vector<std::string>& list, std::string_view codec) {
    return std::find(list.begin(), list.end(), codec) != list.end();
}

// The codec for windows this PC sends to a viewer that decodes `peer_decode` (its hello's list).
inline Decision choose(Choice choice, const std::vector<std::string>& my_encode,
                       const std::vector<std::string>& peer_decode) {
    if (choice == Choice::Jpeg) return {std::string(kJpeg), "--codec jpeg"};
    if (!has(my_encode, kH264)) return {std::string(kJpeg), "no hardware H.264 encoder on this PC"};
    if (!has(peer_decode, kH264)) {
        return {std::string(kJpeg),
                peer_decode.empty() ? "the other PC is an older Sarke" : "the other PC can't decode H.264"};
    }
    return {std::string(kH264), "both PCs can"};
}

// The same for desk mode: H.264 only to a viewer that lists "h264-desk"; else per-window JPEG.
inline Decision choose_desk(Choice choice, const std::vector<std::string>& my_encode,
                            const std::vector<std::string>& peer_decode) {
    if (choice == Choice::Jpeg) return {std::string(kJpeg), "--codec jpeg"};
    if (!has(my_encode, kH264)) return {std::string(kJpeg), "no hardware H.264 encoder on this PC"};
    if (!has(peer_decode, kH264Desk)) return {std::string(kJpeg), "the other PC can't show desk mode as H.264"};
    return {std::string(kH264), "both PCs can"};
}

}  // namespace kvm::codec
