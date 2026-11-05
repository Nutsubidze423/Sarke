# Sarke wire protocol

Protocol version **3** (`kProtocolVersion` in [protocol.h](../common/include/kvm/protocol.h)). The
code is the authority: `protocol.h` defines every message, and this page explains them. Change the
two together, and bump the version when an older peer can't understand the change.

**Version history:**
- **3:** every channel is TLS 1.3 between paired PCs, and `hello` carries `device_id`. The host's
  `hello` carries the session token and ports of the video and file channels, which open with
  `channel_hello`. Version 2 peers can't connect (they don't speak TLS).
- **2:** the hackathon release, Sarke 1.1.0.

## Connections

`kvm_host` listens and `kvm_client` connects. Once connected the roles don't matter: either side
can take control, share windows, copy and send files. Every PC starts as a host; the launcher
(`sarke.exe`) finds the others on the network and connects to the one the user picks.

| Port | Transport | Framing | Carries |
|---|---|---|---|
| 50500 | TCP, **TLS 1.3** | `[u32 len][JSON]` inside TLS, big-endian length, at most 64 KB | control and input messages (table below) |
| 50501 | TCP, **TLS 1.3** | `[u32 header_len][u32 payload_len][header JSON][payload]` inside TLS, big-endian, payload at most 32 MB | `channel_hello`, then video frames, both directions on one connection |
| 50502 | TCP, **TLS 1.3** | same as 50501 | `channel_hello`, then files: `transfer_begin` (`drag`, drag image), `file_begin`, `file_chunk`, `file_end`, `transfer_end` |
| 50503 | UDP | one JSON object per datagram | discovery: `kvm_discover` (broadcast), answered by `kvm_here` |

`TCP_NODELAY` is set on every TCP socket.

**Who may connect** (50500; [SECURITY.md](SECURITY.md) §6.4):
- Both sides present a certificate. The TLS handshake runs first.
- Then the peer's device ID (SHA-256 of its key) must be in this PC's `paired.json`. Otherwise the
  connection is closed **before anything is read from it**.
- A peer that doesn't speak TLS is closed at the handshake. The host recognises Sarke 1.1's
  plaintext hello (protocol v2) by its first bytes and logs "an older Sarke (protocol v2) tried to
  connect".
- A paired peer whose `hello` has another `protocol_version` gets the host's `hello` (with the
  host's version) before it is dropped, so both sides can say which PC to update.

**Pairing** ([SECURITY.md](SECURITY.md) §6.3) is the one exchange allowed with a key that isn't
paired:
1. The client starts it when its user picked the host in the launcher (`--pair-unknown`). It sends
   `pair_request`.
2. Each side derives a 6-digit code from its own TLS session and logs
   `pairing: code 042 917 with 'name' (id) - confirm on both PCs`.
3. The launcher asks its user and answers `pair yes` / `pair no` on the app's stdin.
4. Each side sends `pair_confirm`, and stores the other only if both said yes (`pair_result`).
5. The client then connects again as a paired PC.

The host allows 3 pairing attempts per IP per 10 minutes. For development, `--pair-with <device id>`
pairs by hand (`--print-id` shows the ID).

**Side channels** (50501 video, 50502 files; [SECURITY.md](SECURITY.md) §6.4) belong to the live
control session:
1. The host's `hello` answer carries `session_token` (32 random bytes, 64 hex, new for every
   session), `video_port` and `file_port`. The client opens the side channels only after it.
2. Each side channel runs TLS with the same keys as the control channel. The client checks that the
   host has the control session's key.
3. Its first frame is `channel_hello`: header `{"type": "channel_hello", "token": "<64 hex>"}`,
   payload length 0, at most 1 KB, within 5 s.
4. The host keeps the channel only if the peer's key is the control session's and the token matches.
   Otherwise it closes it, and the live channel stays.
5. When the session ends, the host closes its side channels and refuses the old token.

