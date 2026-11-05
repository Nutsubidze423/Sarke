// SPDX-License-Identifier: Apache-2.0

// desk_test: desk mode on one PC. The VDD is attached next to the primary monitor, and the windows
// dragged onto it are drawn back on the primary monitor by a DeskView, exactly as the other PC would
// draw them (the network is a loopback lambda). Drag a window over the edge and its "ghost" appears
// on the opposite side of this screen.
//
//   desk_test.exe [--side right|left] [--seconds N] [--list]   watch it by hand (Ctrl+C ends)
//   desk_test.exe --auto [--title Paint]                       automatic checks with an open Paint window
//
// --auto moves the window half onto the VDD, checks the tracked rect, that frames go out and get
// drawn, that the overlay hides once the window is back, then puts the window back and detaches.
// Then the same with H.264 (roadmap P2.7b), where this PC has a hardware encoder: one stream of the
// whole VDD (layer "desk", window 0, the VDD's size), starting with a keyframe, seq without gaps,
// every frame decodable, and the layout with the window's corner radius. A DeskView draws that stream
// (roadmap P2.7c: DirectComposition cuts the window out of the picture), and what the screen shows
// there is compared with the window's own picture (PrintWindow).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "desk_share.h"
#include "desk_view.h"
#include "h264_decoder.h"
#include "kvm/annexb.h"
#include "kvm/desk_region.h"
#include "kvm/dpi.h"
#include "kvm/log.h"
#include "kvm/video_stats.h"
#include "virtual_display.h"
#include "window_picker.h"

namespace {

using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

std::optional<kvm::WindowInfo> window_with_title(std::wstring_view part) {
    for (auto& w : kvm::list_capturable_windows()) {
        if (w.title.find(part) != std::wstring::npos) return w;
    }
    return std::nullopt;
}

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

// The primary monitor as it looks now, layered windows included, as a 24-bit BMP.
bool save_screen(const std::wstring& path) {
    const auto r = kvm::display::primary_monitor().rect;
    const int w = r.width(), h = r.height();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;  // bottom-up, as BMP files store it
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ old = SelectObject(mem, bmp);
    const bool ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
    SelectObject(mem, old);
    const DWORD stride = static_cast<DWORD>((w * 3 + 3) & ~3);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + stride * h;
    bool written = false;
    if (ok) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
            fwrite(bits, 1, static_cast<size_t>(stride) * h, f);
            fclose(f);
            written = true;
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return written;
}

// Top-down 32-bit pixels of a w x h bitmap that `draw(mem_dc, screen_dc)` paints; empty if it fails.
template <class Draw>
std::vector<std::uint32_t> grab(int w, int h, Draw draw) {
    std::vector<std::uint32_t> out;
    if (w <= 0 || h <= 0) return out;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    if (HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
        const HGDIOBJ old = SelectObject(mem, bmp);
        if (draw(mem, screen)) {
            GdiFlush();
            const auto* p = static_cast<const std::uint32_t*>(bits);
            out.assign(p, p + static_cast<std::size_t>(w) * h);
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

// How far what the screen shows at `on_screen` is from `hwnd`'s own picture at `in_window` (window
// coordinates, the same size): the mean difference per color channel, 0..255 (255 if a grab fails).
double difference(HWND hwnd, const RECT& in_window, POINT on_screen) {
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    const int w = in_window.right - in_window.left, h = in_window.bottom - in_window.top;
    const auto own = grab(ww, wh, [&](HDC mem, HDC) { return PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT) != FALSE; });
    const auto shown = grab(w, h, [&](HDC mem, HDC screen) {
        return BitBlt(mem, 0, 0, w, h, screen, on_screen.x, on_screen.y, SRCCOPY | CAPTUREBLT) != FALSE;
    });
    if (own.empty() || shown.empty() || in_window.left < 0 || in_window.top < 0 || in_window.right > ww ||
        in_window.bottom > wh) {
        return 255;
    }
    std::uint64_t sum = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint32_t a = own[static_cast<std::size_t>(in_window.top + y) * ww + in_window.left + x];
            const std::uint32_t b = shown[static_cast<std::size_t>(y) * w + x];
            for (int c = 0; c < 24; c += 8) {
                sum += static_cast<std::uint64_t>(
                    std::abs(static_cast<int>((a >> c) & 255) - static_cast<int>((b >> c) & 255)));
            }
        }
    }
    return static_cast<double>(sum) / (3.0 * w * h);
}

// Position of `target` among top-level windows in z-order; 0 = topmost, -1 = not found.
int z_index(HWND target) {
    struct Ctx {
        HWND target;
        int i = 0;
        int found = -1;
    } ctx{target};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(lp);
            if (h == c.target) c.found = c.i;
            ++c.i;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void print_windows(const kvm::DeskShare& share) {
    for (const auto& w : share.tracker().windows()) {
        kvm::log::info("  {} \"{}\" [{}] {},{} {}x{}", reinterpret_cast<void*>(w.hwnd), w.title, w.cls, w.rect.left,
                       w.rect.top, w.rect.width(), w.rect.height());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    auto side = kvm::display::Side::Right;
    int seconds = 0;
    bool list = false;
    bool automatic = false;
    std::wstring title = L"Paint";
    std::wstring shot;  // --shot <file.bmp>: the screen while the window is half over
    bool no_scale = false;
    int settle_s = 0;  // --settle N: wait after attaching (display-change animations) before testing
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--side" && i + 1 < argc) {
            side = std::wstring_view(argv[++i]) == L"left" ? kvm::display::Side::Left : kvm::display::Side::Right;
        } else if (a == L"--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else if (a == L"--list") {
            list = true;
        } else if (a == L"--auto") {
            automatic = true;
        } else if (a == L"--title" && i + 1 < argc) {
            title = argv[++i];
        } else if (a == L"--shot" && i + 1 < argc) {
            shot = argv[++i];
        } else if (a == L"--no-scale") {
            no_scale = true;
        } else if (a == L"--settle" && i + 1 < argc) {
            settle_s = std::stoi(argv[++i]);
        } else if (a == L"--shot-now" && i + 1 < argc) {
            return save_screen(argv[i + 1]) ? 0 : 1;  // just the screenshot (checks save_screen itself)
        }
    }

    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        kvm::log::error("the Virtual Display Driver is not installed");
        return 1;
    }
    const auto primary = kvm::display::primary_monitor();
    if (!vdd->attach(primary.rect.width(), primary.rect.height(), 60, side)) {
        kvm::log::error("could not attach the VDD");
        return 1;
    }
    const auto vdd_rect = vdd->rect();
    if (!vdd_rect) {
        kvm::log::error("the VDD is attached but has no monitor");
        vdd->detach();
        return 1;
    }
    const kvm::display::Rect vr{vdd_rect->rect.left, vdd_rect->rect.top, vdd_rect->rect.right, vdd_rect->rect.bottom};
    kvm::log::info("VDD {} at {},{} {}x{}", kvm::to_utf8(vdd->device()), vr.left, vr.top, vr.width(), vr.height());
    // The "other PC" is this screen: give the VDD its scale, as kvm_host does with the peer's.
    if (!no_scale) vdd->set_scale(primary.scale);
    std::this_thread::sleep_for(std::chrono::seconds(settle_s));

