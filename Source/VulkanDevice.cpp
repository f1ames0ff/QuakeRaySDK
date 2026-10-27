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

#include "VulkanDevice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "HaltonSequence.h"
#include "Matrix.h"
#include "RenderResolutionHelper.h"
#include "QrException.h"
#include "Utils.h"
#include "Const.h"
#include "Generated/ShaderCommonC.h"
#include "RHI/NvrhiContext.h"
#include "RHI/NvrhiFrameSkeleton.h"
#include "RHI/RhiAccelStructs.h"

using namespace qray;

namespace
{

float ElapsedMilliseconds(std::chrono::steady_clock::time_point begin,
                          std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<float, std::milli>(end - begin).count();
}

QrFloat3D SpatialPoint(const SpatialFrame &frame, const QrFloat3D &value)
{
    return frame.IsIdentity() ? value : frame.Point(value);
}

QrFloat3D SpatialDirection(const SpatialFrame &frame, const QrFloat3D &value)
{
    return frame.IsIdentity() ? value : frame.Direction(value);
}

QrTransform SpatialTransform(const SpatialFrame &frame, const QrTransform &value)
{
    return frame.IsIdentity() ? value : frame.Transform(value);
}

QrDirectionalLightUploadInfo SpatialDirectionalLight(const SpatialFrame &frame, const QrDirectionalLightUploadInfo &source)
{
    QrDirectionalLightUploadInfo result = source;
    result.direction = SpatialDirection(frame, source.direction);
    return result;
}

QrSphericalLightUploadInfo SpatialSphericalLight(const SpatialFrame &frame, const QrSphericalLightUploadInfo &source)
{
    QrSphericalLightUploadInfo result = source;
    const float powerScale = frame.Scale() * frame.Scale();
    result.position = SpatialPoint(frame, source.position);
    result.normal = SpatialDirection(frame, source.normal);
    result.radius = frame.Length(source.radius);
    result.color.data[0] *= powerScale;
    result.color.data[1] *= powerScale;
    result.color.data[2] *= powerScale;
    return result;
}

QrSpotLightUploadInfo SpatialSpotLight(const SpatialFrame &frame, const QrSpotLightUploadInfo &source)
{
    QrSpotLightUploadInfo result = source;
    const float powerScale = frame.Scale() * frame.Scale();
    result.position = SpatialPoint(frame, source.position);
    result.direction = SpatialDirection(frame, source.direction);
    result.radius = frame.Length(source.radius);
    result.color.data[0] *= powerScale;
    result.color.data[1] *= powerScale;
    result.color.data[2] *= powerScale;
    return result;
}

QrPolygonalLightUploadInfo SpatialPolygonalLight(const SpatialFrame &frame, const QrPolygonalLightUploadInfo &source)
{
    QrPolygonalLightUploadInfo result = source;
    const float powerScale = frame.Scale() * frame.Scale();

    for (int i = 0; i < 3; i++)
    {
        result.positions[i] = SpatialPoint(frame, source.positions[i]);
    }

    if (frame.IsMirrored())
    {
        // A polygonal light has no normal field: LightManager derives one from the
        // vertex order, so a mirrored frame has to restore the host's authored side
        // by putting that order back.
        std::swap(result.positions[1], result.positions[2]);
    }

    result.color.data[0] *= powerScale;
    result.color.data[1] *= powerScale;
    result.color.data[2] *= powerScale;
    return result;
}

QrTexturedAreaLightUploadInfo SpatialTexturedAreaLight(const SpatialFrame &frame, const QrTexturedAreaLightUploadInfo &source)
{
    QrTexturedAreaLightUploadInfo result = source;
    result.A = SpatialPoint(frame, source.A);
    result.B = SpatialPoint(frame, source.B);
    result.C = SpatialPoint(frame, source.C);
    result.normal = SpatialDirection(frame, source.normal);
    result.area = frame.Area(source.area);
    return result;
}

QrDtalGroupUploadInfo SpatialDtalGroup(const SpatialFrame &frame, const QrDtalGroupUploadInfo &source)
{
    QrDtalGroupUploadInfo result = source;
    result.center = SpatialPoint(frame, source.center);
    result.normal = SpatialDirection(frame, source.normal);
    frame.Aabb(source.boundsMin, source.boundsMax, result.boundsMin, result.boundsMax);
    result.area = frame.Area(source.area);
    result.reach = frame.Length(source.reach);
    result.boundsRadius = frame.Length(source.boundsRadius);
    return result;
}

QrDtalMemberUpload SpatialDtalMember(const SpatialFrame &frame, const QrDtalMemberUpload &source)
{
    QrDtalMemberUpload result = source;
    result.A = SpatialPoint(frame, source.A);
    result.B = SpatialPoint(frame, source.B);
    result.C = SpatialPoint(frame, source.C);
    result.normal = SpatialDirection(frame, source.normal);
    result.area = frame.Area(source.area);
    return result;
}

QrClusterLightSource SpatialClusterLightSource(const SpatialFrame &frame, const QrClusterLightSource &source)
{
    QrClusterLightSource result = source;
    result.origin = SpatialPoint(frame, source.origin);
    result.reach = frame.Length(source.reach);
    result.radius = frame.Length(source.radius);
    return result;
}

QrFogVolume SpatialFogVolume(const SpatialFrame &frame, const QrFogVolume &source)
{
    QrFogVolume result = source;
    result.pointA = SpatialPoint(frame, source.pointA);
    result.pointB = SpatialPoint(frame, source.pointB);
    result.halfExtinctionDistance = frame.Length(source.halfExtinctionDistance);
    result.softface = frame.SoftFace(source.softface);
    return result;
}

QrParticlePoint SpatialParticlePoint(const SpatialFrame &frame, const QrParticlePoint &source)
{
    QrParticlePoint result = source;
    const QrFloat3D position = SpatialPoint(frame, { { source.position[0], source.position[1], source.position[2] } });
    result.position[0] = position.data[0];
    result.position[1] = position.data[1];
    result.position[2] = position.data[2];
    result.size = frame.Length(source.size);
    return result;
}

QrPortalUploadInfo SpatialPortal(const SpatialFrame &frame, const QrPortalUploadInfo &source)
{
    QrPortalUploadInfo result = source;
    result.inPosition = SpatialPoint(frame, source.inPosition);
    result.outPosition = SpatialPoint(frame, source.outPosition);
    result.outDirection = SpatialDirection(frame, source.outDirection);
    result.outUp = SpatialDirection(frame, source.outUp);
    return result;
}

}

