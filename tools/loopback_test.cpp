// SPDX-License-Identifier: Apache-2.0

// loopback_test: the two-PC paths on one PC, with the real programs. Starts kvm_host and kvm_client
// on free ports, each with its own config.json, identity and paired PCs in a temp folder, without
// hooks, clipboard, desk mode or discovery (nothing on this desktop or network changes), and checks:
//   0. each program creates its identity (--print-id) and keeps it;
//   1. a PC whose key isn't paired is closed right after the TLS handshake: its hello is never read;
//   2. an older Sarke (plain TCP, protocol v2; in-process and kvm_fake_peer --plaintext-v2) is closed,
//      and the host logs it as an older Sarke (the launcher shows that as a notification);
//   3. a paired peer speaking another protocol version gets the host's hello (its version), is dropped,
//      and the host logs the mismatch;
//   4. a paired peer speaking this version gets the host's hello back, with its device ID and the
//      session's side-channel token: with it the peer opens video and files; a wrong token or another
//      PC's key is refused there and the live channel stays; the session's end closes its channels;
//   4b. "unpair <id>" on the host's stdin (as the launcher sends it) in the middle of that peer's
//      session: the peer is told (unpaired), all three channels close, and its key is refused after;
//   5. kvm_client, paired with the host, connects (control, video, files) and both sides log rtt;
//   5b. the host unpairs kvm_client: the client is told, forgets the host and doesn't connect again;
//   6. Ctrl+Break, the way the launcher stops them, ends both normally.
//
//   loopback_test.exe            (exit code 0 = all passed; ctest label "loopback")
//   loopback_test.exe --keep     keep the temp folder with both logs

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "kvm/file_transfer.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"
#include "kvm/video.h"
#include "program_util.h"

namespace fs = std::filesystem;
namespace proto = kvm::proto;
using namespace std::chrono_literals;
using namespace kvm::test;  // Program, Checks, free_port, wait_until, ...

namespace {

// Sends `bytes` on a plain TCP connection to 127.0.0.1:port and reports whether the other side closed
// it within `timeout` (what a TLS server does with a peer that doesn't speak TLS).
bool closed_after_plain_bytes(std::uint16_t port, std::string_view bytes, std::chrono::milliseconds timeout) {
    kvm::net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    bool closed = false;
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        send(s, bytes.data(), static_cast<int>(bytes.size()), 0);
        const DWORD ms = static_cast<DWORD>(timeout.count());
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
        char buf[512];
        for (;;) {
            const int n = recv(s, buf, sizeof(buf), 0);  // a TLS alert may come first, then the close
            if (n > 0) continue;
            closed = n == 0 || WSAGetLastError() != WSAETIMEDOUT;
            break;
        }
    }
    closesocket(s);
    return closed;
}

// The first M in `conn`'s messages within `timeout` (other messages are skipped).
template <class M>
std::optional<M> wait_for(kvm::net::Connection& conn, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        auto msg = conn.incoming().pop_for(left);
        if (!msg) return std::nullopt;
        if (const auto* m = std::get_if<M>(&*msg)) return *m;
    }
    return std::nullopt;
}

