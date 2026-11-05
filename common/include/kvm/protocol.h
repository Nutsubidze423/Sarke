// SPDX-License-Identifier: Apache-2.0

// Wire protocol between the two Sarke processes (kvm_host listens, kvm_client connects; once
// connected, both sides can do everything). Every message is listed in docs/PROTOCOL.md: change
// the two together, and bump kProtocolVersion when an older peer can't understand the change.
//
// Port 50500 (control + input): [u32 len][JSON], big-endian length.
// Port 50501 (video):           [u32 header_len][u32 payload_len][header JSON][payload], big-endian.
//
// Every control message is a JSON object with a "type" field. Use encode()/decode() below
// instead of building JSON by hand.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace kvm::proto {

inline constexpr std::uint16_t kControlPort = 50500;
inline constexpr std::uint16_t kVideoPort = 50501;
// Bump when a message changes incompatibly; A rejects a hello with a different version.
// v2: everything is symmetric — KVM, window streams and remote input work A -> B and B -> A.
// 3: every channel is TLS 1.3 between paired PCs (docs/SECURITY.md); hello carries device_id, and the
//    host's hello the session token and ports of the side channels (video, files).
inline constexpr int kProtocolVersion = 3;
// Upper bound for one framed message, guards against garbage lengths.
inline constexpr std::uint32_t kMaxControlMessageBytes = 64 * 1024;
inline constexpr std::uint32_t kMaxFramePayloadBytes = 32 * 1024 * 1024;

enum class MouseButton { Left, Right, Middle, X1, X2 };

NLOHMANN_JSON_SERIALIZE_ENUM(MouseButton, {
                                              {MouseButton::Left, "left"},
                                              {MouseButton::Right, "right"},
                                              {MouseButton::Middle, "middle"},
                                              {MouseButton::X1, "x1"},
                                              {MouseButton::X2, "x2"},
                                          })

// Both directions, first message on a new connection: B (always the TCP client) sends it, A checks
// protocol_version and answers with its own. screen_* = the sender's primary monitor, so a window
// owner can size its virtual display to the viewer's screen.
// desk = the sender can show the peer's windows in desk mode (the peer's screen is one more monitor:
// windows are dragged across and drawn on top of the desktop, see desk_move / FrameHeader::layer).
struct Hello {
    static constexpr std::string_view kType = "hello";
    int protocol_version = kProtocolVersion;
    int screen_w = 0;  // physical pixels
    int screen_h = 0;
    double dpi_scale = 1.0;
    std::string hostname;
    bool desk = false;
    std::string device_id;  // the sender's device ID (64 hex), the same as its TLS key's; for logs
    // Host -> client only: what the side channels (video, files) present in their channel_hello, and
    // where they connect (0 = the client's own config).
    std::string session_token;
    int video_port = 0;
    int file_port = 0;
    // Window-stream codecs this PC can decode (show) and encode (send), e.g. ["h264", "jpeg"]
    // (kvm/codec.h). Missing (an older Sarke) = JPEG only.
    std::vector<std::string> video_decode;
    std::vector<std::string> video_encode;
};

// Both directions (KVM), controller -> controlled. Either PC can be the controller: the side whose
// physical mouse/keyboard crossed its edge; from now on its input drives the receiver. y_norm = edge crossing height
// (0..1). desk = desk mode: the cursor follows desk_move (absolute) instead of mouse_move, no edge logic.
struct FocusEnter {
    static constexpr std::string_view kType = "focus_enter";
    double y_norm = 0.5;
    bool desk = false;
};

// Both directions (KVM). Ends the control started by focus_enter. Controller -> controlled: "I took my
// input back" (emergency key, disconnect). Controlled -> controller: "your cursor came back through my
// edge" (only that side knows). kvm::KvmLink tells the two apart by its own state. y_norm = edge height.
struct FocusLeave {
    static constexpr std::string_view kType = "focus_leave";
    double y_norm = 0.5;
};

// KVM, controller -> controlled: relative mouse movement in physical pixels.
struct MouseMove {
    static constexpr std::string_view kType = "mouse_move";
    int dx = 0;
    int dy = 0;
};

