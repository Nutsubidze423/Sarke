// SPDX-License-Identifier: Apache-2.0

// audio_codec_test: kvm/audio_codec.h (roadmap P3.2). Two seconds of a music-like signal (a chord over
// a bass note, with a slow tremolo) through the Opus encoder and decoder:
//   - one packet per 10 ms frame, about bitrate x 10 ms in size;
//   - decoded, it matches the input after the codec's delay (SNR above kMinSnrDb);
//   - with every 10th packet lost, conceal() fills each gap at about the level that was lost, and the
//     second frame after a gap decodes as well as without losses. Concealment continues the sound but
//     not its exact waveform, so the gap itself is judged by level, not SNR.
// Measured and printed, not asserted:
//   - the same with loss_percent 0: the frames after a gap take ~8 frames to recover (CELT predicts
//     each frame's band energies from the last one), which is why the default is 5;
//   - with raw Opus, what in-band FEC adds for a lost frame: in this low-delay mode (CELT only), and in
//     speech mode at 32 kbit/s (SILK), as a check that the measurement can see FEC at all.

#include <opus.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kvm/audio_codec.h"

namespace {

using kvm::audio::kChannels;
using kvm::audio::kFrameFloats;
using kvm::audio::kFrameSamples;
using kvm::audio::kRate;

constexpr int kFrames = 200;            // 2 s
constexpr int kFirstLoss = 20;          // then every kLossEvery-th frame is lost
constexpr int kLossEvery = 10;
constexpr double kMinSnrDb = 15.0;      // decoded against the input, 128 kbit/s
constexpr double kConcealLevelDb = 6.0;  // concealed frames' level within this of what was lost

int failed = 0;

void check(bool ok, std::string_view what) {
    std::printf("%s %.*s\n", ok ? "[ok]  " : "[FAIL]", static_cast<int>(what.size()), what.data());
    if (!ok) ++failed;
}

// Interleaved stereo: A3, C#4, E4 over A2, the right channel a little out of phase, with a 3 Hz tremolo.
std::vector<float> music() {
    std::vector<float> x(static_cast<std::size_t>(kFrames) * kFrameFloats);
    const double notes[] = {220.0, 277.18, 329.63, 110.0};
    for (std::size_t n = 0; n < x.size() / kChannels; ++n) {
        const double t = static_cast<double>(n) / kRate;
        const double tremolo = 0.75 + 0.25 * std::sin(2.0 * std::numbers::pi * 3.0 * t);
        for (int c = 0; c < kChannels; ++c) {
            double v = 0;
            for (const double f : notes) v += std::sin(2.0 * std::numbers::pi * f * t + c * 0.3);
            x[n * kChannels + c] = static_cast<float>(0.08 * tremolo * v);
        }
    }
    return x;
}

bool lost(int frame) {
    return frame >= kFirstLoss && (frame - kFirstLoss) % kLossEvery == 0;
}

// SNR (dB) of the decoded `y` against the input `x` over the frames `pick` selects, `delay` samples
// per channel later than the input.
template <class Pick>
double snr_db(const std::vector<float>& x, const std::vector<float>& y, int delay, Pick pick) {
    double signal = 0, noise = 0;
    for (int f = 0; f < kFrames; ++f) {
        if (!pick(f)) continue;
        for (int i = 0; i < kFrameFloats; ++i) {
            const long long n = static_cast<long long>(f) * kFrameFloats + i;
            const long long m = n - static_cast<long long>(delay) * kChannels;
            if (m < 0 || n >= static_cast<long long>(y.size())) continue;
            const double ref = x[static_cast<std::size_t>(m)];
            const double err = y[static_cast<std::size_t>(n)] - ref;
            signal += ref * ref;
            noise += err * err;
        }
    }
    return noise > 0 ? 10.0 * std::log10(signal / noise) : 200.0;
}

// The RMS level (dB) of `y` over the frames `pick` selects.
template <class Pick>
double level_db(const std::vector<float>& y, Pick pick) {
    double sum = 0;
    std::size_t n = 0;
    for (int f = 0; f < kFrames; ++f) {
        if (!pick(f)) continue;
        for (int i = 0; i < kFrameFloats; ++i, ++n) {
            const double v = y[static_cast<std::size_t>(f) * kFrameFloats + i];
            sum += v * v;
        }
    }
    return n > 0 && sum > 0 ? 10.0 * std::log10(sum / static_cast<double>(n)) : -200.0;
}

// The delay (samples per channel, up to one frame) at which `y` matches `x` best.
int best_delay(const std::vector<float>& x, const std::vector<float>& y) {
    int best = 0;
    double best_snr = -1e9;
    for (int d = 0; d <= kFrameSamples; ++d) {
        const double s = snr_db(x, y, d, [](int f) { return f >= 2 && f < 60; });
        if (s > best_snr) {
            best_snr = s;
            best = d;
        }
    }
    return best;
}

// Raw Opus with in-band FEC on: the lost frames' SNR when each is recovered from the next packet's
// FEC (`use_fec`) or concealed.
double lost_frames_snr(const std::vector<float>& x, int application, int bitrate, int signal, bool use_fec) {
    int error = 0;
    OpusEncoder* enc = opus_encoder_create(kRate, kChannels, application, &error);
    OpusDecoder* dec = opus_decoder_create(kRate, kChannels, &error);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(signal));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(20));
    opus_int32 delay = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&delay));
    std::vector<std::vector<unsigned char>> packets(kFrames, std::vector<unsigned char>(1500));
    for (int f = 0; f < kFrames; ++f) {
        const int bytes = opus_encode_float(enc, x.data() + static_cast<std::size_t>(f) * kFrameFloats, kFrameSamples,
                                            packets[f].data(), 1500);
        packets[f].resize(bytes > 0 ? static_cast<std::size_t>(bytes) : 0);
    }
    std::vector<float> y(x.size());
    for (int f = 0; f < kFrames; ++f) {
        float* out = y.data() + static_cast<std::size_t>(f) * kFrameFloats;
        if (lost(f) && use_fec && f + 1 < kFrames) {
            const auto& next = packets[f + 1];
            opus_decode_float(dec, next.data(), static_cast<opus_int32>(next.size()), out, kFrameSamples, 1);
        } else if (lost(f)) {
            opus_decode_float(dec, nullptr, 0, out, kFrameSamples, 0);
        } else {
            opus_decode_float(dec, packets[f].data(), static_cast<opus_int32>(packets[f].size()), out, kFrameSamples,
                              0);
        }
    }
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
    return snr_db(x, y, delay, lost);
}

