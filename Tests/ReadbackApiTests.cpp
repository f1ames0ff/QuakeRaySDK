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