// Both directions. KVM: controller -> controlled (x_norm/y_norm unused). Remote input: viewer ->
// window owner, x_norm/y_norm = position inside the streamed picture (0..1).
struct MouseButtonEvent {
    static constexpr std::string_view kType = "mouse_button";
    MouseButton button = MouseButton::Left;
    bool down = false;
    double x_norm = 0.0;
    double y_norm = 0.0;
};

// Both directions. delta in WHEEL_DELTA units (120 = one notch).
struct MouseWheel {
    static constexpr std::string_view kType = "mouse_wheel";
    int delta = 0;
    bool horizontal = false;
};

// Both directions.
struct Key {
    static constexpr std::string_view kType = "key";
    std::uint16_t vk = 0;
    std::uint16_t scan = 0;
    bool down = false;
    bool extended = false;
};

// Window owner -> viewer (either direction): a window stream starts on the video channel.
// width/height = size of the streamed picture (the owner's virtual display) in physical pixels.
struct StreamStart {
    static constexpr std::string_view kType = "stream_start";
    std::uint64_t hwnd = 0;
    std::string title;
    int width = 0;
    int height = 0;
};

// Window owner -> viewer: the stream stopped (window went back). Viewer -> window owner: "give the
// window back" (viewer closed or its return button); the owner restores it and answers with stream_stop.
struct StreamStop {
    static constexpr std::string_view kType = "stream_stop";
    std::uint64_t hwnd = 0;
};

// Viewer -> window owner: pointer position inside the streamed picture, normalized 0..1.
struct RemoteMouseMove {
    static constexpr std::string_view kType = "remote_mouse_move";
    double x_norm = 0.0;
    double y_norm = 0.0;
};

// Both directions: one piece of copied content (kvm/clipboard.h, kvm::clip::Chunk).
// Pieces of one copy share seq; the receiver joins them by offset until total bytes arrived.
struct Clipboard {
    static constexpr std::string_view kType = "clipboard";
    std::uint32_t seq = 0;
    std::string kind;          // "text" (UTF-8) or "png"
    std::uint32_t total = 0;   // bytes of the whole content
    std::uint32_t offset = 0;  // where this piece starts
    std::string data;          // base64, at most kvm::clip::kChunkBytes (32 KB) decoded
};

// Both directions. t is the sender's monotonic clock in microseconds.
struct Ping {
    static constexpr std::string_view kType = "ping";
    std::int64_t t = 0;
};

// Reply to Ping: t is echoed back, t_peer is the replier's clock when it answered
// (lets the sender estimate the clock offset between A and B).
struct Pong {
    static constexpr std::string_view kType = "pong";
    std::int64_t t = 0;
    std::int64_t t_peer = 0;
};

// Desk mode, controller -> controlled: where the cursor is, in physical pixels relative to the
// controlled PC's primary monitor (= the controller's virtual display). Not clamped: it may lie
// beyond the screen while a dragged button is held.
struct DeskMove {
    static constexpr std::string_view kType = "desk_move";
    int x = 0;
    int y = 0;
};

// Desk mode, window owner -> viewer: the cursor shape over the owner's windows ("arrow", "ibeam",
// "hand", "wait", "cross", "sizewe", "sizens", "sizenwse", "sizenesw", "sizeall", "no").
struct DeskCursor {
    static constexpr std::string_view kType = "desk_cursor";
    std::string shape;
};

// One visible piece of a desk-mode picture, in virtual-display pixels.
struct DeskRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// One window on the owner's virtual display: its frame (without the shadow) relative to the virtual
// display. Parts outside desk_w x desk_h are on the owner's own screens and not drawn.
struct DeskWindow {
    std::uint64_t id = 0;  // the owner's HWND
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int radius = 0;  // its corners' radius in px (Windows 11 rounds them); 0 = square
};

