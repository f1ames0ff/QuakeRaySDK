// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include <nvrhi/nvrhi.h>
#include <qray/qray.h>
#include <qray/gpu_context.h>
#include <qray/host_contract.h>

static_assert(nvrhi::c_MaxBindingLayouts >= 13,
              "QuakeRaySDK needs the NVRHI binding-layout limit from its pinned patch");

#ifdef _WIN32
    #ifndef NOMINMAX
        #error "qray::renderer must export NOMINMAX to C++ consumers"
    #endif

    #ifndef QR_STATIC
        #error "qray::renderer must export QR_STATIC"
    #endif

    #ifndef QR_USE_SURFACE_WIN32
        #error "qray::renderer must export QR_USE_SURFACE_WIN32"
    #endif

    #include <cstring>
    #include <windows.h>
#else
    #ifndef QR_STATIC
        #error "qray::renderer must export QR_STATIC"
    #endif

    #ifndef QR_USE_SURFACE_XLIB
        #error "qray::renderer must export QR_USE_SURFACE_XLIB"
    #endif

    #include <cstring>
    #include <X11/Xlib.h>
    #undef None
    #undef Success
    #undef Always
    #undef Bool
    #undef Status
#endif

int main()
{
#ifdef _WIN32
    QrWin32SurfaceCreateInfo surface = {};
    surface.hinstance = GetModuleHandleW(nullptr);
    const bool surfaceOk = surface.hinstance != nullptr;
#else
    QrXlibSurfaceCreateInfo surface = {};
    surface.dpy = nullptr;
    surface.window = 0;
    const bool surfaceOk = true;
#endif

    const char *description = qrGetResultDescription(QR_SUCCESS);
    const uint32_t hostContractVersion = qray::qrayGetHostContractVersion();
    const qray::QrayGpuContractInfo *gpuInfo = qray::qrayGetGpuContractInfo();

    return surfaceOk && description != nullptr &&
           std::strcmp(description, "QR_SUCCESS") == 0 &&
           hostContractVersion == qray::kHostContractVersion &&
           gpuInfo != nullptr &&
           gpuInfo->contractVersion == qray::kGpuContextContractVersion
        ? 0
        : 1;
}
