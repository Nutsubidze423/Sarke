// SPDX-License-Identifier: Apache-2.0

// Desk mode on H.264, viewer side (roadmap P2.7c): the owner's whole virtual display arrives as one
// picture, and each of its windows is cut out of it with DirectComposition (prototype:
// tools/dcomp_desk_prototype.cpp). One surface holds the picture; each window is a visual of that
// surface, clipped to the window's rect with its rounded corners. Overlapping windows come out right
// by themselves: they are cut from one picture that already has the owner's stacking.
//
//   auto c = kvm::DeskCompositor::create(hwnd, gpu, why);   // hwnd: WS_EX_NOREDIRECTIONBITMAP
//   c->set_cuts(cuts);                                      // the windows, bottom first
//   c->set_picture(texture, w, h);                          // or set_picture(image)
//   c->commit();
//
// Clicks: the window gets a region of the cuts' hit rects (plain rectangles: a rounded GDI region would
// cut the antialiased corners back to stairs), so clicks elsewhere go to the windows below. UI thread
// only; it holds the device's lock while it draws (the decoder works on the same device).
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "jpeg_decoder.h"
#include "kvm/gpu_device.h"

namespace kvm {

class DeskCompositor {
public:
    struct Cut {
        RECT rect{};     // the window, in the overlay's client coordinates (= the owner's VDD's)
        RECT hit{};      // rect plus the rim that takes clicks for the owner's resize borders
        int radius = 0;  // rounded corners, px
    };

    // nullptr with `why` set if DirectComposition can't draw into `hwnd` on `gpu`'s device.
    static std::unique_ptr<DeskCompositor> create(HWND hwnd, std::shared_ptr<GpuDevice> gpu, std::string& why);
    DeskCompositor(const DeskCompositor&) = delete;
    DeskCompositor& operator=(const DeskCompositor&) = delete;

    // The windows to show, bottom first. Also sets the window region.
    void set_cuts(const std::vector<Cut>& cuts);
    // A new picture of the whole VDD: a BGRA texture of gpu()'s device, or pixels in memory.
    bool set_picture(ID3D11Texture2D* bgra, int width, int height);
    bool set_picture(const BgraImage& image);
    // Shows what was set since the last commit.
    bool commit();

    const std::shared_ptr<GpuDevice>& gpu() const { return gpu_; }

private:
    DeskCompositor() = default;
    bool ensure_surface(int width, int height);
    void rebuild_visuals();

    HWND hwnd_ = nullptr;
    std::shared_ptr<GpuDevice> gpu_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> root_;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface_;
    int surface_w_ = 0;
    int surface_h_ = 0;
    std::vector<Cut> cuts_;
};

}  // namespace kvm
