// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "VulkanTestContext.h"
#include "RHI/RhiPipeline.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace
{

bool NearlyEqual(float a, float b, float epsilon = 0.0001f)
{
    return std::fabs(a - b) <= epsilon;
}

}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <VertexTransformProbe.comp.spv>\n", argv[0]);
        return 1;
    }

    try
    {
        VulkanTestContext gpu;
        nvrhi::IDevice *device = gpu.context.GetDevice();

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
            .setShaderResourceOffset(0)
            .setUnorderedAccessViewOffset(0)
            .setConstantBufferOffset(0)
            .setSamplerOffset(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
        auto layout = device->createBindingLayout(layoutDesc);
        Require(layout != nullptr, "create vertex-transform layout");

        auto shader = qray::rhi::loadShader(device, argv[1], nvrhi::ShaderType::Compute, argv[1]);
        Require(shader != nullptr, "load vertex-transform probe");

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = shader;
        pipelineDesc.addBindingLayout(layout);
        auto pipeline = device->createComputePipeline(pipelineDesc);
        Require(pipeline != nullptr, "create vertex-transform pipeline");

        nvrhi::BufferDesc desc;
        desc.byteSize = 3 * sizeof(float) * 4;
        desc.structStride = sizeof(float) * 4;
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        auto output = device->createBuffer(desc);

        desc = {};
        desc.byteSize = 3 * sizeof(float) * 4;
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        auto readback = device->createBuffer(desc);

        auto set = device->createBindingSet(
            nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output)),
            layout);
        Require(output != nullptr && readback != nullptr && set != nullptr, "create vertex-transform buffers");

        auto commandList = device->createCommandList();
        commandList->open();
        nvrhi::ComputeState state;
        state.pipeline = pipeline;
        state.addBindingSet(set);
        commandList->setComputeState(state);
        commandList->dispatch(1, 1);
        commandList->copyBuffer(readback, 0, output, 0, desc.byteSize);
        commandList->close();
        device->executeCommandList(commandList);
        Require(device->waitForIdle(), "wait for vertex-transform probe");

        auto data = static_cast<const float *>(device->mapBuffer(readback, nvrhi::CpuAccessMode::Read));
        Require(data != nullptr, "map vertex-transform probe");

        const float expectedNormalLength = std::sqrt(1.0f / 4.0f + 1.0f / 9.0f + 1.0f / 16.0f);
        const float expectedNormal[3] = {
            0.5f / expectedNormalLength,
            (1.0f / 3.0f) / expectedNormalLength,
            0.25f / expectedNormalLength,
        };

        bool valid = NearlyEqual(data[0], expectedNormal[0]) &&
                     NearlyEqual(data[1], expectedNormal[1]) &&
                     NearlyEqual(data[2], expectedNormal[2]) &&
                     NearlyEqual(data[3], 24.0f) &&
                     NearlyEqual(data[7], 0.0f) &&
                     NearlyEqual(data[8], 1.0f) &&
                     NearlyEqual(data[9], -1.0f);

        const float tangentLength = std::sqrt(data[4] * data[4] + data[5] * data[5] + data[6] * data[6]);
        valid = valid && NearlyEqual(tangentLength, 1.0f);

        float values[12] = {};
        std::memcpy(values, data, sizeof(values));
        device->unmapBuffer(readback);

        if (!valid)
        {
            std::fprintf(stderr,
                         "non-uniform vertex transform mismatch: n=(%g,%g,%g) d=%g t=(%g,%g,%g) dot=%g "
                         "handedness=%g expectedHandedness=%g\n",
                         values[0], values[1], values[2], values[3], values[4], values[5], values[6],
                         values[7], values[8], values[9]);
            return 1;
        }

        Require(gpu.errors == 0, "validation errors");
        std::cout << "Non-uniform normal, tangent and handedness passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
