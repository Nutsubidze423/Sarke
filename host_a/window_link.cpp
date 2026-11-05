// SPDX-License-Identifier: Apache-2.0

#include "window_link.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <variant>

// Sandro's viewer lives in client_b; both exes link both module libraries.
#include "../client_b/desk_view.h"
#include "../client_b/viewer_window.h"
#include "desk_input.h"
#include "desk_share.h"
#include "frame_router.h"
#include "kvm/log.h"
#include "remote_input.h"
#include "vdd_modes.h"
#include "virtual_display.h"
#include "window_mover.h"
#include "window_picker.h"
#include "window_share.h"

namespace kvm {

namespace {

using Clock = std::chrono::steady_clock;
// After the link drops, desk mode waits this long before detaching the VDD: a quick reconnect (a
// brief Wi-Fi blip) keeps every window where it is (detaching moves them all back to the real
// monitors). Kept short so a real disconnect doesn't leave a phantom monitor the cursor can wander
// into; the KVM wall blocks the cursor meanwhile, and on a real disconnect the windows should come
// home anyway.
constexpr auto kDeskGrace = std::chrono::seconds(3);
constexpr auto kDeskPoll = std::chrono::milliseconds(30);  // cursor shape, VDD health
constexpr auto kVddRecheck = std::chrono::seconds(1);       // the VDD moved (display settings)

// Never sent: this process's windows (viewer, console, drop box), the desktop and the taskbar.
bool sendable(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || hwnd == GetConsoleWindow()) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        if (std::wcscmp(cls, shell) == 0) return false;
    }
    return true;
}

std::wstring title_of(HWND hwnd) {
    std::wstring title(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()))));
    return title;
}

display::Rect to_rect(const RECT& r) {
    return {r.left, r.top, r.right, r.bottom};
}

// The standard cursor `cursor` is, by name (proto::DeskCursor); "arrow" for anything else.
const char* cursor_shape(HCURSOR cursor) {
    static const struct {
        HCURSOR handle;
        const char* name;
    } kShapes[] = {
        {LoadCursorW(nullptr, IDC_IBEAM), "ibeam"},
        {LoadCursorW(nullptr, IDC_HAND), "hand"},
        {LoadCursorW(nullptr, IDC_WAIT), "wait"},
        {LoadCursorW(nullptr, IDC_APPSTARTING), "appstarting"},
        {LoadCursorW(nullptr, IDC_CROSS), "cross"},
        {LoadCursorW(nullptr, IDC_SIZEWE), "sizewe"},
        {LoadCursorW(nullptr, IDC_SIZENS), "sizens"},
        {LoadCursorW(nullptr, IDC_SIZENWSE), "sizenwse"},
        {LoadCursorW(nullptr, IDC_SIZENESW), "sizenesw"},
        {LoadCursorW(nullptr, IDC_SIZEALL), "sizeall"},
        {LoadCursorW(nullptr, IDC_NO), "no"},
    };
    for (const auto& s : kShapes) {
        if (s.handle == cursor) return s.name;
    }
    return "arrow";
}

}  // namespace

WindowLink::WindowLink(Options options) : options_(options), peer_side_(options.peer_side) {
    std::promise<void> ready;
    auto started = ready.get_future();
    ui_thread_ = std::thread([this, &ready] {
        ui_thread_id_ = GetCurrentThreadId();
        auto viewer = std::make_unique<ViewerWindow>();
        if (!viewer->create(/*visible=*/false)) log::error("windows: the viewer window could not be created");
        viewer->set_jpeg_quality(options_.jpeg_quality);
        viewer->set_reconnects(true);
        // The viewer's clicks, keys and "give it back" (stream_stop on close) go to the window's owner.
        viewer->set_message_sink([this](const proto::Message& m) { return send(m); });
        viewer->set_close_handler([this] {
            if (const HWND h = viewer_hwnd_) ShowWindow(h, SW_HIDE);
        });
        viewer_hwnd_ = viewer->hwnd();
        {
            std::lock_guard lock(mutex_);
            viewer_ = std::move(viewer);
        }
        ready.set_value();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::unique_ptr<ViewerWindow> closing;
        {
            std::lock_guard lock(mutex_);
            closing = std::move(viewer_);
        }
        viewer_hwnd_ = nullptr;
        closing.reset();  // the window is destroyed on the thread that made it
    });
    started.wait();

    if (options_.desk) {
        desk_view_ = std::make_unique<DeskView>();  // the other PC's windows, drawn on this screen
        desk_thread_ = std::jthread([this](std::stop_token stop) { desk_main(stop); });
    }

    watchdog_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::lock_guard lock(mutex_);
            if (share_ && !share_->alive()) {
                log::info("windows: the shared window was closed");
                stop_share(/*tell_peer=*/true);
            }
        }
    });
}

