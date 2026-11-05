// SPDX-License-Identifier: Apache-2.0

// audio_probe: can this PC capture one app's sound, and can the app be silenced locally while it is
// captured (roadmap P3.1, decision D8)? Microsoft documents per-process loopback capture
// (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK) for build 20348 and later; Windows 10 is 19041-19045.
//
//   audio_probe.exe                         the automatic probe (plays two quiet tones, ~15 s)
//   audio_probe.exe --out <dir>             the same, and each capture as a WAV in <dir>
//   audio_probe.exe --pid N [--seconds S] [--out file.wav] [--system]
//                                           capture one process tree (or the whole PC) by hand
//   audio_probe.exe --tone S [--hz F]       play a tone for S seconds (what the probe captures)
//
// The automatic probe starts two copies of itself that play tones (--tone): "the app" at 440 Hz and
// "another app" at 660 Hz. Then it captures:
//   1. the app's process tree only (per-process loopback), and the whole PC (endpoint loopback);
//   2. both again with the app's audio session muted (ISimpleAudioVolume, the volume mixer's mute);
//   3. both again with the app's session volume at -60 and -100 dB instead.
// The answers: does per-process capture work on this Windows build (the app's tone, not the other
// one)? With the session muted, does the capture still hear the app while the PC's output doesn't? If
// muting also silences the capture, P3.6 needs another way to keep the owner silent; 3. tells whether
// a very low session volume, captured as float and raised again, is one.

// The process-loopback types (audioclientactivationparams.h) are declared from NTDDI_WIN10_FE on.
#define NTDDI_VERSION 0x0A00000A

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "mmdevapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

using namespace std::chrono_literals;

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kToneHz = 440.0;   // the app
constexpr double kOtherHz = 660.0;  // another app, which a per-process capture must not hear
constexpr int kQuietDb[] = {60, 100};  // session volumes tried instead of the mute
constexpr double kToneAmplitude = 0.05;  // -26 dBFS: quiet on the speakers, clear in a capture

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h) CloseHandle(h);
    }
};
using unique_handle = std::unique_ptr<void, HandleCloser>;

void say(const std::string& line) {
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

std::string hr_text(HRESULT hr) {
    return std::format("0x{:08X}", static_cast<std::uint32_t>(hr));
}

// Windows' build and update number, e.g. "19045.6456".
std::string windows_build() {
    constexpr auto kKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    wchar_t build[32]{};
    DWORD size = sizeof(build);
    DWORD ubr = 0, ubr_size = sizeof(ubr);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size) !=
        ERROR_SUCCESS) {
        return "?";
    }
    RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &ubr_size);
    std::string out;
    for (const wchar_t* c = build; *c; ++c) out += static_cast<char>(*c);
    return std::format("{}.{}", out, ubr);
}

WAVEFORMATEX pcm16_format() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = kChannels;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = static_cast<WORD>(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

WAVEFORMATEX float_format() {
    WAVEFORMATEX f = pcm16_format();
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.wBitsPerSample = 32;
    f.nBlockAlign = static_cast<WORD>(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

ComPtr<IMMDevice> default_render_device() {
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return nullptr;
    }
    return device;
}

// --tone: a tone on the default speakers for `seconds` (an app whose sound is captured).
int play_tone(int seconds, double hz) {
    const auto device = default_render_device();
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    const WAVEFORMATEX format = pcm16_format();
    if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) ||
        FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  2'000'000 /* 200 ms */, 0, &format, nullptr)) ||
        FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
        say("tone: no speakers to play on");
        return 1;
    }
    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);
    client->Start();
    std::uint64_t n = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        const UINT32 frames = buffer_frames - padding;
        BYTE* data = nullptr;
        if (frames > 0 && SUCCEEDED(render->GetBuffer(frames, &data))) {
            auto* out = reinterpret_cast<std::int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i, ++n) {
                const double t = static_cast<double>(n) / kRate;
                const auto v = static_cast<std::int16_t>(
                    std::lround(kToneAmplitude * 32767.0 * std::sin(2.0 * std::numbers::pi * hz * t)));
                for (int c = 0; c < kChannels; ++c) out[i * kChannels + c] = v;
            }
            render->ReleaseBuffer(frames, 0);
        }
        std::this_thread::sleep_for(10ms);
    }
    client->Stop();
    return 0;
}

// ActivateAudioInterfaceAsync's callback (it must be agile: FtmBase).
class Activation
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    Activation() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        ComPtr<IUnknown> unknown;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(op->GetActivateResult(&hr, &unknown)) && SUCCEEDED(hr)) hr = unknown.As(&client_);
        result_ = hr;
        SetEvent(done_.get());
        return S_OK;
    }
    // The client, or nullptr with `hr` set.
    ComPtr<IAudioClient> wait(HRESULT& hr) {
        if (WaitForSingleObject(done_.get(), 5000) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            return nullptr;
        }
        hr = result_;
        return client_;
    }