// Desk mode, window owner -> viewer: the owner's windows on its virtual display, topmost first. Sent
// whenever one moves, opens, closes or changes order; the pictures come separately on the video
// channel (FrameHeader::window), so moving a window costs only this message.
struct DeskLayout {
    static constexpr std::string_view kType = "desk_layout";
    int desk_w = 0;
    int desk_h = 0;
    bool active = false;  // an owner window on the VDD is in front: the viewer raises its overlay above its own windows
    std::vector<DeskWindow> windows;
};

// Both directions, right after the hellos and whenever the user moves the other PC in the Displays
// window: where the receiver stands next to the sender ("right" = on the sender's right) and when the
// user set that (unix ms; 0 = never set, a default). Both PCs keep the newer one, on a tie the host's;
// the receiver keeps the opposite for itself (it stands right of us = we stand left of it). Each PC
// remembers this per peer (config.json "peers"), so every PC it meets keeps its own place.
struct Layout {
    static constexpr std::string_view kType = "layout";
    std::string you_are;  // "left" / "right" / "top" / "bottom"
    std::int64_t set_at = 0;
};

// Both directions: the user pressed Identify in the Displays window - show the screen numbers on
// this PC's monitors too.
struct Identify {
    static constexpr std::string_view kType = "identify";
};

// Both directions, in a paired session: the sender's user unpaired the receiver (docs/SECURITY.md
// 6.6). The receiver forgets the sender too and doesn't offer to pair again by itself; the sender
// closes the session right after.
struct Unpaired {
    static constexpr std::string_view kType = "unpaired";
};

// Window owner -> viewer, after stream_start (roadmap P2.5): what the window's frames will be, so the
// viewer can get its decoder ready. Only to a PC whose hello listed video_decode.
struct VideoConfig {
    static constexpr std::string_view kType = "video_config";
    std::string codec;  // "jpeg" or "h264"
    int width = 0;
    int height = 0;
};

// Viewer -> window owner: the viewer's H.264 decoder needs a keyframe (it starts, lost a frame, or a
// frame failed the framing check). The owner rate-limits them.
struct VideoKeyframe {
    static constexpr std::string_view kType = "video_keyframe";
};

// Viewer -> window owner, once a second while H.264 frames are shown (roadmap P2.6): how they arrive,
// so the owner can lower its bitrate when they come later and later (kvm/bitrate_control.h).
struct VideoStats {
    static constexpr std::string_view kType = "video_stats";
    int latency_ms = -1;  // capture on the owner -> on screen here, p50 of the last second; -1 unknown
    int fps = 0;  // frames shown in the last second
};

// Pairing (docs/SECURITY.md 6.3): the only messages accepted from a PC whose key isn't paired yet.
// The PC whose user picked the other one sends pair_request. Each side shows the code derived from
// its own TLS session and sends pair_confirm with its user's answer. Each stores the other only when
// both said yes, then sends pair_result.
struct PairRequest {
    static constexpr std::string_view kType = "pair_request";
    std::string name;  // the sender's computer name, for the other user's prompt
};

struct PairConfirm {
    static constexpr std::string_view kType = "pair_confirm";
    bool ok = false;  // the user saw the same code and confirmed
};

struct PairResult {
    static constexpr std::string_view kType = "pair_result";
    bool ok = false;
    std::string reason;  // when !ok, e.g. "cancelled", "timeout"
};