WindowLink::~WindowLink() {
    watchdog_.request_stop();
    if (watchdog_.joinable()) watchdog_.join();
    {
        std::lock_guard lock(mutex_);
        stop_share(/*tell_peer=*/true);  // never leave the window on an invisible monitor
    }
    if (desk_thread_.joinable()) {
        desk_commands_.push({.kind = DeskCommand::Exit});  // windows back, VDD detached
        desk_thread_.join();
    }
    set_video(nullptr);
    desk_view_.reset();
    if (const DWORD id = ui_thread_id_) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (ui_thread_.joinable()) ui_thread_.join();
}

void WindowLink::set_video(std::shared_ptr<net::FrameChannel> channel) {
    ViewerWindow* viewer = nullptr;  // fixed for the object's life once the constructor returned
    {
        std::lock_guard lock(mutex_);
        viewer = viewer_.get();
    }
    std::shared_ptr<net::FrameChannel> old_video;  // declared first: outlives old_router below
    std::unique_ptr<FrameRouter> old_router;
    {
        std::lock_guard lock(video_mutex_);  // never together with mutex_ (see the header)
        old_router = std::move(router_);
        old_video = std::exchange(video_, std::move(channel));
        if (video_ && viewer) {
            router_ = std::make_unique<FrameRouter>(
                *video_, *viewer, desk_view_.get(),
                WindowStream::Feedback{.keyframe = [this] { send(proto::VideoKeyframe{}); },
                                       .stats =
                                           [this](int latency_ms, int fps) {
                                               if (peer_knows_codecs_) send(proto::VideoStats{latency_ms, fps});
                                           }});
        }
    }
    old_router.reset();  // closes the old channel and joins its reader, outside the lock
}

void WindowLink::attach(Sink send_fn, int peer_w, int peer_h) {
    {
        std::lock_guard lock(send_mutex_);
        send_ = std::move(send_fn);
    }
    set_peer_screen(peer_w, peer_h);
    if (desk_view_) desk_view_->set_input_sink([this](const proto::Message& m) { return send(m); });
    std::lock_guard lock(mutex_);
    if (viewer_) viewer_->set_link_state(LinkState::Linked);
}

void WindowLink::set_peer_screen(int peer_w, int peer_h) {
    std::lock_guard lock(mutex_);
    if (peer_w > 0 && peer_h > 0) {
        peer_w_ = peer_w;
        peer_h_ = peer_h;
    }
}

void WindowLink::set_peer(const proto::Hello& hello) {
    set_peer_screen(hello.screen_w, hello.screen_h);
    peer_knows_codecs_ = !hello.video_decode.empty();
    {
        std::lock_guard lock(mutex_);
        codec_ = codec::choose(options_.codec, options_.video_encode, hello.video_decode);
        desk_codec_ = codec::choose_desk(options_.codec, options_.video_encode, hello.video_decode);
        log::info("video: windows sent to '{}' go as {} ({}); desk mode as {} ({})", hello.hostname, codec_.codec,
                  codec_.why, desk_codec_.codec, desk_codec_.why);
    }
    if (!options_.desk) return;
    if (!hello.desk) {
        log::info(
            "desk: the other PC doesn't show desk mode (--no-desk or an older build); Ctrl+Alt+Shift+W uses the "
            "viewer");
        return;
    }
    if (hello.screen_w > 0 && hello.screen_h > 0) {
        desk_commands_.push(
            {.kind = DeskCommand::Start, .width = hello.screen_w, .height = hello.screen_h, .scale = hello.dpi_scale});
    }
}

