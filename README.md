# Sarke

**One desk, many Windows PCs.** Move your mouse off the edge of the screen and it's on the other
PC. Drag a window across and it shows up there while it keeps running where it started. Copy on
one PC, paste on the other; drag files across.

[![CI](https://github.com/Nutsubidze423/Sarke/actions/workflows/ci.yml/badge.svg)](https://github.com/Nutsubidze423/Sarke/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Nutsubidze423/Sarke?label=release)](https://github.com/Nutsubidze423/Sarke/releases/latest)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
![Windows 10 | 11](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-informational)

[Download](https://github.com/Nutsubidze423/Sarke/releases/latest) ·
[Website](https://sarke-nine.vercel.app) · [Roadmap](docs/ROADMAP.md) ·
[Contributing](CONTRIBUTING.md) · [Security](docs/SECURITY.md)

Tools like Mouse Without Borders share a mouse and keyboard. Tools like spacedesk turn a second
device into a monitor. Sarke does both at once: the other PC's screen works as one more monitor, and
that PC stays fully usable with its own apps, mouse and keyboard.

Where it's going: *all your devices become one computer you can sit at from anywhere*. A weak
laptop runs a heavy app or game on your desktop's GPU, with sound. The [roadmap](docs/ROADMAP.md)
has the plan.

> [!IMPORTANT]
> **Updating from 1.1?** Update **both** PCs: 1.2 pairs and encrypts, so it can't talk to 1.1. Each
> PC on 1.2 says so when the other one still runs 1.1. Then pair the two PCs once (step 3 below).

## Features

| | |
|---|---|
| **Shared mouse and keyboard** | Cross the screen edge to control the other PC, in both directions. It can stand left, right, above or below. |
| **Desk mode** | The other PC's screen becomes one more monitor. A window dragged onto it keeps running on its own PC and is drawn over there pixel for pixel, on top of that PC's desktop. Both PCs' mice and keyboards can use it. |
| **Files** | Drag files across the edge and they land where you let go: a folder, the desktop or an app. Files copied with Ctrl+C paste on the other PC. |
| **Clipboard** | Text and images copied on one PC paste on the other. |
| **Finds the other PC** | No IP addresses to type: Sarke lists the PCs running it on the network, and can connect to one automatically next time. |
| **Paired and encrypted** | Two PCs pair once by comparing a 6-digit code on both screens. After that only they can connect to each other, and everything between them (input, clipboard, windows, files) is encrypted with TLS 1.3. |
| **Arranged once** | Set where the other PC stands on either PC; both follow. Each PC remembers where every PC it has met stands. |
| **Window viewer** | Without a virtual display, any window can still be streamed into a viewer on the other PC and controlled from there. |

## Get started

**You need:**
- Two PCs with Windows 10 (version 2004 or later) or Windows 11, 64-bit;
- both on the same local network, which Windows calls **Private** (Settings > Network & internet >
  Wi-Fi or Ethernet > your network). On a Public network the other PC can't reach Sarke, and Sarke
  tells you so. It uses TCP ports 50500–50502 and UDP port 50503.

**Steps:**
1. Download `SarkeSetup-<version>.exe` from the
   [latest release](https://github.com/Nutsubidze423/Sarke/releases/latest) (or the
   [website](https://sarke-nine.vercel.app)), and run it on **both** PCs. It installs:
   - Sarke;
   - the [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) for desk mode;
   - firewall rules that let in only the local network, and only on Private networks.
2. Start Sarke on both PCs. It lives in the system tray and lists the other PCs running Sarke.
3. On one PC, click the other PC in that list. The first time it shows **Pair…**: both PCs then
   show a 6-digit code. Click **Pair** on both only if the codes are the same.
4. Turn on **Auto** to connect to that PC by itself whenever it's on the network. **Paired PCs…** in
   the tray menu lists the PCs this one is paired with, and can unpair them.
5. In the tray menu, choose **Display arrangement** and set which side of this screen the other PC
   is on. Both PCs follow.

Now move the mouse across that edge.

## Shortcuts

| Keys | Action |
|---|---|
| **Ctrl+Alt+Shift+Q** | Emergency stop: control comes back to this PC and input capture stops |
| Ctrl+Alt+Shift+W | Send the window in front to the other PC, or bring it back |
| Ctrl+Alt+Shift+F | Show a drop box: files dropped on it go to the other PC |

## Configuration

Settings live in `config.json`: in `%LOCALAPPDATA%\Sarke\` when installed, or in the repository
for a build from source. Most can be changed from the tray menu. Logs are in
`%LOCALAPPDATA%\Sarke\logs\`. The same folder keeps this PC's key (`identity.bin`, encrypted for
your Windows account) and the PCs it is paired with (`paired.json`).

| Key | Default | Meaning |
|---|---|---|
| `host_ip` | `"auto"` | The PC to connect to; `"auto"` finds it on the network |
| `control_port` | `50500` | Control and input |
| `video_port` | `50501` | Window pictures |
| `file_port` | `50502` | File transfers |
| `client_side` | `"right"` | For a PC not in `peers` yet: where the client stands next to the host (`left`, `right`, `top` or `bottom`) |
| `peers` | `{}` | Where each PC this one has been connected to stands, by computer name; set by Display arrangement |
| `jpeg_quality` | `75` | Picture quality, 1–100 |
| `max_fps` | `30` | Frame rate limit, `0` for none |
| `desk` | `true` | Desk mode; `false` uses the window viewer |

## Troubleshooting

- **"Can't listen on port …".** The message names the program holding the port, or says Windows
  reserved it. Hyper-V, WSL and Docker reserve TCP port ranges; see them with
  `netsh interface ipv4 show excludedportrange protocol=tcp`. Set free ports in `control_port`,
  `video_port` and `file_port` on the PC that says so. The other PC follows the video and file
  ports by itself; `control_port` has to match only when you connect with `--host <IP>`.
- **The other PC isn't listed, or can't connect.** Both PCs must be on the same network, and
  Windows must call it Private (Sarke tells you when it's Public). Guest and campus Wi-Fi often
  block devices from reaching each other: try a phone hotspot (made Private), or
  `kvm_client.exe --host <IP>`.
- **"Sarke versions don't match".** Install the same version on both PCs.
- **The pairing codes differ.** Cancel on both PCs: something else answered. Pair again from the
  list.
- **Clicks don't reach a window.** Windows running as administrator only take input from Sarke
  when Sarke runs as administrator too.
- **Microsoft Defender flags Sarke.** Builds aren't code-signed yet, and the input hooks look
  suspicious to heuristics. The installer can add an exclusion (off by default: an exclusion also
  stops Defender from scanning that folder); signing is planned.

## How it works

Each PC runs the tray app, `sarke.exe`. It starts either the host (`kvm_host.exe`, which waits for
a connection) or the client (`kvm_client.exe`, which connects). Once connected, both sides can do
everything; the roles only decide who listens.

- **Input:** low-level keyboard and mouse hooks with Raw Input on the PC that has the input; the
  other PC replays it with `SendInput`.
- **Desk mode:**
  - a virtual monitor the size of the other PC's screen is attached on its side;
  - each window on it is captured with Windows.Graphics.Capture and sent as JPEG with an alpha mask
    (for rounded corners);
  - the other PC draws it in a transparent overlay at the same coordinates.
- **Files:** a drag that reaches the other screen is caught, the files are sent, and the drag goes
  on from the cursor over there.
- **Security:** each PC has its own key, made at first start. Pairing compares a code derived
  from the TLS session on both screens, so a PC in the middle would show a different one. Every
  connection is TLS 1.3 with both PCs' keys checked against the paired list before anything is read.
  The design is in [docs/SECURITY.md](docs/SECURITY.md).

| Port | Protocol | Carries |
|---|---|---|
| 50500 | TCP, TLS 1.3, length-prefixed JSON | handshake, pairing, arrangement, input, clipboard, window layout |
| 50501 | TCP, TLS 1.3, JSON header + payload | window pictures (JPEG) |
| 50502 | TCP, TLS 1.3, JSON header + payload | files |
| 50503 | UDP broadcast | finding the other PCs (not secret: it only says where to try) |

Every message is described in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Limitations

- Windows only, local network only, one connection at a time.
- Builds aren't code-signed yet.
- Window pictures are JPEG, so fast video is less smooth than locally; sound stays on the PC that
  plays it. Hardware video and audio are Phases 2 and 3 of the roadmap.

## Building from source

You need Visual Studio 2022 (*Desktop development with C++*), CMake 3.25+, Ninja and
[vcpkg](https://vcpkg.io) with `VCPKG_ROOT` set; `tools\setup_env.ps1` installs them. From a
*Developer PowerShell for VS 2022*:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset ci
build\x64-release\tools\sarke.exe
```

The executables are statically linked, so `sarke.exe`, `kvm_host.exe`, `kvm_client.exe`,
`config.json` and `assets\fonts` are all another PC needs.

- **Installer:** `powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1`. It needs
  [Inno Setup 6](https://jrsoftware.org/isinfo.php) and fetches the driver with
  `tools\fetch_vdd.ps1` (pinned and hash-checked).
- **Tests, code style and how we work:** [CONTRIBUTING.md](CONTRIBUTING.md).
- **Releases:** [docs/RELEASING.md](docs/RELEASING.md).

```
common/      shared library: protocol, networking, input capture and replay, clipboard, files
host_a/      kvm_host.exe: window capture, virtual display, desk mode on the sending side
client_b/    kvm_client.exe and the tray app: viewer, desk overlay, launcher, UI
tools/       sarke.exe entry point, tests, previews and scripts
installer/   Inno Setup script
docs/        roadmap, protocol, security, design system, releasing; archive/hackathon is history
```

## Contributing and security

Contributions are welcome: start with [CONTRIBUTING.md](CONTRIBUTING.md) and the
[roadmap](docs/ROADMAP.md). Please report security problems privately through GitHub's **Report a
vulnerability** (Security tab), not as an issue. The design is in
[docs/SECURITY.md](docs/SECURITY.md).

## License and credits

Sarke is open source under the [Apache License 2.0](LICENSE). The third-party components it includes
keep their own licenses; their full texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Built at a hackathon in October 2026 by Luka, Demetre, Temo and Sandro (see [AUTHORS](AUTHORS)).
Sarke includes or uses:
- the [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MIT);
- [libjpeg-turbo](https://libjpeg-turbo.org) (BSD-3-Clause, IJG, zlib), with
  [libspng](https://libspng.org) (BSD-2-Clause) and [zlib](https://zlib.net) (Zlib);
- [nlohmann/json](https://github.com/nlohmann/json) (MIT);
- [OpenSSL](https://www.openssl.org) (Apache-2.0), for TLS 1.3 between PCs;
- the Google Sans and Geist Mono fonts (SIL Open Font License 1.1).
