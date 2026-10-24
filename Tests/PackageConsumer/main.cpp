#include <nvrhi/nvrhi.h>
#include <qray/qray.h>

static_assert(nvrhi::c_MaxBindingLayouts >= 13,
              "QuakeRaySDK needs the NVRHI binding-layout limit from its pinned patch");

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

int main()
{
    QrWin32SurfaceCreateInfo surface = {};
    surface.hinstance = GetModuleHandleW(nullptr);

    const char *description = qrGetResultDescription(QR_SUCCESS);

    return surface.hinstance != nullptr && description != nullptr &&
           std::strcmp(description, "QR_SUCCESS") == 0
        ? 0
        : 1;
}