**Discovery.** A PC looking for others broadcasts `kvm_discover`. Every running host answers
`kvm_here` with `name`, `control_port`, `protocol_version`, `busy` (a client is already connected)
and `device_id` (64 hex, its TLS key). The launcher scans every 2.5 s and lists the PCs in the tray
("Connect to ▸"), one row per device ID: a paired PC can be connected to, any other shows
"Pair…". It connects by itself only to paired PCs set to "Connect automatically" (`auto_connect` in
`paired.json`). Answers aren't authenticated ([SECURITY.md](SECURITY.md) §6.5): they only decide
where to try TLS, and pinning decides the rest.
`config.json` `"host_ip": "auto"` (the default) needs no IP; `kvm_client --host <IP>` connects
directly.

## Control messages (port 50500)

Every message is a JSON object with a `type` field. Use `encode()` / `decode()` from `protocol.h`,
never hand-built JSON.

**Roles in the Direction column:**
- **controller / controlled:** in KVM, the PC whose mouse and keyboard are in use, and the PC they
  are currently driving.
- **window owner / viewer:** the PC where a shared window's process runs, and the PC that shows it.

| `type` | Direction | Fields |
|---|---|---|
| `hello` | client → host, then host → client (the answer) | `protocol_version`, `screen_w`, `screen_h`, `dpi_scale`, `hostname`, `desk` (desk mode on), `device_id` (64 hex, the sender's TLS key; for logs), `video_decode` and `video_encode` (the window-stream codecs the sender can show and send, e.g. `["h264","jpeg"]`; missing = an older Sarke, JPEG only). The host's answer adds `session_token`, `video_port`, `file_port` (side channels, above) |
| `focus_enter` | controller → controlled | `y_norm` (where the cursor enters), `desk` (the cursor follows `desk_move`) |
| `focus_leave` | both | `y_norm`. Controller → controlled: "I took input back". Controlled → controller: "your cursor came back over my edge" |
| `mouse_move` | controller → controlled | `dx`, `dy` (relative) |
| `mouse_button` | both | `button` (`left`/`right`/`middle`/`x1`/`x2`), `down`. Viewer → window owner adds `x_norm`, `y_norm` |
| `mouse_wheel` | both | `delta`, `horizontal` |
| `key` | both | `vk`, `scan`, `down`, `extended` |
| `desk_move` | controller → controlled (desk mode) | `x`, `y`: the cursor on the controlled PC's primary monitor, in px (that is, the controller's VDD) |
| `desk_cursor` | window owner → viewer | `shape` (`arrow`/`ibeam`/`hand`/`sizewe`/…): the cursor shape over the owner's windows |
| `desk_layout` | window owner → viewer | `desk_w`, `desk_h`, `active` (the owner's active window is on the VDD, so the viewer raises its overlay), `windows` [`id`, `x`, `y`, `w`, `h`, `radius`] (the windows on the VDD, top to bottom; `radius`: their rounded corners in px, 0 = square). Moving a window sends only this message |
| `stream_start` | window owner → viewer | `hwnd`, `title`, `width`, `height` |
| `stream_stop` | both | `hwnd`. Owner → viewer: the stream stopped. Viewer → owner: "take it back" |
| `video_config` | window owner → viewer, after `stream_start` | `codec` (`h264`/`jpeg`), `width`, `height`: what the frames will be, so the viewer can get its decoder ready. Only to a PC whose `hello` had `video_decode` |
| `video_keyframe` | viewer → window owner | none. The viewer's H.264 decoder needs a keyframe: it started, lost a frame, or a frame failed the framing check. The viewer sends at most two a second, and the owner acts on at most four |
| `video_stats` | viewer → window owner, once a second while H.264 frames are shown | `latency_ms` (capture on the owner → on screen here, p50 of the last second), `fps`. The owner lowers its H.264 bitrate when the latency rises (roadmap P2.6). Only to a PC whose `hello` had `video_decode` |
| `remote_mouse_move` | viewer → window owner | `x_norm`, `y_norm` (in the streamed window, 0..1; the owner maps them onto the window's current client area) |
| `clipboard` | both | `seq`, `kind` (`text` UTF-8, `png`, or `files`: the copied files' names, with the files going over 50502), `total`, `offset`, `data` (base64, at most 32 KB per message; one copy in several parts) |
| `layout` | both (after `hello`, and on Displays → Apply) | `you_are` (`left`/`right`/`top`/`bottom`: where the **receiver** stands relative to the sender), `set_at` (ms when the user set it; 0 = never). The newer setting wins; on a tie, the host's. Each PC keeps every peer's side in `config.json` `"peers": {name: {side, set_at}}` ([layout_sync](../common/include/kvm/layout_sync.h)) |
| `identify` | both | none. The other PC shows its screen numbers too |
| `ping` / `pong` | both | `t` (`pong` adds `t_peer`, for the clock offset) |
| `pair_request` | pairing initiator → the other PC (key not paired yet) | `name` (shown to the other user only) |
| `pair_confirm` | both, while pairing | `ok`: this user saw the same code and confirmed |
| `pair_result` | both, while pairing | `ok`, `reason` (when not ok) |
| `unpaired` | both, in a paired session | none. The sender's user unpaired the receiver: the receiver forgets the sender too and doesn't offer to pair again by itself; the sender closes the session right after ([SECURITY.md](SECURITY.md) §6.6) |

## Video frames (port 50501)

`frame` header, window owner → viewer:

| Field | Meaning |
|---|---|
| `seq` | frame number, gap-free per stream |
| `t_capture_us` | when it was captured (owner's clock; the viewer maps it with the ping/pong offset) |
| `width`, `height` | picture size |
| `codec` | `jpeg`, or `h264` for a window stream |
| `keyframe` | `h264`: the frame is an IDR picture with SPS and PPS in front, where a viewer can start decoding (missing = false) |
| `layer` | `"desk"` in desk mode |
| `window` | desk mode: the window this picture is of (one window per frame, WGC window capture). With `h264`: 0, the picture is the whole VDD (`desk_w` × `desk_h`) and the viewer cuts each window out at its `desk_layout` rect, rounded by its `radius` |
| `x`, `y` | desk mode: where the picture sits in the window's frame |
| `alpha_bytes` | desk mode: the payload is the JPEG (4:4:4) followed by this many bytes of alpha runs `[u8 alpha][u16 count]` (rounded corners) |

The sender always sends the newest frame and drops older ones; it never queues video.

An `h264` payload is one H.264 access unit in Annex-B form: NAL units, each after a start code
`00 00 01` or `00 00 00 01`. Before a decoder sees it, the viewer checks the framing
([kvm/annexb.h](../common/include/kvm/annexb.h)):
- it starts with a start code;
- no NAL unit is empty, has the forbidden bit set, or has a type H.264 doesn't define for a stream;
- it holds one picture;
- `keyframe` is true exactly when that picture is an IDR picture, and then SPS and PPS come first.

A frame that fails is dropped.

A viewer skips any codec it doesn't show, so an older viewer stays blank on `h264` but doesn't
break.

**Which codec a window goes as** ([kvm/codec.h](../common/include/kvm/codec.h)), decided by the window
owner from the viewer's `hello`:
- `h264` when the owner can encode it (a hardware encoder on its GPU), the viewer lists it in
  `video_decode`, and the owner wasn't started with `--codec jpeg`;
- else `jpeg`, which every Sarke shows. A viewer whose `hello` has no `video_decode` (an older Sarke)
  gets JPEG.

Desk mode follows the same rule with `h264-desk` in the viewer's `video_decode`: one H.264 stream of
the whole VDD instead of a JPEG per window (roadmap P2.7). A viewer lists `h264-desk` only once it
can draw that; until then desk mode stays JPEG.

Both programs log the decision and its reason when a session starts.
