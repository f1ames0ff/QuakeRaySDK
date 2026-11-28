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

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common.h"
#include "Matrix.h"
#include "RHI/NvrhiContext.h"
#include "RHI/NvrhiRequirements.h"
#include "RHI/RhiFrameContext.h"
#include "RHI/RhiGpuParticlePass.h"
#include "RHI/RhiTextureTable.h"
#include "SpatialFrame.h"
#include "Generated/ShaderCommonC.h"

namespace
{

using qray::ShGeometryInstance;
using qray::ShGlobalUniform;
using qray::ShLightEncoded;
using qray::ShVertex;

void Require(bool cond, const std::string &msg)
{
    if (!cond)
    {
        throw std::runtime_error(msg);
    }
}

float HalfToFloat(uint16_t bits)
{
    const uint32_t exp = (bits >> 10u) & 31u;
    float value = 0.0f;
    if (exp == 0u)
    {
        value = std::ldexp(float(bits & 1023u), -24);
    }
    else if (exp == 31u)
    {
        value = std::bit_cast<float>(uint32_t(0x7F800000u | ((bits & 1023u) << 13u)));
    }
    else
    {
        value = std::ldexp(1.0f + float(bits & 1023u) / 1024.0f, int(exp) - 15);
    }
    return (bits & 0x8000u) != 0u ? -value : value;
}

class RtSmokeContext
{
public:
    std::atomic_uint errors = 0;
    bool rtSupported = false;
    qray::NvrhiContext nvrhi;

    bool Init()
    {
        uint32_t layerCount = 0;
        if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS)
        {
            return false;
        }
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        std::vector<const char *> enabledLayers;
        for (const auto &l : layers)
        {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            {
                enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
            }
        }

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "QuakeRay GPU particles smoke";
        app.apiVersion = VK_API_VERSION_1_3;

        VkDebugUtilsMessengerCreateInfoEXT debug{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = DebugCallback;
        debug.pUserData = this;

        VkInstanceCreateInfo instInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instInfo.pNext = &debug;
        instInfo.pApplicationInfo = &app;
        instInfo.enabledExtensionCount = 1;
        instInfo.ppEnabledExtensionNames = m_instExts;
        instInfo.enabledLayerCount = uint32_t(enabledLayers.size());
        instInfo.ppEnabledLayerNames = enabledLayers.data();
        if (vkCreateInstance(&instInfo, nullptr, &m_instance) != VK_SUCCESS)
        {
            return false;
        }

        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger != nullptr)
        {
            createMessenger(m_instance, &debug, nullptr, &m_messenger);
        }

        uint32_t gpuCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &gpuCount, nullptr);
        if (gpuCount == 0u)
        {
            return false;
        }
        std::vector<VkPhysicalDevice> gpus(gpuCount);
        vkEnumeratePhysicalDevices(m_instance, &gpuCount, gpus.data());

        for (VkPhysicalDevice candidate : gpus)
        {
            if (TryCreateRtDevice(candidate))
            {
                rtSupported = true;
                return true;
            }
        }
        return false;
    }

    ~RtSmokeContext()
    {
        nvrhi.Shutdown();
        if (m_vkDevice != VK_NULL_HANDLE)
        {
            vkDestroyDevice(m_vkDevice, nullptr);
        }
        if (m_messenger != VK_NULL_HANDLE)
        {
            auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyMessenger != nullptr)
            {
                destroyMessenger(m_instance, m_messenger, nullptr);
            }
        }
        if (m_instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(m_instance, nullptr);
        }
    }