void VulkanDevice::SetSpatialConvention(const QrSpatialConvention *pConvention)
{
    if (pConvention == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    if (spatialFrameLocked)
    {
        throw QrException(QR_WRONG_FUNCTION_CALL, "The spatial convention must be set before spatial uploads");
    }

    if (!spatialFrame.Configure(*pConvention))
    {
        throw QrException(QR_WRONG_ARGUMENT, "Invalid spatial convention");
    }

    spatialConvention = *pConvention;
}

void VulkanDevice::GetSpatialConvention(QrSpatialConvention *pConvention) const
{
    if (pConvention == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    if (pConvention->size < sizeof(QrSpatialConvention))
    {
        throw QrException(QR_WRONG_ARGUMENT, "QrSpatialConvention size is too small");
    }

    *pConvention = spatialConvention;
}

void VulkanDevice::SetSmokeWindDirection(const QrFloat3D *pDirection)
{
    if (pDirection == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    smokeWindDirection = *pDirection;
    smokeWindDirectionSet = true;
}

void VulkanDevice::GetSmokeWindDirection(QrFloat3D *pDirection) const
{
    if (pDirection == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    *pDirection = smokeWindDirection;
}

void VulkanDevice::LockSpatialFrame()
{
    spatialFrameLocked = true;
}

VkCommandBuffer VulkanDevice::BeginFrame(const QrStartFrameInfo &startInfo)
{
    using Clock = std::chrono::steady_clock;

    const Clock::time_point beginStart = Clock::now();
    const uint32_t previousFrameIndex = currentFrameState.GetFrameIndex();
    uint32_t frameIndex = currentFrameState.IncrementFrameIndexAndGet();

    const Clock::time_point fenceWaitStart = Clock::now();
    if (!waitForOutOfFrameFence)
    {
        Utils::WaitAndResetFence(device, frameFences[frameIndex]);
    }
    else
    {
        Utils::WaitAndResetFences(device, frameFences[frameIndex], outOfFrameFences[frameIndex]);
    }
    const Clock::time_point fenceWaitEnd = Clock::now();

    swapchain->RequestPresentMode(startInfo.presentMode);
    swapchain->SetMaxFrameLatency(startInfo.maxFrameLatency);
    swapchain->SetFullscreenExclusive(startInfo.wantFullscreenExclusive != 0, startInfo.hasFocus != 0);

    assert(pendingPreFrameCmd == VK_NULL_HANDLE);
    pendingPreFrameCmd = currentFrameState.GetPreFrameCmdAndRemove(previousFrameIndex);
    pendingPreFrameCmdFrameIndex = previousFrameIndex;

    if (startInfo.requestShaderReload)
    {
        shaderManager->ReloadShaders();
    }

    cmdManager->PrepareForFrame(frameIndex);

    worldSamplerManager->PrepareForFrame(frameIndex);
    genericSamplerManager->PrepareForFrame(frameIndex);
    textureManager->PrepareForFrame(frameIndex);
    cubemapManager->PrepareForFrame(frameIndex);
    rasterizedDataCollector->Clear(frameIndex);
    decalManager->PrepareForFrame(frameIndex);

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    BeginCmdLabel(cmd, "Prepare for frame");

    scene->PrepareForFrame(cmd, frameIndex);

    const Clock::time_point beginEnd = Clock::now();
    const float beginTotalMs = ElapsedMilliseconds(beginStart, beginEnd);
    const float frameFenceWaitMs = ElapsedMilliseconds(fenceWaitStart, fenceWaitEnd);
    pendingBeginFrameCpuTimings.frameFenceWaitMs = frameFenceWaitMs;
    pendingBeginFrameCpuTimings.presentWaitMs = 0.0f;
    pendingBeginFrameCpuTimings.imageAcquireMs = 0.0f;
    pendingBeginFrameCpuTimings.beginOtherMs = std::max(0.0f, beginTotalMs - frameFenceWaitMs);

    return cmd;
}

void VulkanDevice::AcquireFrame()
{
    if (!currentFrameState.WasFrameStarted())
    {
        throw QrException(QR_FRAME_WASNT_STARTED);
    }

    frameAcquireGate.Acquire([this] {
        AcquireFrameImage();
    });
}

void VulkanDevice::AcquireFrameImage()
{
    const uint32_t frameIndex = currentFrameState.GetFrameIndex();
    const SwapchainAcquireTimings acquireTimings =
        swapchain->AcquireImage(imageAvailableSemaphores[frameIndex]);

    {
        const std::string presentModeName =
            std::string(swapchain->GetPresentModeName()) + ", images " + std::to_string(swapchain->GetImageCount()) +
            (swapchain->IsPresentWaitActive() ? ", present wait on" : ", present wait off");

        if (presentModeName != printedPresentModeName)
        {
            printedPresentModeName = presentModeName;
            Print(("RHI: the swapchain uses " + presentModeName).c_str());
        }
    }

    VkSemaphore semaphoreToWaitOnSubmit = imageAvailableSemaphores[frameIndex];
    VkPipelineStageFlags semaphoreWaitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    waitForOutOfFrameFence = false;

    if (pendingPreFrameCmd != VK_NULL_HANDLE)
    {
        cmdManager->SubmitForFrame(pendingPreFrameCmdFrameIndex,
                                   pendingPreFrameCmd,
                                   semaphoreToWaitOnSubmit, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   inFrameSemaphores[frameIndex],
                                   outOfFrameFences[(frameIndex + 1) % MAX_FRAMES_IN_FLIGHT]);

        pendingPreFrameCmd = VK_NULL_HANDLE;
        semaphoreToWaitOnSubmit = inFrameSemaphores[frameIndex];
        semaphoreWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        waitForOutOfFrameFence = true;
    }

    currentFrameState.SetSemaphore(semaphoreToWaitOnSubmit, semaphoreWaitStage);
    pendingBeginFrameCpuTimings.presentWaitMs = acquireTimings.presentWaitMs;
    pendingBeginFrameCpuTimings.imageAcquireMs = acquireTimings.imageAcquireMs;
}

#define QR_SET_VEC3( dst, x, y, z ) \
    ( dst )[ 0 ] = ( x );           \
    ( dst )[ 1 ] = ( y );           \
    ( dst )[ 2 ] = ( z )

#define QR_SET_VEC3_A( dst, xyz )  \
    ( dst )[ 0 ] = ( xyz )[ 0 ]; \
    ( dst )[ 1 ] = ( xyz )[ 1 ]; \
    ( dst )[ 2 ] = ( xyz )[ 2 ]

#define QR_MAX_VEC3( dst, m ) \
    ( dst )[ 0 ] = std::max( ( dst )[ 0 ], ( m ) );  \
    ( dst )[ 1 ] = std::max( ( dst )[ 1 ], ( m ) );  \
    ( dst )[ 2 ] = std::max( ( dst )[ 2 ], ( m ) )

void VulkanDevice::FillUniform(ShGlobalUniform *gu, const QrDrawFrameInfo &drawInfo) const
{
    const float IdentityMat4x4[16] =
    {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };

    const float aspect = static_cast< float >( renderResolution.Width() ) / static_cast< float >( renderResolution.Height() );

    {
        memcpy( gu->viewPrev, gu->view, 16 * sizeof( float ) );
        memcpy( gu->projectionPrev, gu->projection, 16 * sizeof( float ) );

        memcpy( gu->view, drawInfo.view, 16 * sizeof( float ) );

        Matrix::MakeProjectionMatrix( gu->projection,
                                      aspect,
                                      drawInfo.fovYRadians,
                                      drawInfo.cameraNear,
                                      drawInfo.cameraFar );

        Matrix::Inverse( gu->invView, gu->view );
        Matrix::Inverse( gu->invProjection, gu->projection );

        memcpy( gu->cameraPositionPrev, gu->cameraPosition, 3 * sizeof( float ) );
        gu->cameraPosition[ 0 ] = gu->invView[ 12 ];
        gu->cameraPosition[ 1 ] = gu->invView[ 13 ];
        gu->cameraPosition[ 2 ] = gu->invView[ 14 ];
    }

    {
        static_assert( sizeof( gu->instanceGeomInfoOffset ) == sizeof( gu->instanceGeomInfoOffsetPrev ) );
        memcpy( gu->instanceGeomInfoOffsetPrev, gu->instanceGeomInfoOffset, sizeof( gu->instanceGeomInfoOffset ) );
    }

    {
        gu->frameId   = frameId;
        gu->timeDelta = static_cast< float >( std::max< double >( currentFrameTime - previousFrameTime, 0.001 ) );
        gu->time      = static_cast< float >( currentFrameTime );
    }

    {
        gu->renderWidth  = static_cast< float >( renderResolution.Width() );
        gu->renderHeight = static_cast< float >( renderResolution.Height() );
        assert( ( int )gu->renderWidth % 2 == 0 );

        gu->upscaledRenderWidth  = static_cast< float >( renderResolution.UpscaledWidth() );
        gu->upscaledRenderHeight = static_cast< float >( renderResolution.UpscaledHeight() );

        QrFloat2D jitter = renderResolution.IsNvDlssEnabled() ? HaltonSequence::GetJitter_Halton23( frameId ) :
                           renderResolution.IsAmdFsr3Enabled() ? FidelityFX::FSR::GetJitter( renderResolution.GetResolutionState(), frameId ) :
                           QrFloat2D{ 0, 0 };

        gu->jitterX = jitter.data[ 0 ];
        gu->jitterY = jitter.data[ 1 ];
    }

    {
        gu->stopEyeAdaptation = drawInfo.disableEyeAdaptation;

        if( drawInfo.pTonemappingParams != nullptr )
        {
            gu->minLogLuminance     = drawInfo.pTonemappingParams->minLogLuminance;
            gu->maxLogLuminance     = drawInfo.pTonemappingParams->maxLogLuminance;
            gu->luminanceWhitePoint = drawInfo.pTonemappingParams->luminanceWhitePoint;
        }
        else
        {
            gu->minLogLuminance     = -3.9f;
            gu->maxLogLuminance     = -2.8f;
            gu->luminanceWhitePoint = 10.0f;
        }
    }

    {
        gu->lightCount     = scene->GetLightManager()->GetLightCount();
        gu->lightCountPrev = scene->GetLightManager()->GetLightCountPrev();

        gu->directionalLightExists = scene->GetLightManager()->DoesDirectionalLightExist();
    }

    {
        static_assert( sizeof( gu->skyCubemapRotationTransform ) == sizeof( IdentityMat4x4 ) &&
                       sizeof( IdentityMat4x4 ) == 16 * sizeof( float ), "Recheck skyCubemapRotationTransform sizes" );
        memcpy( gu->skyCubemapRotationTransform, IdentityMat4x4, 16 * sizeof( float ) );

        if( drawInfo.pSkyParams != nullptr )
        {
            const auto& sp = *drawInfo.pSkyParams;

            memcpy( gu->skyColorDefault, sp.skyColorDefault.data, sizeof( float ) * 3 );
            gu->skyColorMultiplier = sp.skyColorMultiplier;
            gu->skyColorSaturation = std::max( sp.skyColorSaturation, 0.0f );
            gu->skyAmbientLod      = std::clamp( sp.skyAmbientLod, 0.0f, 10.0f );
            gu->skyLightMultiplier = std::max( sp.skyLightMultiplier, 0.0f );
            gu->skyNee             = sp.skyNee != 0 ? 1.0f : 0.0f;

            gu->skyType = sp.skyType == QR_SKY_TYPE_CUBEMAP ? SKY_TYPE_CUBEMAP :
                          sp.skyType == QR_SKY_TYPE_RASTERIZED_GEOMETRY ? SKY_TYPE_RASTERIZED_GEOMETRY :
                          sp.skyType == QR_SKY_TYPE_PROCEDURAL ? SKY_TYPE_PROCEDURAL :
                          SKY_TYPE_COLOR;

            gu->skyCubemapIndex = cubemapManager->IsCubemapValid( sp.skyCubemap ) ? sp.skyCubemap : QR_EMPTY_CUBEMAP;

            if( !Utils::IsAlmostZero( drawInfo.pSkyParams->skyCubemapRotationTransform ) )
            {
                Utils::SetMatrix3ToGLSLMat4( gu->skyCubemapRotationTransform, drawInfo.pSkyParams->skyCubemapRotationTransform );
            }
        }
        else
        {
            gu->skyColorDefault[ 0 ] = gu->skyColorDefault[ 1 ] = gu->skyColorDefault[ 2 ] = gu->skyColorDefault[ 3 ] = 1.0f;
            gu->skyColorMultiplier                                                                                    = 1.0f;
            gu->skyColorSaturation                                                                                    = 1.0f;
            gu->skyAmbientLod                                                                                         = 10.0f;
            gu->skyLightMultiplier                                                                                    = 1.0f;
            gu->skyNee                                                                                                = 0.0f;
            gu->skyType                                                                                               = SKY_TYPE_COLOR;
            gu->skyCubemapIndex                                                                                       = QR_EMPTY_CUBEMAP;
        }

        QrFloat3D skyViewerPosition = drawInfo.pSkyParams ? drawInfo.pSkyParams->skyViewerPosition : QrFloat3D{ 0, 0, 0 };

        for( uint32_t i = 0; i < 6; i++ )
        {
            float* viewProjDst = &gu->viewProjCubemap[ 16 * i ];

            Matrix::GetCubemapViewProjMat( viewProjDst, i, skyViewerPosition.data, drawInfo.cameraNear, drawInfo.cameraFar );
        }
    }

    gu->debugShowFlags = 0;

    if( drawInfo.pDebugParams != nullptr )
    {
        QrDebugDrawFlags fs = drawInfo.pDebugParams->drawFlags;

        if( fs & QR_DEBUG_DRAW_ONLY_DIFFUSE_DIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE;
        }
        else if( fs & QR_DEBUG_DRAW_ONLY_DIFFUSE_INDIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE;
        }
        else if( fs & QR_DEBUG_DRAW_ONLY_SPECULAR_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_SPECULAR;
        }
        else if( fs & QR_DEBUG_DRAW_UNFILTERED_DIFFUSE_DIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_DIFFUSE;
        }
        else if( fs & QR_DEBUG_DRAW_UNFILTERED_DIFFUSE_INDIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_INDIRECT;
        }
        else if( fs & QR_DEBUG_DRAW_UNFILTERED_SPECULAR_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_SPECULAR;
        }

        if( fs & QR_DEBUG_DRAW_ALBEDO_WHITE_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ALBEDO_WHITE;
        }
        if( fs & QR_DEBUG_DRAW_MOTION_VECTORS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_MOTION_VECTORS;
        }
        if( fs & QR_DEBUG_DRAW_GRADIENTS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_GRADIENTS;
        }
        if( fs & QR_DEBUG_DRAW_GOD_RAYS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_GOD_RAYS;
        }
        if( fs & QR_DEBUG_DRAW_STATS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_RAY_STATS;
        }
        if( fs & QR_DEBUG_DRAW_LUMA_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_LUMA;
        }

        const uint32_t giProbeBits =
            ( 1u << 15 ) | ( 1u << 16 ) | ( 1u << 17 ) | ( 1u << 18 ) | ( 1u << 19 );
        gu->debugShowFlags |= ( fs & giProbeBits );
    }

    gu->coreQ2RTX = 1u;

    if( drawInfo.pTexturesParams != nullptr )
    {
        gu->normalMapStrength      = drawInfo.pTexturesParams->normalMapStrength;
        gu->emissionMapBoost       = std::max( drawInfo.pTexturesParams->emissionMapBoost, 0.0f );
        gu->emissionMaxScreenColor = std::max( drawInfo.pTexturesParams->emissionMaxScreenColor, 0.0f );
        gu->emissionSharpMask      = std::max( drawInfo.pTexturesParams->emissionSharpMask, 0.0f );
        gu->talSelfLitOffset       = std::max( drawInfo.pTexturesParams->talSelfLitOffset, 0.0f );
        gu->squareInputRoughness   = !!drawInfo.pTexturesParams->squareInputRoughness;
        gu->minRoughness           = std::clamp( drawInfo.pTexturesParams->minRoughness, 0.0f, 1.0f );
        gu->emissionBlendMode      = drawInfo.pTexturesParams->emissionBlendMode;
        gu->emissionBlendStrength  = std::clamp( drawInfo.pTexturesParams->emissionBlendStrength, 0.0f, 1.0f );
        for( uint32_t i = 0; i < QR_LIGHT_STYLE_COUNT; i++ )
        {
            gu->lightStyleScales[ i ] = drawInfo.pTexturesParams->lightStyleScales[ i ];
        }
    }
    else
    {
        gu->normalMapStrength      = 1.0f;
        gu->emissionMapBoost       = 100.0f;
        gu->emissionMaxScreenColor = 1.5f;
        gu->emissionSharpMask      = true;
        gu->talSelfLitOffset       = 0.0f;
        gu->squareInputRoughness   = 1;
        gu->minRoughness           = 0.0f;
        gu->emissionBlendMode      = 0u;
        gu->emissionBlendStrength  = 1.0f;
        for( uint32_t i = 0; i < QR_LIGHT_STYLE_COUNT; i++ )
        {
            gu->lightStyleScales[ i ] = 1.0f;
        }
    }

    if( drawInfo.pIlluminationParams != nullptr )
    {
        gu->polyLightSpotlightFactor   = std::max( 0.0f, drawInfo.pIlluminationParams->polygonalLightSpotlightFactor );
        gu->indirSecondBounce          = !!drawInfo.pIlluminationParams->enableSecondBounceForIndirect;
        gu->lightIndexIgnoreFPVShadows = scene->GetLightManager()->GetLightIndexIgnoreFPVShadows( currentFrameState.GetFrameIndex(), drawInfo.pIlluminationParams->lightUniqueIdIgnoreFirstPersonViewerShadows );
        gu->cellWorldSize              = std::max( drawInfo.pIlluminationParams->cellWorldSize, 2.5e-5f );
        gu->gradientMultDiffuse        = std::clamp( drawInfo.pIlluminationParams->directDiffuseSensitivityToChange, 0.0f, 1.0f );
        gu->gradientMultIndirect       = std::clamp( drawInfo.pIlluminationParams->indirectDiffuseSensitivityToChange, 0.0f, 1.0f );
        gu->gradientMultSpecular       = std::clamp( drawInfo.pIlluminationParams->specularSensitivityToChange, 0.0f, 1.0f );
        gu->q2DepthGradMode            = drawInfo.pIlluminationParams->q2DepthGradMode;
        gu->q2LightStatsMode           = drawInfo.pIlluminationParams->q2LightStatsMode;
        gu->reflRefrEarlyOut           = drawInfo.pIlluminationParams->reflRefrEarlyOut != 0;
        gu->neeLightSamples            = std::clamp( drawInfo.pIlluminationParams->neeLightSamples, 1u, 2u );
        gu->restirParams[0]            = drawInfo.pIlluminationParams->restirEnabled != 0 ? 1u : 0u;
        gu->restirParams[1]            = std::clamp( drawInfo.pIlluminationParams->restirCandidates, 1u, 64u );
        gu->restirParams[2]            = 0u;
        gu->restirParams[3]            = 0u;
        gu->giBounceRays[0]            = std::clamp( drawInfo.pIlluminationParams->giBounceRays, 0.0f, 2.0f );
        gu->fltEnable[0]               = drawInfo.pIlluminationParams->denoiserEnabled != 0 ? 1.0f : 0.0f;
        gu->fixedAlbedo[0]             = std::max( drawInfo.pIlluminationParams->fixedAlbedo, 0.0f );
        gu->sunBounce[0]               = std::max( drawInfo.pIlluminationParams->sunBounceRange, 0.0f );
        gu->sunBounce[1]               = std::max( drawInfo.pIlluminationParams->sunBounceScale, 0.0f );
    }
    else
    {
        gu->polyLightSpotlightFactor   = 2.0f;
        gu->indirSecondBounce          = false;
        gu->lightIndexIgnoreFPVShadows = LIGHT_INDEX_NONE;
        gu->cellWorldSize              = 0.025f;
        gu->gradientMultDiffuse        = 0.5f;
        gu->gradientMultIndirect       = 0.2f;
        gu->gradientMultSpecular       = 0.5f;
        gu->q2DepthGradMode            = 1u;
        gu->q2LightStatsMode           = 1u;
        gu->reflRefrEarlyOut           = 1u;
        gu->neeLightSamples            = 1u;
        gu->restirParams[0]            = 0u;
        gu->restirParams[1]            = 8u;
        gu->restirParams[2]            = 0u;
        gu->restirParams[3]            = 0u;
        gu->giBounceRays[0]            = 1.0f;
        gu->fltEnable[0]               = 1.0f;
        gu->fixedAlbedo[0]             = 0.0f;
        gu->sunBounce[0]               = 50.0f;
        gu->sunBounce[1]               = 1.0f;
    }

    if( drawInfo.pBloomParams != nullptr )
    {
        gu->bloomThreshold              = std::max( drawInfo.pBloomParams->inputThreshold, 0.0f );
        gu->bloomIntensity              = std::max( drawInfo.pBloomParams->bloomIntensity, 0.0f );
        gu->bloomEmissionMultiplier     = std::max( drawInfo.pBloomParams->bloomEmissionMultiplier, 0.0f );
    }
    else
    {
        gu->bloomThreshold              = 15.0f;
        gu->bloomIntensity              = 1.0f;
        gu->bloomEmissionMultiplier     = 64.0f;
    }

    static_assert(
        QR_MEDIA_TYPE_VACUUM == MEDIA_TYPE_VACUUM &&
        QR_MEDIA_TYPE_WATER == MEDIA_TYPE_WATER &&
        QR_MEDIA_TYPE_GLASS == MEDIA_TYPE_GLASS &&
        QR_MEDIA_TYPE_ACID == MEDIA_TYPE_ACID,
        "Interface and GLSL constants must be identical" );

    if( drawInfo.pReflectRefractParams != nullptr )
    {
        const auto& rr = *drawInfo.pReflectRefractParams;

        if( rr.typeOfMediaAroundCamera >= 0 && rr.typeOfMediaAroundCamera < MEDIA_TYPE_COUNT )
        {
            gu->cameraMediaType = rr.typeOfMediaAroundCamera;
        }
        else
        {
            gu->cameraMediaType = MEDIA_TYPE_VACUUM;
        }

        gu->reflectRefractMaxDepth     = std::min( 8u, rr.maxReflectRefractDepth );

        gu->indexOfRefractionGlass = std::max( 0.0f, rr.indexOfRefractionGlass );
        gu->indexOfRefractionWater = std::max( 0.0f, rr.indexOfRefractionWater );

        memcpy( gu->waterColorAndDensity, rr.waterColor.data, 3 * sizeof( float ) );
        gu->waterColorAndDensity[ 3 ] = 0.0f;

        memcpy( gu->acidColorAndDensity, rr.acidColor.data, 3 * sizeof( float ) );
        gu->acidColorAndDensity[ 3 ] = rr.acidDensity;

        gu->forceNoWaterRefraction            = !!rr.forceNoWaterRefraction;
        gu->waterWaveSpeed                    = rr.waterWaveSpeed;
        gu->waterWaveStrength                 = rr.waterWaveNormalStrength;
        gu->turbWarpStrength                  = std::max( 0.0f, rr.turbWarpStrength );
        gu->waterTextureDerivativesMultiplier = std::max( 0.0f, rr.waterWaveTextureDerivativesMultiplier );
        if( rr.waterTextureAreaScale < 0.0001f )
        {
            gu->waterTextureAreaScale = 1.0f;
        }
        else
        {
            gu->waterTextureAreaScale = rr.waterTextureAreaScale;
        }

        gu->noBackfaceReflForNoMediaChange = !!rr.disableBackfaceReflectionsForNoMediaChange;
        // The shader glass-blur mode was removed from the interface: the raygen always runs the
        // normal-map path, and the CmGlassBlur pass only serves the optional denoiser.
        gu->glassBlur                      = false;
        gu->glassDenoise                   = !!rr.glassDenoise;

        // The master switch is the cvar alone. Every per-draw decision is the draw's own proxy
        // flag: a draw whose sprites or triangles were captured discards its raster copy behind a
        // pane (and has a traced stand-in), one that was not - an unsupported blend mode, a
        // per-frame cap, any other effect - keeps its raster copy. A frame that trips a cap must
        // not switch the whole feature off, or every effect flickers between the traced and the
        // raster look as the list crosses the bound.
        gu->glassParticles                 = !!rr.glassParticles;

        gu->twirlPortalNormal = !!rr.portalNormalTwirl;
    }
    else
    {
        gu->cameraMediaType        = MEDIA_TYPE_VACUUM;
        gu->reflectRefractMaxDepth = 2;

        gu->indexOfRefractionGlass = 1.52f;
        gu->indexOfRefractionWater = 1.33f;

        QR_SET_VEC3( gu->waterColorAndDensity, 0.3f, 0.73f, 0.63f );
        gu->waterColorAndDensity[ 3 ] = 0.0f;

        QR_SET_VEC3( gu->acidColorAndDensity, 0.0f, 0.66f, 0.55f );
        gu->acidColorAndDensity[ 3 ] = 10.0f;

        gu->forceNoWaterRefraction            = false;
        gu->waterWaveSpeed                    = 1.0f;
        gu->waterWaveStrength                 = 1.0f;
        gu->turbWarpStrength                  = 1.0f;
        gu->waterTextureDerivativesMultiplier = 1.0f;
        gu->waterTextureAreaScale             = 1.0f;

        // The fallbacks stand for the defaults a host would otherwise send, so
        // they are expressed in meters and converted through the frame the
        // same way SpatialFrame::ConvertDrawFrame does for a real draw.
        if( !spatialFrame.IsIdentity() )
        {
            for( int channel = 0; channel < 3; channel++ )
            {
                gu->waterColorAndDensity[ channel ] = spatialFrame.Transmittance( gu->waterColorAndDensity[ channel ] );
                gu->acidColorAndDensity[ channel ] = spatialFrame.Transmittance( gu->acidColorAndDensity[ channel ] );
            }

            gu->waterWaveSpeed        *= spatialFrame.Scale();
            gu->waterTextureAreaScale *= spatialFrame.Scale();
        }

        gu->noBackfaceReflForNoMediaChange = false;
        gu->glassBlur                      = false;
        gu->glassDenoise                   = false;
        gu->glassParticles                 = false;

        gu->twirlPortalNormal = false;
    }

    gu->rayCullBackFaces  = rayCullBackFacingTriangles ? 1 : 0;
    gu->rayLength         = clamp( drawInfo.rayLength, 0.0025f, ( float )MAX_RAY_LENGTH );
    gu->primaryRayMinDist = clamp( drawInfo.cameraNear, 2.5e-5f, gu->rayLength );

    {
        if( !smokeWindDirectionSet )
        {
            gu->smokeWindX = 0.45f;
            gu->smokeWindY = 0.20f;
            gu->smokeWindZ = 0.87f;
        }
        else
        {
            const QrFloat3D wind = spatialFrame.Direction( smokeWindDirection );
            const float windLength = std::sqrt( wind.data[ 0 ] * wind.data[ 0 ] +
                                                wind.data[ 1 ] * wind.data[ 1 ] +
                                                wind.data[ 2 ] * wind.data[ 2 ] );

            if( windLength > 0.000001f )
            {
                gu->smokeWindX = wind.data[ 0 ] / windLength;
                gu->smokeWindY = wind.data[ 1 ] / windLength;
                gu->smokeWindZ = wind.data[ 2 ] / windLength;
            }
            else
            {
                gu->smokeWindX = 0.45f;
                gu->smokeWindY = 0.20f;
                gu->smokeWindZ = 0.87f;
            }
        }
    }

    {
        gu->rayCullMaskWorld = 0;

        if( drawInfo.rayCullMaskWorld & QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT )
        {
            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_0;
        }

        if( drawInfo.rayCullMaskWorld & QR_DRAW_FRAME_RAY_CULL_WORLD_1_BIT )
        {
            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_1;
        }

        if( drawInfo.rayCullMaskWorld & QR_DRAW_FRAME_RAY_CULL_WORLD_2_BIT )
        {
            if( allowGeometryWithSkyFlag )
            {
                throw QrException( QR_WRONG_ARGUMENT, "QR_DRAW_FRAME_RAY_CULL_WORLD_2_BIT cannot be used, as QrInstanceCreateInfo::allowGeometryWithSkyFlag was true" );
            }

            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_2;
        }

#if RAYCULLMASK_SKY_IS_WORLD2
        if( drawInfo.rayCullMaskWorld & QR_DRAW_FRAME_RAY_CULL_SKY_BIT )
        {
            if( !allowGeometryWithSkyFlag )
            {
                throw QrException( QR_WRONG_ARGUMENT, "QR_DRAW_FRAME_RAY_CULL_SKY_BIT cannot be used, as QrInstanceCreateInfo::allowGeometryWithSkyFlag was false" );
            }

            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_2;
        }
#else
    #error Handle QR_DRAW_FRAME_RAY_CULL_SKY_BIT, if there is no WORLD_2
#endif

        if( allowGeometryWithSkyFlag )
        {
            gu->rayCullMaskWorld_Shadow = ( gu->rayCullMaskWorld & ( ~INSTANCE_MASK_WORLD_2 ) );
        }
        else
        {
            gu->rayCullMaskWorld_Shadow = gu->rayCullMaskWorld;
        }

        // A pane joins shadow rays through its own bit: the light that crosses it
        // is tinted by it and, with depth, leaves its far face bent. Switched off,
        // panes are not on the shadow rays at all and the light passes untinted.
        if( ( drawInfo.pReflectRefractParams == nullptr ) || ( drawInfo.pReflectRefractParams->glassShadows != 0 ) )
        {
            gu->rayCullMaskWorld_Shadow |= INSTANCE_MASK_GLASS;
        }
    }

    gu->waterNormalTextureIndex = textureManager->GetWaterNormalTextureIndex();

    gu->sceneUnitsPerHostUnit = std::max( spatialFrame.Scale(), 1e-6f );

    gu->cameraRayConeSpreadAngle = atanf( ( 2.0f * tanf( drawInfo.fovYRadians * 0.5f ) ) / ( float )renderResolution.Height() );

    if( Utils::IsAlmostZero( drawInfo.worldUpVector ) )
    {
        gu->worldUpVector[ 0 ] = 0.0f;
        gu->worldUpVector[ 1 ] = 1.0f;
        gu->worldUpVector[ 2 ] = 0.0f;
    }
    else
    {
        gu->worldUpVector[ 0 ] = drawInfo.worldUpVector.data[ 0 ];
        gu->worldUpVector[ 1 ] = drawInfo.worldUpVector.data[ 1 ];
        gu->worldUpVector[ 2 ] = drawInfo.worldUpVector.data[ 2 ];
    }

    {
        gu->volumeCameraNear = std::max( drawInfo.cameraNear, 2.5e-5f );
        gu->volumeCameraFar  = std::min(
            drawInfo.cameraFar,
            drawInfo.pVolumetricParams ? drawInfo.pVolumetricParams->volumetricFar : 2.5f );

        if( drawInfo.pVolumetricParams )
        {
            if( drawInfo.pVolumetricParams->enable )
            {
                gu->volumeEnableType = drawInfo.pVolumetricParams->useSimpleDepthBased
                                           ? VOLUME_ENABLE_SIMPLE
                                           : VOLUME_ENABLE_VOLUMETRIC;
            }
            else
            {
                gu->volumeEnableType = VOLUME_ENABLE_NONE;
            }
            gu->volumeScattering = drawInfo.pVolumetricParams->scaterring;
            gu->volumeSourceAsymmetry = std::clamp( drawInfo.pVolumetricParams->sourceAssymetry, -1.0f, 1.0f );

            QR_SET_VEC3_A( gu->volumeAmbient, drawInfo.pVolumetricParams->ambientColor.data );
            QR_MAX_VEC3( gu->volumeAmbient, 0.0f );

            QR_SET_VEC3_A( gu->volumeSourceColor, drawInfo.pVolumetricParams->sourceColor.data );
            QR_MAX_VEC3( gu->volumeSourceColor, 0.0f );

            QR_SET_VEC3_A( gu->volumeDirToSource, drawInfo.pVolumetricParams->sourceDirection.data );
            Utils::Negate( gu->volumeDirToSource );
            Utils::Normalize( gu->volumeDirToSource );
        }
        else
        {
            gu->volumeEnableType      = VOLUME_ENABLE_VOLUMETRIC;
            gu->volumeScattering      = 0.2f;
            gu->volumeSourceAsymmetry = 0.4f;
            QR_SET_VEC3( gu->volumeAmbient, 0.8f, 0.85f, 1.0f );
            QR_SET_VEC3( gu->volumeSourceColor, 0, 0, 0 );
            QR_SET_VEC3( gu->volumeDirToSource, 0, 1, 0 );
        }

        if( gu->volumeEnableType != VOLUME_ENABLE_NONE )
        {
            memcpy( gu->volumeViewProj_Prev, gu->volumeViewProj, 16 * sizeof( float ) );
            memcpy( gu->volumeViewProjInv_Prev, gu->volumeViewProjInv, 16 * sizeof( float ) );

            float volumeproj[ 16 ];
            Matrix::MakeProjectionMatrix( volumeproj,
                                          aspect,
                                          drawInfo.fovYRadians,
                                          gu->volumeCameraNear,
                                          gu->volumeCameraFar );

            Matrix::Multiply( gu->volumeViewProj, gu->view, volumeproj );
            Matrix::Inverse( gu->volumeViewProjInv, gu->volumeViewProj );
        }
    }

    gu->antiFireflyEnabled = !!drawInfo.forceAntiFirefly;

    if( drawInfo.pLevelFogParams != nullptr )
    {
        QR_SET_VEC3_A( gu->levelFogColorDensity, drawInfo.pLevelFogParams->color.data );
        QR_MAX_VEC3( gu->levelFogColorDensity, 0.0f );
        gu->levelFogColorDensity[ 3 ] = std::max( drawInfo.pLevelFogParams->density, 0.0f );

        QR_SET_VEC3( gu->levelFogSkyBlend, 0.0f, 0.0f, 0.0f );
        gu->levelFogSkyBlend[ 0 ] = std::clamp( drawInfo.pLevelFogParams->skyBlend, 0.0f, 1.0f );
    }
    else
    {
        QR_SET_VEC3( gu->levelFogColorDensity, 0.0f, 0.0f, 0.0f );
        gu->levelFogColorDensity[ 3 ] = 0.0f;

        QR_SET_VEC3( gu->levelFogSkyBlend, 0.0f, 0.0f, 0.0f );
    }

    {
        for (uint32_t i = 0; i < MAX_FOG_VOLUMES; i++)
        {
            gu->fogIsActive[i * 4] = 0;
        }

        for (uint32_t i = 0; i < fogVolumeCount; i++)
        {
            const QrFogVolume &v = fogVolumes[i];

            if (v.halfExtinctionDistance <= 0.0f ||
                v.pointA.data[0] == v.pointB.data[0] ||
                v.pointA.data[1] == v.pointB.data[1] ||
                v.pointA.data[2] == v.pointB.data[2])
            {
                continue;
            }

            const float *pa = v.pointA.data;
            const float *pb = v.pointB.data;

            for (int axis = 0; axis < 3; axis++)
            {
                gu->fogMins[i * 4 + axis] = std::min(pa[axis], pb[axis]);
                gu->fogMaxs[i * 4 + axis] = std::max(pa[axis], pb[axis]);
            }
            gu->fogMins[i * 4 + 3] = 0.0f;
            gu->fogMaxs[i * 4 + 3] = 0.0f;

            gu->fogColor[i * 4 + 0] = v.color.data[0];
            gu->fogColor[i * 4 + 1] = v.color.data[1];
            gu->fogColor[i * 4 + 2] = v.color.data[2];
            gu->fogColor[i * 4 + 3] = 0.0f;

            const float density = 0.69315f / v.halfExtinctionDistance;

            gu->fogDensity[i * 4 + 0] = 0.0f;
            gu->fogDensity[i * 4 + 1] = 0.0f;
            gu->fogDensity[i * 4 + 2] = 0.0f;

            if (1 <= v.softface && v.softface <= 6)
            {
                const int axis = (v.softface - 1) / 2;
                const float pos0 = (v.softface & 1) ? pa[axis] : pb[axis];
                const float pos1 = (v.softface & 1) ? pb[axis] : pa[axis];
                const float a = density / (pos1 - pos0);
                const float b = -pos0 * a;
                gu->fogDensity[i * 4 + axis] = a;
                gu->fogDensity[i * 4 + 3] = b;
            }
            else
            {
                gu->fogDensity[i * 4 + 3] = density;
            }

            gu->fogIsActive[i * 4] = 1;
        }
    }
}

void VulkanDevice::RequestScreenshot(const char *pFilePath)
{
    if (pFilePath != nullptr && pFilePath[0] != '\0')
    {
        pendingScreenshotPath = pFilePath;
    }
}

QrResult VulkanDevice::GetFrameReadbackRequirements(QrFrameReadbackFormat format, QrBool32 includeUi,
                                                     uint32_t *pWidth, uint32_t *pHeight,
                                                     uint32_t *pRowPitch, uint64_t *pBufferSize) const
{
    if (pWidth == nullptr || pHeight == nullptr || pRowPitch == nullptr || pBufferSize == nullptr)
    {
        return QR_WRONG_ARGUMENT;
    }

    if (nvrhiFrameSkeleton == nullptr || nvrhiFrameSkeleton->IsUnavailable())
    {
        return QR_FEATURE_UNAVAILABLE;
    }

    return nvrhiFrameSkeleton->GetFrameReadbackRequirements(format, includeUi, *pWidth, *pHeight,
                                                            *pRowPitch, *pBufferSize);
}

QrResult VulkanDevice::RequestFrameReadback(const QrFrameReadbackInfo *pInfo, uint64_t *pRequestId)
{
    if (pInfo == nullptr || pRequestId == nullptr)
    {
        return QR_WRONG_ARGUMENT;
    }

    if (nvrhiFrameSkeleton == nullptr || nvrhiFrameSkeleton->IsUnavailable())
    {
        return QR_FEATURE_UNAVAILABLE;
    }

    return nvrhiFrameSkeleton->RequestFrameReadback(currentFrameState.GetFrameIndex(), *pInfo,
                                                     *pRequestId);
}

QrResult VulkanDevice::GetFrameReadbackStatus(uint64_t requestId, QrBool32 *pDone) const
{
    if (pDone == nullptr)
    {
        return QR_WRONG_ARGUMENT;
    }

    if (nvrhiFrameSkeleton == nullptr || nvrhiFrameSkeleton->IsUnavailable())
    {
        return QR_FEATURE_UNAVAILABLE;
    }

    return nvrhiFrameSkeleton->GetFrameReadbackStatus(requestId, *pDone);
}

bool VulkanDevice::RenderThroughRhi(const QrDrawFrameInfo &drawInfo)
{
    if (nvrhiFrameSkeleton == nullptr || nvrhiFrameSkeleton->IsUnavailable())
    {
        throw QrException(QR_GRAPHICS_API_ERROR,
                          "RHI: the frame skeleton is unavailable, the frame cannot be drawn");
    }

    const uint32_t frameIndex = currentFrameState.GetFrameIndex();

    CpuFrameProfiler *cpuProfiler = cpuFrameProfiler.IsEnabled() ? &cpuFrameProfiler : nullptr;
    CpuProfileScope prepareInputs(cpuProfiler, QR_CPU_PASS_PREPARE);
    framebuffers->PrepareForSize(renderResolution.GetResolutionState());

    const ShGlobalUniform *globalUniform = uniform->GetData();
    const QrFloat3D skyViewerPosition =
        drawInfo.pSkyParams ? drawInfo.pSkyParams->skyViewerPosition : QrFloat3D{ 0, 0, 0 };

    const std::vector<RasterizedDataCollector::DrawInfo> &skyDraws =
        rasterizedDataCollector->GetSkyDrawInfos();

    const std::vector<RasterizedDataCollector::DrawInfo> &worldDraws =
        rasterizedDataCollector->GetRasterDrawInfos();

    smokeDraws.clear();
    for (const RasterizedDataCollector::DrawInfo &info : worldDraws)
    {
        if ((info.pipelineState & QR_RASTERIZED_GEOMETRY_STATE_SMOKE) != 0)
        {
            smokeDraws.push_back(info);
        }
    }

    particleDraws.clear();
    for (const RasterizedDataCollector::DrawInfo &info : worldDraws)
    {
        if ((info.pipelineState & QR_RASTERIZED_GEOMETRY_STATE_PARTICLE) != 0)
        {
            particleDraws.push_back(info);
        }
    }

    const std::vector<RasterizedDataCollector::ParticlePointDrawInfo> &particlePointDraws =
        rasterizedDataCollector->GetParticlePointDrawInfos();

    const std::vector<RasterizedDataCollector::DrawInfo> &swapchainDraws =
        rasterizedDataCollector->GetSwapchainDrawInfos();

    const std::shared_ptr<ASManager> &asManager = scene->GetASManager();
    ShVertPreprocessing preprocessing = {};
    prepareInputs.Finish();
    if (!drawInfo.renderUiOnly)
    {
        CpuProfileScope legacyAs(cpuProfiler, QR_CPU_PASS_LEGACY_AS);
        const auto prepare = asManager->PrepareForBuildingTLAS(
            frameIndex, *uniform->GetData(), uniform->GetData()->rayCullMaskWorld,
            allowGeometryWithSkyFlag, drawInfo.disableRayTracedGeometry);
        preprocessing = prepare.second;
    }

    CpuProfileScope fillInputs(cpuProfiler, QR_CPU_PASS_PREPARE);

    NvrhiFrameSkeleton::SkyFrameInputs sky = {};
    sky.renderUiOnly = drawInfo.renderUiOnly != 0;
    sky.cpuProfiler = cpuProfiler;
    sky.framebuffers = framebuffers.get();
    sky.draws = skyDraws.data();
    sky.drawCount = static_cast<uint32_t>(skyDraws.size());
    sky.width = renderResolution.Width();
    sky.height = renderResolution.Height();
    sky.upscaledWidth = renderResolution.UpscaledWidth();
    sky.upscaledHeight = renderResolution.UpscaledHeight();
    memcpy(sky.view, globalUniform->view, sizeof(sky.view));
    memcpy(sky.projection, globalUniform->projection, sizeof(sky.projection));
    sky.jitter[0] = globalUniform->jitterX;
    sky.jitter[1] = globalUniform->jitterY;
    memcpy(sky.skyViewerPos, skyViewerPosition.data, sizeof(sky.skyViewerPos));
    sky.applyVertexColorGamma = rasterizedVertexColorGamma;
    memcpy(sky.skyFaceViewProj, globalUniform->viewProjCubemap, sizeof(sky.skyFaceViewProj));
    sky.worldDraws = worldDraws.data();
    sky.worldDrawCount = static_cast<uint32_t>(worldDraws.size());
    sky.smokeDraws = smokeDraws.data();
    sky.smokeDrawCount = static_cast<uint32_t>(smokeDraws.size());
    sky.particleDraws = particleDraws.data();
    sky.particleDrawCount = static_cast<uint32_t>(particleDraws.size());
    sky.particlePointDraws = particlePointDraws.data();
    sky.particlePointDrawCount = static_cast<uint32_t>(particlePointDraws.size());
    sky.particlePointGeometry = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
        rasterizedDataCollector->GetParticlePointStagingBuffer(frameIndex)));

    const std::vector<ParticleProxy> &particleProxies =
        rasterizedDataCollector->GetParticleProxies();
    sky.particleProxies = particleProxies.empty() ? nullptr : particleProxies.data();
    sky.particleProxyCount = static_cast<uint32_t>(particleProxies.size());

    sky.swapchainDraws = swapchainDraws.data();
    sky.swapchainDrawCount = static_cast<uint32_t>(swapchainDraws.size());
    sky.swapchainVertexStaging = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
        rasterizedDataCollector->GetVertexStagingBuffer(frameIndex)));
    sky.swapchainIndexStaging = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
        rasterizedDataCollector->GetIndexStagingBuffer(frameIndex)));
    sky.swapchainVertexStagingSize = rasterizedDataCollector->GetVertexBufferSize();
    sky.swapchainIndexStagingSize = rasterizedDataCollector->GetIndexBufferSize();
    sky.disableRasterization = drawInfo.disableRasterization;
    sky.uniform = uniform;
    sky.tonemapping = tonemapping.get();
    sky.disableEyeAdaptation = drawInfo.disableEyeAdaptation != 0;
    if (drawInfo.pTonemappingParams != nullptr)
    {
        sky.exposureBias = drawInfo.pTonemappingParams->exposureBias;
        sky.tonemapPower = std::clamp(drawInfo.pTonemappingParams->tonemapPower, 0.0f, 1.0f);
        sky.tonemapType = std::min(drawInfo.pTonemappingParams->tonemapType, 4u);
        sky.exposureParams = *drawInfo.pTonemappingParams;
    }
    sky.rayCullMaskWorld = uniform->GetData()->rayCullMaskWorld;
    sky.allowGeometryWithSkyFlag = allowGeometryWithSkyFlag;
    sky.disableRayTracedGeometry = drawInfo.disableRayTracedGeometry;

    if (!drawInfo.renderUiOnly)
    {
        const float godRaysIntensity = (drawInfo.pSkyParams == nullptr)
            ? 1.0f : std::max(drawInfo.pSkyParams->godRaysIntensity, 0.0f);
        const bool godRaysEnabled =
            ((drawInfo.pSkyParams == nullptr) || (drawInfo.pSkyParams->godRaysEnabled != 0)) &&
            (godRaysIntensity > 0.0f);

        float sunColor[3] = {}, sunDir[3] = {}, sunAngularRadius = 0.0047f;
        const bool sunExists =
            scene->GetLightManager()->GetLastDirectionalLight(sunColor, sunDir, &sunAngularRadius);

        const bool useSkyBrightest =
            (drawInfo.pSkyParams != nullptr) &&
            (drawInfo.pSkyParams->skyType == QR_SKY_TYPE_RASTERIZED_GEOMETRY) &&
            (drawInfo.pSkyParams->godRaysFromSkyTexture != 0);

        const bool sunShaftsOn = godRaysEnabled && (sunExists || useSkyBrightest);
        const bool lightShaftsEnabled = drawInfo.pSkyParams != nullptr &&
                                        drawInfo.pSkyParams->lightShaftsEnabled != 0;
        LightManager::LightShaft lightShafts[LightManager::MAX_LIGHT_SHAFTS] = {};
        const uint32_t lightShaftCount = lightShaftsEnabled
            ? scene->GetLightManager()->CollectLightShafts(lightShafts, LightManager::MAX_LIGHT_SHAFTS)
            : 0;
        const bool lightShaftsOn = lightShaftCount > 0;
        const bool godRaysOn = sunShaftsOn || lightShaftsOn;

        static constexpr float kGodRaysRadianceScale = 500.0f;
        const float lightShaftIntensity = (drawInfo.pSkyParams != nullptr)
            ? std::max(drawInfo.pSkyParams->lightShaftsIntensity, 0.0f) : 1.0f;

        sky.godRays.enabled = godRaysOn;
        sky.godRays.intensity = kGodRaysRadianceScale * godRaysIntensity;
        sky.godRays.eccentricity = 0.75f;
        {
            static constexpr float stepScales[5] = {2.0f, 1.5f, 1.0f, 0.75f, 0.5f};
            const uint32_t quality = (drawInfo.pSkyParams != nullptr)
                ? std::min(drawInfo.pSkyParams->godRaysQuality, 4u) : 2u;
            sky.godRays.sunStepScale = stepScales[quality];
        }
        sky.godRays.lightShaftCount = lightShaftCount;
        sky.godRays.lightShaftIntensity = lightShaftIntensity;
        sky.godRays.lightShaftSteps = (drawInfo.pSkyParams != nullptr)
            ? std::min(16u, 8u + std::min(drawInfo.pSkyParams->lightShaftsResolution, 2u) * 4u) : 12u;

        if (godRaysOn)
        {
            for (int k = 0; k < 3; k++)
            {
                if (sunShaftsOn && useSkyBrightest)
                {
                    sky.godRays.shadowLightDirection[k] = -drawInfo.pSkyParams->godRaysSkyDirection.data[k];
                    sky.godRays.sunDirection[k] = drawInfo.pSkyParams->godRaysSkyDirection.data[k];
                    sky.godRays.sunColor[k] = drawInfo.pSkyParams->godRaysSkyColor.data[k];
                }
                else if (sunShaftsOn)
                {
                    sky.godRays.shadowLightDirection[k] = sunDir[k];
                    sky.godRays.sunDirection[k] = -sunDir[k];
                    sky.godRays.sunColor[k] = sunColor[k];
                }
            }
        }

        if (!sunShaftsOn && lightShaftsOn)
        {
            const bool validDirection =
                lightShafts[0].direction[0] * lightShafts[0].direction[0] +
                lightShafts[0].direction[1] * lightShafts[0].direction[1] +
                lightShafts[0].direction[2] * lightShafts[0].direction[2] > 0.0001f;

            for (int k = 0; k < 3; k++)
            {
                sky.godRays.shadowLightDirection[k] =
                    validDirection ? lightShafts[0].direction[k] : (k == 2 ? 1.0f : 0.0f);
            }
        }

        for (uint32_t i = 0; i < lightShaftCount; i++)
        {
            memcpy (sky.godRays.lightShafts[i].position, lightShafts[i].position, sizeof (lightShafts[i].position));
            memcpy (sky.godRays.lightShafts[i].direction, lightShafts[i].direction, sizeof (lightShafts[i].direction));
            memcpy (sky.godRays.lightShafts[i].color, lightShafts[i].color, sizeof (lightShafts[i].color));
            memcpy (sky.godRays.lightShafts[i].cone, lightShafts[i].cone, sizeof (lightShafts[i].cone));
            memcpy (sky.godRays.lightShafts[i].tangentU, lightShafts[i].tangentU, sizeof (lightShafts[i].tangentU));
            memcpy (sky.godRays.lightShafts[i].tangentV, lightShafts[i].tangentV, sizeof (lightShafts[i].tangentV));
        }

        sky.godRays.hasAabb = scene->HasAABB();
        if (sky.godRays.hasAabb)
        {
            float aabbMin[3], aabbMax[3];
            scene->GetAABB(aabbMin, aabbMax);

            for (int k = 0; k < 3; k++)
            {
                const float halfSize = std::max((aabbMax[k] - aabbMin[k]) * 0.5f, 0.025f);
                sky.godRays.aabbMin[k] = aabbMin[k];
                sky.godRays.aabbMax[k] = aabbMax[k];
                sky.godRays.worldCenter[k] = (aabbMin[k] + aabbMax[k]) * 0.5f;
                sky.godRays.worldHalfSizeInv[k] = 1.0f / halfSize;
            }

            sky.godRays.staticCollector = scene->GetASManager()->GetStaticCollector().get();
            sky.godRays.dynamicCollector = scene->GetASManager()->GetDynamicCollector(frameIndex).get();
        }
    }

    sky.portalStaging = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(portalList->GetStagingBuffer(frameIndex)));
    sky.portalDevice = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(portalList->GetDeviceLocalBuffer()));
    sky.portalSize = static_cast<uint64_t>(portalList->GetBufferSize());
    portalList->ResetUploads();

    sky.decalStaging = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(decalManager->GetStagingBuffer(frameIndex)));
    sky.decalDevice = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(decalManager->GetDeviceLocalBuffer()));
    sky.decalBufferSize = static_cast<uint64_t>(decalManager->GetBufferSize());
    sky.decalCopySize = static_cast<uint64_t>(decalManager->GetCopySize());
    sky.decalCount = decalManager->GetDecalCount();

    sky.renderResolution = &renderResolution;
    sky.cameraNear = drawInfo.cameraNear;
    sky.cameraFar = drawInfo.cameraFar;
    sky.fovYRadians = drawInfo.fovYRadians;

    sky.postEffectParams = drawInfo.postEffectParams;
    sky.postEffectFrameId = frameId;

    if (!drawInfo.renderUiOnly)
    {
        RhiProceduralSkyPass::Params p = {};

        p.skyTint[0] = globalUniform->skyColorDefault[0];
        p.skyTint[1] = globalUniform->skyColorDefault[1];
        p.skyTint[2] = globalUniform->skyColorDefault[2];

        p.sunDiscColor[0] = p.sunDiscColor[1] = p.sunDiscColor[2] = 1.0f;
        if (drawInfo.pSkyParams)
        {
            p.sunDiscColor[0] = drawInfo.pSkyParams->sunDiscColor.data[0];
            p.sunDiscColor[1] = drawInfo.pSkyParams->sunDiscColor.data[1];
            p.sunDiscColor[2] = drawInfo.pSkyParams->sunDiscColor.data[2];
        }

        float sunColor[3], sunDir[3], sunAngularRadius = 0.0047f;
        const bool hasSun = scene->GetLightManager()->GetLastDirectionalLight(sunColor, sunDir, &sunAngularRadius);
        p.sunDirection[3] = hasSun ? 1.0f : 0.0f;
        if (hasSun)
        {
            p.sunDirection[0] = -sunDir[0];
            p.sunDirection[1] = -sunDir[1];
            p.sunDirection[2] = -sunDir[2];
        }
        else
        {
            float d[3] = { 0.3f, 0.5f, 0.8f };
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            p.sunDirection[0] = d[0] / len;
            p.sunDirection[1] = d[1] / len;
            p.sunDirection[2] = d[2] / len;
        }
        p.skyParams[0] = globalUniform->skyColorMultiplier;
        p.skyParams[1] = globalUniform->skyColorSaturation;
        p.skyParams[2] = 30.0f;
        float sunDiscSize = drawInfo.pSkyParams != nullptr ? drawInfo.pSkyParams->sunDiscSize : 1.0f;
        sunDiscSize = std::isfinite(sunDiscSize) ? std::clamp(sunDiscSize, 0.0f, 10.0f) : 1.0f;
        p.skyParams[3] = 0.025f * sunDiscSize;

        const uint32_t cloudsQuality = drawInfo.pSkyParams != nullptr
            ? std::min(drawInfo.pSkyParams->skyCloudsQuality, uint32_t(QR_SKY_CLOUDS_MAX_QUALITY))
            : 2;
        float cloudAltitude = 3500.0f;
        float cloudThickness = 2250.0f;

        p.cloudColor[3] = globalUniform->time;
        if (drawInfo.pSkyParams)
        {
            const float *c = &drawInfo.pSkyParams->skyCubemapRotationTransform.matrix[0][0];
            p.cloudColor[0] = c[0];
            p.cloudColor[1] = c[1];
            p.cloudColor[2] = c[2];
            p.cloudParams[0] = c[3];
            p.cloudParams[1] = c[4];
            p.cloudParams[2] = c[5];
            p.cloudParams[3] = c[6];

            if (c[7] > 0.0f)
            {
                cloudAltitude = c[7];
            }
            if (c[8] > 0.0f)
            {
                cloudThickness = c[8];
            }
        }

        p.skyTint[3] = 0.0f;
        const float wind = RhiCloudsPass::GetWindSpeed(p.cloudParams[2], cloudAltitude);
        p.cloudParams[2] = wind;

        constexpr float PI = 3.14159265358979323846f;
        const float faceAngles[6][2] = {
            { 0.0f,        PI / 2.0f },
            { 0.0f,       -PI / 2.0f },
            { -PI / 2.0f, 0.0f       },
            {  PI / 2.0f, 0.0f       },
            { 0.0f,        0.0f      },
            { 0.0f,        PI        },
        };

        float view[16];
        const float origin[3] = { 0.0f, 0.0f, 0.0f };
        for (uint32_t face = 0; face < 6; face++)
        {
            Matrix::GetViewMatrix(view, origin, faceAngles[face][0], faceAngles[face][1], 0.0f);

            p.faceBasis[face * 3 + 0][0] = view[0];  p.faceBasis[face * 3 + 0][1] = view[4];  p.faceBasis[face * 3 + 0][2] = view[8];
            p.faceBasis[face * 3 + 1][0] = view[1];  p.faceBasis[face * 3 + 1][1] = view[5];  p.faceBasis[face * 3 + 1][2] = view[9];
            p.faceBasis[face * 3 + 2][0] = view[2];  p.faceBasis[face * 3 + 2][1] = view[6];  p.faceBasis[face * 3 + 2][2] = view[10];
        }

        RhiCloudsPass::LayerParams clouds = {};
        static_assert(offsetof(RhiCloudsPass::LayerParams, cloudLayer) == sizeof(RhiProceduralSkyPass::Params),
                      "the layer params must start with the procedural sky params");
        memcpy(&clouds, &p, sizeof(p));

        if (hasSun)
        {
            clouds.sunDiscColor[0] = sunColor[0];
            clouds.sunDiscColor[1] = sunColor[1];
            clouds.sunDiscColor[2] = sunColor[2];
        }

        clouds.cloudParams[2] = wind;

        clouds.cloudLayer[0] = cloudAltitude;
        clouds.cloudLayer[1] = cloudThickness;
        clouds.cloudLayer[2] = 1.0f;
        clouds.cloudLayer[3] = 1.0f;
        clouds.cloudMarch[0] = float(RhiCloudsPass::GetViewSteps(cloudsQuality));
        clouds.cloudMarch[2] = 0.35f;
        clouds.cloudMarch[3] = 0.75f;
        clouds.cloudAnchor[0] = globalUniform->cameraPosition[0];
        clouds.cloudAnchor[1] = globalUniform->cameraPosition[1];
        clouds.cloudAnchor[2] = globalUniform->cameraPosition[2];
        sky.cloudsParams = clouds;

        RhiCloudsPass::ShadowParams cloudsShadow = {};
        cloudsShadow.sunDirection[0] = p.sunDirection[0];
        cloudsShadow.sunDirection[1] = p.sunDirection[1];
        cloudsShadow.sunDirection[2] = p.sunDirection[2];
        cloudsShadow.sunDirection[3] = cloudAltitude;
        cloudsShadow.cloudLayer[0] = cloudThickness;
        cloudsShadow.cloudLayer[1] = p.cloudParams[0];
        cloudsShadow.cloudLayer[2] = p.cloudParams[1];
        cloudsShadow.cloudLayer[3] = clouds.cloudMarch[2];
        cloudsShadow.cloudMarch[0] = p.cloudColor[3];
        cloudsShadow.cloudMarch[1] = wind;
        sky.cloudsShadowParams = cloudsShadow;

        sky.cloudsLayer = p.cloudParams[3] > 0.5f && p.skyParams[1] > 0.0f;
        sky.cloudsQuality = cloudsQuality;

        auto *cloudUniform = uniform->GetData();
        const bool cloudsEnabled = globalUniform->skyType == SKY_TYPE_PROCEDURAL &&
                                   p.cloudParams[3] > 0.5f && p.skyParams[1] > 0.0f;
        const bool volumeEnabled = cloudsEnabled &&
                                   rhiCloudsPass != nullptr && rhiCloudsPass->IsCreated() &&
                                   rhiProceduralSkyPass != nullptr && rhiProceduralSkyPass->IsCreated();
        cloudUniform->cloudLayerMotion[0] = cloudsEnabled ? p.cloudParams[2] : 0.0f;
        cloudUniform->cloudLayerMotion[1] = volumeEnabled ? cloudAltitude : 0.0f;
        cloudUniform->cloudLayerMotion[2] = volumeEnabled ? cloudThickness : 0.0f;
        cloudUniform->cloudLayerMotion[3] = cloudsEnabled ? std::clamp(p.skyParams[1], 0.0f, 1.0f) : 0.0f;
        const auto placement = RhiCloudsPass::MakeShadowPlacement(clouds);
        memcpy(cloudUniform->cloudShadowPlacement, placement.data(), sizeof(cloudUniform->cloudShadowPlacement));
        if (!volumeEnabled)
        {
            cloudUniform->cloudShadowPlacement[0] = 0.0f;
        }

        sky.proceduralSkyParams = p;
    }

    if (!pendingScreenshotPath.empty())
    {
        nvrhiFrameSkeleton->RequestScreenshot(pendingScreenshotPath);
        pendingScreenshotPath.clear();
    }

    FrameSubmissionSynchronization submission;
    const auto acquireForSubmission = [&] {
        AcquireFrame();

        submission.semaphoreWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        submission.semaphoreToWait =
            currentFrameState.GetSemaphoreForWaitAndRemove(&submission.semaphoreWaitStage);
        submission.semaphoreToSignal =
            swapchain->GetRenderFinishedSemaphore(swapchain->GetCurrentImageIndex());

        assert(submission.semaphoreToWait != VK_NULL_HANDLE);
        assert(submission.semaphoreToSignal != VK_NULL_HANDLE);

        if (cpuProfiler != nullptr)
        {
            cpuProfiler->Add(QR_CPU_PASS_FRAME_FENCE_WAIT, pendingBeginFrameCpuTimings.frameFenceWaitMs);
            cpuProfiler->Add(QR_CPU_PASS_PRESENT_WAIT, pendingBeginFrameCpuTimings.presentWaitMs);
            cpuProfiler->Add(QR_CPU_PASS_IMAGE_ACQUIRE, pendingBeginFrameCpuTimings.imageAcquireMs);
            cpuProfiler->Add(QR_CPU_PASS_BEGIN_OTHER, pendingBeginFrameCpuTimings.beginOtherMs);
        }
    };

    fillInputs.Finish();
    if (gpuFrameHooks.pfnCallback != nullptr)
    {
        acquireForSubmission();
        sky.gpuFrameCallback = [this](QrayGpuFramePhase phase, nvrhi::ICommandList *pCommandList,
                                      nvrhi::ITexture *pOutputColor, nvrhi::ITexture *pBackBuffer,
                                      uint32_t frameIndex) {
            if (inGpuFrameCallback)
            {
                Print("GPU frame callback re-entry is not allowed\n");
                return;
            }

            QrayGpuFrameCallbackInfo info = {};
            info.header = { sizeof(QrayGpuFrameCallbackInfo), kGpuContextContractVersion };
            info.phase = phase;
            info.frameIndex = frameIndex;
            info.pCommandList = pCommandList;
            info.pOutputColor = pOutputColor;
            info.pBackBuffer = pBackBuffer;

            inGpuFrameCallback = true;
            try
            {
                gpuFrameHooks.pfnCallback(&info, gpuFrameHooks.pUserData);
            }
            catch (...)
            {
                Print("GPU frame callback threw an exception; the frame continues after the recorded host commands\n");
            }
            inGpuFrameCallback = false;
        };
    }
    else
    {
        sky.acquireSwapchainForPresent = [&acquireForSubmission] {
            acquireForSubmission();
            return true;
        };
    }

    if (!nvrhiFrameSkeleton->Render(swapchain.get(), frameIndex, sky, submission))
    {
        if (submission.semaphoreToWait != VK_NULL_HANDLE)
        {
            currentFrameState.SetSemaphore(submission.semaphoreToWait, submission.semaphoreWaitStage);
        }
        return false;
    }

    if (!drawInfo.renderUiOnly)
    {
        CpuProfileScope legacyAs(cpuProfiler, QR_CPU_PASS_LEGACY_AS);
        scene->PreprocessVertices(currentFrameState.GetCmdBuffer(), frameIndex, uniform, preprocessing);
    }

    {
        CpuProfileScope submit(cpuProfiler, QR_CPU_PASS_LEGACY_SUBMIT);
        cmdManager->Submit(currentFrameState.GetCmdBuffer(), frameFences[frameIndex]);
    }

    {
        CpuProfileScope present(cpuProfiler, QR_CPU_PASS_PRESENT);
        const SwapchainPresentTimings presentTimings =
            swapchain->Present(queues, submission.semaphoreToSignal);
        if (cpuProfiler != nullptr)
        {
            cpuProfiler->Add(QR_CPU_PASS_PRESENT_FENCE_WAIT, presentTimings.fenceWaitMs);
        }
    }

    frameId++;
    return true;
}

