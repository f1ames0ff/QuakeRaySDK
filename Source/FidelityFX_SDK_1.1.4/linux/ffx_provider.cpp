// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// SPDX-License-Identifier: MIT

#include "ffx_provider.h"
#include "ffx_provider_fsr3upscale.h"

static constexpr ffxProvider *providers[] = {
    &ffxProvider_FSR3Upscale::Instance,
};

static constexpr size_t providerCount = sizeof(providers) / sizeof(providers[0]);

const ffxProvider *GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void *device)
{
    (void)device;

    for (size_t i = 0; i < providerCount; ++i)
    {
        if (overrideId != 0 && providers[i]->GetId() != overrideId)
        {
            continue;
        }

        if (providers[i]->CanProvide(descType))
        {
            return providers[i];
        }
    }

    return nullptr;
}

const ffxProvider *GetAssociatedProvider(ffxContext *context)
{
    const InternalContextHeader *hdr = (const InternalContextHeader *)(*context);
    return hdr->provider;
}

uint64_t GetProviderCount(ffxStructType_t descType, void *device)
{
    return GetProviderVersions(descType, device, UINT64_MAX, nullptr, nullptr);
}

uint64_t GetProviderVersions(ffxStructType_t descType, void *device, uint64_t capacity, uint64_t *versionIds, const char **versionNames)
{
    (void)device;

    uint64_t count = 0;

    for (size_t i = 0; i < providerCount; ++i)
    {
        if (count >= capacity)
        {
            break;
        }

        if (providers[i]->CanProvide(descType))
        {
            const uint64_t index = count++;
            if (versionIds != nullptr)
            {
                versionIds[index] = providers[i]->GetId();
            }
            if (versionNames != nullptr)
            {
                versionNames[index] = providers[i]->GetVersionName();
            }
        }
    }

    return count;
}
