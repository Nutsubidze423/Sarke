// SPDX-License-Identifier: Apache-2.0

#include "desk_compositor.h"

#include <dxgi.h>

#include <algorithm>
#include <utility>

#include "kvm/log.h"

#pragma comment(lib, "dcomp.lib")

namespace kvm {

namespace {

using Microsoft::WRL::ComPtr;

}  // namespace

std::unique_ptr<DeskCompositor> DeskCompositor::create(HWND hwnd, std::shared_ptr<GpuDevice> gpu, std::string& why) {
    auto c = std::unique_ptr<DeskCompositor>(new DeskCompositor());
    c->hwnd_ = hwnd;
    c->gpu_ = std::move(gpu);
    ComPtr<IDXGIDevice> dxgi;
    if (!hwnd || !c->gpu_ || FAILED(c->gpu_->device()->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&c->device_))) ||
        FAILED(c->device_->CreateTargetForHwnd(hwnd, TRUE, &c->target_)) ||
        FAILED(c->device_->CreateVisual(&c->root_))) {
        why = "desk: DirectComposition can't draw the overlay";
        return nullptr;
    }
    c->target_->SetRoot(c->root_.Get());
    return c;
}

bool DeskCompositor::ensure_surface(int width, int height) {
    if (surface_ && width == surface_w_ && height == surface_h_) return true;
    surface_.Reset();
    if (width <= 0 || height <= 0 ||
        FAILED(device_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_ALPHA_MODE_IGNORE, &surface_))) {
        return false;
    }
    surface_w_ = width;
    surface_h_ = height;
    rebuild_visuals();  // they show the new surface
    return true;
}

void DeskCompositor::rebuild_visuals() {
    root_->RemoveAllVisuals();
    if (!surface_) return;
    for (const Cut& cut : cuts_) {
        ComPtr<IDCompositionVisual> visual;
        ComPtr<IDCompositionRectangleClip> clip;
        if (FAILED(device_->CreateVisual(&visual)) || FAILED(device_->CreateRectangleClip(&clip))) continue;
        clip->SetLeft(static_cast<float>(cut.rect.left));
        clip->SetTop(static_cast<float>(cut.rect.top));
        clip->SetRight(static_cast<float>(cut.rect.right));
        clip->SetBottom(static_cast<float>(cut.rect.bottom));
        if (cut.radius > 0) {
            const auto r = static_cast<float>(cut.radius);
            clip->SetTopLeftRadiusX(r);
            clip->SetTopLeftRadiusY(r);
            clip->SetTopRightRadiusX(r);
            clip->SetTopRightRadiusY(r);
            clip->SetBottomLeftRadiusX(r);
            clip->SetBottomLeftRadiusY(r);
            clip->SetBottomRightRadiusX(r);
            clip->SetBottomRightRadiusY(r);
        }
        visual->SetContent(surface_.Get());
        visual->SetClip(clip.Get());
        visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);  // antialiased clip edges (rounded corners)
        root_->AddVisual(visual.Get(), TRUE, nullptr);           // on top of the ones before
    }
}

void DeskCompositor::set_cuts(const std::vector<Cut>& cuts) {
    cuts_ = cuts;
    rebuild_visuals();
    // Clicks: only on the windows (and their resize rims); anywhere else they reach what is below.
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    for (const Cut& cut : cuts_) {
        HRGN one = CreateRectRgn(cut.hit.left, cut.hit.top, cut.hit.right, cut.hit.bottom);
        CombineRgn(region, region, one, RGN_OR);
        DeleteObject(one);
    }
    SetWindowRgn(hwnd_, region, TRUE);  // the system owns the region now
}

bool DeskCompositor::set_picture(ID3D11Texture2D* bgra, int width, int height) {
    if (!bgra || !ensure_surface(width, height)) return false;
    const GpuDevice::Lock lock(*gpu_);
    ComPtr<ID3D11Texture2D> into;
    POINT offset{};
    if (FAILED(surface_->BeginDraw(nullptr, IID_PPV_ARGS(&into), &offset))) return false;
    const D3D11_BOX box{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
    gpu_->context()->CopySubresourceRegion(into.Get(), 0, static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
                                           bgra, 0, &box);
    return SUCCEEDED(surface_->EndDraw());
}

bool DeskCompositor::set_picture(const BgraImage& image) {
    if (image.empty() || !ensure_surface(image.width, image.height)) return false;
    const GpuDevice::Lock lock(*gpu_);
    ComPtr<ID3D11Texture2D> into;
    POINT offset{};
    if (FAILED(surface_->BeginDraw(nullptr, IID_PPV_ARGS(&into), &offset))) return false;
    const D3D11_BOX box{static_cast<UINT>(offset.x),
                        static_cast<UINT>(offset.y),
                        0,
                        static_cast<UINT>(offset.x + image.width),
                        static_cast<UINT>(offset.y + image.height),
                        1};
    gpu_->context()->UpdateSubresource(into.Get(), 0, &box, image.pixels.data(), static_cast<UINT>(image.stride()), 0);
    return SUCCEEDED(surface_->EndDraw());
}

bool DeskCompositor::commit() {
    return SUCCEEDED(device_->Commit());
}

}  // namespace kvm
