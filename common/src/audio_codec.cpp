// SPDX-License-Identifier: Apache-2.0

#include "kvm/audio_codec.h"

#include <opus.h>

#include <format>

namespace kvm {

namespace {

// Enough for any 10 ms Opus frame (the format's own limit is 1275 bytes per frame).
constexpr int kMaxPacket = 1500;

}  // namespace

void AudioEncoder::Free::operator()(OpusEncoder* e) const {
    opus_encoder_destroy(e);
}

void AudioDecoder::Free::operator()(OpusDecoder* d) const {
    opus_decoder_destroy(d);
}

AudioEncoder::~AudioEncoder() = default;
AudioDecoder::~AudioDecoder() = default;

std::unique_ptr<AudioEncoder> AudioEncoder::create(const Options& options, std::string& why) {
    int error = OPUS_OK;
    auto out = std::unique_ptr<AudioEncoder>(new AudioEncoder());
    out->encoder_.reset(
        opus_encoder_create(audio::kRate, audio::kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error));
    if (error != OPUS_OK || !out->encoder_) {
        why = std::format("audio: the Opus encoder can't start: {}", opus_strerror(error));
        return nullptr;
    }
    OpusEncoder* e = out->encoder_.get();
    if ((error = opus_encoder_ctl(e, OPUS_SET_BITRATE(options.bitrate))) != OPUS_OK ||
        (error = opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(options.complexity))) != OPUS_OK ||
        (error = opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC))) != OPUS_OK ||
        (error = opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(options.loss_percent))) != OPUS_OK) {
        why = std::format("audio: Opus refuses {} bit/s, complexity {}: {}", options.bitrate, options.complexity,
                          opus_strerror(error));
        return nullptr;
    }
    return out;
}

bool AudioEncoder::encode(std::span<const float> frame, std::vector<std::uint8_t>& packet) {
    if (frame.size() != static_cast<std::size_t>(audio::kFrameFloats)) return false;
    packet.resize(kMaxPacket);
    const opus_int32 bytes =
        opus_encode_float(encoder_.get(), frame.data(), audio::kFrameSamples, packet.data(), kMaxPacket);
    if (bytes < 0) {
        packet.clear();
        return false;
    }
    packet.resize(static_cast<std::size_t>(bytes));
    return true;
}

int AudioEncoder::delay_samples() const {
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder_.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    return lookahead;
}

std::unique_ptr<AudioDecoder> AudioDecoder::create(std::string& why) {
    int error = OPUS_OK;
    auto out = std::unique_ptr<AudioDecoder>(new AudioDecoder());
    out->decoder_.reset(opus_decoder_create(audio::kRate, audio::kChannels, &error));
    if (error != OPUS_OK || !out->decoder_) {
        why = std::format("audio: the Opus decoder can't start: {}", opus_strerror(error));
        return nullptr;
    }
    return out;
}

bool AudioDecoder::decode(std::span<const std::uint8_t> packet, std::vector<float>& pcm) {
    if (packet.empty()) return false;
    pcm.resize(audio::kFrameFloats);
    const int samples = opus_decode_float(decoder_.get(), packet.data(), static_cast<opus_int32>(packet.size()),
                                          pcm.data(), audio::kFrameSamples, 0);
    return samples == audio::kFrameSamples;
}

bool AudioDecoder::conceal(std::vector<float>& pcm) {
    pcm.resize(audio::kFrameFloats);
    const int samples = opus_decode_float(decoder_.get(), nullptr, 0, pcm.data(), audio::kFrameSamples, 0);
    return samples == audio::kFrameSamples;
}

}  // namespace kvm
