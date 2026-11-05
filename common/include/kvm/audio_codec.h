// SPDX-License-Identifier: Apache-2.0

// The shared app's sound as Opus (roadmap P3.2): 48 kHz stereo, 10 ms frames of interleaved float
// samples (L R L R ..., kFrameSamples per channel), OPUS_APPLICATION_RESTRICTED_LOWDELAY so the codec
// itself adds as little delay as it can.
//
//   std::string why;
//   auto encoder = kvm::AudioEncoder::create({.bitrate = 128'000}, why);
//   auto decoder = kvm::AudioDecoder::create(why);
//   std::vector<std::uint8_t> packet;
//   std::vector<float> pcm;
//   encoder->encode(frame, packet);   // frame: kFrameFloats samples
//   decoder->decode(packet, pcm);     // the frame again, kFrameFloats samples
//   decoder->conceal(pcm);            // a frame that never came: Opus's packet-loss concealment
//
// No in-band FEC: it is a feature of Opus's speech coder (SILK), and this low-delay mode codes
// everything with its music coder (CELT); audio_codec_test measured no difference.
//
// Opus is an implementation detail: its headers aren't needed to use this.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace kvm {

namespace audio {
inline constexpr int kRate = 48'000;
inline constexpr int kChannels = 2;
inline constexpr int kFrameMs = 10;
inline constexpr int kFrameSamples = kRate * kFrameMs / 1000;  // per channel: 480
inline constexpr int kFrameFloats = kFrameSamples * kChannels;  // one frame, interleaved: 960
}  // namespace audio

class AudioEncoder {
public:
    struct Options {
        int bitrate = 128'000;  // bit/s
        int complexity = 5;     // 0..10: CPU against quality
        // Expected share of packets the decoder will conceal (late or lost), 0..100. Above 0, Opus leans
        // less on the previous frame: after a gap the second frame is back to normal, against ~8 frames
        // at 0, for 0.5 dB less SNR (audio_codec_test).
        int loss_percent = 5;
    };

    // nullptr with `why` set if Opus refuses the settings.
    static std::unique_ptr<AudioEncoder> create(const Options& options, std::string& why);
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // One frame (audio::kFrameFloats samples) into `packet`; false if Opus fails.
    bool encode(std::span<const float> frame, std::vector<std::uint8_t>& packet);
    // The delay the encoder adds (its lookahead), in samples per channel.
    int delay_samples() const;

private:
    AudioEncoder() = default;
    struct Free {
        void operator()(OpusEncoder* e) const;
    };
    std::unique_ptr<OpusEncoder, Free> encoder_;
};

class AudioDecoder {
public:
    static std::unique_ptr<AudioDecoder> create(std::string& why);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // A packet into one frame (audio::kFrameFloats samples in `pcm`); false if it doesn't decode.
    bool decode(std::span<const std::uint8_t> packet, std::vector<float>& pcm);
    // A frame for a packet that was lost or came too late, made up from what came before.
    bool conceal(std::vector<float>& pcm);

private:
    AudioDecoder() = default;
    struct Free {
        void operator()(OpusDecoder* d) const;
    };
    std::unique_ptr<OpusDecoder, Free> decoder_;
};

}  // namespace kvm