void WindowLink::detach() {
    {
        std::lock_guard lock(mutex_);
        stop_share(/*tell_peer=*/false);  // the link is gone; the window comes home anyway
        if (viewer_) {
            viewer_->stop_stream();
            viewer_->set_link_state(LinkState::Lost);
        }
    }
    if (desk_view_) {
        desk_view_->set_input_sink(nullptr);
        desk_view_->clear();
    }
    if (options_.desk) desk_commands_.push({.kind = DeskCommand::Stop});
    std::lock_guard lock(send_mutex_);
    send_ = nullptr;
}

void WindowLink::set_peer_side(display::Side side) {
    if (peer_side_.exchange(side) == side) return;
    if (options_.desk) desk_commands_.push({.kind = DeskCommand::Side});
}

void WindowLink::disable_desk() {
    if (options_.desk) desk_commands_.push({.kind = DeskCommand::Disable});
}

bool WindowLink::handle(const proto::Message& msg) {
    if (const auto* config = std::get_if<proto::VideoConfig>(&msg)) {
        // The other PC's window is coming as `codec`: the viewer gets its decoder ready.
        std::lock_guard lock(video_mutex_);
        if (router_) router_->prepare(config->codec);
        return true;
    }
    std::lock_guard lock(mutex_);
    if (std::holds_alternative<proto::VideoKeyframe>(msg)) {
        if (share_) share_->request_keyframe();  // our window, shown over there: its decoder needs one
        std::lock_guard desk_lock(desk_share_mutex_);
        if (desk_share_) desk_share_->request_keyframe();  // or our desk
        return true;
    }
    if (const auto* stats = std::get_if<proto::VideoStats>(&msg)) {
        if (stats->latency_ms < 0) return true;
        if (share_) share_->on_viewer_stats(stats->latency_ms);  // its bitrate
        std::lock_guard desk_lock(desk_share_mutex_);
        if (desk_share_) desk_share_->on_viewer_stats(stats->latency_ms);
        return true;
    }
    // The peer's input on one of our windows: the viewer's (window stream) or its desk overlay's.
    const auto remote_input = [&] {
        if (input_) return input_->handle(msg);
        if (desk_input_) return desk_input_->handle(msg);
        return false;
    };
    return std::visit(proto::Overloaded{
                          [this](const proto::StreamStart& s) {
                              if (viewer_) viewer_->start_stream(s);  // the other PC's window, shown here
                              return true;
                          },
                          [this](const proto::StreamStop& s) {
                              if (share_ && s.hwnd == reinterpret_cast<std::uint64_t>(share_->window())) {
                                  log::info("windows: the other PC gives the window back");
                                  stop_share(/*tell_peer=*/false);
                              } else if (viewer_) {
                                  viewer_->stop_stream();
                              }
                              return true;
                          },
                          [this](const proto::DeskCursor& c) {
                              if (desk_view_) desk_view_->set_cursor_shape(c.shape);
                              return true;
                          },
                          [this](const proto::DeskLayout& l) {
                              if (desk_view_)
                                  desk_view_->set_layout(l);  // the other PC's windows: where, in what order
                              return true;
                          },
                          [&](const proto::RemoteMouseMove&) {
                              remote_input();
                              return true;
                          },
                          [&](const proto::MouseButtonEvent&) { return remote_input(); },
                          [&](const proto::MouseWheel&) { return remote_input(); },
                          [&](const proto::Key&) { return remote_input(); },
                          [](const auto&) { return false; },
                      },
                      msg);
}

void WindowLink::toggle_hud() {
    std::lock_guard lock(mutex_);
    if (viewer_) viewer_->toggle_hud();
}

void WindowLink::toggle() {
    toggle(GetForegroundWindow());
}

