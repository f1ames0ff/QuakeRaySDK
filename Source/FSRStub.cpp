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

#include "FSR.h"

namespace qray
{
namespace FidelityFX
{

FSR::FSR(VkDevice device, VkPhysicalDevice physDevice, UserPrint *pUserPrint)
    : m_device(device)
    , m_physDevice(physDevice)
    , m_pUserPrint(pUserPrint)
    , m_context(nullptr)
    , m_requestedTechnique(QR_RENDER_UPSCALE_TECHNIQUE_NEAREST)
    , m_renderWidth(0)
    , m_renderHeight(0)
    , m_displayWidth(0)
    , m_displayHeight(0)
    , m_hasSize(false)
{
}

FSR::~FSR() = default;

void FSR::SetUpscaleVersion(QrRenderUpscaleTechnique technique)
{
    m_requestedTechnique = technique;
}

void FSR::OnFramebuffersSizeChange(const ResolutionState &resolutionState)
{
    m_renderWidth = resolutionState.renderWidth;
    m_renderHeight = resolutionState.renderHeight;
    m_displayWidth = resolutionState.upscaledWidth;
    m_displayHeight = resolutionState.upscaledHeight;
    m_hasSize = true;
}

FramebufferImageIndex FSR::Apply(VkCommandBuffer cmd,
                                 uint32_t frameIndex,
                                 const std::shared_ptr<Framebuffers> &framebuffers,
                                 const RenderResolutionHelper &renderResolution,
                                 QrFloat2D jitterOffset,
                                 float timeDelta,
                                 float nearPlane,
                                 float farPlane,
                                 float fovVerticalRad,
                                 bool resetAccumulation)
{
    (void)cmd;
    (void)frameIndex;
    (void)framebuffers;
    (void)renderResolution;
    (void)jitterOffset;
    (void)timeDelta;
    (void)nearPlane;
    (void)farPlane;
    (void)fovVerticalRad;
    (void)resetAccumulation;

    return FB_IMAGE_INDEX_FINAL;
}

QrFloat2D FSR::GetJitter(const ResolutionState &resolutionState, uint32_t frameId)
{
    (void)resolutionState;
    (void)frameId;

    return QrFloat2D{0.0f, 0.0f};
}

bool FSR::IsUpscaleVersionAvailable(QrRenderUpscaleTechnique technique)
{
    (void)technique;

    return false;
}

}
}
