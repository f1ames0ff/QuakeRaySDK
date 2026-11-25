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

#include "TextureObserver.h"

#include <algorithm>
#include <iterator>

#include "TextureManager.h"
#include "Generated/ShaderCommonC.h"

bool qray::TextureObserver::HaveChanged(std::vector<DependentFile> &files)
{
    bool changed = false;

    for (DependentFile &file : files)
    {
        std::error_code ec;
        const auto lastWriteTime = std::filesystem::last_write_time(file.path, ec);
        if (ec)
        {
            continue;
        }

        if (lastWriteTime > file.lastWriteTime)
        {
            file.lastWriteTime = lastWriteTime;
            changed = true;
        }
    }

    return changed;
}

void qray::TextureObserver::ReuploadChangedFiles(const std::function<VkCommandBuffer()> &getCmd,
                                                  uint32_t frameIndex,
                                                  TextureManager &manager,
                                                  ImageLoaderDev *loader,
                                                  QrMaterial materialIndex,
                                                  const std::vector<DependentFile> &files)
{
    for (const DependentFile &file : files)
    {
        auto newImage = loader->Load(file.path);

        if (!newImage)
        {
            continue;
        }

        if (newImage->dataSize != file.dataSize)
        {
            assert(0 &&
                "Trying to hot-reload the image, but the data size is mismatching with what originally was specified."
                "A new texture file must have the same image size.");
            continue;
        }

        QrMaterialUpdateInfo info =
        {
            .target = materialIndex,
            .textures =
            {
                .pDataAlbedoAlpha               = file.textureType == MATERIAL_ALBEDO_ALPHA_INDEX                 ? newImage->pData : nullptr,
                .pDataRoughnessMetallicEmission = file.textureType == MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX  ? newImage->pData : nullptr,
                .pDataNormal                    = file.textureType == MATERIAL_NORMAL_INDEX                       ? newImage->pData : nullptr,
            },
        };

        const VkCommandBuffer cmd = getCmd ? getCmd() : VK_NULL_HANDLE;
        if (cmd != VK_NULL_HANDLE)
        {
            manager.UpdateMaterial(cmd, frameIndex, info);
        }

        loader->FreeLoaded();
    }
}

void qray::TextureObserver::CheckPathsAndReupload(const std::function<VkCommandBuffer()> &getCmd,
                                                  uint32_t frameIndex,
                                                  TextureManager &manager,
                                                  ImageLoaderDev *loader)
{
    if (loader == nullptr || materials.empty())
    {
        return;
    }

    {
        using namespace std::chrono_literals;

        constexpr auto frequency = 0.05s;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastCheck < frequency)
        {
            return;
        }

        lastCheck = now;
    }

    const size_t total = materials.size();
    const size_t checkCount = std::min(total, kMaxMaterialsPerTick);
    if (nextMaterialOffset >= total)
    {
        nextMaterialOffset = 0;
    }

    auto it = materials.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(nextMaterialOffset));
    nextMaterialOffset = (nextMaterialOffset + checkCount) % total;

    for (size_t checked = 0; checked < checkCount; checked++, ++it)
    {
        if (it == materials.end())
        {
            it = materials.begin();
        }

        auto &[materialIndex, files] = *it;
        if (!HaveChanged(files))
        {
            continue;
        }

        ReuploadChangedFiles(getCmd, frameIndex, manager, loader, materialIndex, files);
    }
}

void qray::TextureObserver::RegisterPath(QrMaterial index, std::optional<std::filesystem::path> path, const std::optional<ImageLoader::ResultInfo> &imageInfo, uint32_t textureType)
{
    if (index == QR_NO_MATERIAL)
    {
        return;
    }

    if (!path || path->empty())
    {
        return;
    }

    if (!imageInfo || imageInfo->dataSize == 0 || imageInfo->pData == nullptr)
    {
        return;
    }

    if (materials.find(index) == materials.end())
    {
        materials[index] = {};
    }

    std::error_code existsEc;
    if (!std::filesystem::exists(path.value(), existsEc) || existsEc)
    {
        return;
    }

    std::error_code timeEc;
    const auto lastWriteTime = std::filesystem::last_write_time(path.value(), timeEc);
    if (timeEc)
    {
        return;
    }

    materials[index].emplace_back(
        DependentFile
        {
            .path = std::move(path.value()),
            .lastWriteTime = lastWriteTime,
            .dataSize = imageInfo->dataSize,
            .format = imageInfo->format,
            .textureType = textureType,
        });
}

void qray::TextureObserver::Remove(QrMaterial index)
{
    materials.erase(index);
}