private:
    unique_handle done_;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> client_;
};

// One capture: interleaved samples in -1..1, or why it failed.
struct Capture {
    std::vector<float> samples;
    int rate = kRate;
    int channels = kChannels;
    int discontinuities = 0;  // packets flagged AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY (a glitch before them)
    std::string error;
};

// Reads `client` (initialized, not started) for `duration`. Event-driven if `event` is set, else polled.
void read_capture(IAudioClient& client, const WAVEFORMATEX& format, HANDLE event, std::chrono::milliseconds duration,
                  Capture& out) {
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client.GetService(IID_PPV_ARGS(&capture)))) {
        out.error = "no capture service";
        return;
    }
    const bool is_float =
        format.wBitsPerSample == 32 &&
        (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
         (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    const bool is_pcm16 = format.wBitsPerSample == 16;
    if (!is_float && !is_pcm16) {
        out.error = std::format("format {} bits not handled", format.wBitsPerSample);
        return;
    }
    out.rate = static_cast<int>(format.nSamplesPerSec);
    out.channels = format.nChannels;
    client.Start();
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        if (event) {
            WaitForSingleObject(event, 100);
        } else {
            std::this_thread::sleep_for(5ms);
        }
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const std::size_t count = static_cast<std::size_t>(frames) * out.channels;
            // The first packet of a stream carries the flag too: count only later ones.
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !out.samples.empty()) ++out.discontinuities;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                out.samples.insert(out.samples.end(), count, 0.0f);
            } else if (is_float) {
                const auto* f = reinterpret_cast<const float*>(data);
                out.samples.insert(out.samples.end(), f, f + count);
            } else {
                const auto* s = reinterpret_cast<const std::int16_t*>(data);
                for (std::size_t i = 0; i < count; ++i) out.samples.push_back(s[i] / 32768.0f);
            }
            capture->ReleaseBuffer(frames);
        }
    }
    client.Stop();
}

// Per-process loopback: what `pid` and its child processes play, nothing else. As float samples, or
// 16-bit PCM.
Capture capture_process(DWORD pid, std::chrono::milliseconds duration, bool as_float = true) {
    Capture out;
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    const auto handler = Microsoft::WRL::Make<Activation>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation,
                                             handler.Get(), &op);
    if (FAILED(hr)) {
        out.error = "ActivateAudioInterfaceAsync failed: " + hr_text(hr);
        return out;
    }
    const auto client = handler->wait(hr);
    if (!client) {
        out.error = "activation failed: " + hr_text(hr);
        return out;
    }
    // A process-loopback client has no mix format of its own: it converts to the one asked for.
    const WAVEFORMATEX format = as_float ? float_format() : pcm16_format();
    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        2'000'000, 0, &format, nullptr);
    if (FAILED(hr)) {
        out.error = "Initialize failed: " + hr_text(hr);
        return out;
    }
    const unique_handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (FAILED(client->SetEventHandle(event.get()))) {
        out.error = "SetEventHandle failed";
    } else {
        read_capture(*client.Get(), format, event.get(), duration, out);
    }
    return out;
}

// Endpoint loopback: everything the default speakers play (the fallback).
Capture capture_system(std::chrono::milliseconds duration) {
    Capture out;
    const auto device = default_render_device();
    ComPtr<IAudioClient> client;
    if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        out.error = "no default speakers";
        return out;
    }
    WAVEFORMATEX* raw_mix = nullptr;
    if (FAILED(client->GetMixFormat(&raw_mix))) {
        out.error = "no mix format";
        return out;
    }
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix(raw_mix, &CoTaskMemFree);
    const HRESULT hr =
        client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2'000'000, 0, mix.get(), nullptr);
    if (FAILED(hr)) {
        out.error = "Initialize failed: " + hr_text(hr);
    } else {
        read_capture(*client.Get(), *mix, nullptr, duration, out);
    }
    return out;
}

struct Level {
    double rms_db = -200;    // the whole signal, dBFS
    double tone_db = -200;   // its 440 Hz part (Goertzel), dBFS
    double other_db = -200;  // its 660 Hz part
};

double db(double rms) {
    return rms > 0 ? 20.0 * std::log10(rms) : -200.0;
}