void VulkanDevice::EndFrame(VkCommandBuffer cmd)
{
    AcquireFrame();

    CpuFrameProfiler *cpuProfiler = cpuFrameProfiler.IsEnabled() ? &cpuFrameProfiler : nullptr;
    uint32_t frameIndex = currentFrameState.GetFrameIndex();
    VkPipelineStageFlags semaphoreWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSemaphore semaphoreToWait = currentFrameState.GetSemaphoreForWaitAndRemove(&semaphoreWaitStage);

    const VkSemaphore renderFinishedSemaphore =
        swapchain->GetRenderFinishedSemaphore(swapchain->GetCurrentImageIndex());

    {
        CpuProfileScope submit(cpuProfiler, QR_CPU_PASS_LEGACY_SUBMIT);
        cmdManager->Submit(
            cmd,
            semaphoreToWait,
            semaphoreWaitStage,
            renderFinishedSemaphore,
            frameFences[frameIndex]);
    }

    {
        CpuProfileScope present(cpuProfiler, QR_CPU_PASS_PRESENT);
        const SwapchainPresentTimings presentTimings = swapchain->Present(queues, renderFinishedSemaphore);
        if (cpuProfiler != nullptr)
        {
            cpuProfiler->Add(QR_CPU_PASS_PRESENT_FENCE_WAIT, presentTimings.fenceWaitMs);
        }
    }

    frameId++;
}

