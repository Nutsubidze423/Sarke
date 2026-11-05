# Sarke performance measurements

Every number Sarke's docs, README or PRs claim comes from this page, together with how it was
measured and on what hardware. Older numbers stay as history.

## Machines

| Name | CPU | GPU | Windows |
|---|---|---|---|
| **Celeron laptop** (Demetre's dev PC) | Intel Celeron N4020, 2 cores, 1.1 GHz | Intel UHD Graphics 600 | 10 Pro 22H2 (19045) |

## TLS 1.3 overhead (roadmap P1.2, P1.5)

**What:** the round trip of a 64-byte message over 127.0.0.1, echoed by the other end. The same
socket pair runs once as plain TCP (`TCP_NODELAY`) and once through `kvm::tls::Stream` (OpenSSL
3.6.5, TLS 1.3). Median of 2000 round trips. Measured in `tools/tls_test.cpp` (it prints, it
doesn't check).

| Date | Machine | Plain TCP | TLS 1.3 | Added by TLS |
|---|---|---|---|---|
| 2026-10-20 | Celeron laptop, 3 runs | 56 / 59 / 58 µs | 93 / 96 / 94 µs | about +37 µs |

**What it means:** a mouse move or key press crosses the control channel once, so TLS adds about
0.04 ms on this CPU. A home Wi-Fi round trip is a few milliseconds, and the phone hotspot at the
hackathon measured 11–591 ms. This is a loopback figure: it shows the CPU cost of TLS, not the
network.

## Video channel over TLS 1.3 (roadmap P1.7)

**What:** the same tests built twice: from `main` before P1.7 (plain TCP) and from
`phase1/side-channels` (TLS 1.3, OpenSSL 3.6.5). Both run on 127.0.0.1, so one CPU does both
the encrypting and the decrypting. Runs alternated, with nothing else building.
- **Throughput:** `video_test`, 300 frames of 250 KB as fast as the channel takes them.
- **Frame time:** `video_sender_test` step 4. A synthetic 1920×1080 picture is JPEG-encoded
  (quality 75) and sent at most 30 fps. Measured from capture until B has the frame.

| Date | Machine | | Plain TCP | TLS 1.3 |
|---|---|---|---|---|
| 2026-10-21 | Celeron laptop, 3 runs | throughput | 4677 / 5243 / 4543 frames/s (1136–1311 MB/s) | 914 / 851 / 912 frames/s (213–228 MB/s) |
| 2026-10-21 | Celeron laptop, 3 runs | capture → received, 30 fps cap | 30.9 / 31.7 / 30.8 ms, 30 fps | 26.9 / 27.5 / 31.8 ms, 30 fps |

**What it means:**
- TLS costs about 1 ms of CPU per 250 KB frame here, for encrypt and decrypt together.
- 30 fps of 250 KB frames is 7.5 MB/s, about 3% of what the TLS channel carried.
- The frame time didn't change measurably: the run-to-run spread (27–32 ms) is bigger than the
  TLS cost, and both builds kept 30 fps.
- Over a real network each PC does only half of the TLS work, and Wi-Fi bandwidth is the limit
  long before this is.

## Video: capture to screen (roadmap P2.1 baseline)

**How it's measured:**
- `kvm_client --stats run.csv` (or `kvm_host`, whichever PC shows the frames) writes one row per
  frame shown ([kvm/video_stats.h](../common/include/kvm/video_stats.h)), and
  `video_stats_report run.csv "<content>"` turns that into the row below.
- **Latency:** from capture on the sending PC (its clock, mapped with the ping/pong offset) until
  the picture is on screen here. That is `UpdateLayeredWindowIndirect` in desk mode, or `EndDraw` in
  the window viewer.
- **Decode:** JPEG decode plus the alpha mask. **Present:** from decoded to on screen.
- **Dropped:** frame numbers that were never shown. Each desk-mode window counts on its own.
- The viewer's HUD (Ctrl+Alt+Shift+H) and a log line every 5 s show the same numbers for the last
  second.

**Loopback on this PC** ([tools/baseline_loopback.ps1](../tools/baseline_loopback.ps1)):
- **Setup:**
  - the real kvm_host and kvm_client on 127.0.0.1, in desk mode;
  - one 1280×800 window on the virtual display for 30 s;
  - JPEG quality 75 at 4:4:4, a 30 fps cap.
- **Caveat:** one CPU does both PCs' work (capture, encode, decode, compose), so this is a floor for
  the CPU cost, not a network figure.
- **Runs:** two, minutes apart. The second ran right after `ctest --preset local`.

| Date | Machine | Content | fps | Mbit/s | Latency p50 / p95 / p99 (ms) | Encode (ms, sender) | Decode avg (ms) | Present avg (ms) | Dropped |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-21 | Celeron laptop | static document (Notepad) | 29.9 / 28.2 | 71.4 / 67.2 | 50 / 73 / 82 · 70 / 98 / 140 | 17–18 | 14.5 / 17.2 | 7.7 / 7.9 | 0 / 2 |
| 2026-10-21 | Celeron laptop | browser scrolling (Edge) | 23.7 / 25.5 | 78.7 / 41.0 | 92 / 138 / 176 · 134 / 203 / 274 | 26–27 | 23.7 / 16.3 | 9.7 / 9.5 | 0 / 5 |
| 2026-10-21 | Celeron laptop | animation, 40 moving boxes (Edge canvas) | 27.9 / 23.8 | 27.8 / 23.8 | 76 / 111 / 145 · 87 / 132 / 155 | 22–23 | 16.8 / 18.0 | 9.8 / 10.0 | 0 / 0 |

**What it shows:**
- **Desk mode resends unchanged windows at the full frame rate.** A document that doesn't change
  costs 67–71 Mbit/s and a full JPEG encode, about 17 ms, 30 times a second. Sending only when the
  window changed, or H.264 (P2.3, where a still picture costs almost nothing), would remove most of
  that.
- **On this CPU, JPEG is most of the latency:**
  - encode 17–27 ms on the sender, decode 14–24 ms here, present 8–10 ms;
  - p50 from capture to screen is 50–134 ms depending on the content.
- **Scrolling and animation stay under the 30 fps cap** (24–26 fps): the encode, at 22–27 ms per
  picture, is the bottleneck.
- **The two runs differ by up to 40%:** what's on screen while scrolling, and background load.
  Compare changes with several runs, not one.

**Still to measure (needs two PCs):**
- home Wi-Fi and a phone hotspot, with the same content;
- video playback and a Photoshop or Blender window;
- the window viewer as well as desk mode.

## Window viewer: JPEG and H.264 side by side (roadmap P2.4c)

**How it's measured:** [tools/baseline_loopback.ps1](../tools/baseline_loopback.ps1) `-Mode viewer -Codec jpeg|h264`.
- **Setup:**
  - the real kvm_host and kvm_client on 127.0.0.1;
  - the content window is sent to the client's window viewer, the way Ctrl+Alt+Shift+W sends it, for 30 s;
  - a 1920×1080 virtual display, a 30 fps cap;
  - JPEG at quality 75; H.264 at 8 Mbit/s CBR (`kvm_host --codec h264`).
- **Columns:** as in the P2.1 table above, from kvm_client's `--stats`. Decode includes the conversion to BGRA.
- **Caveat:** both PCs run on one PC. For H.264 that means one UHD 600 does everything: the content's own drawing, composing both screens, capture, NV12, encode, decode and present.

| Date | Machine | Content | Codec | fps | Mbit/s | Latency p50 / p95 / p99 (ms) | Decode avg / p95 (ms) | Present avg / p95 (ms) | Dropped |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-21 | Celeron laptop | static document (Notepad) | JPEG | 28.9 | 58.3 | 71 / 93 / 113 | 14.2 / 20.0 | 9.3 / 12.4 | 11 |
| 2026-10-21 | Celeron laptop | static document (Notepad) | H.264 | 30.1 | 7.7 | 51 / 73 / 85 | 2.3 / 4.9 | 6.7 / 13.8 | 2 |
| 2026-10-21 | Celeron laptop | browser scrolling (Edge) | JPEG | 18.3 | 35.2 | 117 / 189 / 228 | 18.4 / 24.0 | 37.6 / 96.4 | 62 |
| 2026-10-21 | Celeron laptop | browser scrolling (Edge) | H.264 | 16.1 | 4.4 | 159 / 236 / 512 | 7.7 / 33.5 | 26.6 / 70.1 | 19 |
| 2026-10-21 | Celeron laptop | animation, 40 moving boxes (Edge canvas) | JPEG | 20.0 | 19.2 | 107 / 188 / 205 | 16.5 / 23.5 | 35.9 / 88.7 | 55 |
| 2026-10-21 | Celeron laptop | animation, 40 moving boxes (Edge canvas) | H.264 | 13.6 | 3.8 | 171 / 255 / 306 | 4.7 / 21.2 | 27.1 / 86.3 | 2 |

**What it shows:**
- **H.264 needs 5–8× less bandwidth:** 4–8 Mbit/s against 19–58 Mbit/s for JPEG.
- **Decoding costs the viewer much less:** 2–8 ms on the GPU, against 14–18 ms for JPEG on the CPU.
- **A document:**
  - H.264 holds 30 fps at 51 ms p50, where JPEG gives 29 fps at 71 ms;
  - JPEG's 58 Mbit/s is a full picture 30 times a second.
- **Scrolling and animation are slower with H.264 on this one PC:**
  - 14–16 fps against JPEG's 18–20 fps, with higher latency;
  - the host logs ~290 pictures "dropped by a busy encoder" in 30 s. The GPU is the bottleneck here: one UHD 600 draws the content and both PCs' screens, and encodes and decodes.
  - With two PCs, the sender's GPU only captures and encodes. Measuring that is the regression checklist's R18.
- **Still to measure:**
  - two PCs, on Wi-Fi and a hotspot;
  - whether two pictures in flight in the encoder (instead of one) keep up with motion (P2.6).

## H.264 bitrate on a link that slows down (roadmap P2.6)

**How it's measured:** `h264_test` (label `gpu`), `H264Sender` sending moving 1280×720 content at 30 fps
and 6 Mbit/s through a fake channel. Each frame takes as long as it would on the wire at the cap:
20 Mbit/s for 4 s, then 1.5 Mbit/s for 6 s, then 20 again for 14 s. Three runs.

| Date | Machine | Under 1.5 Mbit/s after the link slows | Back to 5 Mbit/s after it recovers | Last 3 s at 1.5 Mbit/s: out / capture→sent p95 | Pictures not sent (late) | Frames a viewer shows that don't decode |
|---|---|---|---|---|---|---|
| 2026-10-21 | Celeron laptop (UHD 600) | 1.5–2.0 s, in one step to 1.1 Mbit/s | 5.5–10 s | 0.93–0.99 Mbit/s / 593–994 ms | 35–72 of ~650 | 0 |

**What it means:**
- **Intel's encoder can't change its bitrate while running.** It accepts a new value and keeps the old one: 5.3–5.6 Mbit/s out after being set to 1. It also reports the setting as not modifiable (`S_FALSE`).
- So a change means a new encoder, made while the old one goes on. It takes 0.4–1 s to start here, and its stream begins with a keyframe.
- **Down is fast:** while send is busy, bytes sent ÷ time in send is the link's speed, and the bitrate goes straight to 80% of it.
- **Up is slow on purpose:** ×1.15 per 500 ms after 1.5 s without a sign of a full link.
- **Noise-like content** (every pixel new every frame) doesn't fit 1.5 Mbit/s even at the 1 Mbit/s minimum. There only skipping to a keyframe keeps the delay bounded: p95 1.1–1.6 s in earlier runs with such content.

## Desk mode viewer: one picture, DirectComposition crops (roadmap P2.7a)

**How it's measured:** [tools/dcomp_desk_prototype.cpp](../tools/dcomp_desk_prototype.cpp).
- **Setup:** the VDD (1920×1080, 60 Hz) is captured whole on the GPU. Each picture is copied into a DirectComposition surface, and `Commit` shows it as clipped visuals (one per window on the VDD) on an overlay over the primary monitor.
- **Content:** one or two Paint windows on the VDD.
- **Time:** the viewer's CPU per picture, from 5 s windows over five runs.

| Date | Machine | Pictures/s | Copy into the surface p50 / p95 (ms) | Commit p50 / p95 (ms) | Today's desk path, per picture (P2.1 table) |
|---|---|---|---|---|---|
| 2026-11-05 | Celeron laptop (UHD 600) | ~54 | 0.07–0.09 / under 0.8 | 0.8 / under 1.9 | 14–24 ms JPEG decode + 7.7–10 ms CPU compose |

**What it means:** the viewer's CPU stops being the limit in desk mode. What's left per picture is the H.264 decode and conversion (2–3 ms, P2.4a) and ~1 ms of composition calls.

## Desk mode: JPEG and H.264 side by side (roadmap P2.7c)

**How it's measured:** [tools/baseline_loopback.ps1](../tools/baseline_loopback.ps1) `-Mode desk -Codec jpeg|h264`.
- **Setup:**
  - the real kvm_host and kvm_client on 127.0.0.1, in desk mode;
  - one 1280×800 window on the 1920×1080 virtual display for 30 s, a 30 fps cap;
  - JPEG: one picture per window, quality 75 at 4:4:4, plus its alpha runs;
  - H.264: one picture of the whole virtual display at 8 Mbit/s CBR, which the viewer cuts the window out of with DirectComposition.
- **Columns:** as in the P2.1 table, from kvm_client's `--stats`. Decode includes the conversion to BGRA. Present is from decoded until `UpdateLayeredWindowIndirect` (JPEG) or `Commit` (H.264) returns.
- **Runs:** one per codec and content, back to back in one session.
- **Caveat:** as in the P2.4c table, one UHD 600 draws the content and both screens, and encodes and decodes.

| Date | Machine | Content | Codec | fps | Mbit/s | Latency p50 / p95 / p99 (ms) | Decode avg / p95 (ms) | Present avg / p95 (ms) | Dropped |
|---|---|---|---|---|---|---|---|---|---|
| 2026-11-05 | Celeron laptop | static document (Notepad) | JPEG | 24.5 | 58.2 | 78 / 134 / 229 | 17.3 / 29.2 | 7.9 / 9.5 | 0 |
| 2026-11-05 | Celeron laptop | static document (Notepad) | H.264 | 29.3 | 7.8 | 44 / 58 / 195 | 1.9 / 2.7 | 0.8 / 1.1 | 0 |
| 2026-11-05 | Celeron laptop | browser scrolling (Edge) | JPEG | 22.4 | 74.4 | 99 / 143 / 178 | 23.4 / 37.8 | 10.1 / 13.7 | 0 |
| 2026-11-05 | Celeron laptop | browser scrolling (Edge) | H.264 | 29.0 | 8.2 | 62 / 122 / 185 | 3.5 / 13.0 | 0.8 / 1.3 | 0 |
| 2026-11-05 | Celeron laptop | animation, 40 moving boxes (Edge canvas) | JPEG | 28.2 | 28.1 | 76 / 104 / 124 | 16.9 / 24.1 | 10.1 / 12.7 | 0 |
| 2026-11-05 | Celeron laptop | animation, 40 moving boxes (Edge canvas) | H.264 | 26.6 | 7.5 | 108 / 145 / 181 | 8.2 / 33.0 | 0.9 / 1.8 | 9 |

**What it shows:**
- **H.264 needs 4–9× less bandwidth:** 7.5–8.2 Mbit/s against 28–74 Mbit/s for JPEG.
- **The viewer's work per picture drops from 25–34 ms to 3–9 ms:** decode 2–8 ms on the GPU instead of 17–23 ms on the CPU, and ~1 ms to show it instead of 8–10 ms of composing on the CPU.
- **A document and scrolling are faster and smoother:**
  - 29 fps at 44 and 62 ms p50, against 22–25 fps at 78 and 99 ms for JPEG.
- **Animation is slower with H.264 on this one PC:** 108 ms p50 against 76 ms.
  - The host logs 92 pictures "dropped by a busy encoder" in 30 s, and the decode p95 is 33 ms. The GPU is the limit again: it also draws the animation and both screens.
  - With two PCs, the sender's GPU only captures and encodes. Measuring that is the regression checklist's R11 and R18.
- **Compared with the window viewer's H.264 (P2.4c table):** the same animation there ran at 13.6 fps and 171 ms p50. Desk mode shows it at 26.6 fps.
- **8 Mbit/s even for a still document:** the encoder runs at a constant bitrate. P2.6 lowers it only when the link can't keep up.

## BGRA to NV12 on the GPU (roadmap P2.3a)

**What:** `nv12_test` (label `gpu`) converts a 1920×1080 BGRA texture to NV12 with the D3D11 video
processor ([host_a/nv12_converter.h](../host_a/nv12_converter.h)), 60 times, then waits for the GPU:
the average per conversion, including the copy into the processor's input texture.

| Date | Machine | 1920×1080 BGRA → NV12 |
|---|---|---|
| 2026-10-21 | Celeron laptop (UHD 600) | 7.0 ms |

The same frame takes 17–27 ms to JPEG-encode on this CPU (above), and NV12 is only the input of
the hardware H.264 encoder (P2.3b), whose time is still to measure.

## H.264 encode on the GPU (roadmap P2.3b)

**What:** `h264_test` (label `gpu`):
- **Input:** 60 moving 1920×1080 pictures at 30 fps (a gradient and a moving block) go BGRA → NV12
  ([nv12_converter.h](../host_a/nv12_converter.h)), into a copy, and into the hardware encoder
  ([h264_encoder.h](../host_a/h264_encoder.h)): Intel Quick Sync, CBR at 8 Mbit/s, low latency, no
  B-frames.
- **Time:** from just before the conversion until the access unit came out.
- **Runs:** 5, back to back.

| Date | Machine | Time p50 (ms) | Time p95 (ms) | Bitrate (target 8.0 Mbit/s) | Dropped |
|---|---|---|---|---|---|
| 2026-10-21 | Celeron laptop (UHD 600) | 20.1–23.8 | 34.7–50.1, once 121.8 | 6.69–6.96 Mbit/s | 0, once 2 of 60 |

**What it means:**
- The CPU only queues work here, against a 17–27 ms JPEG encode on the CPU for a smaller,
  1280×800 window (above).
- The bitrate stays near the target, against desk mode's 24–79 Mbit/s with JPEG.
- Whether the picture holds up at that bitrate is P2.4's measurement, when the decoder can turn it
  back into pixels (PSNR).

## H.264 decode on the GPU (roadmap P2.4a)

**What:** `h264_test` (label `gpu`):
- **Input:** the 60 access units from the encode above, decoded by
  [h264_decoder.h](../client_b/h264_decoder.h) on a D3D11 device of its own, like the viewer's.
- **Decoder:** Media Foundation's H.264 decoder, DXVA, low latency.
- **Time:** one `decode()` call, which returns with the access unit's picture as a texture. The GPU
  may still be finishing it.
- **PSNR:** luma against the source pictures.
- **Runs:** 5.

| Date | Machine | decode() p50 (ms) | decode() p95 (ms) | Luma PSNR mean / lowest (dB) |
|---|---|---|---|---|
| 2026-10-21 | Celeron laptop (UHD 600) | 0.6–0.7 | 1.0–2.0 | 55.9 / 55.2 |

**What it means:**
- The viewer's CPU only hands the decoder the data.
- The PSNR is high because the content is smooth synthetic gradients. Real window content gets its
  numbers with the HUD in P2.4c.

## JPEG encode (hackathon baseline, for roadmap Phase 2)

| Date | Machine | Frame | Result | Source |
|---|---|---|---|---|
| 2026-10-20 | Celeron laptop | synthetic 1920×1080, quality 75 | avg 21.5 ms, p95 35.0 ms, 515 KB | `jpeg_test` (`bench` label) |
| 2026-10-17 | Temo's laptop | 1080p window | 4.2–5.3 ms | hackathon commit messages, not re-measured |

## Hardware codecs (roadmap P2.2)

**How:** `mft_probe --markdown` ([tools/mft_probe.cpp](../tools/mft_probe.cpp)) lists:
- every H.264 / HEVC / AV1 encoder and decoder that Media Foundation offers (`MFTEnumEx`), and
  what each takes;
- what the GPU itself decodes (`ID3D11VideoDevice` decode profiles, which Microsoft's decoder MFT
  uses through DXVA when it gets a D3D11 device).

Identical entries (one per graphics adapter, and a virtual display adds some) are listed once.

**Celeron laptop (Intel UHD Graphics 600), 2026-10-21:**

| Codec | Kind | Name | Hardware | Vendor | D3D11 | Async | Low latency | Raw formats | 4:4:4 format |
|---|---|---|---|---|---|---|---|---|---|
| H.264 | encoder | Intel® Quick Sync Video H.264 Encoder MFT | yes | Intel | yes | yes | yes | NV12 ARGB32 | - |
| H.264 | encoder | H264 Encoder MFT | no | - | no | no | yes | IYUV YV12 NV12 YUY2 | - |
| H.264 | decoder | Microsoft H264 Video Decoder MFT | no | - | yes | no | yes | NV12 YV12 IYUV I420 YUY2 | - |
| HEVC | encoder | Intel® Hardware H265 Encoder MFT | yes | Intel | yes | yes | yes | NV12 ARGB32 P010 | - |

| GPU | Decodes in hardware (D3D11 video) |
|---|---|
| Intel(R) UHD Graphics 600 | H.264, HEVC Main, HEVC Main10, VP9 |

**What it means for Phase 2:**
- **H.264 encode in hardware:** Intel Quick Sync, async, on D3D11 textures, with low-latency mode.
  That is what P2.3 needs for zero-copy encode from WGC.
- **H.264 decode in hardware:** the GPU decodes H.264 (and HEVC, VP9). Microsoft's H.264 decoder
  MFT works on D3D11, so P2.4 can decode on the GPU. This answers the open question about this
  laptop: it can be the weak PC in the demo with H.264.
- **HEVC:** encoded in hardware, and the GPU decodes it. But there is no HEVC decoder MFT: Windows
  needs the "HEVC Video Extensions" from the Store for that. HEVC (P2.10) can't be assumed on an
  ordinary PC.
- **AV1:** neither encode nor decode here.
- **4:4:4:** no encoder lists a 4:4:4 input (AYUV, Y410, Y416). ARGB32 input is converted to
  4:2:0 by the encoder, so P2.10's 4:4:4 isn't on this PC's table.
- **Still to probe:** the two demo PCs.