// The RMS level (dBFS) of the `hz` component of `mono`: Goertzel per 100 ms block (440 and 660 Hz fall
// on its 10 Hz bins), the blocks' powers averaged, so a gap in the capture doesn't lower it.
double tone_level(const std::vector<double>& mono, int rate, double hz) {
    const std::size_t block = static_cast<std::size_t>(rate) / 10;
    const double coeff = 2.0 * std::cos(2.0 * std::numbers::pi * hz / rate);
    double sum = 0;
    std::size_t blocks = 0;
    for (std::size_t start = 0; block > 0 && start + block <= mono.size(); start += block, ++blocks) {
        double s1 = 0, s2 = 0;
        for (std::size_t i = start; i < start + block; ++i) {
            const double s0 = mono[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = std::max(s1 * s1 + s2 * s2 - coeff * s1 * s2, 0.0);
        const double amplitude = 2.0 * std::sqrt(power) / static_cast<double>(block);
        sum += amplitude * amplitude / 2.0;  // the sine's mean square
    }
    return blocks > 0 ? db(std::sqrt(sum / static_cast<double>(blocks))) : -200.0;
}

Level measure(const Capture& c) {
    Level l;
    if (c.samples.empty() || c.channels <= 0) return l;
    // Mono: the channels' average.
    const std::size_t frames = c.samples.size() / c.channels;
    std::vector<double> mono(frames);
    double sum = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        double v = 0;
        for (int ch = 0; ch < c.channels; ++ch) v += c.samples[i * c.channels + ch];
        mono[i] = v / c.channels;
        sum += mono[i] * mono[i];
    }
    l.rms_db = db(std::sqrt(sum / static_cast<double>(frames)));
    l.tone_db = tone_level(mono, c.rate, kToneHz);
    l.other_db = tone_level(mono, c.rate, kOtherHz);
    return l;
}

// The tones are at -26 dBFS peak (-29 as RMS); silence is far below.
bool hears(double level_db) {
    return level_db > -45.0;
}

bool write_wav(const std::filesystem::path& path, const Capture& c) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const auto data_bytes = static_cast<std::uint32_t>(c.samples.size() * 2);
    const auto put32 = [&](std::uint32_t v) { fwrite(&v, 4, 1, f); };
    const auto put16 = [&](std::uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    put32(36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(static_cast<std::uint16_t>(c.channels));
    put32(static_cast<std::uint32_t>(c.rate));
    put32(static_cast<std::uint32_t>(c.rate * c.channels * 2));
    put16(static_cast<std::uint16_t>(c.channels * 2));
    put16(16);
    fwrite("data", 1, 4, f);
    put32(data_bytes);
    for (const float s : c.samples) {
        const auto v = static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return true;
}

// The audio session `pid` plays through on the default speakers (the volume mixer's entry).
ComPtr<ISimpleAudioVolume> session_of(DWORD pid) {
    const auto device = default_render_device();
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (!device || FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
        FAILED(manager->GetSessionEnumerator(&sessions))) {
        return nullptr;
    }
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        DWORD session_pid = 0;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
            FAILED(control2->GetProcessId(&session_pid)) || session_pid != pid) {
            continue;
        }
        ComPtr<ISimpleAudioVolume> volume;
        if (SUCCEEDED(control.As(&volume))) return volume;
    }
    return nullptr;
}

std::string describe(const Capture& c, const Level& l) {
    if (!c.error.empty()) return "failed: " + c.error;
    return std::format(
        "{:.1f} s, level {:.1f} dBFS; the app (440 Hz) {:.1f}, the other (660 Hz) {:.1f} dBFS; {} gap(s)",
        static_cast<double>(c.samples.size()) / c.channels / c.rate, l.rms_db, l.tone_db, l.other_db,
        c.discontinuities);
}

// Starts this program with `args` (a tone); the process handle, or none.
unique_handle start_self(const std::wstring& args, DWORD& pid) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::format(L"\"{}\" {}", self, args);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    pid = pi.dwProcessId;
    return unique_handle{pi.hProcess};
}

