// SPDX-License-Identifier: Apache-2.0

// dcomp_desk_prototype: roadmap P2.7a, desk mode drawn from one picture of the whole virtual display.
// Today's desk mode captures each window on its own and composes them on the CPU into one layered
// window (desk_view.h). P2.7 would send one H.264 stream of the whole VDD instead; this prototype
// checks the viewer half of that on one PC:
//   - the VDD is attached and captured whole, on the GPU (ScreenCapture::start_gpu);
//   - an overlay over the primary monitor (DirectComposition, no redirection bitmap) holds one
//     surface with that picture, and one visual per window on the VDD (DeskTracker), each clipped to
//     the window's frame, with rounded corners if asked;
//   - clicks outside the windows go through to what is below (a window region);
//   - every 5 s it logs the cost per frame (copy into the surface + Commit), p50/p95.
// Overlapping windows come out right by themselves: they are all cut from the same picture, which
// already has the real stacking.
//
//   dcomp_desk_prototype.exe [--seconds 20] [--radius 0] [--side right|left]
//
// Move a window onto the VDD (to the right of the primary monitor): it appears on the primary monitor
// at the same place, as the other PC would draw it.

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capture.h"
#include "desk_tracker.h"
#include "kvm/desk_region.h"
#include "kvm/dpi.h"
#include "kvm/gpu_device.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "virtual_display.h"
#include "window_picker.h"

#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND create_overlay(const kvm::display::Rect& r) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SarkeDcompPrototype";
    RegisterClassExW(&wc);
    // No redirection bitmap: DirectComposition draws it. Never activated, not in Alt+Tab.
    const HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                      wc.lpszClassName, L"Sarke desk (prototype)", WS_POPUP, r.left, r.top, r.width(),
                                      r.height(), nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