private:
    const char *m_instExts[1] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDevice m_vkDevice = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;

    static bool HasExtension(const std::vector<VkExtensionProperties> &exts, const char *name)
    {
        for (const auto &e : exts)
        {
            if (std::strcmp(e.extensionName, name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool TryCreateRtDevice(VkPhysicalDevice physical)
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, exts.data());

        const char *reqExts[4] = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        };
        for (uint32_t i = 0; i < 3u; i++)
        {
            if (!HasExtension(exts, reqExts[i]))
            {
                return false;
            }
        }
        const bool hasRtPipe = HasExtension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);

        const qray::NvrhiRequirements reqs = qray::QueryNvrhiRequirements(physical);
        if (!reqs.IsCriticalSupported())
        {
            return false;
        }

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipe{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
        };
        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
        };
        VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        rtPipe.pNext = nullptr;
        rayQuery.pNext = hasRtPipe ? &rtPipe : nullptr;
        accel.pNext = &rayQuery;
        f13.pNext = &accel;
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(physical, &f2);
        if (!f12.bufferDeviceAddress || !accel.accelerationStructure || !rayQuery.rayQuery)
        {
            return false;
        }

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, queues.data());
        uint32_t family = 0;
        while (family < qCount &&
               (queues[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                   (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
        {
            family++;
        }
        if (family >= qCount)
        {
            return false;
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;

        VkDeviceCreateInfo di{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        di.pNext = &f12;
        di.pEnabledFeatures = &f2.features;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = hasRtPipe ? 4u : 3u;
        di.ppEnabledExtensionNames = reqExts;
        if (vkCreateDevice(physical, &di, nullptr, &m_vkDevice) != VK_SUCCESS)
        {
            return false;
        }

        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(m_vkDevice, family, 0, &queue);

        qray::NvrhiDeviceInfo info;
        info.instance = m_instance;
        info.physicalDevice = physical;
        info.device = m_vkDevice;
        info.graphicsQueue = queue;
        info.graphicsQueueIndex = family;
        info.instanceExtensions = m_instExts;
        info.instanceExtensionCount = 1;
        info.deviceExtensions = reqExts;
        info.deviceExtensionCount = hasRtPipe ? 4u : 3u;
        info.bufferDeviceAddressSupported = true;
        info.uniformBufferUpdateAfterBindSupported = reqs.descriptorBindingUniformBufferUpdateAfterBind;

        std::string error;
        auto print = [this](const char *s)
        {
            std::cerr << s << '\n';
            if (std::strstr(s, "ERROR") != nullptr)
            {
                errors++;
            }
        };
        return nvrhi.Init(info, print, error);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT *data,
        void *user)
    {
        auto *self = static_cast<RtSmokeContext *>(user);
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
        {
            self->errors++;
        }
        std::cerr << data->pMessage << '\n';
        return VK_FALSE;
    }
};

struct SmokeSceneResources
{
    nvrhi::BufferHandle staticVerts;
    nvrhi::BufferHandle staticIndices;
    nvrhi::BufferHandle dynamicDummy;
    nvrhi::BufferHandle indexDummy;
    nvrhi::BufferHandle geomInstances;
    nvrhi::BufferHandle matchPrev;
    nvrhi::BufferHandle uniformBuf;
    nvrhi::rt::AccelStructHandle blas;
    nvrhi::rt::AccelStructHandle tlas;

    nvrhi::BindingLayoutHandle lightLayout;
    nvrhi::BindingLayoutHandle rayStatsLayout;
    nvrhi::BufferHandle lightDummy;
    nvrhi::BufferHandle lightStatsDummy;
    nvrhi::BufferHandle rayStatsDummy;
    nvrhi::BindingSetHandle lightSet;
    nvrhi::BindingSetHandle rayStatsSet;

    nvrhi::TextureHandle color0;
    nvrhi::TextureHandle color1;
    nvrhi::TextureHandle depthTex;
    nvrhi::TextureHandle pingSh;
    nvrhi::TextureHandle pingCocg;
    nvrhi::FramebufferHandle framebuffer;
};

nvrhi::BufferHandle MakeStructBuf(nvrhi::IDevice *dev, uint64_t bytes, uint32_t stride,
                                  nvrhi::ResourceStates state, const char *name,
                                  bool uav = false, bool accelInput = false)
{
    nvrhi::BufferDesc d;
    d.byteSize = bytes;
    d.structStride = stride;
    d.canHaveUAVs = uav;
    d.isAccelStructBuildInput = accelInput;
    d.keepInitialState = true;
    d.initialState = state;
    d.debugName = name;
    return dev->createBuffer(d);
}

void BuildSmokeFloorGeom(nvrhi::IDevice *dev, SmokeSceneResources &r)
{
    // 4-vertex horizontal floor at y = 0 (X/Z spanning [-10, +10] in Y-up camera space).
    std::array<ShVertex, 4> verts = {};
    const float coords[4][3] = {
        { -10.0f, 0.0f, -10.0f },
        {  10.0f, 0.0f, -10.0f },
        {  10.0f, 0.0f,  10.0f },
        { -10.0f, 0.0f,  10.0f },
    };
    for (int i = 0; i < 4; i++)
    {
        verts[size_t(i)].position[0] = coords[i][0];
        verts[size_t(i)].position[1] = coords[i][1];
        verts[size_t(i)].position[2] = coords[i][2];
        verts[size_t(i)].normal[1] = 1.0f;
    }
    const std::array<uint32_t, 6> indices = { 0u, 1u, 2u, 0u, 2u, 3u };

    ShGeometryInstance gi = {};
    gi.model[0] = gi.model[5] = gi.model[10] = gi.model[15] = 1.0f;
    gi.prevModel[0] = gi.prevModel[5] = gi.prevModel[10] = gi.prevModel[15] = 1.0f;
    gi.baseVertexIndex = 0u;
    gi.baseIndexIndex = 0u;
    gi.prevBaseVertexIndex = UINT32_MAX;
    gi.clusterOverride = 2u;
    gi.materialColors[0][0] = gi.materialColors[0][1] = gi.materialColors[0][2] = gi.materialColors[0][3] = 1.0f;

    r.staticVerts = MakeStructBuf(dev, sizeof(verts), sizeof(ShVertex),
                                  nvrhi::ResourceStates::ShaderResource, "smokeStaticVerts", false, true);
    r.staticIndices = MakeStructBuf(dev, sizeof(indices), sizeof(uint32_t),
                                    nvrhi::ResourceStates::ShaderResource, "smokeStaticIndices", false, true);
    r.dynamicDummy = MakeStructBuf(dev, sizeof(ShVertex), sizeof(ShVertex),
                                   nvrhi::ResourceStates::ShaderResource, "smokeDynDummy");
    r.indexDummy = MakeStructBuf(dev, sizeof(uint32_t), sizeof(uint32_t),
                                 nvrhi::ResourceStates::ShaderResource, "smokeIdxDummy");
    r.geomInstances = MakeStructBuf(dev, sizeof(gi), sizeof(ShGeometryInstance),
                                    nvrhi::ResourceStates::ShaderResource, "smokeGeomInst");
    r.matchPrev = MakeStructBuf(dev, sizeof(int32_t) * 8u, sizeof(int32_t),
                                nvrhi::ResourceStates::ShaderResource, "smokeMatchPrev");

    nvrhi::BufferDesc ud;
    ud.byteSize = sizeof(ShGlobalUniform);
    ud.isConstantBuffer = true;
    ud.keepInitialState = true;
    ud.initialState = nvrhi::ResourceStates::ConstantBuffer;
    r.uniformBuf = dev->createBuffer(ud);

    ShGlobalUniform gu = {};
    const float camPos[3] = { 0.0f, 1.0f, 4.5f };
    gu.cameraPosition[0] = camPos[0];
    gu.cameraPosition[1] = camPos[1];
    gu.cameraPosition[2] = camPos[2];
    qray::Matrix::GetViewMatrix(gu.view, camPos, 0.0f, 0.0f, 0.0f);
    qray::Matrix::Inverse(gu.invView, gu.view);
    qray::Matrix::MakeProjectionMatrix(gu.projection, 1.0f, 1.0f, 0.1f, 50.0f);
    qray::Matrix::Inverse(gu.invProjection, gu.projection);
    gu.renderWidth = 64.0f;
    gu.renderHeight = 64.0f;
    gu.upscaledRenderWidth = 64.0f;
    gu.upscaledRenderHeight = 64.0f;
    gu.rayCullMaskWorld = INSTANCE_MASK_WORLD_0;
    gu.worldUpVector[1] = 1.0f;
    gu.instanceGeomInfoOffset[0] = 0;
    gu.instanceGeomCount[0] = 1;

    nvrhi::rt::GeometryDesc geomDesc;
    geomDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
    geomDesc.flags = nvrhi::rt::GeometryFlags::Opaque;
    geomDesc.geometryData.triangles
        .setVertexBuffer(r.staticVerts)
        .setVertexOffset(0)
        .setVertexStride(sizeof(ShVertex))
        .setVertexCount(4)
        .setVertexFormat(nvrhi::Format::RGB32_FLOAT)
        .setIndexBuffer(r.staticIndices)
        .setIndexOffset(0)
        .setIndexCount(6)
        .setIndexFormat(nvrhi::Format::R32_UINT);

    nvrhi::rt::AccelStructDesc blasDesc;
    blasDesc.isTopLevel = false;
    blasDesc.bottomLevelGeometries = { geomDesc };
    r.blas = dev->createAccelStruct(blasDesc);
    Require(r.blas != nullptr, "create floor BLAS");

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = 1;
    r.tlas = dev->createAccelStruct(tlasDesc);
    Require(r.tlas != nullptr, "create floor TLAS");

    auto cmd = dev->createCommandList();
    cmd->open();
    cmd->writeBuffer(r.staticVerts, verts.data(), sizeof(verts));
    cmd->writeBuffer(r.staticIndices, indices.data(), sizeof(indices));
    cmd->writeBuffer(r.geomInstances, &gi, sizeof(gi));
    cmd->writeBuffer(r.uniformBuf, &gu, sizeof(gu));
    cmd->buildBottomLevelAccelStruct(r.blas, &geomDesc, 1);

    nvrhi::rt::InstanceDesc instDesc;
    std::memcpy(instDesc.transform, nvrhi::rt::c_IdentityTransform, sizeof(instDesc.transform));
    instDesc.instanceID = 0u;
    instDesc.instanceMask = INSTANCE_MASK_WORLD_0;
    instDesc.flags = nvrhi::rt::InstanceFlags::None;
    instDesc.setBLAS(r.blas);
    cmd->buildTopLevelAccelStruct(r.tlas, &instDesc, 1);
    cmd->close();
    dev->executeCommandList(cmd);
    dev->waitForIdle();
}

void BuildSmokeTargetsAndBindings(nvrhi::IDevice *dev, SmokeSceneResources &r)
{
    nvrhi::BindingLayoutDesc ld;
    ld.visibility = nvrhi::ShaderType::AllRayTracing | nvrhi::ShaderType::Vertex |
                    nvrhi::ShaderType::Pixel | nvrhi::ShaderType::Compute;
    ld.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0).setUnorderedAccessViewOffset(0));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_OFFSETS));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_LIGHTS));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(BINDING_LIGHT_SOURCES_Q2_LIGHT_STATS));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_CLUSTER_SKY_VIS));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_DTAL_MEMBERS));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL_OFFSETS));
    ld.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL));
    r.lightLayout = dev->createBindingLayout(ld);

    nvrhi::BindingLayoutDesc rsd;
    rsd.visibility = nvrhi::ShaderType::AllGraphics | nvrhi::ShaderType::AllRayTracing;
    rsd.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
    rsd.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    r.rayStatsLayout = dev->createBindingLayout(rsd);

    r.lightDummy = MakeStructBuf(dev, sizeof(ShLightEncoded) * 4u, sizeof(ShLightEncoded),
                                 nvrhi::ResourceStates::ShaderResource, "smokeLightDummy");
    r.lightStatsDummy = MakeStructBuf(dev, 64u, sizeof(uint32_t),
                                      nvrhi::ResourceStates::UnorderedAccess, "smokeLightStats", true);
    r.rayStatsDummy = MakeStructBuf(dev, 1024u, 1024u,
                                    nvrhi::ResourceStates::UnorderedAccess, "smokeRayStats", true);

    nvrhi::BindingSetDesc lsd;
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES, r.lightDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_OFFSETS, r.lightDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_LIGHTS, r.lightDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(BINDING_LIGHT_SOURCES_Q2_LIGHT_STATS, r.lightStatsDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_CLUSTER_SKY_VIS, r.lightDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_DTAL_MEMBERS, r.lightDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL_OFFSETS, r.lightDummy));
    lsd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL, r.lightDummy));
    r.lightSet = dev->createBindingSet(lsd, r.lightLayout);

    nvrhi::BindingSetDesc rssd;
    rssd.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, r.rayStatsDummy));
    r.rayStatsSet = dev->createBindingSet(rssd, r.rayStatsLayout);

    auto makeTex = [&](nvrhi::Format fmt, bool isRt, bool isDepth, bool isUav)
    {
        nvrhi::TextureDesc td;
        td.width = 64u;
        td.height = 64u;
        td.format = fmt;
        td.isRenderTarget = isRt || isDepth;
        td.isUAV = isUav;
        td.isShaderResource = true;
        td.keepInitialState = true;
        td.initialState = isDepth ? nvrhi::ResourceStates::DepthWrite
                                  : (isUav ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::RenderTarget);
        return dev->createTexture(td);
    };
    r.color0 = makeTex(nvrhi::Format::RGBA16_FLOAT, true, false, false);
    r.color1 = makeTex(nvrhi::Format::RGBA16_FLOAT, true, false, false);
    r.depthTex = makeTex(nvrhi::Format::D32, false, true, false);
    r.pingSh = makeTex(nvrhi::Format::RGBA16_FLOAT, false, false, true);
    r.pingCocg = makeTex(nvrhi::Format::RGBA16_FLOAT, false, false, true);

    nvrhi::FramebufferDesc fbd;
    fbd.addColorAttachment(r.color0);
    fbd.addColorAttachment(r.color1);
    fbd.setDepthAttachment(r.depthTex);
    r.framebuffer = dev->createFramebuffer(fbd);
    Require(r.framebuffer != nullptr, "create particle smoke framebuffer");
}