// Video channel header (port 50501), followed by `payload_len` bytes of encoded image.
// layer "" = a window stream for the viewer window. layer "desk" = desk mode: the picture of one
// window of the owner's virtual display (`window` = DeskWindow::id), drawn at that window's place
// from the newest desk_layout, offset by (x, y). The payload is the JPEG, followed by `alpha_bytes`
// of its transparency (rounded corners): per row, runs of [u8 alpha][u16 count, little-endian];
// alpha_bytes 0 = fully opaque. The JPEG's colours are premultiplied by that alpha.
// codec "h264" (window streams): the payload is one H.264 access unit in Annex-B form (kvm/annexb.h);
// keyframe = it is an IDR picture with SPS and PPS in front, where a viewer can start decoding.
// A viewer skips codecs it doesn't show.
struct FrameHeader {
    static constexpr std::string_view kType = "frame";
    std::uint32_t seq = 0;
    std::int64_t t_capture_us = 0;  // A's monotonic clock
    int width = 0;
    int height = 0;
    std::string codec = "jpeg";  // "jpeg" or "h264"
    bool keyframe = false;  // h264 only
    std::string layer;
    int x = 0;
    int y = 0;
    int desk_w = 0;
    int desk_h = 0;
    std::vector<DeskRect> rects;
    std::uint64_t window = 0;
    std::uint32_t alpha_bytes = 0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Hello, protocol_version, screen_w, screen_h, dpi_scale, hostname, desk,
                                                device_id, session_token, video_port, file_port, video_decode,
                                                video_encode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FocusEnter, y_norm, desk)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FocusLeave, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MouseMove, dx, dy)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MouseButtonEvent, button, down, x_norm, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MouseWheel, delta, horizontal)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Key, vk, scan, down, extended)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StreamStart, hwnd, title, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StreamStop, hwnd)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RemoteMouseMove, x_norm, y_norm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Clipboard, seq, kind, total, offset, data)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Ping, t)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Pong, t, t_peer)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeskMove, x, y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeskCursor, shape)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeskRect, x, y, w, h)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeskWindow, id, x, y, w, h, radius)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DeskLayout, desk_w, desk_h, active, windows)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Layout, you_are, set_at)
inline void to_json(nlohmann::json& j, const Identify&) {
    j = nlohmann::json::object();
}
inline void from_json(const nlohmann::json&, Identify&) {}
inline void to_json(nlohmann::json& j, const Unpaired&) {
    j = nlohmann::json::object();
}
inline void from_json(const nlohmann::json&, Unpaired&) {}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(VideoConfig, codec, width, height)
inline void to_json(nlohmann::json& j, const VideoKeyframe&) {
    j = nlohmann::json::object();
}
inline void from_json(const nlohmann::json&, VideoKeyframe&) {}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(VideoStats, latency_ms, fps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PairRequest, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PairConfirm, ok)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PairResult, ok, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FrameHeader, seq, t_capture_us, width, height, codec, keyframe, layer,
                                                x, y, desk_w, desk_h, rects, window, alpha_bytes)

// Any message that travels on the control channel (port 50500).
using Message =
    std::variant<Hello, FocusEnter, FocusLeave, MouseMove, MouseButtonEvent, MouseWheel, Key, StreamStart, StreamStop,
                 RemoteMouseMove, Clipboard, Ping, Pong, DeskMove, DeskCursor, DeskLayout, Layout, Identify,
                 PairRequest, PairConfirm, PairResult, Unpaired, VideoConfig, VideoKeyframe, VideoStats>;

// Serializes a message struct (including FrameHeader) to JSON with its "type" field set.
template <class M>
nlohmann::json encode(const M& msg) {
    nlohmann::json j = msg;
    j["type"] = M::kType;
    return j;
}

inline nlohmann::json encode(const Message& msg) {
    return std::visit([](const auto& m) { return encode(m); }, msg);
}

// The "type" string of a message, e.g. for logging.
inline std::string_view type_of(const Message& msg) {
    return std::visit([](const auto& m) { return std::decay_t<decltype(m)>::kType; }, msg);
}

// Parses a control-channel message. Returns nullopt for unknown types or malformed fields;
// never throws.
inline std::optional<Message> decode(const nlohmann::json& j) {
    const auto it = j.find("type");
    if (it == j.end() || !it->is_string()) {
        return std::nullopt;
    }
    const auto& type = it->get_ref<const std::string&>();

    std::optional<Message> out;
    try {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (void)((type == std::variant_alternative_t<I, Message>::kType
                        ? (out.emplace(std::in_place_index<I>, j.get<std::variant_alternative_t<I, Message>>()), true)
                        : false) ||
                   ...);
        }(std::make_index_sequence<std::variant_size_v<Message>>{});
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    return out;
}

// Visitor helper: std::visit(kvm::proto::Overloaded{[](const Hello&) {...}, ...}, msg);
template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

}  // namespace kvm::proto
