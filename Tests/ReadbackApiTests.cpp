#include <qray/qray.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

int main()
{
    if (std::strcmp(QR_API_VERSION, "1.05.0000") != 0)
    {
        std::fprintf(stderr, "unexpected QR_API_VERSION: %s\n", QR_API_VERSION);
        return 1;
    }

    if (QR_FRAME_ID_LATEST != 0)
    {
        std::fprintf(stderr, "QR_FRAME_ID_LATEST must be zero\n");
        return 1;
    }

    if (sizeof(QrFrameReadbackInfo) != 56)
    {
        std::fprintf(stderr, "unexpected QrFrameReadbackInfo size: %zu\n", sizeof(QrFrameReadbackInfo));
        return 1;
    }

    if (offsetof(QrFrameReadbackInfo, pDst) != 40 ||
        offsetof(QrFrameReadbackInfo, frameId) != 48)
    {
        std::fprintf(stderr, "unexpected QrFrameReadbackInfo layout\n");
        return 1;
    }

    if (QR_READBACK_FAILED == QR_SUCCESS)
    {
        std::fprintf(stderr, "QR_READBACK_FAILED must not alias QR_SUCCESS\n");
        return 1;
    }

    std::puts("readback API contract passed");
    return 0;
}