uint32_t VerifySmokePoolAndColorReadback(nvrhi::IDevice *dev,
                                         qray::RhiGpuParticlePass &pass,
                                         const SmokeSceneResources &r)
{
    nvrhi::BufferDesc stageDesc;
    stageDesc.byteSize = uint64_t(pass.GetActiveSlotLimit()) * sizeof(qray::RhiGpuParticlePass::ShGpuParticle);
    stageDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    stageDesc.debugName = "smokePoolReadback";
    nvrhi::BufferHandle poolStage = dev->createBuffer(stageDesc);
    nvrhi::TextureDesc colStageDesc = r.color0->getDesc();
    colStageDesc.isRenderTarget = false;
    colStageDesc.keepInitialState = false;
    nvrhi::StagingTextureHandle colStage = dev->createStagingTexture(colStageDesc, nvrhi::CpuAccessMode::Read);

    auto cmd = dev->createCommandList();
    cmd->open();
    cmd->copyBuffer(poolStage, 0, pass.GetParticlePoolBuffer(), 0, stageDesc.byteSize);
    cmd->copyTexture(colStage, nvrhi::TextureSlice(), r.color0, nvrhi::TextureSlice());
    cmd->close();
    dev->executeCommandList(cmd);
    dev->waitForIdle();

    const auto *poolData = static_cast<const qray::RhiGpuParticlePass::ShGpuParticle *>(
        dev->mapBuffer(poolStage, nvrhi::CpuAccessMode::Read));
    Require(poolData != nullptr, "map particle pool readback");

    for (uint32_t i = 0; i < 8u; i++)
    {
        Require(poolData[i].posAndAge[1] >= -0.05f, "Retro square fell through floor plane!");
        Require((poolData[i].meta[0] & 0xFFu) == 2u, "Retro square mode tag lost");
        Require(poolData[i].meta[2] == 2u, "Retro square cluster was not updated from floor hit triangle");
    }

    bool movedPositiveX = false;
    for (uint32_t i = 32u; i < 40u; i++)
    {
        if (poolData[i].posAndAge[0] > 3.0f)
        {
            movedPositiveX = true;
        }
    }
    dev->unmapBuffer(poolStage);
    Require(movedPositiveX, "LUT_3D vector field did not accelerate particles along +X");

    size_t pitch = 0;
    const auto *px = static_cast<const uint8_t *>(
        dev->mapStagingTexture(colStage, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    Require(px != nullptr, "map color0 readback");
    uint32_t litPixels = 0u;
    for (uint32_t y = 0; y < 64u; y++)
    {
        const auto *row = reinterpret_cast<const uint16_t *>(px + y * pitch);
        for (uint32_t x = 0; x < 64u; x++)
        {
            const float sum = HalfToFloat(row[x * 4u + 0u]) +
                              HalfToFloat(row[x * 4u + 1u]) +
                              HalfToFloat(row[x * 4u + 2u]);
            if (sum > 0.01f)
            {
                litPixels++;
            }
        }
    }
    dev->unmapStagingTexture(colStage);
    Require(litPixels > 0u, "Rendered framebuffer has zero lit particle pixels");
    return litPixels;
}

qray::RhiGpuParticlePass::SpawnSubmission MakeModeBurst(QrGpuParticleRenderMode mode, float originX,
                                                        float originY, float velY, uint32_t flags)
{
    qray::RhiGpuParticlePass::SpawnSubmission sub = {};
    sub.burst.origin = { { originX, originY, 0.0f } };
    sub.burst.direction = { { 0.0f, velY >= 0.0f ? 1.0f : -1.0f, 0.0f } };
    sub.burst.coneHalfAngleRadians = 0.05f;
    sub.burst.count = 8u;
    sub.burst.speedMin = std::abs(velY);
    sub.burst.speedMax = std::abs(velY);
    sub.burst.sizeStartMin = 0.25f;
    sub.burst.sizeStartMax = 0.25f;
    sub.burst.sizeEndScale = 1.0f;
    sub.burst.velocityStretch = 0.03f;
    sub.burst.lifetimeMin = 5.0f;
    sub.burst.lifetimeMax = 5.0f;
    sub.burst.gravityScale = 1.0f;
    sub.burst.drag = 0.0f;
    sub.burst.restitution = 0.35f;
    sub.burst.friction = 0.7f;
    sub.burst.colorStart = { { 1.0f, 0.7f, 0.3f, 1.0f } };
    sub.burst.colorEnd = { { 1.0f, 0.5f, 0.2f, 1.0f } };
    sub.burst.emission = 5.0f;
    sub.burst.material = QR_NO_MATERIAL;
    sub.burst.renderMode = mode;
    sub.burst.flags = flags;
    return sub;
}

void RunSmokeSimulationLoop(
    nvrhi::IDevice *dev,
    qray::rhi::RhiFrameContext &frameCtx,
    qray::rhi::RhiTextureTable &texTable,
    qray::RhiGpuParticlePass &pass,
    const SmokeSceneResources &r,
    const qray::RhiGpuParticlePass::VolumeSubmission &vol,
    const std::array<qray::RhiGpuParticlePass::SpawnSubmission, 5> &spawns)
{
    float viewProj[16] = {};
    const float camPos[3] = { 0.0f, 1.0f, 4.5f };
    float view[16] = {}, proj[16] = {};
    qray::Matrix::GetViewMatrix(view, camPos, 0.0f, 0.0f, 0.0f);
    qray::Matrix::MakeProjectionMatrix(proj, 1.0f, 1.0f, 0.1f, 50.0f);
    qray::Matrix::Multiply(viewProj, view, proj);

    qray::rhi::RhiAccelStructs::VertexDataBuffers vb = {};
    vb.staticVertices = r.staticVerts;
    vb.dynamicVertices = r.dynamicDummy;
    vb.staticIndices = r.staticIndices;
    vb.dynamicIndices = r.indexDummy;
    vb.geometryInstances = r.geomInstances;
    vb.geometryInstancesMatchPrev = r.matchPrev;
    vb.previousDynamicVertices = r.dynamicDummy;
    vb.previousDynamicIndices = r.indexDummy;

    for (int step = 0; step < 30; step++)
    {
        const uint32_t fi = uint32_t(step % qray::MAX_FRAMES_IN_FLIGHT);
        frameCtx.BeginSlot(fi);
        nvrhi::ICommandList *cmd = frameCtx.GetCommandList(fi);
        texTable.TrackPendingTextures(cmd);

        if (step == 0)
        {
            cmd->clearTextureFloat(r.color0, nvrhi::AllSubresources, nvrhi::Color(0.0f));
            cmd->clearTextureFloat(r.color1, nvrhi::AllSubresources, nvrhi::Color(0.0f));
            cmd->clearDepthStencilTexture(r.depthTex, nvrhi::AllSubresources, true, 1.0f, false, 0u);
        }

        qray::RhiGpuParticlePass::SimInputs si = {};
        si.frameIndex = fi;
        si.pUniformBuffer = r.uniformBuf;
        si.pTopLevel = r.tlas;
        si.vertexData = vb;
        si.pSpawns = step == 0 ? spawns.data() : nullptr;
        si.spawnCount = step == 0 ? uint32_t(spawns.size()) : 0u;
        si.pVolumes = &vol;
        si.volumeCount = 1u;
        si.dt = 0.03f;
        si.time = float(step) * 0.03f;
        si.worldUp[1] = 1.0f;
        si.enableCollision = true;
        Require(pass.Simulate(cmd, si), "RhiGpuParticlePass::Simulate");

        if (step == 29)
        {
            qray::RhiGpuParticlePass::RenderInputs ri = {};
            ri.frameIndex = fi;
            ri.pFramebuffer = r.framebuffer;
            ri.pPingLfShTexture = r.pingSh;
            ri.pPingLfCocgTexture = r.pingCocg;
            ri.pUniformBuffer = r.uniformBuf;
            ri.pTopLevel = r.tlas;
            ri.pLightSet = r.lightSet;
            ri.pRayStatsSet = r.rayStatsSet;
            ri.pViewProj = viewProj;
            ri.width = 64u;
            ri.height = 64u;
            ri.color0Format = nvrhi::Format::RGBA16_FLOAT;
            ri.color1Format = nvrhi::Format::RGBA16_FLOAT;
            Require(pass.Render(cmd, ri), "RhiGpuParticlePass::Render");
        }
        frameCtx.EndSlot(fi);
    }
    dev->waitForIdle();
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const std::string shaderDir = argc > 1 ? (std::string(argv[1]) + "/") : "Build/";
        RtSmokeContext ctx;
        if (!ctx.Init())
        {
            std::cout << "qray_gpu_particles_smoke: skipped (RayQuery / AccelerationStructure unavailable)\n";
            return 0;
        }

        nvrhi::IDevice *dev = ctx.nvrhi.GetDevice();
        qray::rhi::RhiFrameContext frameCtx;
        Require(frameCtx.Create(dev, qray::MAX_FRAMES_IN_FLIGHT), "RhiFrameContext::Create");
        qray::rhi::RhiTextureTable texTable;
        Require(texTable.Create(dev, 64u, &frameCtx), "RhiTextureTable::Create");

        SmokeSceneResources r;
        BuildSmokeFloorGeom(dev, r);
        BuildSmokeTargetsAndBindings(dev, r);

        qray::RhiGpuParticlePass pass;
        qray::RhiGpuParticlePass::CreateInfo ci = {};
        ci.pDevice = dev;
        ci.pFrameContext = &frameCtx;
        ci.pTextureTable = &texTable;
        ci.pShaderFolderPath = shaderDir.c_str();
        ci.maxParticles = 256u;
        Require(pass.Create(ci), "RhiGpuParticlePass::Create");
        pass.SetBorrowedLayouts(r.lightLayout, r.rayStatsLayout);

        // Allocate an LUT_3D brick driving particles along +X.
        std::vector<float> lutVecs(2u * 2u * 2u * 3u, 0.0f);
        for (size_t i = 0; i < 8u; i++)
        {
            lutVecs[i * 3u + 0u] = 4.0f;
        }
        const QrVectorFieldCreateInfo vfi = { 2u, 2u, 2u, lutVecs.data() };
        std::vector<uint16_t> brick;
        qray::RhiGpuParticlePass::ResampleVectorFieldToBrick(vfi, qray::SpatialFrame(), brick);
        const uint32_t lutSlot = pass.AllocateVectorFieldSlot(brick);
        Require(lutSlot != UINT32_MAX, "AllocateVectorFieldSlot");

        qray::RhiGpuParticlePass::VolumeSubmission vol = {};
        vol.volume.kind = QR_VECTOR_FIELD_KIND_LUT_3D;
        vol.volume.worldFromLocal.matrix[0][0] = 8.0f;
        vol.volume.worldFromLocal.matrix[1][1] = 8.0f;
        vol.volume.worldFromLocal.matrix[2][2] = 8.0f;
        vol.volume.worldFromLocal.matrix[0][3] = 2.0f;
        vol.volume.intensity = 1.0f;
        vol.volume.maxSpeed = 6.0f;
        vol.volume.tightness = 0.9f;
        vol.volume.edgeFalloff = 0.0f;
        vol.atlasSlot = lutSlot;

        // Spawn 4 modes: floor-colliding retro square + sparks + billboard + debris,
        // plus one set starting inside the LUT vector field at x = 2.0 to verify lateral deflection.
        const uint32_t colFlags = QR_GPU_PARTICLE_FLAG_COLLIDE_WORLD_BIT |
                                  QR_GPU_PARTICLE_FLAG_REST_ON_FLOOR_BIT |
                                  QR_GPU_PARTICLE_FLAG_SELF_LIT_BIT;
        std::array<qray::RhiGpuParticlePass::SpawnSubmission, 5> spawns = {
            MakeModeBurst(QR_GPU_PARTICLE_RENDER_RETRO_SQUARE, -1.0f, 0.45f, -3.5f, colFlags),
            MakeModeBurst(QR_GPU_PARTICLE_RENDER_SPARK,         0.0f, 0.6f,  -1.0f, colFlags),
            MakeModeBurst(QR_GPU_PARTICLE_RENDER_BILLBOARD,    -0.5f, 0.8f,   0.0f, colFlags),
            MakeModeBurst(QR_GPU_PARTICLE_RENDER_DEBRIS_3D,     0.5f, 0.8f,  -2.0f, colFlags),
            MakeModeBurst(QR_GPU_PARTICLE_RENDER_BILLBOARD,     2.0f, 1.0f,   0.0f,
                          QR_GPU_PARTICLE_FLAG_AFFECTED_BY_VECTOR_FIELDS_BIT | QR_GPU_PARTICLE_FLAG_SELF_LIT_BIT),
        };

        RunSmokeSimulationLoop(dev, frameCtx, texTable, pass, r, vol, spawns);

        const uint32_t litPixels = VerifySmokePoolAndColorReadback(dev, pass, r);
        pass.ReleaseTargets();
        frameCtx.WaitForIdle();
        Require(ctx.errors == 0u, "Vulkan validation errors during GPU particle smoke");
        std::cout << "qray_gpu_particles_smoke: passed (litPixels=" << litPixels << ")\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "qray_gpu_particles_smoke failed: " << ex.what() << '\n';
        return 1;
    }
}
