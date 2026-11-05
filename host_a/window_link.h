// SPDX-License-Identifier: Apache-2.0

// Window glue for kvm_host and kvm_client (owner: Temo): one per process, the same on both PCs.
//
// Desk mode (Options::desk set, both PCs say hello{desk}): the other PC's screen is one more monitor.
// While connected, a Virtual Display Driver (VDD) monitor of the peer's size sits on the peer's side;
// windows dragged onto it keep running here and the peer draws them at the same place on its screen
// (DeskShare -> DeskView), where its own mouse and keyboard can use them too (DeskInput). The other
// PC's windows show up here the same way. Ctrl+Alt+Shift+W sends the window in front over (maximized
// on the peer's screen) and back.
//
// Without desk mode (no VDD, --no-desk), Ctrl+Alt+Shift+W streams the window into Sandro's viewer
// window on the other PC (M1b), as before.
//
//   kvm::WindowLink windows({.jpeg_quality = q, .max_fps = fps, .peer_side = side, .desk = &region});
//   windows.set_video(channel);                  // each video connection (shared, both directions)
//   per session:
//     windows.attach(send, peer_w, peer_h);      // send = control messages to the peer
//     windows.set_peer(hello);                   // the peer's hello: its screen, desk, codecs
//     if (!kvm.handle(m) && !windows.handle(m)) { ... }
//     windows.detach();                          // windows back here (after a grace period in desk mode)
//   hotkey Ctrl+Alt+Shift+W -> windows.toggle();
//   emergency exit          -> windows.disable_desk();
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "kvm/codec.h"
#include "kvm/desk_region.h"
#include "kvm/dpi.h"
#include "kvm/protocol.h"
#include "kvm/queue.h"
#include "kvm/video.h"

namespace kvm {

class ViewerWindow;
class FrameRouter;
class WindowShare;
class RemoteInput;
class DeskShare;
class DeskView;
class DeskInput;
class WindowMover;

class WindowLink {
public:
    using Sink = std::function<bool(const proto::Message&)>;

    struct Options {
        int jpeg_quality = 75;
        int max_fps = 30;
        // Windows sent to the viewer: H.264 when this PC encodes it (video_encode, from
        // probe_video_codecs()) and the peer decodes it, unless --codec jpeg (kvm/codec.h).
        codec::Choice codec = codec::Choice::Auto;
        std::vector<std::string> video_encode = {"jpeg"};
        display::Side peer_side = display::Side::Right;  // where the VDD goes
        desk::Region* desk = nullptr;                    // desk mode; nullptr = off. Outlives the link.
        bool own_vdd = true;  // false: show the peer's windows but never attach this PC's VDD (--desk-no-vdd)
    };

    explicit WindowLink(Options options);
    ~WindowLink();  // windows back here, VDD detached, viewers closed

    WindowLink(const WindowLink&) = delete;
    WindowLink& operator=(const WindowLink&) = delete;

    // The current video connection (port 50501). Frames of this PC's windows go out on it and the
    // other PC's frames are read from it. nullptr = none.
    void set_video(std::shared_ptr<net::FrameChannel> channel);

    // A control session starts: `send` reaches the peer, peer_w x peer_h is its screen (the VDD size).
    void attach(Sink send, int peer_w, int peer_h);
    void set_peer_screen(int peer_w, int peer_h);
    // The peer's hello: its screen size, and whether it shows desk mode. Starts desk mode if it can.
    void set_peer(const proto::Hello& hello);
    // The session ended: this PC's window comes back, the viewers show nothing.
    void detach();

    // stream_* / remote_* / key / mouse_* / desk_cursor from the peer. False = not for windows.
    bool handle(const proto::Message& msg);

    // Ctrl+Alt+Shift+W: send the foreground window over, or bring it back (see above).
    void toggle();
    // The same for `window` instead of the one in front (the "share <hwnd>" launcher command).
    void toggle(HWND window);
    // Ctrl+Alt+Shift+H: shows or hides the viewer's HUD.
    void toggle_hud();

    // Desk mode off for good (emergency exit): windows back, VDD detached. Returns at once.
    void disable_desk();
    bool desk_live() const { return desk_live_; }

    // The other PC stands on `side` now (kvm/layout_sync.h): the VDD moves to that side of this PC's
    // screens (desk mode re-shares from there). Any thread.
    void set_peer_side(display::Side side);

    void set_rtt_ms(int rtt_ms, std::int64_t clock_offset_us);

private:
    struct DeskCommand {
        enum Kind { Start, Stop, Disable, Exit, Side } kind = Start;  // Side: peer_side_ changed
        int width = 0;
        int height = 0;
        double scale = 1.0;  // the peer's display scale
    };

    void stop_share(bool tell_peer);  // mutex_ held
    bool send(const proto::Message& msg);
    bool send_frame(const proto::FrameHeader& header, std::span<const std::uint8_t> payload);
    void desk_main(std::stop_token stop);
    void toggle_desk(HWND window);

    Options options_;
    std::atomic<display::Side> peer_side_;  // options_.peer_side, changed by set_peer_side()
    // mutex_: share_, input_, viewer_, desk_input_, peer size. video_mutex_: video_, router_ (never
    // held while waiting on mutex_, so stopping a share can join its sender thread). send_mutex_: send_.
    // parked_mutex_: parked_ (windows sent over with Ctrl+Alt+Shift+W in desk mode).
    std::mutex mutex_;
    std::mutex video_mutex_;
    std::mutex send_mutex_;
    std::mutex parked_mutex_;
    Sink send_;
    int peer_w_ = 1920;
    int peer_h_ = 1080;
    codec::Decision codec_{"jpeg", "no peer yet"};   // mutex_: what windows sent to the viewer go as
    codec::Decision desk_codec_{"jpeg", "no peer yet"};  // mutex_: what desk mode goes as
    std::mutex desk_share_mutex_;  // desk_share_ (the desk thread's share, for the viewer's feedback)
    DeskShare* desk_share_ = nullptr;
    std::atomic<bool> peer_knows_codecs_{false};  // its hello listed video_decode: it understands video_config
    std::shared_ptr<net::FrameChannel> video_;
    std::unique_ptr<FrameRouter> router_;
    std::unique_ptr<WindowShare> share_;
    std::unique_ptr<RemoteInput> input_;
    std::unique_ptr<ViewerWindow> viewer_;
    std::unique_ptr<DeskView> desk_view_;
    std::unique_ptr<DeskInput> desk_input_;
    std::map<HWND, std::unique_ptr<WindowMover>> parked_;
    std::atomic<bool> desk_live_{false};
    std::atomic<HWND> viewer_hwnd_{nullptr};
    std::atomic<DWORD> ui_thread_id_{0};
    BlockingQueue<DeskCommand> desk_commands_;
    std::thread ui_thread_;
    std::jthread watchdog_;     // ends a share whose window was closed
    std::jthread desk_thread_;  // VDD attach/detach, DeskShare, cursor shape
};

}  // namespace kvm
