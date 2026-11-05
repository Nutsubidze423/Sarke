// SPDX-License-Identifier: Apache-2.0

// Checks kvm/codec.h, the rule for which codec a window stream goes out in (roadmap P2.5): H.264
// only when this PC encodes it and the viewer decodes it, JPEG with --codec jpeg, for an older
// viewer (no list in its hello) and for one that can't decode H.264.
// Usage: codec_test.exe   (exit code 0 = all passed)

#include <cstdio>
#include <string>
#include <vector>

#include "kvm/codec.h"

namespace codec = kvm::codec;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    const std::vector<std::string> both = {"h264", "jpeg"}, jpeg_only = {"jpeg"}, none;

    check(codec::parse("auto") == codec::Choice::Auto && codec::parse("h264") == codec::Choice::Auto &&
              codec::parse("jpeg") == codec::Choice::Jpeg && !codec::parse("vp9") && !codec::parse(""),
          "--codec: auto and h264 mean auto, jpeg forces JPEG, anything else is refused");

    const auto best = codec::choose(codec::Choice::Auto, both, both);
    check(best.codec == "h264" && best.why == "both PCs can", "both PCs can: h264");
    check(codec::choose(codec::Choice::Jpeg, both, both).codec == "jpeg", "--codec jpeg: jpeg, whatever both can");
    const auto no_encoder = codec::choose(codec::Choice::Auto, jpeg_only, both);
    check(no_encoder.codec == "jpeg" && no_encoder.why == "no hardware H.264 encoder on this PC",
          "no hardware encoder here: jpeg");
    const auto older = codec::choose(codec::Choice::Auto, both, none);
    check(older.codec == "jpeg" && older.why == "the other PC is an older Sarke",
          "an older viewer (no list in its hello): jpeg, so it still shows the window");
    const auto cant = codec::choose(codec::Choice::Auto, both, jpeg_only);
    check(cant.codec == "jpeg" && cant.why == "the other PC can't decode H.264", "a viewer without H.264: jpeg");

    // Desk mode: one picture of the whole virtual display, only to a viewer that can draw it.
    const std::vector<std::string> desk = {"h264-desk", "h264", "jpeg"};
    check(codec::choose_desk(codec::Choice::Auto, both, desk).codec == "h264", "desk mode, both can: h264");
    const auto window_only = codec::choose_desk(codec::Choice::Auto, both, both);
    check(window_only.codec == "jpeg" && window_only.why == "the other PC can't show desk mode as H.264",
          "desk mode to a viewer that decodes H.264 but doesn't list h264-desk: jpeg");
    check(codec::choose_desk(codec::Choice::Jpeg, both, desk).codec == "jpeg" &&
              codec::choose_desk(codec::Choice::Auto, jpeg_only, desk).codec == "jpeg",
          "desk mode with --codec jpeg, or without an encoder: jpeg");

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