#pragma region qray interface implementation

void VulkanDevice::StartFrame(const QrStartFrameInfo *startInfo)
{
    statsApiCallsGeometry.store(0, std::memory_order_relaxed);
    statsApiCallsRasterized.store(0, std::memory_order_relaxed);
    statsApiCallsLights.store(0, std::memory_order_relaxed);

    if (currentFrameState.WasFrameStarted())
    {
        throw QrException(QR_FRAME_WASNT_ENDED);
    }

    if (startInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    frameAcquireGate.Reset();
    VkCommandBuffer newFrameCmd = BeginFrame(*startInfo);
    currentFrameState.OnBeginFrame(newFrameCmd);
}

void VulkanDevice::DrawFrame(const QrDrawFrameInfo *drawInfo)
{
    if (!currentFrameState.WasFrameStarted())
    {
        throw QrException(QR_FRAME_WASNT_STARTED);
    }

    if (drawInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    SpatialFrameDrawStorage storage;

    if (!spatialFrame.IsIdentity())
    {
        ConvertDrawFrame(spatialFrame, *drawInfo, storage);
        drawInfo = &storage.info;
    }

    cpuFrameProfiler.Reset(drawInfo->enableCpuProfiling != 0);
    CpuFrameProfiler *cpuProfiler = cpuFrameProfiler.IsEnabled() ? &cpuFrameProfiler : nullptr;
    CpuProfileScope prepare(cpuProfiler, QR_CPU_PASS_PREPARE);
    statsCpuTimingValid = false;
    statsRenderedUiOnly = drawInfo->renderUiOnly != 0;
    statsGpuTimingValid = false;
    statsGpuFrameMs = 0.0f;
    std::fill_n(statsGpuPassMs, QR_GPU_PASS_COUNT, 0.0f);

    if (rhiAccelStructs != nullptr)
    {
        rhiAccelStructs->ResetDynamicFrameCounters();
    }

    VkCommandBuffer cmd = currentFrameState.GetCmdBuffer();
    const uint32_t frameIndex = currentFrameState.GetFrameIndex();

    previousFrameTime = currentFrameTime;
    currentFrameTime = drawInfo->currentTime;

    if (rayStats)
    {
        statsRays = rayStats->GetRays(frameIndex);
        rayStats->GetRaysPerCategory(frameIndex, statsRaysPerCategory);
        lastReflRefrTraced = statsRaysPerCategory[RAY_STATS_CATEGORY_REFLECTION_REFRACTION] != 0;
        rayStats->Reset(frameIndex);
    }

    statsRasterUploadBytes = rasterizedDataCollector->GetUploadedBytes();
    statsRasterUploadDroppedBatches = rasterizedDataCollector->GetDroppedUploadBatches();

    const double dt = std::max(currentFrameTime - previousFrameTime, 0.0001);
    const float fps = static_cast<float>(1.0 / dt);
    statsSmoothedFps = statsSmoothedFps <= 0.0f ? fps : statsSmoothedFps * 0.92f + fps * 0.08f;
    statsFpsX10 = static_cast<uint32_t>(std::clamp(statsSmoothedFps * 10.0f, 0.0f, 99999.0f));

    renderResolution.Setup(drawInfo->pRenderResolutionParams,
                           swapchain->GetWidth(), swapchain->GetHeight(), nvDlss);

    if (!lastUpscaleTechnique.has_value() ||
        *lastUpscaleTechnique != renderResolution.GetUpscaleTechnique())
    {
        lastUpscaleTechnique = renderResolution.GetUpscaleTechnique();
        amdFsr->SetUpscaleVersion(*lastUpscaleTechnique);
    }

    prepare.Finish();
    {
        CpuProfileScope hotReload(cpuProfiler, QR_CPU_PASS_HOT_RELOAD);
        textureManager->CheckForHotReload(cmd, frameIndex);
    }

    const bool canRender = renderResolution.Width() > 0 && renderResolution.Height() > 0;

    if (canRender)
    {
        CpuProfileScope fillUniform(cpuProfiler, QR_CPU_PASS_PREPARE);
        FillUniform(uniform->GetData(), *drawInfo);
    }

    if (canRender)
    {
        {
            CpuProfileScope descriptors(cpuProfiler, QR_CPU_PASS_DESCRIPTORS);
            const bool mipLodBiasUpdated = worldSamplerManager->TryChangeMipLodBias(frameIndex, renderResolution.GetMipLodBias());
            textureManager->SubmitDescriptors(frameIndex, drawInfo->pTexturesParams, mipLodBiasUpdated);
        }

        if (!drawInfo->renderUiOnly)
        {
            CpuProfileScope staging(cpuProfiler, QR_CPU_PASS_STAGING);
            rasterizedDataCollector->CopyFromStaging(cmd, frameIndex);
        }

        if (RenderThroughRhi(*drawInfo))
        {
            if (nvrhiFrameSkeleton != nullptr)
            {
                float frameMs = 0.0f;
                float passMs[QR_GPU_PASS_COUNT] = {};

                statsGpuTimingValid = nvrhiFrameSkeleton->GetGpuTimings(&frameMs, passMs);
                statsGpuFrameMs = frameMs;
                std::copy_n(passMs, QR_GPU_PASS_COUNT, statsGpuPassMs);
            }

            currentFrameState.OnEndFrame();
            statsCpuTimingValid = cpuFrameProfiler.IsEnabled();
            return;
        }

        if (!warnedSkeletonRefusedFrame)
        {
            warnedSkeletonRefusedFrame = true;
            Print("Warning: RHI: the frame skeleton refused the frame, the acquired image is presented as-is");
        }
    }

    EndFrame(cmd);
    currentFrameState.OnEndFrame();
    statsCpuTimingValid = cpuFrameProfiler.IsEnabled();
}

bool VulkanDevice::IsSuspended() const
{
    if (!swapchain)
    {
        return false;
    }

    return !swapchain->IsExtentOptimal();
}

bool VulkanDevice::IsSurfaceUnavailable() const
{
    if (!swapchain)
    {
        return false;
    }

    return !swapchain->HasValidExtent();
}

bool VulkanDevice::IsRenderUpscaleTechniqueAvailable(QrRenderUpscaleTechnique technique) const
{
    switch (technique)
    {
        case QR_RENDER_UPSCALE_TECHNIQUE_NEAREST:
        case QR_RENDER_UPSCALE_TECHNIQUE_LINEAR:
            return true;
        case QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3:
            return FidelityFX::FSR::IsUpscaleVersionAvailable(technique);
        case QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS:
            return nvDlss->IsDlssAvailable();
        default:
            throw QrException(QR_WRONG_ARGUMENT, "Incorrect technique was passed to qrIsRenderUpscaleTechniqueAvailable");
    }
}

void VulkanDevice::Print(const char *pMessage) const
{
    userPrint->Print(pMessage);
}

void VulkanDevice::GetFrameStats(uint32_t *pRays, uint32_t *pFpsX10) const
{
    uint32_t fpsX10 = statsFpsX10;
    uint32_t presentFpsX10 = 0;
    if (swapchain && swapchain->GetPresentFpsX10(&presentFpsX10))
    {
        fpsX10 = presentFpsX10;
    }

    if (pRays != nullptr)
    {
        *pRays = statsRays;
    }
    if (pFpsX10 != nullptr)
    {
        *pFpsX10 = fpsX10;
    }
}

void VulkanDevice::GetFrameStatsEx(QrFrameStats *pStats) const
{
    static_assert(RAY_STATS_CATEGORY_COUNT == QR_RAY_STATS_CATEGORY_COUNT + 1, "Ray stats category count mismatch");

    if (pStats == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    memset(pStats, 0, sizeof(QrFrameStats));

    pStats->raysTotal = statsRays;
    for (uint32_t i = 0; i < QR_RAY_STATS_CATEGORY_COUNT; i++)
    {
        pStats->raysPerCategory[i] = statsRaysPerCategory[i];
    }
    pStats->raysParticle = statsRaysPerCategory[RAY_STATS_CATEGORY_PARTICLE];
    pStats->fpsX10 = statsFpsX10;
    uint32_t presentFpsX10 = 0;
    if (swapchain && swapchain->GetPresentFpsX10(&presentFpsX10))
    {
        pStats->fpsX10 = presentFpsX10;
    }

    pStats->gpuTimingValid = statsGpuTimingValid ? 1 : 0;
    pStats->gpuFrameMs = statsGpuFrameMs;
    for (uint32_t i = 0; i < QR_GPU_PASS_COUNT; i++)
    {
        pStats->gpuPassMs[i] = statsGpuPassMs[i];
    }

    pStats->apiCallsGeometry = statsApiCallsGeometry.load(std::memory_order_relaxed);
    pStats->apiCallsRasterized = statsApiCallsRasterized.load(std::memory_order_relaxed);
    pStats->apiCallsLights = statsApiCallsLights.load(std::memory_order_relaxed);
    pStats->cpuTimingValid = statsCpuTimingValid ? 1 : 0;
    pStats->renderedUiOnly = statsRenderedUiOnly ? 1 : 0;
    std::copy_n(cpuFrameProfiler.GetMilliseconds().data(), QR_CPU_PASS_COUNT, pStats->cpuPassMs);

    pStats->rasterUploadBytes = statsRasterUploadBytes;
    pStats->rasterUploadDroppedBatches = statsRasterUploadDroppedBatches;

    if (rhiAccelStructs != nullptr)
    {
        rhiAccelStructs->GetDynamicFrameCounters(&pStats->dynamicBlasBuilds, &pStats->dynamicBlasRecreations,
                                                 &pStats->dynamicBlasCreateMs);

        uint32_t filterCount = 0;
        const rhi::RhiAccelStructs::DynamicFilterCounters *filters =
            rhiAccelStructs->GetDynamicFilterCounters(&filterCount);

        if (filterCount > QR_DYNAMIC_BLAS_FILTER_MAX)
        {
            filterCount = QR_DYNAMIC_BLAS_FILTER_MAX;
        }

        pStats->dynamicFilterCount = filterCount;
        for (uint32_t i = 0; i < filterCount; i++)
        {
            pStats->dynamicFilters[i].filter = filters[i].filter;
            pStats->dynamicFilters[i].builds = filters[i].builds;
            pStats->dynamicFilters[i].recreates = filters[i].recreates;
            pStats->dynamicFilters[i].positionalFails = filters[i].positionalFails;
            pStats->dynamicFilters[i].sortedPassPositionalFails = filters[i].sortedPassPositionalFails;
            pStats->dynamicFilters[i].sortedFails = filters[i].sortedFails;
            pStats->dynamicFilters[i].sortedCountFails = filters[i].sortedCountFails;
            pStats->dynamicFilters[i].sortedShapeFails = filters[i].sortedShapeFails;
            pStats->dynamicFilters[i].geometryCount = filters[i].geometryCount;
            pStats->dynamicFilters[i].envelopeCount = filters[i].envelopeCount;
            pStats->dynamicFilters[i].envelopePrimitives = filters[i].envelopePrimitives;
            pStats->dynamicFilters[i].listPrimitives = filters[i].listPrimitives;
            pStats->dynamicFilters[i].failDeltaSum = filters[i].failDeltaSum;
            pStats->dynamicFilters[i].failDeltaMin = filters[i].failDeltaMin;
            pStats->dynamicFilters[i].failDeltaMax = filters[i].failDeltaMax;
        }
    }
}

void VulkanDevice::GetAdapterInfo(QrAdapterInfo *pInfo) const
{
    if (pInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    memset(pInfo, 0, sizeof(QrAdapterInfo));

    if (physDevice == nullptr)
    {
        return;
    }

    const VkPhysicalDeviceProperties &properties = physDevice->GetProperties();
    const VkPhysicalDeviceDriverProperties &driverProperties = physDevice->GetDriverProperties();

    std::snprintf(pInfo->name, sizeof(pInfo->name), "%s", properties.deviceName);
    std::snprintf(pInfo->driverName, sizeof(pInfo->driverName), "%s", driverProperties.driverName);
    std::snprintf(pInfo->driverInfo, sizeof(pInfo->driverInfo), "%s", driverProperties.driverInfo);
    pInfo->vendorId = properties.vendorID;
    pInfo->deviceId = properties.deviceID;
    pInfo->driverVersion = properties.driverVersion;
    pInfo->apiVersion = properties.apiVersion;
}

QrResult VulkanDevice::GetGpuContext(QrayGpuContext *pContext) const
{
    if (pContext == nullptr ||
        pContext->header.size < sizeof(QrayGpuContext) ||
        (pContext->header.version >> 16) != kGpuContextContractMajor)
        return QR_WRONG_ARGUMENT;

    if (nvrhi == nullptr || physDevice == nullptr || queues == nullptr)
        return QR_FEATURE_UNAVAILABLE;

    nvrhi::IDevice *pDevice = nvrhi->GetDevice();
    if (pDevice == nullptr)
        return QR_FEATURE_UNAVAILABLE;

    pContext->pDevice = pDevice;
    pContext->pVulkanDevice = dynamic_cast<nvrhi::vulkan::IDevice *>(pDevice);

    QrayGpuCapabilities &capabilities = pContext->capabilities;
    capabilities.header = { sizeof(QrayGpuCapabilities), kGpuContextContractVersion };
    capabilities.flags = QRAY_GPU_CAP_SHARED_NVRHI_DEVICE | QRAY_GPU_CAP_NATIVE_VULKAN;
    if (timelineSemaphoreEnabled)
        capabilities.flags |= QRAY_GPU_CAP_TIMELINE_SEMAPHORE;
    if (queues->GetGraphicsQueueCount() > 1 ||
        queues->GetComputeQueueCount() > 1 ||
        queues->GetTransferQueueCount() > 1)
        capabilities.flags |= QRAY_GPU_CAP_ADDITIONAL_QUEUES;
    capabilities.framesInFlight = MAX_FRAMES_IN_FLIGHT;
    capabilities.vulkanApiVersion = physDevice->GetProperties().apiVersion;
    capabilities.graphicsApi = pDevice->getGraphicsAPI();
    capabilities.graphicsQueueCount = queues->GetGraphicsQueueCount();
    capabilities.computeQueueCount = queues->GetComputeQueueCount();
    capabilities.transferQueueCount = queues->GetTransferQueueCount();

    QrayNativeVulkanInfo &native = pContext->nativeVulkan;
    native.header = { sizeof(QrayNativeVulkanInfo), kGpuContextContractVersion };
    native.instance = instance;
    native.physicalDevice = physDevice->Get();
    native.device = device;
    native.surface = surface;
    native.graphicsQueue = queues->GetGraphics();
    native.graphicsQueueFamily = queues->GetIndexGraphics();
    native.graphicsQueueIndex = 0;
    native.graphicsQueueCount = queues->GetGraphicsQueueCount();
    native.computeQueue = queues->GetCompute();
    native.computeQueueFamily = queues->GetIndexCompute();
    native.computeQueueIndex = 0;
    native.computeQueueCount = queues->GetComputeQueueCount();
    native.transferQueue = queues->GetTransfer();
    native.transferQueueFamily = queues->GetIndexTransfer();
    native.transferQueueIndex = 0;
    native.transferQueueCount = queues->GetTransferQueueCount();
    native.properties = physDevice->GetProperties();
    native.memoryProperties = physDevice->GetMemoryProperties();
    return QR_SUCCESS;
}

QrResult VulkanDevice::SetGpuFrameHooks(const QrayGpuFrameHooks *pHooks)
{
    if (pHooks == nullptr)
    {
        gpuFrameHooks = {};
        return QR_SUCCESS;
    }

    if (pHooks->header.size < sizeof(QrayGpuFrameHooks) ||
        (pHooks->header.version >> 16) != kGpuContextContractMajor ||
        pHooks->reserved != 0)
        return QR_WRONG_ARGUMENT;

    gpuFrameHooks = *pHooks;
    return QR_SUCCESS;
}

static uint64_t UploadNowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static void UploadAtomicMax(std::atomic<uint64_t> &value, uint64_t candidate)
{
    uint64_t current = value.load(std::memory_order_relaxed);

    while (candidate > current && !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed))
    {
    }
}

static thread_local uint32_t g_uploadLane = QR_UPLOAD_LANE_UNATTRIBUTED;
static thread_local uint32_t g_uploadLaneStack[8];
static thread_local uint32_t g_uploadLaneDepth = 0;

VulkanDevice::UploadLockScope::UploadLockScope(VulkanDevice &device_, uint32_t lane_)
    : device(device_), lane(lane_), enabled(false), waitNs(0), holdStartNs(0)
{
    enabled = device.uploadLockTracing.load(std::memory_order_relaxed) && lane < QR_UPLOAD_LANE_COUNT;

    if (enabled)
    {
        const uint64_t t0 = UploadNowNs();
        device.geometryUploadMutex.lock();
        const uint64_t t1 = UploadNowNs();
        waitNs = t1 - t0;
        holdStartNs = t1;
    }
    else
    {
        device.geometryUploadMutex.lock();
    }
}

VulkanDevice::UploadLockScope::~UploadLockScope()
{
    if (enabled)
    {
        const uint64_t holdNs = UploadNowNs() - holdStartNs;

        device.uploadLockWaitSumNs[lane].fetch_add(waitNs, std::memory_order_relaxed);
        UploadAtomicMax(device.uploadLockWaitMaxNs[lane], waitNs);
        device.uploadLockHoldSumNs[lane].fetch_add(holdNs, std::memory_order_relaxed);
        UploadAtomicMax(device.uploadLockHoldMaxNs[lane], holdNs);
        device.uploadLockCalls[lane].fetch_add(1, std::memory_order_relaxed);
    }

    device.geometryUploadMutex.unlock();
}

void VulkanDevice::UploadLanePush(uint32_t lane)
{
    if (g_uploadLaneDepth < 8)
        g_uploadLaneStack[g_uploadLaneDepth++] = g_uploadLane;

    g_uploadLane = lane;
}

void VulkanDevice::UploadLanePop()
{
    if (g_uploadLaneDepth > 0)
        g_uploadLane = g_uploadLaneStack[--g_uploadLaneDepth];
}

void VulkanDevice::SetUploadLockTracing(bool enable)
{
    uploadLockTracing.store(enable, std::memory_order_relaxed);
}

void VulkanDevice::SetDynamicShapeSortedReuse(bool enable)
{
    if (rhiAccelStructs != nullptr)
    {
        rhiAccelStructs->SetSortedShapeReuse(enable);
    }
}

void VulkanDevice::SetDynamicShapeHeadroom(uint32_t percent)
{
    if (rhiAccelStructs != nullptr)
    {
        rhiAccelStructs->SetDynamicHeadroom(percent);
    }
}

void VulkanDevice::SetDynamicShapeRefit(bool enable)
{
    if (rhiAccelStructs != nullptr)
    {
        rhiAccelStructs->SetShapeRefit(enable);
    }
}

void VulkanDevice::ResetUploadLockStats()
{
    for (uint32_t i = 0; i < QR_UPLOAD_LANE_COUNT; ++i)
    {
        uploadLockWaitSumNs[i].store(0, std::memory_order_relaxed);
        uploadLockWaitMaxNs[i].store(0, std::memory_order_relaxed);
        uploadLockHoldSumNs[i].store(0, std::memory_order_relaxed);
        uploadLockHoldMaxNs[i].store(0, std::memory_order_relaxed);
        uploadLockCalls[i].store(0, std::memory_order_relaxed);
    }

    if (scene)
        scene->ResetBoundsScanStats();
}

void VulkanDevice::GetUploadLockStats(QrUploadLockStats *pStats) const
{
    if (pStats == nullptr)
        return;

    std::memset(pStats, 0, sizeof(*pStats));
    pStats->valid = true;

    for (uint32_t i = 0; i < QR_UPLOAD_LANE_COUNT; ++i)
    {
        pStats->lanes[i].calls = uploadLockCalls[i].load(std::memory_order_relaxed);
        pStats->lanes[i].waitSumNs = uploadLockWaitSumNs[i].load(std::memory_order_relaxed);
        pStats->lanes[i].waitMaxNs = uploadLockWaitMaxNs[i].load(std::memory_order_relaxed);
        pStats->lanes[i].holdSumNs = uploadLockHoldSumNs[i].load(std::memory_order_relaxed);
        pStats->lanes[i].holdMaxNs = uploadLockHoldMaxNs[i].load(std::memory_order_relaxed);
    }

    if (scene)
    {
        pStats->boundsScanSumNs = scene->GetBoundsScanSumNs();
        pStats->boundsScanCalls = scene->GetBoundsScanCalls();
        pStats->boundsScanVerts = scene->GetBoundsScanVerts();
    }
}

void VulkanDevice::UploadGeometry(const QrGeometryUploadInfo *uploadInfo)
{
    UploadLockScope geometryLock(*this, g_uploadLane);
    statsApiCallsGeometry.fetch_add(1, std::memory_order_relaxed);

    using namespace std::string_literals;

    if (uploadInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    if (uploadInfo->pVertices == nullptr || uploadInfo->vertexCount == 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Incorrect vertex data");
    }

    if ((uploadInfo->pIndices == nullptr && uploadInfo->indexCount != 0) ||
        (uploadInfo->pIndices != nullptr && uploadInfo->indexCount == 0))
    {
        throw QrException(QR_WRONG_ARGUMENT, "Incorrect index data");
    }

    if (uploadInfo->geomType != QR_GEOMETRY_TYPE_STATIC &&
        uploadInfo->geomType != QR_GEOMETRY_TYPE_STATIC_MOVABLE &&
        uploadInfo->geomType != QR_GEOMETRY_TYPE_DYNAMIC &&

        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_ONLY_REFLECT &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT &&
        uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT &&

        uploadInfo->visibilityType != QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0 &&
        uploadInfo->visibilityType != QR_GEOMETRY_VISIBILITY_TYPE_WORLD_1 &&
        uploadInfo->visibilityType != QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2 &&
        uploadInfo->visibilityType != QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON &&
        uploadInfo->visibilityType != QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER &&
        uploadInfo->visibilityType != QR_GEOMETRY_VISIBILITY_TYPE_SKY)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Incorrect type of ray traced geometry");
    }

    if (allowGeometryWithSkyFlag)
    {
        if (uploadInfo->visibilityType == QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2)
        {
            throw QrException(QR_WRONG_ARGUMENT, "Geometry with QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2 cannot be used, as QrInstanceCreateInfo::allowGeometryWithSkyFlag was true");
        }
    }
    else
    {
        if (uploadInfo->visibilityType == QR_GEOMETRY_VISIBILITY_TYPE_SKY)
        {
            throw QrException(QR_WRONG_ARGUMENT, "Geometry with QR_GEOMETRY_VISIBILITY_TYPE_SKY cannot be used, as QrInstanceCreateInfo::allowGeometryWithSkyFlag was false");
        }
    }

    if ((uploadInfo->flags & QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT) != 0 &&
        (uploadInfo->flags & QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT) != 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT and QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT must be set separately");
    }

    if (scene->DoesUniqueIDExist(uploadInfo->uniqueID))
    {
        if (uploadInfo->geomType == QR_GEOMETRY_TYPE_DYNAMIC && scene->DoesDynamicUniqueIDExist(uploadInfo->uniqueID))
        {
            return;
        }

        throw QrException(QR_WRONG_ARGUMENT, "Geometry with ID="s + std::to_string(uploadInfo->uniqueID) + " already exists");
    }

    if (uploadInfo->pPortalIndex != nullptr && uploadInfo->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Geometry's pPortalIndex is non-null, but geometry is not marked as portal");
    }

    if (uploadInfo->pPortalIndex == nullptr && uploadInfo->passThroughType == QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Geometry is marked as portal, but pPortalIndex is null");
    }

    if (uploadInfo->pPortalIndex && *(uploadInfo->pPortalIndex) >= PORTAL_MAX_COUNT)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Geometry's portal index must be in [0, 62]");
    }

    LockSpatialFrame();

    QrGeometryUploadInfo converted = *uploadInfo;
    std::vector<uint32_t> convertedIndices;
    std::vector<QrVertex> convertedVertices;

    if (!spatialFrame.IsIdentity())
    {
        converted = spatialFrame.Geometry(*uploadInfo);
    }

    const bool reversesWinding = spatialFrame.ReversesWinding(uploadInfo->transform);

    if (reversesWinding)
    {
        if (uploadInfo->pIndices != nullptr && uploadInfo->indexCount >= 3)
        {
            convertedIndices.assign(uploadInfo->pIndices, uploadInfo->pIndices + uploadInfo->indexCount);

            for (uint32_t index = 0; index + 2 < uploadInfo->indexCount; index += 3)
            {
                std::swap(convertedIndices[index + 1], convertedIndices[index + 2]);
            }

            converted.pIndices = convertedIndices.data();
        }
        else if (uploadInfo->pIndices == nullptr && uploadInfo->vertexCount >= 3)
        {
            convertedVertices.assign(uploadInfo->pVertices, uploadInfo->pVertices + uploadInfo->vertexCount);

            for (uint32_t vertex = 0; vertex + 2 < uploadInfo->vertexCount; vertex += 3)
            {
                std::swap(convertedVertices[vertex + 1], convertedVertices[vertex + 2]);
            }

            converted.pVertices = convertedVertices.data();
        }
    }

    if (!spatialFrame.IsIdentity() || reversesWinding)
    {
        uploadInfo = &converted;
    }

    geometryWindingFlips[uploadInfo->uniqueID] = reversesWinding;

    scene->Upload(currentFrameState.GetFrameIndex(), *uploadInfo);
}

void VulkanDevice::UpdateGeometryTransform(const QrUpdateTransformInfo *updateInfo)
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);
    if (updateInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrUpdateTransformInfo converted = *updateInfo;
    const bool reversesWinding = spatialFrame.ReversesWinding(updateInfo->transform);
    const auto flipIt = geometryWindingFlips.find(updateInfo->movableStaticUniqueID);

    if (flipIt != geometryWindingFlips.end() && flipIt->second != reversesWinding)
    {
        throw QrException(QR_CANT_UPDATE_TRANSFORM, "Transform update of geometry with unique ID=" + std::to_string(updateInfo->movableStaticUniqueID) + " reverses its winding; re-upload the geometry instead");
    }

    if (!spatialFrame.IsIdentity())
    {
        converted.transform = SpatialTransform(spatialFrame, updateInfo->transform);
        updateInfo = &converted;
    }

    scene->UpdateTransform(*updateInfo);
}

void VulkanDevice::UpdateGeometryTexCoords(const QrUpdateTexCoordsInfo *updateInfo)
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);
    if (updateInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UpdateTexCoords(*updateInfo);
}