struct Stream {
    bool ok = true;  // every frame encoded, decoded or concealed
    std::size_t bytes = 0;
    int delay = 0;               // the encoder's, samples per channel
    std::vector<float> clean;    // every packet decoded
    std::vector<float> lossy;    // the lost ones concealed
};

// `x` through the wrapper, decoded twice: with every packet, and with the lost() ones concealed.
Stream run(const std::vector<float>& x, int loss_percent, std::string& why) {
    Stream out;
    auto encoder = kvm::AudioEncoder::create({.bitrate = 128'000, .loss_percent = loss_percent}, why);
    auto decoder = kvm::AudioDecoder::create(why);
    auto lossy = kvm::AudioDecoder::create(why);
    if (!encoder || !decoder || !lossy) {
        out.ok = false;
        return out;
    }
    out.delay = encoder->delay_samples();
    out.clean.resize(x.size());
    out.lossy.resize(x.size());
    std::vector<float> pcm;
    std::vector<std::uint8_t> packet;
    for (int f = 0; f < kFrames; ++f) {
        const auto at = static_cast<std::ptrdiff_t>(f) * kFrameFloats;
        out.ok = encoder->encode(std::span<const float>(x.data() + at, kFrameFloats), packet) && out.ok;
        out.bytes += packet.size();
        out.ok = decoder->decode(packet, pcm) && out.ok;
        std::copy(pcm.begin(), pcm.end(), out.clean.begin() + at);
        out.ok = (lost(f) ? lossy->conceal(pcm) : lossy->decode(packet, pcm)) && out.ok;
        std::copy(pcm.begin(), pcm.end(), out.lossy.begin() + at);
    }
    return out;
}

}  // namespace

int main() {
    const std::vector<float> x = music();
    std::string why;
    auto encoder = kvm::AudioEncoder::create({}, why);
    auto decoder = kvm::AudioDecoder::create(why);
    check(encoder && decoder, "the encoder and decoder start" + (why.empty() ? "" : ": " + why));
    if (!encoder || !decoder) return 1;
    const bool refused = !kvm::AudioEncoder::create({.bitrate = 0}, why);
    check(refused, std::format("0 bit/s is refused ({})", why));
    std::vector<std::uint8_t> packet;
    check(!encoder->encode(std::span<const float>(x.data(), 100), packet), "a frame of the wrong size is refused");
    std::vector<float> pcm;
    check(!decoder->decode({}, pcm), "an empty packet is refused");

    const Stream s = run(x, kvm::AudioEncoder::Options{}.loss_percent, why);
    check(s.ok, std::format("{} frames of 10 ms encode, decode, and the lost ones are concealed", kFrames));
    const double per_packet = static_cast<double>(s.bytes) / kFrames;
    check(per_packet > 100 && per_packet < 220,
          std::format("{:.0f} bytes per packet on average, {:.0f} kbit/s", per_packet, per_packet * 8 * 100 / 1000));
    const int delay = best_delay(x, s.clean);
    check(delay == s.delay, std::format("the output is {} samples ({:.1f} ms) late, the encoder's own delay ({})",
                                        delay, delay * 1000.0 / kRate, s.delay));
    const double snr = snr_db(x, s.clean, delay, [](int f) { return f >= 2; });
    check(snr > kMinSnrDb, std::format("decoded against the input: SNR {:.1f} dB (more than {})", snr, kMinSnrDb));

    // The input's level over a lost frame, `delay` later: what the concealment stands in for.
    std::vector<float> shifted(x.size());
    for (std::size_t n = static_cast<std::size_t>(delay) * kChannels; n < x.size(); ++n) {
        shifted[n] = x[n - static_cast<std::size_t>(delay) * kChannels];
    }
    const double was = level_db(shifted, lost), concealed = level_db(s.lossy, lost);
    check(std::abs(concealed - was) < kConcealLevelDb,
          std::format("the concealed frames are at {:.1f} dB where {:.1f} dB was lost", concealed, was));
    const auto second_after = [](int f) { return f > kFirstLoss && lost(f - 2); };
    const double after = snr_db(x, s.lossy, delay, second_after);
    const double after_clean = snr_db(x, s.clean, delay, second_after);
    check(after > after_clean - 1.0,
          std::format("the second frame after a loss decodes normally: SNR {:.1f} dB ({:.1f} without losses)", after,
                      after_clean));
    const auto first_after = [](int f) { return f > kFirstLoss && lost(f - 1); };
    const Stream s0 = run(x, 0, why);
    std::printf(
        "after a loss, SNR of the 1st / 2nd frame: loss_percent %d %.1f / %.1f dB; loss_percent 0 %.1f / "
        "%.1f dB (%.1f dB without losses, %.1f with loss_percent 0)\n",
        kvm::AudioEncoder::Options{}.loss_percent, snr_db(x, s.lossy, delay, first_after), after,
        snr_db(x, s0.lossy, delay, first_after), snr_db(x, s0.lossy, delay, second_after), snr,
        snr_db(x, s0.clean, delay, [](int f) { return f >= 2; }));

    // What a frame costs this CPU (printed, not asserted): the whole signal 5 times, encode then decode.
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::vector<std::uint8_t>> packets;
    for (int rep = 0; rep < 5; ++rep) {
        for (int f = 0; f < kFrames; ++f) {
            encoder->encode(
                std::span<const float>(x.data() + static_cast<std::ptrdiff_t>(f) * kFrameFloats, kFrameFloats),
                packets.emplace_back());
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (const auto& p : packets) decoder->decode(p, pcm);
    const auto t2 = std::chrono::steady_clock::now();
    const auto per_frame_us = [&](auto d) {
        return std::chrono::duration<double, std::micro>(d).count() / static_cast<double>(packets.size());
    };
    std::printf("per 10 ms frame: encode %.0f us, decode %.0f us\n", per_frame_us(t1 - t0), per_frame_us(t2 - t1));

    // In-band FEC (raw Opus): measured, not asserted.
    const double fec_low = lost_frames_snr(x, OPUS_APPLICATION_RESTRICTED_LOWDELAY, 128'000, OPUS_SIGNAL_MUSIC, true);
    const double plc_low = lost_frames_snr(x, OPUS_APPLICATION_RESTRICTED_LOWDELAY, 128'000, OPUS_SIGNAL_MUSIC, false);
    const double fec_voip = lost_frames_snr(x, OPUS_APPLICATION_VOIP, 32'000, OPUS_SIGNAL_VOICE, true);
    const double plc_voip = lost_frames_snr(x, OPUS_APPLICATION_VOIP, 32'000, OPUS_SIGNAL_VOICE, false);
    std::printf(
        "in-band FEC, lost frames' SNR (FEC / concealment): low-delay 128 kbit/s %.1f / %.1f dB; "
        "speech mode 32 kbit/s %.1f / %.1f dB\n",
        fec_low, plc_low, fec_voip, plc_voip);

    std::printf(failed == 0 ? "audio_codec_test: all passed\n" : "audio_codec_test: %d FAILED\n", failed);
    return failed == 0 ? 0 : 1;
}
