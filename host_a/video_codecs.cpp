// SPDX-License-Identifier: Apache-2.0

#include "video_codecs.h"

#include "../client_b/h264_decoder.h"
#include "h264_encoder.h"
#include "kvm/codec.h"
#include "kvm/gpu_device.h"
#include "kvm/log.h"

namespace kvm {

VideoCodecs probe_video_codecs() {
    VideoCodecs c;
    if (H264Decoder::available()) {
        c.decode.emplace_back(codec::kH264);
        c.decode.emplace_back(codec::kH264Desk);  // desk mode from one picture (client_b/desk_view.h)
    }
    c.decode.emplace_back(codec::kJpeg);
    if (const auto gpu = GpuDevice::create(); gpu && H264Encoder::available(*gpu)) c.encode.emplace_back(codec::kH264);
    c.encode.emplace_back(codec::kJpeg);
    log::info("video: this PC decodes {}, encodes {}", codec::has(c.decode, codec::kH264) ? "H.264 and JPEG" : "JPEG",
              codec::has(c.encode, codec::kH264) ? "H.264 (on the GPU) and JPEG" : "JPEG");
    return c;
}

}  // namespace kvm