void WindowLink::toggle(HWND foreground) {
    if (foreground && foreground == viewer_hwnd_.load()) {
        PostMessageW(foreground, WM_CLOSE, 0, 0);  // the viewer sends stream_stop: the window goes home
        return;
    }
    if (desk_live_) {
        toggle_desk(foreground);
        return;
    }
    std::lock_guard lock(mutex_);
    if (share_) {
        stop_share(/*tell_peer=*/true);  // this PC's window is over there: bring it back
        return;
    }
    {
        std::lock_guard send_lock(send_mutex_);
        if (!send_) {
            log::warn("windows: not connected to the other PC");
            return;
        }
    }
    if (!sendable(foreground)) {
        log::warn("windows: click the window to send first, then press Ctrl+Alt+Shift+W");
        return;
    }
    // Frames go out on whatever video connection is current (it can be replaced on reconnect);
    // without one they are dropped and the share keeps running.
    try {
        share_ = WindowShare::start(
            foreground,
            [this](const proto::FrameHeader& h, std::span<const std::uint8_t> p) { return send_frame(h, p); },
            {.width = peer_w_,
             .height = peer_h_,
             .hz = 60,
             .side = peer_side_.load(),
             .jpeg_quality = options_.jpeg_quality,
             .max_fps = options_.max_fps,
             .codec = codec_.codec});
    } catch (const std::exception& e) {
        log::error("windows: {}", e.what());
        return;
    }
    input_ = std::make_unique<RemoteInput>(share_->window(), share_->monitor_rect());
    const std::string title = to_utf8(title_of(foreground));
    send(proto::StreamStart{.hwnd = reinterpret_cast<std::uint64_t>(foreground),
                            .title = title,
                            .width = share_->width(),
                            .height = share_->height()});
    if (peer_knows_codecs_) {
        send(proto::VideoConfig{.codec = share_->codec(), .width = share_->width(), .height = share_->height()});
    }
    log::info("windows: \"{}\" is on the other PC now ({}x{}); Ctrl+Alt+Shift+W brings it back", title, share_->width(),
              share_->height());
}