void VulkanDevice::UploadRasterizedGeometry(const QrRasterizedGeometryUploadInfo *pUploadInfo,
                                                const float *pViewProjection, const QrViewport *pViewport)
{
    UploadLockScope geometryLock(*this, g_uploadLane);
    statsApiCallsRasterized.fetch_add(1, std::memory_order_relaxed);

    if (pUploadInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    if (pUploadInfo->renderType != QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT &&
        pUploadInfo->renderType != QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN &&
        pUploadInfo->renderType != QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Incorrect render type of rasterized geometry");
    }

    if (pUploadInfo->pVertices == nullptr || pUploadInfo->vertexCount == 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Vertex data / count is null");
    }

    if ((pUploadInfo->pIndices == nullptr && pUploadInfo->indexCount != 0) ||
        (pUploadInfo->pIndices != nullptr && pUploadInfo->indexCount == 0))
    {
        throw QrException(QR_WRONG_ARGUMENT, "Index data / count must be both not null or null");
    }

    const uint32_t particleFlags =
        QR_RASTERIZED_GEOMETRY_STATE_PARTICLE | QR_RASTERIZED_GEOMETRY_STATE_PARTICLE_SPRITE;

    if (pUploadInfo->renderType != QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT &&
        (pUploadInfo->pipelineState &
         (QR_RASTERIZED_GEOMETRY_STATE_SMOKE | particleFlags)) != 0)
    {
        throw QrException(QR_WRONG_ARGUMENT,
                          "Smoke and particle states are only valid for DEFAULT rasterized geometry");
    }

    if ((pUploadInfo->pipelineState & QR_RASTERIZED_GEOMETRY_STATE_SMOKE) != 0 &&
        (pUploadInfo->pipelineState & particleFlags) != 0)
    {
        throw QrException(QR_WRONG_ARGUMENT,
                          "SMOKE and PARTICLE raster states are mutually exclusive");
    }

    if (pUploadInfo->renderType == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN &&
        (pUploadInfo->pipelineState &
         (QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE)) != 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "SWAPCHAIN rasterized geometry cannot use depth state");
    }

    // An upload outside a started frame cannot be drawn -- the collector is
    // reset when the next frame starts -- so it is dropped here instead of
    // piling up while frames are skipped (a minimized window keeps the game,
    // and with it any draw producing uploads, running).
    if (!currentFrameState.WasFrameStarted())
    {
        if (!printedRasterUploadWithoutFrame)
        {
            printedRasterUploadWithoutFrame = true;
            Print("RHI: rasterized geometry uploaded outside a frame was dropped; the renderer is not drawing (minimized?)");
        }
        return;
    }

    if (pUploadInfo->renderType != QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN)
    {
        LockSpatialFrame();
    }

    QrRasterizedGeometryUploadInfo converted = *pUploadInfo;
    float convertedViewProjection[16] = {};
    std::vector<QrVertex> convertedVertices;
    std::vector<uint32_t> convertedIndices;

    const bool spatialRenderType =
        pUploadInfo->renderType != QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
    const bool spatialConversion = !spatialFrame.IsIdentity() && spatialRenderType;
    const bool reversesWinding = spatialRenderType &&
        ShouldReverseRasterizedWinding(
            spatialFrame.ReversesWinding(pUploadInfo->transform), pUploadInfo->pipelineState);

    if (spatialConversion)
    {
        converted.transform = SpatialTransform(spatialFrame, pUploadInfo->transform);
    }

    if (reversesWinding)
    {
        if (pUploadInfo->pIndices != nullptr && pUploadInfo->indexCount >= 3)
        {
            const uint32_t *sourceIndices = static_cast<const uint32_t *>(pUploadInfo->pIndices);
            convertedIndices.assign(sourceIndices, sourceIndices + pUploadInfo->indexCount);

            for (uint32_t index = 0; index + 2 < pUploadInfo->indexCount; index += 3)
            {
                std::swap(convertedIndices[index + 1], convertedIndices[index + 2]);
            }

            converted.pIndices = convertedIndices.data();
        }
        else if (pUploadInfo->pIndices == nullptr && pUploadInfo->vertexCount >= 3)
        {
            convertedVertices.assign(pUploadInfo->pVertices, pUploadInfo->pVertices + pUploadInfo->vertexCount);

            for (uint32_t vertex = 0; vertex + 2 < pUploadInfo->vertexCount; vertex += 3)
            {
                std::swap(convertedVertices[vertex + 1], convertedVertices[vertex + 2]);
            }
        }
    }

    if (spatialConversion)
    {
        if ((pUploadInfo->pipelineState & QR_RASTERIZED_GEOMETRY_STATE_SMOKE) != 0)
        {
            if (convertedVertices.empty())
            {
                convertedVertices.assign(pUploadInfo->pVertices, pUploadInfo->pVertices + pUploadInfo->vertexCount);
            }

            for (QrVertex &vertex : convertedVertices)
            {
                vertex.normal[1] = spatialFrame.Length(vertex.normal[1]);
            }

            converted.smokeNoise.data[0] *= spatialFrame.InvScale();
            converted.smokeNoise.data[1] *= spatialFrame.InvScale();
            converted.smokeNoise.data[2] *= spatialFrame.InvScale();
            converted.smokeNoise.data[3] *= spatialFrame.Scale();
        }

        if (pViewProjection != nullptr)
        {
            spatialFrame.ViewProjection(pViewProjection, convertedViewProjection);
            pViewProjection = convertedViewProjection;
        }
    }

    if (!convertedVertices.empty())
    {
        converted.pVertices = convertedVertices.data();
    }

    if (spatialConversion || reversesWinding)
    {
        pUploadInfo = &converted;
    }

    if (!rasterizedDataCollector->AddGeometry(currentFrameState.GetFrameIndex(), *pUploadInfo, pViewProjection, pViewport))
    {
        if (!printedRasterOverflow)
        {
            printedRasterOverflow = true;
            Print("RHI: the rasterized geometry buffer is full; the rest of the frame's overlays are skipped (raise rasterizedMaxVertexCount)");
        }
    }
}

