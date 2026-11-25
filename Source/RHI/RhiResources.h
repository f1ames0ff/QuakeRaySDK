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

#pragma once

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qray::rhi
{

// Thin helpers that add the qray conventions (debug names, the upload path, the states) on top
// of nvrhi::IDevice, so that later stages create resources in one place only.
//
// The factories copy the desc, set the debug name and forward to the device. A failed creation
// gives a null handle; the caller decides what that means for its stage.

nvrhi::BufferHandle createBuffer(nvrhi::IDevice *device, const nvrhi::BufferDesc &desc, std::string_view debugName);
nvrhi::TextureHandle createTexture(nvrhi::IDevice *device, const nvrhi::TextureDesc &desc, std::string_view debugName);

// nvrhi::SamplerDesc has no debugName field at the pinned revision, so the argument cannot be
// applied; it is kept for symmetry with the other two factories.
nvrhi::SamplerHandle createSampler(nvrhi::IDevice *device, const nvrhi::SamplerDesc &desc, std::string_view debugName);

// Uploads `size` bytes at `offset` into `buffer` through the command list. This is what the
// legacy AutoBuffer did by hand: staging buffer, vkCmdCopyBuffer, barrier. NVRHI owns that path
// inside the command list (on Vulkan: vkCmdUpdateBuffer for writes of 64 KiB or less with a
// 4-byte-aligned offset, an upload-manager staging copy plus vkCmdCopyBuffer otherwise) and
// places the barriers around CopyDest itself.
//
// With automatic barriers on, the state tracker has to know the buffer's prior state: create
// buffers that go through this helper with keepInitialState = true.
void writeBuffer(nvrhi::ICommandList *commandList, nvrhi::IBuffer *buffer, const void *data, size_t size, size_t offset = 0);

// A 1x1 opaque white texture, filled through the given command list; the skeleton samples it so
// that the texture+sampler path is exercised while the image stays what it was.
nvrhi::TextureHandle createWhiteTexture(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, std::string_view debugName);

// Generates mip levels 1..mipLevels-1 of a cubemap (or 2D array texture) from mip 0 using native
// vkCmdBlitImage (VK_FILTER_LINEAR) across all array layers and transitions the entire texture to
// `finalState`.
void generateCubemapMips(nvrhi::ICommandList *commandList,
                         nvrhi::ITexture *cubemap,
                         uint32_t baseSize,
                         uint32_t mipLevels,
                         nvrhi::ResourceStates finalState = nvrhi::ResourceStates::NonPixelShaderResource);

}