// Desk mode: the window in front goes onto the other PC's screen, maximized, or comes back.
void WindowLink::toggle_desk(HWND window) {
    if (!sendable(window)) {
        log::warn("windows: click the window to send first, then press Ctrl+Alt+Shift+W");
        return;
    }
    display::Rect vdd;
    bool live = false;
    if (!options_.desk->vdd_rect(vdd, live) || !live) return;
    const std::string title = to_utf8(title_of(window));

    std::lock_guard lock(parked_mutex_);
    if (const auto it = parked_.find(window); it != parked_.end()) {
        it->second->restore(window);
        parked_.erase(it);
        log::info("desk: \"{}\" is back here", title);
        return;
    }
    RECT wr{};
    GetWindowRect(window, &wr);
    if (vdd.contains({(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2})) {
        // Dragged over by hand: bring it to the middle of the primary monitor, as it is.
        const auto primary = display::primary_monitor().work;
        if (IsZoomed(window)) ShowWindow(window, SW_RESTORE);
        GetWindowRect(window, &wr);
        const int w = std::min<int>(wr.right - wr.left, primary.width());
        const int h = std::min<int>(wr.bottom - wr.top, primary.height());
        SetWindowPos(window, nullptr, primary.left + (primary.width() - w) / 2,
                     primary.top + (primary.height() - h) / 2, w, h, SWP_NOZORDER);
        log::info("desk: \"{}\" is back here", title);
        return;
    }
    auto mover = std::make_unique<WindowMover>();
    const RECT target{vdd.left, vdd.top, vdd.right, vdd.bottom};
    switch (mover->send(window, target)) {
    case WindowMover::Result::Ok:
        parked_[window] = std::move(mover);
        log::info("desk: \"{}\" is on the other PC's screen; Ctrl+Alt+Shift+W brings it back", title);
        break;
    case WindowMover::Result::Elevated:
        log::warn("desk: \"{}\" runs as administrator; run this app as administrator to move it", title);
        break;
    default: log::warn("desk: \"{}\" could not be moved", title); break;
    }
}

void WindowLink::desk_main(std::stop_token stop) {
    desk::Region& region = *options_.desk;
    std::optional<VirtualDisplay> vdd = options_.own_vdd ? VirtualDisplay::find() : std::nullopt;
    std::string device;
    if (!options_.own_vdd) {
        log::info("desk: --desk-no-vdd - this PC only shows the other PC's windows");
    } else if (!vdd) {
        log::info(
            "desk: no Virtual Display Driver here - this PC shows the other PC's windows, but can't send its own");
    } else {
        device = to_utf8(vdd->device());
        region.set_vdd({.device = device});  // the hooks know which monitor is ours from now on
        if (vdd->attached()) {
            log::info("desk: detaching the virtual display left over from last time");
            vdd->detach();
        }
    }

    std::unique_ptr<DeskShare> share;
    // The share goes; the viewer's feedback (handle()) must not reach it any more.
    const auto forget_share = [&] {
        {
            std::lock_guard desk_lock(desk_share_mutex_);
            desk_share_ = nullptr;
        }
        share.reset();
    };
    display::Rect vdd_rect;
    SIZE size{0, 0};
    std::optional<Clock::time_point> stop_at;
    bool disabled = false;
    std::string last_shape;
    auto next_recheck = Clock::now();

    const auto go_live = [&](bool live) {
        region.set_vdd({.device = device, .rect = vdd_rect, .live = live});
        desk_live_ = live;
        std::lock_guard lock(mutex_);
        if (live && !desk_input_) desk_input_ = std::make_unique<DeskInput>(region);
        if (!live) desk_input_.reset();  // releases what the peer held
    };
    const auto finish = [&] {
        stop_at.reset();
        if (!share && !(vdd && vdd->attached())) return;
        go_live(false);
        forget_share();
        {
            std::lock_guard lock(parked_mutex_);
            for (auto& [hwnd, mover] : parked_) mover->restore(hwnd);
            parked_.clear();
        }
        if (vdd && vdd->attached()) vdd->detach();  // Windows moves what is left to the real monitors
        vdd_rect = {};
        region.set_vdd({.device = device});
        size = {};
        stop_at.reset();
        log::info("desk: off");
    };
    const auto make_share = [&](const VirtualDisplay::MonitorRect& r) {
        try {
            std::string codec;
            {
                std::lock_guard lock(mutex_);
                codec = desk_codec_.codec;
            }
            share = std::make_unique<DeskShare>(
                r, region,
                [this](const proto::FrameHeader& hdr, std::span<const std::uint8_t> p) { return send_frame(hdr, p); },
                [this](const proto::DeskLayout& layout) { send(layout); },
                DeskShare::Options{
                    .jpeg_quality = std::max(options_.jpeg_quality, 80), .max_fps = options_.max_fps, .codec = codec});
            std::lock_guard desk_lock(desk_share_mutex_);
            desk_share_ = share.get();
            return true;
        } catch (const std::exception& e) {
            log::error("desk: {}", e.what());
            forget_share();
            return false;
        }
    };
    const auto start = [&](int w, int h, double scale) {
        stop_at.reset();
        if (!vdd || disabled) return;
        // The VDD must be exactly the other screen's size (1:1 mapping): add a missing mode to the
        // driver once. Not while windows sit on the VDD: restarting the driver would drop them.
        if (!share && !vdd->attached() && vdd_modes::ensure(w, h)) {
            if (auto again = VirtualDisplay::find()) {
                vdd = std::move(again);
                device = to_utf8(vdd->device());
                region.set_vdd({.device = device});
            }
            if (vdd->attached()) vdd->detach();  // Windows may have extended onto it on its return
        }
        const SIZE want = vdd->best_mode(w, h);
        if (share && want.cx == size.cx && want.cy == size.cy && vdd->attached()) {
            vdd->set_scale(scale);
            go_live(true);   // reconnected within the grace period: everything stayed where it was
            share->resend();  // the peer starts from nothing
            log::info("desk: back on");
            return;
        }
        go_live(false);
        forget_share();
        const auto side = peer_side_.load();
        const bool ok = vdd->attached() ? vdd->match_peer(w, h, side) : vdd->attach(w, h, 60, side);
        const auto r = vdd->rect();
        if (!ok || !r) {
            log::error("desk: the virtual display could not be attached - Ctrl+Alt+Shift+W uses the viewer");
            if (vdd->attached()) vdd->detach();
            region.set_vdd({.device = device});
            return;
        }
        // Windows on it get the peer's scale, so they look over there like the peer's own windows.
        vdd->set_scale(scale);
        vdd_rect = to_rect(r->rect);
        region.set_vdd({.device = device, .rect = vdd_rect, .live = false});
        if (!make_share(*r)) {
            vdd->detach();
            vdd_rect = {};
            region.set_vdd({.device = device});
            return;
        }
        size = want;
        go_live(true);
        log::info("desk: on - the other PC's screen ({}x{}) is on the {}: drag windows over the edge", vdd_rect.width(),
                  vdd_rect.height(), display::to_string(side));
    };

    while (!stop.stop_requested()) {
        if (auto cmd = desk_commands_.pop_for(kDeskPoll)) {
            switch (cmd->kind) {
            case DeskCommand::Start: start(cmd->width, cmd->height, cmd->scale); break;
            case DeskCommand::Side:
                    // The other PC stands elsewhere now: move the VDD there. The recheck below sees it
                    // moved and shares it anew from its new place.
                if (share && vdd && vdd->attached()) {
                    const auto side = peer_side_.load();
                    if (vdd->place(side))
                        log::info("desk: the other PC's screen moved to the {}", display::to_string(side));
                    next_recheck = Clock::now();
                }
                break;
            case DeskCommand::Stop:
                if (share) {
                    go_live(false);  // the cursor can't go there meanwhile; windows stay put
                    stop_at = Clock::now() + kDeskGrace;
                }
                break;
            case DeskCommand::Disable:
                disabled = true;
                finish();
                break;
            case DeskCommand::Exit: finish(); return;
            }
        }
        const auto now = Clock::now();
        if (stop_at && now >= *stop_at) finish();
        if (share && now >= next_recheck) {
            next_recheck = now + kVddRecheck;
            const auto r = vdd->rect();
            if (!r) {
                log::warn("desk: the virtual display went away");
                finish();
            } else if (to_rect(r->rect) != vdd_rect) {
                // Moved in the display settings: positions are relative to it, so start sharing anew.
                vdd_rect = to_rect(r->rect);
                region.set_vdd({.device = device, .rect = vdd_rect, .live = desk_live_.load()});
                forget_share();
                if (!make_share(*r)) finish();
            }
        }
        // The cursor shape over our windows on the other PC's screen (I-beam, hand, resize arrows...).
        if (share && desk_live_) {
            CURSORINFO ci{sizeof(ci)};
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) &&
                vdd_rect.contains({ci.ptScreenPos.x, ci.ptScreenPos.y})) {
                const std::string shape = cursor_shape(ci.hCursor);
                if (shape != last_shape && send(proto::DeskCursor{.shape = shape})) last_shape = shape;
            }
        }
    }
    finish();
}