QrResult VulkanDevice::UploadParticles(const QrParticleUploadInfo *pUploadInfo)
{
    UploadLockScope geometryLock(*this, g_uploadLane);
    statsApiCallsRasterized++;

    if (pUploadInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    if (pUploadInfo->pPoints == nullptr || pUploadInfo->count == 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Particle point data / count is null");
    }

    if (!currentFrameState.WasFrameStarted())
    {
        if (!printedRasterUploadWithoutFrame)
        {
            printedRasterUploadWithoutFrame = true;
            Print("RHI: particle points uploaded outside a frame were dropped; the renderer is not drawing (minimized?)");
        }
        return QR_SUCCESS;
    }

    LockSpatialFrame();

    QrParticleUploadInfo converted = *pUploadInfo;
    std::vector<QrParticlePoint> convertedPoints;

    if (!spatialFrame.IsIdentity())
    {
        convertedPoints.resize(pUploadInfo->count);

        for (uint32_t i = 0; i < pUploadInfo->count; i++)
        {
            convertedPoints[i] = SpatialParticlePoint(spatialFrame, pUploadInfo->pPoints[i]);
        }

        converted.pPoints = convertedPoints.data();
        converted.viewRight = SpatialDirection(spatialFrame, pUploadInfo->viewRight);
        converted.viewUp = SpatialDirection(spatialFrame, pUploadInfo->viewUp);
        pUploadInfo = &converted;
    }

    if (!rasterizedDataCollector->AddParticles(currentFrameState.GetFrameIndex(), *pUploadInfo))
    {
        if (!printedParticlePointOverflow || frameId - lastParticlePointOverflowWarnFrameId >= 120u)
        {
            printedParticlePointOverflow = true;
            lastParticlePointOverflowWarnFrameId = frameId;
            Print("RHI: the particle point buffer is full; the overflowing particles are skipped this frame (raise rasterizedMaxVertexCount)");
        }

        return QR_CANT_UPLOAD_RASTERIZED_GEOMETRY;
    }

    return QR_SUCCESS;
}

void VulkanDevice::UploadDecal(const QrDecalUploadInfo *pUploadInfo)
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);
    if (pUploadInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrDecalUploadInfo converted = *pUploadInfo;
    if (!spatialFrame.IsIdentity())
    {
        converted.transform = SpatialTransform(spatialFrame, pUploadInfo->transform);
        pUploadInfo = &converted;
    }

    decalManager->Upload(currentFrameState.GetFrameIndex(), *pUploadInfo, textureManager);
}

