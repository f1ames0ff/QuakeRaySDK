#include <qray/qray.h>

#include <cstddef>
#include <cstdio>
#include <string_view>

static_assert(std::string_view(QR_API_VERSION) == "1.06.0000", "unexpected QR_API_VERSION");
static_assert(QR_FRAME_ID_LATEST == 0, "QR_FRAME_ID_LATEST must be zero");
static_assert(sizeof(QrFrameReadbackInfo) == 56, "unexpected QrFrameReadbackInfo size");
static_assert(offsetof(QrFrameReadbackInfo, pDst) == 40 &&
                  offsetof(QrFrameReadbackInfo, frameId) == 48,
              "unexpected QrFrameReadbackInfo layout");
static_assert(QR_READBACK_FAILED != QR_SUCCESS, "QR_READBACK_FAILED must not alias QR_SUCCESS");

int main()
{
    std::puts("readback API contract passed");
    return 0;
}