void WindowLink::set_rtt_ms(int rtt_ms, std::int64_t clock_offset_us) {
    {
        std::lock_guard lock(mutex_);
        if (viewer_) viewer_->set_rtt_ms(rtt_ms);
    }
    std::lock_guard lock(video_mutex_);
    if (router_) router_->set_clock_offset_us(clock_offset_us);
}

void WindowLink::stop_share(bool tell_peer) {
    if (!share_) return;
    const HWND hwnd = share_->window();
    if (input_) {
        input_->release_all();
        input_.reset();
    }
    share_.reset();  // sender, capture, window back where it was, VDD off
    if (tell_peer) send(proto::StreamStop{.hwnd = reinterpret_cast<std::uint64_t>(hwnd)});
    log::info("windows: the window is back here");
}

bool WindowLink::send(const proto::Message& msg) {
    Sink sink;
    {
        std::lock_guard lock(send_mutex_);
        sink = send_;
    }
    return sink && sink(msg);
}

bool WindowLink::send_frame(const proto::FrameHeader& header, std::span<const std::uint8_t> payload) {
    std::shared_ptr<net::FrameChannel> channel;
    {
        std::lock_guard video_lock(video_mutex_);
        channel = video_;
    }
    if (channel) channel->send_frame(header, payload);
    return true;  // without a channel frames are dropped; the share keeps running
}

}  // namespace kvm