std::optional<proto::Hello> wait_for_hello(kvm::net::Connection& conn, std::chrono::milliseconds timeout) {
    return wait_for<proto::Hello>(conn, timeout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool keep = argc > 1 && std::wstring_view(argv[1]) == L"--keep";
    Checks check;

    // Ctrl+Break needs a console shared with the programs: get one (hidden) if ctest gave none.
    if (GetConsoleCP() == 0 && AllocConsole()) {
        if (const HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
    }

    const fs::path bin = exe_dir().parent_path();  // build\<preset>
    const fs::path host_exe = bin / L"host_a" / L"kvm_host.exe";
    const fs::path client_exe = bin / L"client_b" / L"kvm_client.exe";
    if (!fs::exists(host_exe) || !fs::exists(client_exe)) {
        kvm::log::error("kvm_host.exe / kvm_client.exe not found next to {}", bin.string());
        return 1;
    }

    const fs::path dir = fs::temp_directory_path() / std::format(L"sarke_loopback_test_{}", GetCurrentProcessId());
    fs::create_directories(dir);
    const std::uint16_t control = free_port(), video = free_port(), files = free_port();
    check(control && video && files && control != video && video != files && control != files,
          std::format("free ports {} / {} / {}", control, video, files));
    const std::string ports =
        std::format(R"("control_port": {}, "video_port": {}, "file_port": {})", control, video, files);
    std::ofstream(dir / "host.json") << "{" << ports << "}\n";
    std::ofstream(dir / "client.json") << "{" << ports << R"(, "host_ip": "127.0.0.1"})" << "\n";

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const std::wstring quiet = L" --no-kvm --no-clipboard --no-desk";
    // Each program keeps its identity and paired PCs in its own folder here, never in this user's.
    const auto data = [&](std::wstring_view who) { return L" --data-dir \"" + (dir / who).wstring() + L"\""; };

    kvm::log::info("0) identities");
    const auto id_of = [&](const fs::path& exe, std::wstring_view who) -> std::optional<kvm::tls::DeviceId> {
        Program p(job, exe, data(who) + L" --print-id", dir / (std::wstring(who) + L"-id.log"));
        if (!p.started() || p.wait_exit(30s) != 0u) return std::nullopt;
        return device_id_in(p.log());
    };
    const auto host_id = id_of(host_exe, L"host-data");
    const auto client_id = id_of(client_exe, L"client-data");
    check(host_id && client_id && *host_id != *client_id, "kvm_host and kvm_client each create an identity");
    const auto again = id_of(host_exe, L"host-data");
    check(again && host_id && *again == *host_id, "and keep it (the same device ID on the next start)");
    if (!host_id || !client_id) {
        CloseHandle(job);
        kvm::log::error("no identities; logs: {}", dir.string());
        return 1;
    }
    const auto peer = kvm::tls::Credentials::generate("loopback-peer");          // paired with the host
    const auto stranger = kvm::tls::Credentials::generate("loopback-stranger");  // not paired

    {
        Program host(job, host_exe,
                     L"--config \"" + (dir / "host.json").wstring() + L"\" --no-discovery" + quiet +
                         data(L"host-data") + L" --pair-with " + hex(*client_id) + L" --pair-with " + hex(peer.id()),
                     dir / "host.log");
        check(host.started() && wait_until([&] { return host.log_has("listening on ports"); }, 15s),
              "kvm_host starts and listens");

        // A TLS connection to the host as `me`, pinning the host's key the way kvm_client does.
        const auto connect_as = [&](const kvm::tls::Credentials& me) -> std::unique_ptr<kvm::net::Connection> {
            auto pending = kvm::net::connect("127.0.0.1", control, me, 3s);
            if (!pending || pending->stream->peer_id() != *host_id) return nullptr;
            return std::make_unique<kvm::net::Connection>(std::move(*pending));
        };

        kvm::log::info("1) a PC that isn't paired");
        {
            auto conn = connect_as(stranger);
            check(conn != nullptr, "the TLS handshake completes, and the host's key is the one it printed");
            if (conn) {
                conn->send(proto::Hello{.hostname = "hello-from-stranger"});
                check(wait_until([&] { return !conn->is_open(); }, 5s), "the host closes it");
            }
            check(wait_until([&] { return host.log_has("isn't paired with this PC"); }, 3s),
                  "the host logs the unknown key");
            check(!host.log_has("hello-from-stranger"), "and never read its hello: nothing from it is parsed (S3)");
        }

        kvm::log::info("2) an older Sarke (protocol v2, no TLS)");
        {
            const char v2_hello[] = "\x00\x00\x00\x12{\"type\":\"hello\"}  ";
            check(closed_after_plain_bytes(control, std::string_view(v2_hello, sizeof(v2_hello) - 1), 5s),
                  "the host closes a connection that doesn't speak TLS");
            check(wait_until([&] { return host.log_has(kvm::tls::kOlderSarke); }, 3s),
                  "and logs it as an older Sarke to update");
            const auto before = count(host.log(), kvm::tls::kOlderSarke);
            Program fake(job, exe_dir() / L"kvm_fake_peer.exe",
                         std::format(L"--plaintext-v2 --host 127.0.0.1 --port {}", control), dir / "fake-v2.log");
            const auto code = fake.started() ? fake.wait_exit(15s) : std::nullopt;
            check(code == 0u && wait_until([&] { return count(host.log(), kvm::tls::kOlderSarke) > before; }, 3s),
                  "kvm_fake_peer --plaintext-v2: closed, and logged the same way");
        }

        kvm::log::info("3) a paired peer speaking protocol v{}", proto::kProtocolVersion + 1);
        {
            auto conn = connect_as(peer);
            check(conn != nullptr, "it connects");
            if (conn) {
                conn->send(proto::Hello{.protocol_version = proto::kProtocolVersion + 1, .hostname = "loopback-old"});
                const auto answer = wait_for_hello(*conn, 5s);
                check(answer && answer->protocol_version == proto::kProtocolVersion,
                      "it gets the host's hello first, with the host's version");
                check(wait_until([&] { return !conn->is_open(); }, 6s), "the host drops it");
                check(wait_until([&] { return host.log_has("version: 'loopback-old' at 127.0.0.1"); }, 3s) &&
                          host.log_has(std::format("speaks protocol v{}, this PC v{}", proto::kProtocolVersion + 1,
                                                   proto::kProtocolVersion)),
                      "the host logs the version mismatch");
            }
        }

        kvm::log::info("4) a paired peer speaking protocol v{}", proto::kProtocolVersion);
        {
            auto conn = connect_as(peer);
            check(conn != nullptr, "it connects");
            if (conn) {
                conn->send(proto::Hello{.screen_w = 1920, .screen_h = 1080, .hostname = "loopback-peer"});
                const auto hello = wait_for_hello(*conn, 5s);
                check(
                    hello && hello->protocol_version == proto::kProtocolVersion &&
                        hello->device_id == kvm::tls::to_hex(*host_id),
                    std::format("the host answers hello v{} with its device ID", hello ? hello->protocol_version : 0));
                check(hello && hello->session_token.size() == 64 && hello->video_port == video &&
                          hello->file_port == files,
                      "and with the session's side channels: a 32-byte token and the ports");
                const std::string token = hello ? hello->session_token : std::string();
                const auto refusals = [&] { return count(host.log(), "- refused"); };

                // Side channels (docs/SECURITY.md 6.4, S4): the session's own peer, with its token.
                auto live = kvm::net::connect_video("127.0.0.1", video, peer, *host_id, token, 3s);
                check(live && wait_until([&] { return host.log_has("video channel from"); }, 3s),
                      "with the token, the session's peer opens the video channel");
                std::atomic<bool> live_closed = false;
                std::thread watch([&] {
                    while (live && live->recv_frame()) {}
                    live_closed = true;
                });
                const auto before = refusals();
                auto wrong_token =
                    kvm::net::connect_video("127.0.0.1", video, peer, *host_id, std::string(64, '0'), 3s);
                auto other_key = kvm::net::connect_video("127.0.0.1", video, stranger, *host_id, token, 3s);
                auto other_files = kvm::files::connect_files("127.0.0.1", files, stranger, *host_id, token, {},
                                                             dir / "stranger-in", 3s);
                check(wait_until([&] { return refusals() >= before + 3; }, 5s),
                      "a wrong token, or another PC's key with the right token, is refused on video and files");
                check(
                    !host.log_has("file channel from") && !live_closed && count(host.log(), "video channel from") == 1,
                    "and doesn't replace the live video channel");
                auto own_files =
                    kvm::files::connect_files("127.0.0.1", files, peer, *host_id, token, {}, dir / "peer-in", 3s);
                check(own_files && wait_until([&] { return host.log_has("file channel from"); }, 3s),
                      "the session's peer opens the file channel");

                conn->close();
                check(wait_until([&] { return host.log_has("'loopback-peer' session ended"); }, 5s),
                      "the host ends that session when the peer leaves");
                check(wait_until([&] { return live_closed.load(); }, 5s), "and closes the session's video channel");
                const auto after = refusals();
                auto late = kvm::net::connect_video("127.0.0.1", video, peer, *host_id, token, 3s);
                check(wait_until([&] { return refusals() >= after + 1; }, 5s),
                      "after the session, its token opens nothing");
                if (live) live->close();
                watch.join();
            }
        }

        kvm::log::info("4b) unpairing a PC in the middle of its session");
        {
            auto conn = connect_as(peer);
            if (conn) conn->send(proto::Hello{.hostname = "loopback-peer"});
            const auto hello = conn ? wait_for_hello(*conn, 5s) : std::nullopt;
            check(hello.has_value(), "the paired peer connects again");
            const std::string token = hello ? hello->session_token : std::string();
            const auto videos = count(host.log(), "video channel from");
            const auto file_channels = count(host.log(), "file channel from");
            auto video_ch = kvm::net::connect_video("127.0.0.1", video, peer, *host_id, token, 3s);
            auto files_ch = kvm::net::connect_side_channel("127.0.0.1", files, peer, *host_id, token, 3s);
            check(video_ch && files_ch &&
                      wait_until(
                          [&] {
                              return count(host.log(), "video channel from") > videos &&
                                     count(host.log(), "file channel from") > file_channels;
                          },
                          3s),
                  "with its video and file channels");
            std::atomic<bool> video_closed = false, files_closed = false;
            std::thread watch_video([&] {
                while (video_ch && video_ch->recv_frame()) {}
                video_closed = true;
            });
            std::thread watch_files([&] {
                char byte = 0;
                while (files_ch && files_ch->stream->recv_exact(&byte, 1) == 0) {}
                files_closed = true;
            });
            check(host.send_line("unpair " + kvm::tls::to_hex(peer.id())), "the launcher sends \"unpair <its ID>\"");
            check(conn && wait_for<proto::Unpaired>(*conn, 5s).has_value(), "the peer is told: unpaired");
            check(conn && wait_until([&] { return !conn->is_open(); }, 5s), "its control channel closes");
            check(wait_until([&] { return video_closed && files_closed; }, 5s), "and its video and file channels");
            check(host.log_has("is no longer paired with this PC - its session is closed"), "the host logs it");
            if (video_ch) video_ch->close();
            if (files_ch) files_ch->stream->abort();
            watch_video.join();
            watch_files.join();

            const auto unknown = count(host.log(), "isn't paired with this PC");
            auto retry = connect_as(peer);
            if (retry) retry->send(proto::Hello{.hostname = "loopback-peer"});
            check(retry && wait_until([&] { return !retry->is_open(); }, 5s) &&
                      count(host.log(), "isn't paired with this PC") > unknown,
                  "after that its key is refused like any unknown key");
        }

        kvm::log::info("5) kvm_client, paired with the host");
        {
            Program client(job, client_exe,
                           L"--config \"" + (dir / "client.json").wstring() + L"\" --host 127.0.0.1" + quiet +
                               L" --codec jpeg" + data(L"client-data") + L" --pair-with " + hex(*host_id),
                           dir / "client.log");
            check(client.started() && wait_until([&] { return client.log_has("connected to host"); }, 15s),
                  "the client connects");
            check(wait_until([&] { return client.log_has("video channel connected"); }, 5s), "video channel");
            check(wait_until([&] { return client.log_has("file channel connected"); }, 5s), "file channel");
            check(wait_until([&] { return count(host.log(), " connected from ") >= 2; }, 5s),
                  "the host takes the client");
            // Both sides log the link every 5 s once the first ping/pong is through.
            check(wait_until([&] { return client.log_has("link: rtt") && host.log_has("link: rtt"); }, 15s),
                  "both sides log the link (rtt)");
            // Codec negotiation (roadmap P2.5): each side picks for the windows it sends, from the
            // other's hello. The client was told --codec jpeg; the host picks H.264 only with a hardware
            // encoder (CI runners have none).
            check(client.log_has("go as jpeg (--codec jpeg)"), "the client sends windows as JPEG: --codec jpeg");
            check(host.log_has("go as h264 (both PCs can)") ||
                      host.log_has("go as jpeg (no hardware H.264 encoder on this PC)"),
                  "the host picks from the client's hello: H.264 if it has an encoder, else JPEG");
            // Desk mode the same way, from "h264-desk" in the client's hello (P2.7c).
            check(host.log_has("desk mode as h264 (both PCs can)") ||
                      host.log_has("desk mode as jpeg (no hardware H.264 encoder on this PC)"),
                  "desk mode too: H.264 if the host has an encoder, else JPEG");

            kvm::log::info("5b) the host unpairs kvm_client");
            check(host.send_line("unpair " + kvm::tls::to_hex(*client_id)), "\"unpair <the client's ID>\" to the host");
            check(wait_until([&] { return client.log_has("unpaired this PC"); }, 5s), "the client is told and says so");
            check(wait_until([&] { return client.log_has("reconnecting in"); }, 5s), "its session ends");
            check(
                read_file(dir / L"client-data" / L"paired.json").find(kvm::tls::to_hex(*host_id)) == std::string::npos,
                "the client forgot the host too");
            check(wait_until([&] { return client.log_has("which isn't paired with this PC"); }, 5s) &&
                      count(client.log(), "connected to host") == 1,
                  "so its next try stops at the host's key: it doesn't connect again");

            kvm::log::info("6) Ctrl+Break, the way the launcher stops them");
            const auto client_code = client.stop(10s);
            check(client_code == 0u, std::format("the client stops with exit code 0 (got {})",
                                                 client_code ? static_cast<long>(*client_code) : -1L));
        }
        const auto host_code = host.stop(10s);
        check(host_code == 0u,
              std::format("the host stops with exit code 0 (got {})", host_code ? static_cast<long>(*host_code) : -1L));
    }
    // Pairing by code (docs/SECURITY.md 6.3): a host and a client that have never met, the client started
    // the way the launcher starts it when the user picks a PC (--pair-unknown). The test plays both users:
    // it answers each "pairing: code ..." line on the program's stdin, as the launcher does.
    const auto code_in = [](const std::string& log) -> std::string {
        const auto at = log.find("pairing: code ");
        return at == std::string::npos || at + 21 > log.size() ? std::string() : log.substr(at + 14, 7);
    };
    for (const bool both_agree : {true, false}) {
        const std::wstring tag = both_agree ? L"pair" : L"cancel";
        kvm::log::info("{}",
                       both_agree ? "7) pairing by code, both users confirm" : "8) pairing by code, one user cancels");
        const std::uint16_t c = free_port(), v = free_port(), f = free_port();
        const fs::path config = dir / (tag + L".json");
        std::ofstream(config) << std::format(R"({{"control_port": {}, "video_port": {}, "file_port": {}}})", c, v, f)
                              << "\n";
        Program host(job, host_exe,
                     L"--config \"" + config.wstring() + L"\" --no-discovery" + quiet + data(tag + L"-host"),
                     dir / (tag + L"-host.log"));
        check(host.started() && wait_until([&] { return host.log_has("listening on ports"); }, 15s),
              "a new kvm_host listens");
        Program client(
            job, client_exe,
            L"--config \"" + config.wstring() + L"\" --host 127.0.0.1 --pair-unknown" + quiet + data(tag + L"-client"),
            dir / (tag + L"-client.log"));
        const bool asked =
            wait_until([&] { return host.log_has("pairing: code") && client.log_has("pairing: code"); }, 20s);
        const std::string host_code = code_in(host.log()), client_code = code_in(client.log());
        check(asked && host_code.size() == 7 && host_code == client_code,
              std::format("both PCs show the same code: {} / {}", host_code, client_code));
        check(!host.log_has("connected from") && !client.log_has("connected to host"),
              "nothing connects before both users answer");
        client.send_line("pair yes");
        host.send_line(both_agree ? "pair yes" : "pair no");
        if (both_agree) {
            check(wait_until(
                      [&] { return host.log_has("pairing: paired with") && client.log_has("pairing: paired with"); },
                      10s),
                  "both store the other");
            check(
                wait_until([&] { return client.log_has("connected to host") && host.log_has("connected from"); }, 15s),
                "the client connects again, now as a paired PC");
        } else {
            check(wait_until(
                      [&] {
                          return host.log_has("pairing: cancelled on this PC") &&
                                 client.log_has("pairing: cancelled on the other PC");
                      },
                      10s),
                  "both see the cancel");
            std::this_thread::sleep_for(3s);
            check(!host.log_has("pairing: paired") && !client.log_has("pairing: paired") &&
                      !client.log_has("connected to host"),
                  "nothing is stored and nothing connects");
        }
        const auto client_code_exit = client.stop(10s);
        const auto host_code_exit = host.stop(10s);
        check(client_code_exit == 0u && host_code_exit == 0u, "both stop normally");
    }

    CloseHandle(job);

    if (check.failed || keep) {
        kvm::log::info("logs: {}", dir.string());
    } else {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