void VulkanDevice::UploadPortal(const QrPortalUploadInfo *pUploadInfo)
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);
    if (pUploadInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrPortalUploadInfo converted = *pUploadInfo;
    if (!spatialFrame.IsIdentity())
    {
        converted = SpatialPortal(spatialFrame, *pUploadInfo);
        pUploadInfo = &converted;
    }

    portalList->Upload(currentFrameState.GetFrameIndex(), *pUploadInfo);
}

void VulkanDevice::SubmitStaticGeometries()
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);
    scene->SubmitStatic();
}

void VulkanDevice::StartNewStaticScene()
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);
    scene->StartNewStatic();
}

void VulkanDevice::UploadDirectionalLight(const QrDirectionalLightUploadInfo *pLightInfo)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pLightInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrDirectionalLightUploadInfo converted = SpatialDirectionalLight(spatialFrame, *pLightInfo);
    scene->UploadLight(currentFrameState.GetFrameIndex(), converted);
}

void VulkanDevice::UploadSphericalLight(const QrSphericalLightUploadInfo *pLightInfo)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pLightInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrSphericalLightUploadInfo converted = SpatialSphericalLight(spatialFrame, *pLightInfo);
    scene->UploadLight(currentFrameState.GetFrameIndex(), converted);
}