int probe(const std::optional<std::filesystem::path>& out_dir) {
    say(std::format("Windows build {}", windows_build()));
    if (!default_render_device()) {
        say("no default speakers: nothing to probe");
        return 1;
    }
    DWORD app = 0, other = 0;
    const auto app_process = start_self(std::format(L"--tone 30 --hz {}", kToneHz), app);
    const auto other_process = start_self(std::format(L"--tone 30 --hz {}", kOtherHz), other);
    if (!app_process || !other_process) {
        say("couldn't start the tones");
        return 1;
    }
    say(std::format("the app plays {} Hz in process {}, another app {} Hz in process {}", kToneHz, app, kOtherHz,
                    other));
    ComPtr<ISimpleAudioVolume> session;
    for (int i = 0; i < 40 && !session; ++i) {
        std::this_thread::sleep_for(50ms);
        session = session_of(app);
    }
    std::this_thread::sleep_for(300ms);  // the streams are running

    std::vector<std::pair<std::string, Level>> levels;
    const auto run = [&](const std::string& name, const Capture& c) {
        const Level l = measure(c);
        say(std::format("  {}: {}", name, describe(c, l)));
        if (out_dir && c.error.empty()) write_wav(*out_dir / (name + ".wav"), c);
        if (c.error.empty()) levels.emplace_back(name, l);
    };
    const auto level = [&](std::string_view name) -> std::optional<Level> {
        for (const auto& [n, l] : levels) {
            if (n == name) return l;
        }
        return std::nullopt;
    };
    say("both playing:");
    run("process", capture_process(app, 2s));
    run("process-pcm16", capture_process(app, 2s, false));
    run("system", capture_system(2s));
    float volume = 1.0f;
    const bool have_session = session && SUCCEEDED(session->GetMasterVolume(&volume));
    if (have_session && SUCCEEDED(session->SetMute(TRUE, nullptr))) {
        std::this_thread::sleep_for(200ms);
        say("the app's session muted (the volume mixer's mute):");
        run("process-muted", capture_process(app, 2s));
        run("system-muted", capture_system(2s));
        session->SetMute(FALSE, nullptr);
    }
    for (const int quiet_db : kQuietDb) {
        const auto v = static_cast<float>(std::pow(10.0, -quiet_db / 20.0));
        if (!have_session || FAILED(session->SetMasterVolume(v, nullptr))) break;
        std::this_thread::sleep_for(200ms);
        say(std::format("the app's session volume at -{} dB instead:", quiet_db));
        run(std::format("process-quiet{}", quiet_db), capture_process(app, 2s));
        run(std::format("system-quiet{}", quiet_db), capture_system(2s));
        session->SetMasterVolume(volume, nullptr);
    }
    if (!have_session) say("the app's audio session wasn't found: questions 2 and 3 are not answered");
    TerminateProcess(app_process.get(), 0);
    TerminateProcess(other_process.get(), 0);
    WaitForSingleObject(app_process.get(), 2000);
    WaitForSingleObject(other_process.get(), 2000);

    say("");
    const auto process = level("process");
    const auto system = level("system");
    if (!process) {
        say(std::format("1. Per-process loopback on build {}: not available", windows_build()));
        return 0;
    }
    say(std::format("1. Per-process loopback on build {}: {}", windows_build(),
                    hears(process->tone_db) && !hears(process->other_db)
                        ? "works: it hears the app and not the other app"
                    : hears(process->tone_db) ? "hears the app, but the other app too"
                                              : "runs but doesn't hear the app"));
    if (system) {
        say(std::format("   The whole PC's capture has both: the app {:.1f}, the other {:.1f} dBFS", system->tone_db,
                        system->other_db));
    }
    if (const auto pm = level("process-muted"), sm = level("system-muted"); pm && sm) {
        say(std::format("2. With the app's session muted: the per-process capture {}; the PC's output {}.",
                        hears(pm->tone_db) ? "still hears it" : "is silent too",
                        hears(sm->tone_db) ? "still has it" : "doesn't have it"));
    }
    for (const int quiet_db : kQuietDb) {
        const auto pq = level(std::format("process-quiet{}", quiet_db));
        const auto sq = level(std::format("system-quiet{}", quiet_db));
        if (!pq || !sq) continue;
        // Raised again: the tone back at its level, and the rest (noise) far below it.
        say(std::format(
            "3. With the app's session at -{0} dB: the per-process capture has it at {1:.1f} dBFS, "
            "{2:.1f} after +{0} dB ({3:.1f} at full volume; all of it {4:.1f}); the PC's output {5:.1f} "
            "dBFS.",
            quiet_db, pq->tone_db, pq->tone_db + quiet_db, process->tone_db, pq->rms_db + quiet_db, sq->tone_db));
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    std::optional<DWORD> pid;
    int seconds = 5;
    std::optional<std::filesystem::path> out;
    bool system = false;
    std::optional<int> tone_seconds;
    double hz = kToneHz;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--tone" && i + 1 < argc) {
            tone_seconds = std::stoi(argv[++i]);
        } else if (a == L"--hz" && i + 1 < argc) {
            hz = std::stod(argv[++i]);
        } else if (a == L"--pid" && i + 1 < argc) {
            pid = static_cast<DWORD>(std::stoul(argv[++i]));
        } else if (a == L"--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else if (a == L"--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (a == L"--system") {
            system = true;
        }
    }
    if (tone_seconds) return play_tone(*tone_seconds, hz);
    if (!pid && !system) return probe(out);

    const auto duration = std::chrono::milliseconds(seconds * 1000);
    const Capture c = system ? capture_system(duration) : capture_process(*pid, duration);
    say(describe(c, measure(c)));
    if (out && c.error.empty() && !write_wav(*out, c)) say("couldn't write the WAV");
    return c.error.empty() ? 0 : 1;
}