double percentile(std::vector<double> v, int p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() * static_cast<std::size_t>(p) / 100];
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    int seconds = 20;
    float radius = 0;
    auto side = kvm::display::Side::Right;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--seconds") seconds = std::stoi(argv[++i]);
        if (a == L"--radius") radius = std::stof(argv[++i]);
        if (a == L"--side") side = std::wstring_view(argv[++i]) == L"left" ? kvm::display::Side::Left : side;
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
    struct Detach {
        kvm::VirtualDisplay& v;
        ~Detach() { v.detach(); }
    } detach{*vdd};
    const auto where = vdd->rect();
    if (!where) {
        kvm::log::error("the VDD is attached but has no monitor");
        return 1;
    }
    const kvm::display::Rect vr{where->rect.left, where->rect.top, where->rect.right, where->rect.bottom};
    vdd->set_scale(primary.scale);
    kvm::log::info("VDD at {},{} {}x{}; primary {}x{}", vr.left, vr.top, vr.width(), vr.height(), primary.rect.width(),
                   primary.rect.height());

    auto gpu = kvm::GpuDevice::create();
    if (!gpu) return 1;

    // The newest picture of the whole VDD.
    std::mutex frame_mutex;
    std::optional<kvm::GpuFrame> newest;
    std::atomic<std::uint64_t> captured{0};
    auto capture = kvm::ScreenCapture::for_monitor(where->monitor, false, gpu);
    capture->start_gpu([&](kvm::GpuFrame&& f) {
        std::lock_guard lock(frame_mutex);
        newest = std::move(f);
        ++captured;
    });

    // Which windows are on the VDD, and where (screen coordinates, topmost first).
    kvm::desk::Region region;
    region.set_vdd({.device = kvm::to_utf8(vdd->device()), .rect = vr, .live = true});
    std::mutex windows_mutex;
    std::vector<kvm::display::Rect> frames;
    std::atomic<bool> windows_changed{true};
    kvm::DeskTracker tracker(region, [&](const std::vector<kvm::DeskTracker::Window>& ws) {
        std::lock_guard lock(windows_mutex);
        frames.clear();
        for (const auto& w : ws) frames.push_back(w.frame);
        windows_changed = true;
    });

    // The overlay and its DirectComposition tree: one surface (the whole VDD), one visual per window.
    const HWND overlay = create_overlay(primary.rect);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root;
    ComPtr<IDCompositionSurface> surface;
    if (!overlay || FAILED(gpu->device()->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp))) ||
        FAILED(dcomp->CreateTargetForHwnd(overlay, TRUE, &target)) || FAILED(dcomp->CreateVisual(&root)) ||
        FAILED(dcomp->CreateSurface(static_cast<UINT>(vr.width()), static_cast<UINT>(vr.height()),
                                    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &surface))) {
        kvm::log::error("DirectComposition setup failed");
        return 1;
    }
    target->SetRoot(root.Get());
    std::vector<ComPtr<IDCompositionVisual>> visuals;

    std::vector<double> copy_ms, commit_ms;
    std::uint64_t drawn = 0, shown_windows = 0;
    auto next_log = std::chrono::steady_clock::now() + 5s;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::uint64_t last_captured = 0;
    while (!g_stop && std::chrono::steady_clock::now() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        // The windows changed: one clipped visual each, and the window region for clicks.
        if (windows_changed.exchange(false)) {
            std::vector<kvm::display::Rect> now;
            {
                std::lock_guard lock(windows_mutex);
                now = frames;
            }
            root->RemoveAllVisuals();
            visuals.clear();
            HRGN region_all = CreateRectRgn(0, 0, 0, 0);
            for (const auto& f : now) {
                // VDD coordinates: where the other PC draws it on its primary monitor.
                const float l = static_cast<float>(std::max(f.left, vr.left) - vr.left);
                const float t = static_cast<float>(std::max(f.top, vr.top) - vr.top);
                const float r = static_cast<float>(std::min(f.right, vr.right) - vr.left);
                const float b = static_cast<float>(std::min(f.bottom, vr.bottom) - vr.top);
                if (r <= l || b <= t) continue;
                ComPtr<IDCompositionVisual> v;
                ComPtr<IDCompositionRectangleClip> clip;
                dcomp->CreateVisual(&v);
                dcomp->CreateRectangleClip(&clip);
                clip->SetLeft(l);
                clip->SetTop(t);
                clip->SetRight(r);
                clip->SetBottom(b);
                if (radius > 0) {
                    clip->SetTopLeftRadiusX(radius);
                    clip->SetTopLeftRadiusY(radius);
                    clip->SetTopRightRadiusX(radius);
                    clip->SetTopRightRadiusY(radius);
                    clip->SetBottomLeftRadiusX(radius);
                    clip->SetBottomLeftRadiusY(radius);
                    clip->SetBottomRightRadiusX(radius);
                    clip->SetBottomRightRadiusY(radius);
                }
                v->SetContent(surface.Get());
                v->SetClip(clip.Get());
                v->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);  // antialiased clip edges (rounded corners)
                root->AddVisual(v.Get(), TRUE, nullptr);
                visuals.push_back(v);
                // Rectangles: a window region is all-or-nothing per pixel and also cuts what DComp draws,
                // so a rounded one would undo the clip's antialiasing. Clicks on the few corner pixels
                // outside the rounding land on the overlay; nothing is drawn there.
                HRGN one =
                    CreateRectRgn(static_cast<int>(l), static_cast<int>(t), static_cast<int>(r), static_cast<int>(b));
                CombineRgn(region_all, region_all, one, RGN_OR);
                DeleteObject(one);
            }
            SetWindowRgn(overlay, region_all, TRUE);  // the system owns the region now
            shown_windows = visuals.size();
        }

        // A new picture: into the surface, then one Commit.
        std::optional<kvm::GpuFrame> frame;
        {
            std::lock_guard lock(frame_mutex);
            if (captured != last_captured) {
                frame = newest;
                last_captured = captured;
            }
        }
        if (frame && frame->texture) {
            const std::int64_t t0 = kvm::steady_now_us();
            ComPtr<ID3D11Texture2D> into;
            POINT offset{};
            {
                const kvm::GpuDevice::Lock lock(*gpu);
                if (SUCCEEDED(surface->BeginDraw(nullptr, IID_PPV_ARGS(&into), &offset))) {
                    const D3D11_BOX box{0,
                                        0,
                                        0,
                                        static_cast<UINT>(std::min(frame->width, vr.width())),
                                        static_cast<UINT>(std::min(frame->height, vr.height())),
                                        1};
                    gpu->context()->CopySubresourceRegion(into.Get(), 0, static_cast<UINT>(offset.x),
                                                          static_cast<UINT>(offset.y), 0, frame->texture.Get(), 0,
                                                          &box);
                    surface->EndDraw();
                }
            }
            const std::int64_t t1 = kvm::steady_now_us();
            dcomp->Commit();
            const std::int64_t t2 = kvm::steady_now_us();
            copy_ms.push_back(static_cast<double>(t1 - t0) / 1000.0);
            commit_ms.push_back(static_cast<double>(t2 - t1) / 1000.0);
            ++drawn;
        } else if (!visuals.empty()) {
            dcomp->Commit();  // only the clips changed
            std::this_thread::sleep_for(5ms);
        } else {
            std::this_thread::sleep_for(5ms);
        }

        if (std::chrono::steady_clock::now() >= next_log) {
            next_log += 5s;
            kvm::log::info(
                "{} windows shown, {} pictures in 5 s; copy into the surface p50 {:.2f} / p95 {:.2f} ms, "
                "Commit p50 {:.2f} / p95 {:.2f} ms",
                shown_windows, drawn, percentile(copy_ms, 50), percentile(copy_ms, 95), percentile(commit_ms, 50),
                percentile(commit_ms, 95));
            copy_ms.clear();
            commit_ms.clear();
            drawn = 0;
        }
    }
    capture->stop();
    DestroyWindow(overlay);
    return 0;
}