void VulkanDevice::BeginDeferredLightUploads(uint32_t slot)
{
    scene->BeginDeferredLightUploads(slot);
}

void VulkanDevice::EndDeferredLightUploads()
{
    scene->EndDeferredLightUploads();
}

void VulkanDevice::FlushDeferredLightUploads()
{
    scene->FlushDeferredLightUploads();
}

void VulkanDevice::SetParticleProxyGate(uint32_t gate, uint32_t glassParticles)
{
    std::lock_guard<std::mutex> geometryLock(geometryUploadMutex);

    const bool hasGlass = rhiAccelStructs != nullptr && rhiAccelStructs->HasGlassInstances();
    const bool enabled = gate == 0u ? true : (gate == 2u ? false : (glassParticles != 0u && (lastReflRefrTraced || hasGlass)));

    if (rasterizedDataCollector != nullptr)
    {
        rasterizedDataCollector->SetParticleProxyCaptureEnabled(enabled);
    }

    if (enabled != particleProxyGateOpen)
    {
        particleProxyGateOpen = enabled;
        fprintf(stderr, "qray: particle proxy gate %s (refl/refr traced last frame: %s, glass instances: %s)\n",
                enabled ? "open" : "closed", lastReflRefrTraced ? "yes" : "no", hasGlass ? "present" : "absent");
    }
}

void VulkanDevice::UploadSpotlight(const QrSpotLightUploadInfo *pLightInfo)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pLightInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrSpotLightUploadInfo converted = SpatialSpotLight(spatialFrame, *pLightInfo);
    scene->UploadLight(currentFrameState.GetFrameIndex(), converted);
}

void VulkanDevice::UploadPolygonalLight(const QrPolygonalLightUploadInfo *pLightInfo)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pLightInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    QrPolygonalLightUploadInfo converted = SpatialPolygonalLight(spatialFrame, *pLightInfo);
    scene->UploadLight(currentFrameState.GetFrameIndex(), converted);
}

void VulkanDevice::UploadTexturedAreaLight(const QrTexturedAreaLightUploadInfo *pLightInfo)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pLightInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    const MaterialTextures textures = textureManager->GetMaterialTextures(pLightInfo->material);
    const uint32_t textureIndex = textures.indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];

    QrTexturedAreaLightUploadInfo converted = SpatialTexturedAreaLight(spatialFrame, *pLightInfo);
    scene->UploadLight(currentFrameState.GetFrameIndex(), converted, textureIndex);
}

void VulkanDevice::UploadTexturedAreaLights(const QrTexturedAreaLightUploadInfo *pLightInfos, uint32_t count)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pLightInfos == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    const uint32_t materialCacheSize = 512;
    uint32_t cachedMaterial[materialCacheSize];
    uint32_t cachedTextureIndex[materialCacheSize];
    bool     cachedValid[materialCacheSize] = {};

    const uint32_t frameIndex = currentFrameState.GetFrameIndex();

    for (uint32_t i = 0; i < count; i++)
    {
        const QrTexturedAreaLightUploadInfo *pLightInfo = pLightInfos + i;
        const uint32_t slot = pLightInfo->material & (materialCacheSize - 1);
        uint32_t textureIndex;

        if (cachedValid[slot] && cachedMaterial[slot] == pLightInfo->material)
        {
            textureIndex = cachedTextureIndex[slot];
        }
        else
        {
            const MaterialTextures textures = textureManager->GetMaterialTextures(pLightInfo->material);
            textureIndex = textures.indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];

            cachedMaterial[slot] = pLightInfo->material;
            cachedTextureIndex[slot] = textureIndex;
            cachedValid[slot] = true;
        }

        const QrTexturedAreaLightUploadInfo converted = SpatialTexturedAreaLight(spatialFrame, *pLightInfo);
        scene->UploadLight(frameIndex, converted, textureIndex);
    }
}

void VulkanDevice::UploadDtalGroups(const QrDtalGroupUploadBatch *pUploadInfo)
{
    if (pUploadInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    if (pUploadInfo->groupCount == 0 || pUploadInfo->pGroups == nullptr || pUploadInfo->pMembers == nullptr)
    {
        return;
    }

    std::vector<QrDtalGroupUploadInfo> convertedGroups;
    std::vector<QrDtalMemberUpload> convertedMembers;
    QrDtalGroupUploadBatch convertedBatch = *pUploadInfo;

    if (!spatialFrame.IsIdentity())
    {
        convertedGroups.resize(pUploadInfo->groupCount);
        convertedMembers.resize(pUploadInfo->memberCount);

        for (uint32_t i = 0; i < pUploadInfo->groupCount; i++)
        {
            convertedGroups[i] = SpatialDtalGroup(spatialFrame, pUploadInfo->pGroups[i]);
        }

        for (uint32_t i = 0; i < pUploadInfo->memberCount; i++)
        {
            convertedMembers[i] = SpatialDtalMember(spatialFrame, pUploadInfo->pMembers[i]);
        }

        convertedBatch.pGroups = convertedGroups.data();
        convertedBatch.pMembers = convertedMembers.data();
    }

    std::vector<uint32_t> textureIndices(pUploadInfo->groupCount);

    const uint32_t materialCacheSize = 512;
    uint32_t cachedMaterial[materialCacheSize];
    uint32_t cachedTextureIndex[materialCacheSize];
    bool     cachedValid[materialCacheSize] = {};

    for (uint32_t i = 0; i < pUploadInfo->groupCount; i++)
    {
        const QrDtalGroupUploadInfo *pGroup = pUploadInfo->pGroups + i;
        const uint32_t slot = pGroup->material & (materialCacheSize - 1);

        if (cachedValid[slot] && cachedMaterial[slot] == pGroup->material)
        {
            textureIndices[i] = cachedTextureIndex[slot];
        }
        else
        {
            const MaterialTextures textures = textureManager->GetMaterialTextures(pGroup->material);
            textureIndices[i] = textures.indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];

            cachedMaterial[slot] = pGroup->material;
            cachedTextureIndex[slot] = textureIndices[i];
            cachedValid[slot] = true;
        }
    }

    if (!scene->UploadDtalGroups(currentFrameState.GetFrameIndex(), convertedBatch, textureIndices.data()))
    {
        throw QrException(QR_WRONG_ARGUMENT, "DTAL group batch did not fit the renderer light storage");
    }
}

void VulkanDevice::UploadClusterLightSources(const QrClusterLightSourcesUploadInfo *pInfo)
{
    BeginClusterLightSources(pInfo, 1);
    RunClusterLightSourceSlice(0, 1);
    FinishClusterLightSources();
    RunClusterListPublishSlice(0, 1);
    CommitClusterListPublication();
}

void VulkanDevice::BeginClusterLightSources(const QrClusterLightSourcesUploadInfo *pInfo, uint32_t sliceCount)
{
    statsApiCallsLights.fetch_add(1, std::memory_order_relaxed);

    if (pInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    std::vector<QrClusterLightSource> convertedSources;
    QrClusterLightSourcesUploadInfo converted = *pInfo;

    if (!spatialFrame.IsIdentity())
    {
        converted.topUpReach = spatialFrame.Length(pInfo->topUpReach);

        if (pInfo->pLights != nullptr)
        {
            convertedSources.resize(pInfo->numLights);

            for (uint32_t i = 0; i < pInfo->numLights; i++)
            {
                convertedSources[i] = SpatialClusterLightSource(spatialFrame, pInfo->pLights[i]);
            }

            converted.pLights = convertedSources.data();
        }
    }

    clusterLightLists->BeginSources(*worldLights, converted, scene->GetLightManager().get(), userPrint.get(),
                                    currentFrameState.GetFrameIndex(), sliceCount);
}

void VulkanDevice::RunClusterLightSourceSlice(uint32_t slice, uint32_t sliceCount)
{
    clusterLightLists->RunTopUpSlice(slice, sliceCount);
}

void VulkanDevice::FinishClusterLightSources()
{
    clusterLightLists->FinishSources();
}

void VulkanDevice::RunClusterListPublishSlice(uint32_t slice, uint32_t sliceCount)
{
    scene->GetLightManager()->RunClusterListPublishSlice(slice, sliceCount);
}

void VulkanDevice::CommitClusterListPublication()
{
    scene->GetLightManager()->CommitClusterListPublication();
}

void VulkanDevice::GetClusterLightStats(QrClusterLightStats *pStats)
{
    if (pStats == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    *pStats = clusterLightLists->GetStats();
}

void VulkanDevice::GetClusterLightGrants(uint32_t *pGranted, uint32_t *pDenied, uint32_t maxCount,
                                         uint32_t *pCount)
{
    clusterLightLists->GetGrants(pGranted, pDenied, maxCount, pCount);
}

void VulkanDevice::GetClusterLightList(uint32_t cluster, uint64_t *pLightUniqueIds, uint32_t maxCount,
                                       uint32_t *pCount)
{
    clusterLightLists->GetClusterList(cluster, pLightUniqueIds, maxCount, pCount);
}

void VulkanDevice::GetClusterLightTail(uint32_t cluster, uint64_t *pLightUniqueIds, float *pProb,
                                       float *pMarginal, uint32_t *pAlias, float *pBeta, uint32_t maxCount,
                                       uint32_t *pCount)
{
    clusterLightLists->GetClusterTail(cluster, pLightUniqueIds, pProb, pMarginal, pAlias, pBeta, maxCount, pCount);
}

void VulkanDevice::UploadWorldLights(const QrWorldLightsUploadInfo *pInfo)
{
    if (pInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    LockSpatialFrame();

    std::vector<QrFloat3D> clusterMins;
    std::vector<QrFloat3D> clusterMaxs;
    std::vector<QrWorldLightFace> faceRecords;
    std::vector<QrVertex> faceVertices;
    QrWorldLightsUploadInfo converted = *pInfo;

    if (!spatialFrame.IsIdentity())
    {
        if (pInfo->pClusterMins != nullptr && pInfo->pClusterMaxs != nullptr)
        {
            clusterMins.resize(pInfo->numClusters);
            clusterMaxs.resize(pInfo->numClusters);

            for (uint32_t i = 0; i < pInfo->numClusters; i++)
            {
                spatialFrame.Aabb(pInfo->pClusterMins[i], pInfo->pClusterMaxs[i],
                                  clusterMins[i], clusterMaxs[i]);
            }

            converted.pClusterMins = clusterMins.data();
            converted.pClusterMaxs = clusterMaxs.data();
        }

        if (pInfo->pFaceVertices != nullptr)
        {
            faceVertices.resize(pInfo->numFaceVertices);

            for (uint32_t i = 0; i < pInfo->numFaceVertices; i++)
            {
                faceVertices[i] = pInfo->pFaceVertices[i];
                const QrFloat3D position = SpatialPoint(spatialFrame, { {
                    pInfo->pFaceVertices[i].position[0],
                    pInfo->pFaceVertices[i].position[1],
                    pInfo->pFaceVertices[i].position[2],
                } });
                const QrFloat3D normal = SpatialDirection(spatialFrame, { {
                    pInfo->pFaceVertices[i].normal[0],
                    pInfo->pFaceVertices[i].normal[1],
                    pInfo->pFaceVertices[i].normal[2],
                } });
                faceVertices[i].position[0] = position.data[0];
                faceVertices[i].position[1] = position.data[1];
                faceVertices[i].position[2] = position.data[2];
                faceVertices[i].normal[0] = normal.data[0];
                faceVertices[i].normal[1] = normal.data[1];
                faceVertices[i].normal[2] = normal.data[2];
            }

            converted.pFaceVertices = faceVertices.data();
        }
    }

    if (spatialFrame.IsMirrored() && pInfo->numFaces > 0)
    {
        if (pInfo->pFaces == nullptr || faceVertices.empty())
        {
            throw QrException(QR_WRONG_ARGUMENT, "Mirrored world-light faces require both face and vertex arrays");
        }

        faceRecords.assign(pInfo->pFaces, pInfo->pFaces + pInfo->numFaces);

        if (!ReverseWorldLightFaceOrder(faceRecords, faceVertices))
        {
            throw QrException(QR_WRONG_ARGUMENT, "World-light face range is outside the vertex array");
        }

        converted.pFaces = faceRecords.data();
    }

    worldLights->Upload(converted, userPrint.get());

    scene->GetLightManager()->SetClusterSkyVisibility(worldLights->GetClusterSkyVisibility(),
                                                      worldLights->GetClusterCount());
}

void VulkanDevice::SetFogVolumes(uint32_t count, const QrFogVolume *pVolumes)
{
    LockSpatialFrame();

    if (count == 0 || pVolumes == nullptr)
    {
        fogVolumeCount = 0;
        fogVolumes = {};
        return;
    }

    count = std::min(count, static_cast<uint32_t>(QR_MAX_FOG_VOLUMES));

    for (uint32_t i = 0; i < count; i++)
    {
        fogVolumes[i] = spatialFrame.IsIdentity() ? pVolumes[i] : SpatialFogVolume(spatialFrame, pVolumes[i]);
    }
    fogVolumeCount = count;
}

void VulkanDevice::CreateMaterial(const QrMaterialCreateInfo *createInfo, QrMaterial *result)
{
    if (createInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    *result = textureManager->CreateMaterial
    (
        currentFrameState.GetCmdBufferForMaterials(cmdManager),
        currentFrameState.GetFrameIndex(),
        *createInfo
    );
}

void VulkanDevice::CreateAnimatedMaterial(const QrAnimatedMaterialCreateInfo *createInfo, QrMaterial *result)
{
    if (createInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    if (createInfo->frameCount == 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Animated materials must have non-zero amount of frames");
    }

    *result = textureManager->CreateAnimatedMaterial
    (
        currentFrameState.GetCmdBufferForMaterials(cmdManager),
        currentFrameState.GetFrameIndex(),
        *createInfo
    );
}

void VulkanDevice::ChangeAnimatedMaterialFrame(QrMaterial animatedMaterial, uint32_t frameIndex)
{
    if (!currentFrameState.WasFrameStarted())
    {
        throw QrException(QR_FRAME_WASNT_STARTED);
    }

    bool wasChanged = textureManager->ChangeAnimatedMaterialFrame(animatedMaterial, frameIndex);
}

void VulkanDevice::UpdateMaterial(const QrMaterialUpdateInfo *updateInfo)
{
    if (updateInfo == nullptr)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Argument is null");
    }

    // Out-of-frame calls (the live material editor restoring a snapshot from a
    // console command, before qrStartFrame) use the pre-frame command buffer,
    // exactly like CreateMaterial does.
    bool wasUpdated = textureManager->UpdateMaterial(
        currentFrameState.GetCmdBufferForMaterials(cmdManager), currentFrameState.GetFrameIndex(), *updateInfo);
}

bool VulkanDevice::CanUpdateMaterialContents(QrMaterial material, QrExtent2D size) const
{
    return textureManager->CanUpdateMaterialContents(material, size);
}

void VulkanDevice::DestroyMaterial(QrMaterial material)
{
    textureManager->DestroyMaterial(currentFrameState.GetFrameIndex(), material);
}

void VulkanDevice::CreateSkyboxCubemap(const QrCubemapCreateInfo *createInfo, QrCubemap *result)
{
    *result = cubemapManager->CreateCubemap(currentFrameState.GetCmdBufferForMaterials(cmdManager), currentFrameState.GetFrameIndex(), *createInfo);
}

void VulkanDevice::DestroyCubemap(QrCubemap cubemap)
{
    cubemapManager->DestroyCubemap(currentFrameState.GetFrameIndex(), cubemap);
}
#pragma endregion