    Checks check;
    {
        kvm::desk::Region region;
        region.set_vdd({.device = kvm::to_utf8(vdd->device()), .rect = vr, .live = true});
        kvm::DeskView view;
        std::atomic<std::uint64_t> bytes{0};
        auto loopback = [&](const kvm::proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
            bytes += payload.size();
            view.present(kvm::net::Frame{.header = h, .payload = {payload.begin(), payload.end()}});
            return true;
        };
        std::atomic<int> layouts{0};
        std::mutex layout_mutex;
        kvm::proto::DeskLayout last_layout;
        kvm::DeskShare share(*vdd_rect, region, loopback,
                             [&](const kvm::proto::DeskLayout& layout) {
                                 ++layouts;
                                 {
                                     std::lock_guard lock(layout_mutex);
                                     last_layout = layout;
                                 }
                                 view.set_layout(layout);
                             },
                             {.jpeg_quality = 80, .max_fps = 30});

        if (automatic) {
            const auto window = window_with_title(title);
            check(window.has_value(), "a window to test with is open");
            if (window) {
                const HWND hwnd = window->hwnd;
                WINDOWPLACEMENT saved{sizeof(saved)};
                GetWindowPlacement(hwnd, &saved);
                ShowWindow(hwnd, SW_RESTORE);
                const int w = 800, h = 600;
                const int x = side == kvm::display::Side::Right ? vr.left - w / 2 : vr.right - w / 2;
                SetWindowPos(hwnd, HWND_TOP, x, vr.top + 100, w, h, SWP_NOACTIVATE);

                // The part on the VDD starts at the VDD's inner edge. (Its size changes: crossing onto a
                // monitor with another DPI scale rescales the window, like between real monitors.)
                const bool tracked = wait_until(
                    [&] {
                        for (const auto& r : region.rects()) {
                            const bool at_edge =
                                side == kvm::display::Side::Right ? r.left == vr.left : r.right == vr.right;
                            if (at_edge && r.width() > 100 && r.width() < vr.width() && r.height() > 300) return true;
                        }
                        return false;
                    },
                    2s);
                check(tracked, "the half on the VDD is tracked");
                print_windows(share);
                check(layouts > 0, "the layout goes out");
                check(wait_until([&] { return share.frames_sent() > 0; }, 3s), "the window's picture goes out");
                check(wait_until([&] { return view.frames_shown() > 0; }, 3s), "frames are drawn");
                check(wait_until([&] { return IsWindowVisible(view.hwnd()) != FALSE; }, 2s), "the overlay is shown");
                // Dragging it further over: the layout follows every step, no new pictures needed.
                const int before_layouts = layouts;
                for (int i = 1; i <= 10; ++i) {
                    SetWindowPos(hwnd, nullptr, x + 20 * i, vr.top + 100, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    std::this_thread::sleep_for(30ms);
                }
                check(wait_until([&] { return layouts - before_layouts >= 5; }, 1s),
                      std::format("moving it sends the new place ({} layouts for 10 steps)", layouts - before_layouts));
                kvm::display::Point inside{side == kvm::display::Side::Right ? vr.left + 50 : vr.right - 50,
                                           vr.top + 300};
                check(region.hit(inside), "a point on the window hits");
                check(!region.hit({inside.x, vr.top + 1000}), "a point below it doesn't");
                std::this_thread::sleep_for(500ms);  // look at it: the ghost is on the other side of the screen
                if (!shot.empty()) {
                    check(save_screen(shot), "screenshot saved");
                    check(view.save_surface(shot + L".overlay.bmp"), "overlay surface saved");
                }
                {
                    RECT now{};
                    GetWindowRect(hwnd, &now);
                    std::lock_guard lock(layout_mutex);
                    kvm::log::info("window at {},{} {}x{}; layout {}x{}:", now.left, now.top, now.right - now.left,
                                   now.bottom - now.top, last_layout.desk_w, last_layout.desk_h);
                    for (const auto& lw : last_layout.windows) {
                        kvm::log::info("  {:x} at {},{} {}x{}", lw.id, lw.x, lw.y, lw.w, lw.h);
                    }
                    RECT overlay{};
                    GetWindowRect(view.hwnd(), &overlay);
                    kvm::log::info("overlay window {},{} {}x{} visible {}", overlay.left, overlay.top,
                                   overlay.right - overlay.left, overlay.bottom - overlay.top,
                                   IsWindowVisible(view.hwnd()) != FALSE);
                }
                std::this_thread::sleep_for(1000ms);

                // The overlay isn't topmost, and a window this PC raises comes in front of it.
                check((GetWindowLongPtrW(view.hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) == 0, "the overlay is not topmost");
                const HWND probe =
                    CreateWindowExW(0, L"STATIC", L"local window", WS_POPUP | WS_VISIBLE, inside.x - 80, vr.top + 150,
                                    320, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                SetForegroundWindow(probe);
                SetWindowPos(probe, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                check(wait_until([&] { return z_index(probe) >= 0 && z_index(probe) < z_index(view.hwnd()); }, 1s),
                      "a window raised here goes in front of the transferred window");
                DestroyWindow(probe);

                SetWindowPlacement(hwnd, &saved);
                check(wait_until([&] { return region.rects().empty(); }, 2s), "back home: nothing tracked");
                check(wait_until([&] { return IsWindowVisible(view.hwnd()) == FALSE; }, 2s), "the overlay is hidden");
            }
        } else {
            kvm::log::info("drag a window over the {} edge; Ctrl+C ends", kvm::display::to_string(side));
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (!g_stop && (seconds == 0 || std::chrono::steady_clock::now() < end)) {
                std::this_thread::sleep_for(1s);
                kvm::log::info("sent {} frames ({} KB), drawn {}", share.frames_sent(), bytes / 1024,
                               view.frames_shown());
                if (list) print_windows(share);
            }
        }
    }
    if (automatic) {
        // H.264: one stream of the whole VDD.
        const auto window = window_with_title(title);
        if (window) {
            kvm::desk::Region region;
            region.set_vdd({.device = kvm::to_utf8(vdd->device()), .rect = vr, .live = true});
            std::mutex mutex;
            std::vector<kvm::proto::FrameHeader> headers;
            std::vector<std::vector<std::uint8_t>> units;
            kvm::proto::DeskLayout layout;
            kvm::DeskView view;  // the other PC: draws the stream (outlives the share)
            view.set_clock_offset_us(0);  // one clock
            auto capture = [&](const kvm::proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
                {
                    std::lock_guard lock(mutex);
                    headers.push_back(h);
                    units.emplace_back(payload.begin(), payload.end());
                }
                view.present(kvm::net::Frame{.header = h, .payload = {payload.begin(), payload.end()}});
                return true;
            };
            const HWND hwnd = window->hwnd;
            WINDOWPLACEMENT saved{sizeof(saved)};
            GetWindowPlacement(hwnd, &saved);
            bool h264 = false;
            {
                kvm::DeskShare share(*vdd_rect, region, capture,
                                     [&](const kvm::proto::DeskLayout& l) {
                                         {
                                             std::lock_guard lock(mutex);
                                             layout = l;
                                         }
                                         view.set_layout(l);
                                     },
                                     {.max_fps = 30, .codec = "h264"});
                h264 = std::string_view(share.codec()) == "h264";
                if (h264) {
                    ShowWindow(hwnd, SW_RESTORE);
                    const int x = side == kvm::display::Side::Right ? vr.left - 400 : vr.right - 400;
                    SetWindowPos(hwnd, HWND_TOP, x, vr.top + 100, 800, 600, SWP_NOACTIVATE);
                    std::this_thread::sleep_for(2500ms);
                    // The viewer side: the window, cut out of the picture, on this screen.
                    check(wait_until([&] { return view.frames_shown() > 0; }, 3s),
                          std::format("H.264 pictures are drawn ({})", view.frames_shown()));
                    check(wait_until([&] { return IsWindowVisible(view.hwnd()) != FALSE; }, 2s),
                          "the DirectComposition overlay is shown");
                    check((GetWindowLongPtrW(view.hwnd(), GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0,
                          "it is the DirectComposition window");
                    // The part of the window on the VDD, away from its edges, against what this screen
                    // shows at the same place (the VDD's 0,0 is the primary monitor's).
                    RECT wr{};
                    GetWindowRect(hwnd, &wr);
                    const RECT on_vdd{vr.left, vr.top, vr.right, vr.bottom};
                    RECT part{};
                    IntersectRect(&part, &wr, &on_vdd);
                    InflateRect(&part, -40, -40);
                    const RECT in_window{part.left - wr.left, part.top - wr.top, part.right - wr.left,
                                         part.bottom - wr.top};
                    const POINT on_screen{primary.rect.left + part.left - vr.left,
                                          primary.rect.top + part.top - vr.top};
                    const double diff = difference(hwnd, in_window, on_screen);
                    check(diff < 12, std::format("this screen shows the window ({}x{} px, mean difference {:.1f})",
                                                 part.right - part.left, part.bottom - part.top, diff));
                    if (!shot.empty()) check(save_screen(shot + L".h264.bmp"), "H.264 screenshot saved");
                    const auto s = kvm::video_stats().summary("desk");
                    kvm::log::info("desk h264: {} frames shown, latency p50 {} ms", s.frames,
                                   s.latency_p50_ms ? std::format("{:.1f}", *s.latency_p50_ms) : "-");
                } else {
                    kvm::log::info("no hardware H.264 encoder here: the H.264 checks are skipped");
                }
            }  // the share stops: no more frames
            SetWindowPlacement(hwnd, &saved);
            if (h264) {
                std::lock_guard lock(mutex);
                bool whole = !headers.empty();
                for (const auto& h : headers) {
                    whole = whole && h.layer == "desk" && h.codec == "h264" && h.window == 0 && h.width == vr.width() &&
                            h.height == vr.height() && h.desk_w == vr.width();
                }
                check(whole, std::format("{} frames, each the whole VDD as h264 ({}x{})", headers.size(), vr.width(),
                                         vr.height()));
                bool in_order = !headers.empty() && headers.front().keyframe;
                for (std::size_t i = 1; i < headers.size(); ++i) {
                    in_order = in_order && headers[i].seq == headers[i - 1].seq + 1;
                }
                check(in_order, "the first is a keyframe, seq has no gaps");
                std::string why;
                auto decoder = kvm::H264Decoder::create(nullptr, why);
                std::size_t decoded = 0;
                for (std::size_t i = 0; decoder && i < units.size(); ++i) {
                    if (!kvm::annexb::check(units[i], headers[i].keyframe).empty()) continue;
                    const auto picture = decoder->decode(units[i], headers[i].t_capture_us);
                    if (picture && picture->width == vr.width() && picture->height == vr.height()) ++decoded;
                }
                check(decoded == units.size(), std::format("{} of {} decode to the VDD's size", decoded, units.size()));
                const auto it = std::find_if(layout.windows.begin(), layout.windows.end(), [&](const auto& w) {
                    return w.id == reinterpret_cast<std::uint64_t>(hwnd);
                });
                check(it != layout.windows.end(), std::format("the layout has the window, corner radius {} px",
                                                              it != layout.windows.end() ? it->radius : -1));
            }
        }
    }
    vdd->detach();
    if (automatic && check.failed == 0) kvm::log::info("desk_test: all checks passed");
    if (automatic && check.failed != 0) kvm::log::error("desk_test: {} check(s) FAILED", check.failed);
    return check.failed == 0 ? 0 : 1;
}
