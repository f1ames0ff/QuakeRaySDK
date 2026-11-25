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

#include "RhiAccelStructs.h"

#include "DynamicBlasShape.h"
#include "RhiFrameContext.h"
#include "RhiPipeline.h"

#include "../ASComponent.h"
#include "../ASManager.h"
#include "../Generated/ShaderCommonC.h"
#include "../GeomInfoManager.h"
#include "../VertexCollector.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace qray;

// The two structures the instance synthesis crosses. The engine's VkAccelerationStructureInstanceKHR
// has a 48-byte row-major transform followed by the 24-bit custom index, the 8-bit mask, the 24-bit
// SBT offset, the 8-bit flags and the 64-bit BLAS reference; rt::InstanceDesc mirrors that field for
// field and flag bit for flag (nvrhi.h:1752-1783). The conversion below is field by field, so that
// the reference can become this module's BLAS handle, but the sizes and the flag bits still have to
// agree for the two records to describe the same instance.
static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64, "The engine's instance records must stay 64 bytes");
static_assert(sizeof(nvrhi::rt::InstanceDesc) == 64, "rt::InstanceDesc is the 64-byte Vulkan instance");
static_assert(sizeof(VkTransformMatrixKHR) == sizeof(nvrhi::rt::AffineTransform),
              "The engine's VkTransformMatrixKHR and rt::AffineTransform are both 12 row-major floats");
static_assert(static_cast<uint32_t>(nvrhi::rt::InstanceFlags::TriangleCullDisable) ==
                  static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR),
              "InstanceFlags and VkGeometryInstanceFlagsKHR must agree");
static_assert(static_cast<uint32_t>(nvrhi::rt::InstanceFlags::TriangleFrontCounterclockwise) ==
                  static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR),
              "InstanceFlags and VkGeometryInstanceFlagsKHR must agree");
static_assert(static_cast<uint32_t>(nvrhi::rt::InstanceFlags::ForceOpaque) ==
                  static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR),
              "InstanceFlags and VkGeometryInstanceFlagsKHR must agree");
static_assert(static_cast<uint32_t>(nvrhi::rt::InstanceFlags::ForceNonOpaque) ==
                  static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR),
              "InstanceFlags and VkGeometryInstanceFlagsKHR must agree");

// The element strides the vertex-data set binds with (its StructuredBuffer_SRV items): the sizes the
// RT shaders' std430 types have, so a change to either struct has to change the bindings as well.
static_assert(sizeof(ShVertex) == 80, "ShVertex must stay the 80-byte vertex the vertex-data set binds");
static_assert(sizeof(ShGeometryInstance) == 256,
              "ShGeometryInstance must stay the 256-byte geometry record the vertex-data set binds");

namespace
{

// The engine builds every static component with PREFER_FAST_TRACE and every dynamic one with
// PREFER_FAST_BUILD: SetupBLAS/UpdateBLAS pass fastTrace = !IsFastBuild(filter) to ASBuilder, and
// IsFastBuild is true for CF_DYNAMIC only (ASManager.cpp:460, :495, :1197-1205). NVRHI re-queries the
// build sizes at build time with these same flags and requires the create-time allocation to cover
// them (vulkan-raytracing.cpp:803-815), so the flag has to be identical here and in createAccelStruct.
constexpr nvrhi::rt::AccelStructBuildFlags STATIC_BLAS_BUILD_FLAGS =
    nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
const nvrhi::rt::AccelStructBuildFlags STATIC_MOVABLE_BLAS_BUILD_FLAGS =
    nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
const nvrhi::rt::AccelStructBuildFlags DYNAMIC_BLAS_BUILD_FLAGS =
    nvrhi::rt::AccelStructBuildFlags::PreferFastBuild | nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
constexpr nvrhi::rt::AccelStructBuildFlags TOP_LEVEL_BUILD_FLAGS =
    nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;

// The module's TLAS bound: the engine's filter grid holds one static component and one dynamic
// filter per (change frequency, pass through, primary visibility) combination, at most one instance
// each (ASManager.cpp:55-68, VertexCollectorFilterType.cpp:29-33 keeps the grid at
// MAX_TOP_LEVEL_INSTANCE_COUNT). The engine's per-instance uniform arrays index by the same order, so
// never exceeding this keeps the two indexings aligned.
constexpr uint32_t MAX_TLAS_INSTANCES = MAX_TOP_LEVEL_INSTANCE_COUNT;

// The AABB record matches VkAabbPositionsKHR (min xyz, max xyz = 24 bytes).
static_assert(sizeof(VkAabbPositionsKHR) == 24, "The particle AABB record must stay a VkAabbPositionsKHR");

// The engine's vertex-preprocessing blob, by the file name ShaderManager maps "CVertexPreprocess" to
// (ShaderManager.cpp:66): the RHI pass and the legacy renderer load the same shader.
const char *const VERTEX_PREPROCESS_SHADER_FILE_NAME = "CmVertexPreprocess.comp.spv";

// The specialization constant of that blob and the mode the RHI pass runs it in:
// [[vk::constant_id(0)]] const uint preprocessMode (CmVertexPreprocess.comp.hlsl:54), which the
// engine's own pipelines specialize to each of the three modes (VertexPreprocessing.cpp:124-168).
// VERT_PREPROC_MODE_ONLY_DYNAMIC processes exactly the instances the push mask marks dynamic, which
// is what the per-slot copies of this module need.
constexpr uint32_t VERTEX_PREPROCESS_MODE_SPEC_ID = 0;
constexpr uint32_t VERTEX_PREPROCESS_CHUNK_SPEC_ID = 1;
constexpr uint32_t VERTEX_PREPROCESS_DYNAMIC_CHUNKS = 64;

// The descriptor of a native wrap of an engine buffer that an acceleration-structure build reads.
//
//  - isAccelStructBuildInput is what the validation device checks on every vertex, index or AABB
//    buffer of a BLAS build (validation-commandlist.cpp:1342, :1451, :1487); without it the NVRHI
//    validation wrapper would reject the build. The Vulkan backend turns the flag into
//    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR only for buffers it creates
//    itself (vulkan-buffer.cpp:72-73); on a native wrap it is bookkeeping, and the engine's own
//    buffers already carry the usage bit (VertexCollector.cpp:66-84).
//  - The engine writes these buffers with its own Vulkan commands, so NVRHI cannot have seen the
//    writes and would report an unknown prior state on the first use without a claim
//    (state-tracking.cpp:290-297). Claiming AccelStructBuildInput - the state every build requires
//    (vulkan-raytracing.cpp:709-711, :1054) - with keepInitialState makes the first use
//    transition-free and keeps the claim for every later list.
//  - A buffer starts from initialState even on the very first command list, because
//    getBufferStateTracking reads it unconditionally under keepInitialState
//    (state-tracking.cpp:447-450), unlike a texture, which starts from Common and needs a
//    beginTrackingTextureState call (RhiTextureSource.h:30-44). No such call is needed here.
nvrhi::BufferDesc MakeASInputBufferDesc(uint64_t byteSize, std::string debugName)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.isAccelStructBuildInput = true;
    desc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
    desc.keepInitialState = true;
    desc.debugName = std::move(debugName);
    return desc;
}

// The descriptor of a native wrap of an engine *staging* buffer that the RHI list only copies from.
//
// The engine fills these buffers with plain host stores (persistently mapped, host coherent,
// VertexCollector.cpp:112-150), so NVRHI has never seen a write and, without a claim, would report an
// unknown prior state on the first use (state-tracking.cpp:290-297). CopySource is the state
// copyBuffer requires of the source (vulkan-buffer.cpp:249); claiming it with keepInitialState keeps
// the copy transition-free on every list, which is correct because the engine never writes the
// buffer from the GPU.
nvrhi::BufferDesc MakeCopySourceBufferDesc(uint64_t byteSize, std::string debugName)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.initialState = nvrhi::ResourceStates::CopySource;
    desc.keepInitialState = true;
    desc.debugName = std::move(debugName);
    return desc;
}

// The descriptor of a native wrap of an engine device-local buffer that an RT shader reads as a
// structured buffer (the static vertex and index buffers of the vertex-data set, set 3).
//
//  - structStride is what a StructuredBuffer binding requires: the validation device rejects a
//    buffer whose desc has none (validation-device.cpp:1693-1699) and the binding-set creation
//    asserts the same (vulkan-resource-bindings.cpp:535-536). The value is the shader's element
//    stride, not the wrapped buffer's size, and on a native wrap it is bookkeeping only.
//  - NonPixelShaderResource is the state the engine's own barriers leave these buffers in after the
//    level load (CopyFromStaging's barrier declares SHADER_READ | SHADER_WRITE for the vertex data,
//    VertexCollector.cpp:674-675) and the state a ray-tracing-visible binding layout requires of a
//    shader-resource item (state-tracking.cpp:455-464 returns NonPixelShaderResource when the layout
//    has no pixel visibility, which the RT sets use). Claiming it with keepInitialState makes the
//    first bind transition-free and keeps the claim for every later list. The engine's writes are
//    invisible to NVRHI, so without a claim the first use would report an unknown prior state
//    (state-tracking.cpp:290-297).
nvrhi::BufferDesc MakeVertexDataBufferDesc(uint64_t byteSize, uint32_t structStride,
                                           bool isVertexBuffer, bool isIndexBuffer,
                                           std::string debugName)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.structStride = structStride;

    // The flags serve the raster consumer of the same static buffers: the RHI shadow-map pass binds
    // them as vertex/index buffers and NVRHI's binding checks read the flags (the Vulkan usage bits
    // come from the engine's own creation, VertexCollector.cpp - a native wrap cannot add bits).
    desc.isVertexBuffer = isVertexBuffer;
    desc.isIndexBuffer = isIndexBuffer;

    // The RHI vertex-preprocessing pass binds the static vertex buffer as a UAV (binding 0 of its
    // set 1), so that wrap has to declare the capability too (validation-device.cpp:1709-1715); the
    // engine's own preprocessing binds the same buffer as a storage buffer, which its creation
    // carries (VertexCollector.cpp:73-77). The index wrap stays without the flag - the pass reads it
    // as an SRV.
    desc.canHaveUAVs = isVertexBuffer;

    desc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    desc.keepInitialState = true;
    desc.debugName = std::move(debugName);
    return desc;
}

// The debug name of one of this module's BLAS: the engine's name for the change-frequency /
// pass-through pair ("BLAS static opaque", VertexCollectorFilterType.cpp:177-205) plus the filter
// grid index, which disambiguates the five primary-visibility variants that share it (GetID is the
// 0..44 grid index the engine keys its dynamic build hashes by, ASManager.cpp:637).
std::string MakeBlasDebugName(VertexCollectorFilterTypeFlags filter)
{
    const char *filterName = VertexCollectorFilterTypeFlags_GetNameForBLAS(filter);

    return std::string("RHI ") + (filterName != nullptr ? filterName : "BLAS") +
           " (filter " + std::to_string(VertexCollectorFilterTypeFlags_GetID(filter)) + ")";
}

// One entry of the engine's per-surface geometry list as an NVRHI geometry descriptor.
//
// The engine's descriptors store absolute device addresses into the collector's device-local
// buffers; NVRHI takes a handle plus an offset (getBufferAddress adds the offset to the wrapped
// buffer's own device address, vulkan-raytracing.cpp:40-47), so the offsets below are the same
// subtractions GetGeometryDrawInfos performs (VertexCollector.cpp:832-835, :861-866). For a dynamic
// build the handle is this module's copy buffer instead of the engine's device-local buffer, but the
// offsets stay the same: the copy starts at 0 with the same layout, and the addresses used for the
// subtraction are the engine's device-local ones.
//
// 'transforms' is the collector's CPU-side staging array of one row-major 3x4 matrix per geometry;
// the engine's transformData address selects the matrix, exactly as GetGeometryDrawInfos does
// (VertexCollector.cpp:873-885). NVRHI copies the 12 floats into its own upload buffer at build time
// and gives the build that device address (vulkan-raytracing.cpp:120-146), so the engine's device
// transforms buffer is never referenced by NVRHI.
// The topology identity of one geometry: everything a refit build may not change (the buffers,
// their offsets and counts, the stride, the indexedness, the flags and the transform's presence),
// and deliberately not the transform values, which a refit is meant to pick up.
static void AppendGeometryIdentity(std::vector<uint64_t> &id,
                                   const VkAccelerationStructureGeometryKHR &src,
                                   const VkAccelerationStructureBuildRangeInfoKHR &range,
                                   VkDeviceAddress vertexBufferAddress,
                                   VkDeviceAddress indexBufferAddress,
                                   nvrhi::IBuffer *vertexBuffer,
                                   nvrhi::IBuffer *indexBuffer)
{
    const VkAccelerationStructureGeometryTrianglesDataKHR &triangles = src.geometry.triangles;
    const bool indexed = triangles.indexType == VK_INDEX_TYPE_UINT32;

    id.push_back(reinterpret_cast<uintptr_t>(vertexBuffer));
    id.push_back(reinterpret_cast<uintptr_t>(indexBuffer));
    id.push_back(triangles.vertexData.deviceAddress - vertexBufferAddress);
    id.push_back(triangles.vertexStride);
    id.push_back(triangles.maxVertex);
    id.push_back(indexed ? 1u : 0u);
    id.push_back(indexed ? (triangles.indexData.deviceAddress - indexBufferAddress) : 0u);
    id.push_back(indexed ? triangles.indexType : 0u);
    id.push_back(range.primitiveCount);
    id.push_back(range.primitiveOffset);
    id.push_back(range.firstVertex);
    id.push_back((src.flags & VK_GEOMETRY_OPAQUE_BIT_KHR) != 0 ? 1u : 0u);
    id.push_back(triangles.transformData.deviceAddress);
}

nvrhi::rt::GeometryDesc MakeGeometryDesc(const VkAccelerationStructureGeometryKHR &src,
                                         const VkAccelerationStructureBuildRangeInfoKHR &range,
                                         VkDeviceAddress vertexBufferAddress,
                                         VkDeviceAddress indexBufferAddress,
                                         VkDeviceAddress transformsBufferAddress,
                                         const VkTransformMatrixKHR *transforms,
                                         nvrhi::IBuffer *vertexBuffer,
                                         nvrhi::IBuffer *indexBuffer)
{
    const VkAccelerationStructureGeometryTrianglesDataKHR &srcTriangles = src.geometry.triangles;

    nvrhi::rt::GeometryTriangles triangles;
    triangles.vertexBuffer = vertexBuffer;
    triangles.vertexOffset = srcTriangles.vertexData.deviceAddress - vertexBufferAddress;
    // The engine's vertex format is R32G32B32_SFLOAT with the position as the first member of the
    // 80-byte ShVertex (VertexCollector.cpp:121-123, :285-288).
    triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
    triangles.vertexStride = srcTriangles.vertexStride;
    // The engine stores the vertex count in maxVertex (VertexCollector.cpp:286); NVRHI turns its
    // vertexCount into maxVertex = count - 1 (vulkan-raytracing.cpp:118), the highest index the
    // data can reference.
    triangles.vertexCount = srcTriangles.maxVertex;

    if (srcTriangles.indexType == VK_INDEX_TYPE_UINT32)
    {
        triangles.indexBuffer = indexBuffer;
        triangles.indexOffset = srcTriangles.indexData.deviceAddress - indexBufferAddress;
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        // NVRHI derives its primitive count as indexCount / 3 (vulkan-raytracing.cpp:177-179) and
        // the engine's range holds its own count (VertexCollector.cpp:226, :311), so this
        // reproduces it exactly.
        triangles.indexCount = range.primitiveCount * 3;
    }
    else
    {
        // Non-indexed geometry: both sides take vertexCount / 3 primitives.
        triangles.indexFormat = nvrhi::Format::UNKNOWN;
    }

    nvrhi::rt::GeometryDesc result;
    result.setTriangles(triangles);

    if (transforms != nullptr && transformsBufferAddress != 0 &&
        srcTriangles.transformData.deviceAddress >= transformsBufferAddress)
    {
        const uint32_t transformIndex =
            uint32_t((srcTriangles.transformData.deviceAddress - transformsBufferAddress) /
                     sizeof(VkTransformMatrixKHR));

        nvrhi::rt::AffineTransform transform;
        memcpy(transform, &transforms[transformIndex], sizeof(transform));
        result.setTransform(transform);
    }

    // The engine marks fully opaque geometry OPAQUE and everything else NO_DUPLICATE_ANY_HIT
    // (VertexCollector.cpp:280-281).
    result.setFlags((src.flags & VK_GEOMETRY_OPAQUE_BIT_KHR) != 0
                        ? nvrhi::rt::GeometryFlags::Opaque
                        : nvrhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation);

    return result;
}

// One record filled by ASManager::GetTLASInstanceForFilter as this module's instance: every field is
// translated, and the reference - which the engine's helper deliberately leaves alone, because a
// bare filter carries no BLAS - becomes this module's handle (the one thing the engine cannot
// supply). Field-by-field, not memcpy, so the reference is setBLAS and the flag bits are the
// explicitly asserted ones.
nvrhi::rt::InstanceDesc MakeInstanceDesc(const VkAccelerationStructureInstanceKHR &src,
                                         nvrhi::rt::IAccelStruct *blas)
{
    nvrhi::rt::InstanceDesc result;

    nvrhi::rt::AffineTransform transform;
    memcpy(transform, &src.transform, sizeof(transform));
    result.setTransform(transform);

    result.setInstanceID(src.instanceCustomIndex);
    result.setInstanceMask(src.mask);
    result.setInstanceContributionToHitGroupIndex(src.instanceShaderBindingTableRecordOffset);
    result.setFlags(static_cast<nvrhi::rt::InstanceFlags>(src.flags));
    result.setBLAS(blas);

    return result;
}

// The create-time headroom of a dynamic filter (see RhiAccelStructs.h, "Dynamic BLAS sizing and
// replacement"): from the real build descriptors and the real shape - where the shape holds five
// numbers per geometry: primitive count, vertex count (the engine's maxVertex), vertex stride,
// uint32-indexedness and transform presence - builds the padded descriptors the handle is created
// from and the padded shape that becomes its reuse envelope. Every real geometry keeps its identity
// fields and gets its primitive and vertex counts padded by 'percent' with small floors; 'percent'
// percent more slots are appended, sized from the list's mean counts and carrying the list's most
// common stride/indexedness/transform triple, so a list that fluctuates within that padding reuses
// the handle and its allocation.
struct IdentityBucket
{
    uint32_t stride = 0;
    uint32_t indexed = 0;
    uint32_t hasTransform = 0;
    size_t   firstIndex = 0;
    size_t   count = 0;
};

struct IdentityKey
{
    uint32_t stride = 0;
    uint32_t indexed = 0;
    uint32_t hasTransform = 0;
    size_t   index = 0;
};

static bool IncrementMatchingIdentityBucket(IdentityBucket *buckets,
                                            size_t bucketCount,
                                            const IdentityKey &key)
{
    for (size_t b = 0; b < bucketCount; b++)
    {
        if (buckets[b].stride == key.stride &&
            buckets[b].indexed == key.indexed &&
            buckets[b].hasTransform == key.hasTransform)
        {
            buckets[b].count++;
            return true;
        }
    }
    return false;
}

static void RecordIdentityBucket(const IdentityKey &key,
                                 IdentityBucket (&buckets)[16],
                                 size_t &bucketCount,
                                 std::vector<IdentityBucket> &overflowBuckets)
{
    if (IncrementMatchingIdentityBucket(buckets, bucketCount, key) ||
        IncrementMatchingIdentityBucket(overflowBuckets.data(), overflowBuckets.size(), key))
    {
        return;
    }

    const IdentityBucket nb{ key.stride, key.indexed, key.hasTransform, key.index, 1 };
    if (bucketCount < 16)
    {
        buckets[bucketCount++] = nb;
    }
    else
    {
        overflowBuckets.push_back(nb);
    }
}

static size_t FindDominantIdentityIndex(const std::vector<uint32_t> &shape, size_t geometryCount)
{
    IdentityBucket buckets[16];
    size_t bucketCount = 0;
    std::vector<IdentityBucket> overflowBuckets;

    for (size_t i = 0; i < geometryCount; i++)
    {
        const IdentityKey key{ shape[i * 5 + 2], shape[i * 5 + 3], shape[i * 5 + 4], i };
        RecordIdentityBucket(key, buckets, bucketCount, overflowBuckets);
    }

    size_t reference = 0;
    size_t referenceMatches = 0;
    for (size_t b = 0; b < bucketCount; b++)
    {
        if (buckets[b].count > referenceMatches)
        {
            referenceMatches = buckets[b].count;
            reference = buckets[b].firstIndex;
        }
    }
    for (const IdentityBucket &ob : overflowBuckets)
    {
        if (ob.count > referenceMatches)
        {
            referenceMatches = ob.count;
            reference = ob.firstIndex;
        }
    }
    return reference;
}

static void BuildDynamicHeadroom(const std::vector<nvrhi::rt::GeometryDesc> &geometries,
                                 const std::vector<uint32_t> &shape, uint32_t percent,
                                 std::vector<nvrhi::rt::GeometryDesc> &paddedGeometries,
                                 std::vector<uint32_t> &paddedShape)
{
    constexpr uint32_t MIN_PRIMITIVE_PADDING = 16;
    constexpr uint32_t MIN_VERTEX_PADDING = 32;
    constexpr size_t   MIN_EXTRA_SLOTS = 8;

    const size_t geometryCount = shape.size() / 5;

    if (percent == 0 || geometryCount == 0 || geometryCount != geometries.size())
    {
        return;
    }

    const auto padCount = [percent](uint32_t value, uint32_t minimum) {
        const uint32_t scaled = value + uint32_t((uint64_t(value) * percent + 99) / 100);

        return scaled > value + minimum ? scaled : value + minimum;
    };

    uint64_t primitiveSum = 0;
    uint64_t vertexSum = 0;

    for (size_t i = 0; i < geometryCount; i++)
    {
        primitiveSum += shape[i * 5 + 0];
        vertexSum += shape[i * 5 + 1];
    }

    const size_t   reference = FindDominantIdentityIndex(shape, geometryCount);

    const size_t   extraSlots = geometryCount * percent / 100 > MIN_EXTRA_SLOTS
                                    ? geometryCount * percent / 100
                                    : MIN_EXTRA_SLOTS;
    const uint32_t reservePrimitives =
        padCount(uint32_t((primitiveSum + geometryCount - 1) / geometryCount), MIN_PRIMITIVE_PADDING);
    const uint32_t reserveVertices =
        padCount(uint32_t((vertexSum + geometryCount - 1) / geometryCount), MIN_VERTEX_PADDING);

    paddedGeometries.reserve(geometries.size() + extraSlots);
    paddedShape.reserve((geometryCount + extraSlots) * 5);

    for (size_t i = 0; i < geometryCount; i++)
    {
        nvrhi::rt::GeometryDesc padded = geometries[i];
        nvrhi::rt::GeometryTriangles &triangles = padded.geometryData.triangles;
        const bool indexed = triangles.indexFormat != nvrhi::Format::UNKNOWN;
        const uint32_t primitiveCount = padCount(shape[i * 5 + 0], MIN_PRIMITIVE_PADDING);
        const uint32_t vertexCount = padCount(shape[i * 5 + 1], MIN_VERTEX_PADDING);
        // The non-indexed primitive count derives from the vertex count (vertexCount / 3), so the
        // envelope element carries that same derivation and the allocation covers it.
        const uint32_t envelopePrimitives = indexed ? primitiveCount : vertexCount / 3;

        if (indexed)
        {
            triangles.indexCount = primitiveCount * 3;
        }
        triangles.vertexCount = vertexCount;

        paddedGeometries.push_back(padded);
        paddedShape.push_back(envelopePrimitives);
        paddedShape.push_back(vertexCount);
        paddedShape.push_back(shape[i * 5 + 2]);
        paddedShape.push_back(shape[i * 5 + 3]);
        paddedShape.push_back(shape[i * 5 + 4]);
    }

    for (size_t i = 0; i < extraSlots; i++)
    {
        nvrhi::rt::GeometryDesc padded = geometries[reference];
        nvrhi::rt::GeometryTriangles &triangles = padded.geometryData.triangles;
        const bool indexed = triangles.indexFormat != nvrhi::Format::UNKNOWN;

        if (indexed)
        {
            triangles.indexCount = reservePrimitives * 3;
        }
        triangles.vertexCount = reserveVertices;

        paddedGeometries.push_back(padded);
        paddedShape.push_back(indexed ? reservePrimitives : reserveVertices / 3);
        paddedShape.push_back(reserveVertices);
        paddedShape.push_back(shape[reference * 5 + 2]);
        paddedShape.push_back(shape[reference * 5 + 3]);
        paddedShape.push_back(shape[reference * 5 + 4]);
    }
}

}

namespace qray::rhi
{

RhiAccelStructs::RhiAccelStructs() = default;

RhiAccelStructs::~RhiAccelStructs()
{
    if (device != nullptr)
    {
        // The structures and the wraps may still be referenced by a recorded, not yet finished
        // submission, and the engine's own fences prove nothing about the NVRHI queue
        // (RhiFrameContext.h). Teardown therefore waits for the device first - exactly as the frame
        // skeleton's destructor does (NvrhiFrameSkeleton.cpp:138-144) - and then drops the handles
        // directly, because the retire queue may be gone by now.
        device->waitForIdle();
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        topLevel[i] = nullptr;

        for (DynamicBlas &blas : dynamicBlas[i])
        {
            blas.handle = nullptr;
        }
        dynamicBlas[i].clear();

        dynamicCopies[i].vertex = nullptr;
        dynamicCopies[i].index = nullptr;
        dynamicCopies[i].vertexCapacity = 0;
        dynamicCopies[i].indexCapacity = 0;
        dynamicCopies[i].vertexBytes = 0;
        dynamicCopies[i].indexBytes = 0;

        stagingVertexBuffer[i] = nullptr;
        stagingIndexBuffer[i] = nullptr;

        geometryStagingBuffer[i] = nullptr;
        vertexDataCopies[i].geometryInstances = nullptr;
        vertexDataCopies[i].geometryInstancesCapacity = 0;
        vertexDataCopies[i].matchPrev = nullptr;
        vertexDataCopies[i].matchPrevCapacity = 0;
    }

    staticBlas.clear();

    vertexBuffer = nullptr;
    indexBuffer = nullptr;
    staticVertexDataBuffer = nullptr;
    staticIndexDataBuffer = nullptr;
    previousVertexBuffer = nullptr;
    previousVertexCapacity = 0;
    previousIndexBuffer = nullptr;
    previousIndexCapacity = 0;

    vertexPreprocessAllPipeline = nullptr;
    vertexPreprocessPipeline = nullptr;
    vertexPreprocessAllSpecializedShader = nullptr;
    vertexPreprocessSpecializedShader = nullptr;
    vertexPreprocessShader = nullptr;
    vertexPreprocessPushLayout = nullptr;
    vertexPreprocessVertexDataLayout = nullptr;
    vertexPreprocessUniformLayout = nullptr;
    vertexPreprocessUniformSet = nullptr;
    vertexPreprocessUniformBuffer = nullptr;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vertexPreprocessTargets[i].set = nullptr;

        for (nvrhi::IBuffer *&buffer : vertexPreprocessTargets[i].buffers)
        {
            buffer = nullptr;
        }
    }

    device = nullptr;
    frameContext = nullptr;
    asManager = nullptr;
}

bool RhiAccelStructs::Create(nvrhi::IDevice *pDevice,
                             RhiFrameContext *pFrameContext,
                             ASManager *pAsManager,
                             PrintFunction pfnPrint)
{
    if (device != nullptr)
    {
        return true;
    }

    print = std::move(pfnPrint);

    if (pDevice == nullptr || pFrameContext == nullptr || pAsManager == nullptr)
    {
        print("Warning: RHI: the acceleration structures need the RHI device, the frame context and "
              "the engine's AS manager");
        return false;
    }

    const std::shared_ptr<VertexCollector> &staticCollector = pAsManager->GetStaticCollector();
    if (staticCollector == nullptr)
    {
        print("Warning: RHI: the engine's static vertex collector is unavailable, no acceleration "
              "structures are created");
        return false;
    }

    // The wraps reference engine buffers that belong to the collectors and stay alive for the run
    // (the collectors never re-create their buffers; Reset only clears the collected geometry), so
    // one wrap per buffer is enough.
    nvrhi::BufferHandle vertex = pDevice->createHandleForNativeBuffer(
        nvrhi::ObjectTypes::VK_Buffer,
        nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(staticCollector->GetVertexBuffer()))),
        MakeASInputBufferDesc(staticCollector->GetVertexBufferSize(), "RHI static vertices (AS input)"));

    nvrhi::BufferHandle index = pDevice->createHandleForNativeBuffer(
        nvrhi::ObjectTypes::VK_Buffer,
        nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(staticCollector->GetIndexBuffer()))),
        MakeASInputBufferDesc(staticCollector->GetIndexBufferSize(), "RHI static indices (AS input)"));

    if (vertex == nullptr || index == nullptr)
    {
        print("Warning: RHI: failed to wrap the engine's static geometry buffers, no acceleration "
              "structures are created");
        return false;
    }

    // The per-slot dynamic staging buffers are the copy sources of the per-frame transfer
    // (MakeCopySourceBufferDesc above holds the state rationale); their sizes equal the device-local
    // sizes (VertexCollector.cpp:121-136), which the wrap needs for its bookkeeping desc. Slot 0
    // owns the device-local buffers, the other slots have their own staging (VertexCollector.cpp:
    // 91-110).
    nvrhi::BufferHandle stagingVertex[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BufferHandle stagingIndex[MAX_FRAMES_IN_FLIGHT];

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        const std::shared_ptr<VertexCollector> &dynamicCollector = pAsManager->GetDynamicCollector(i);
        if (dynamicCollector == nullptr)
        {
            print("Warning: RHI: the engine's dynamic vertex collector is unavailable, no "
                  "acceleration structures are created");
            return false;
        }

        const VkBuffer engineStagingVertex = dynamicCollector->GetStagingVertexBuffer();
        const VkBuffer engineStagingIndex = dynamicCollector->GetStagingIndexBuffer();
        if (engineStagingVertex == VK_NULL_HANDLE || engineStagingIndex == VK_NULL_HANDLE)
        {
            print("Warning: RHI: the engine's dynamic staging buffers are unavailable, no "
                  "acceleration structures are created");
            return false;
        }

        stagingVertex[i] = pDevice->createHandleForNativeBuffer(
            nvrhi::ObjectTypes::VK_Buffer,
            nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(engineStagingVertex))),
            MakeCopySourceBufferDesc(dynamicCollector->GetVertexBufferSize(),
                                     "RHI dynamic vertices slot " + std::to_string(i) + " (copy source)"));

        stagingIndex[i] = pDevice->createHandleForNativeBuffer(
            nvrhi::ObjectTypes::VK_Buffer,
            nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(engineStagingIndex))),
            MakeCopySourceBufferDesc(dynamicCollector->GetIndexBufferSize(),
                                     "RHI dynamic indices slot " + std::to_string(i) + " (copy source)"));

        if (stagingVertex[i] == nullptr || stagingIndex[i] == nullptr)
        {
            print("Warning: RHI: failed to wrap the engine's dynamic staging buffers, no "
                  "acceleration structures are created");
            return false;
        }
    }

    device = pDevice;
    frameContext = pFrameContext;
    asManager = pAsManager;

    // The vertex-data set (RT set 3) the traced passes read. The static buffers are wrapped a second
    // time, with the stride the StructuredBuffer bindings need and the state the engine's own level
    // load leaves them in (MakeVertexDataBufferDesc); the AS-input wraps above are deliberately left
    // without a stride, so the two uses never share a NVRHI tracking entry. The geometry manager's
    // per-slot staging buffers become the copy sources of the per-frame geometry-record copies
    // (MakeCopySourceBufferDesc holds the state rationale: the engine writes them with host stores).
    // A missing engine object or a failed wrap does not disable the AS stream - it only makes the
    // vertex-data getters answer with nulls, and the caller then keeps the set unbound.
    nvrhi::BufferHandle staticVertexData;
    nvrhi::BufferHandle staticIndexData;
    nvrhi::BufferHandle geometryStaging[MAX_FRAMES_IN_FLIGHT];

    const std::shared_ptr<GeomInfoManager> &geomInfoMgr = pAsManager->GetGeomInfoManager();
    if (geomInfoMgr == nullptr)
    {
        print("Warning: RHI: the engine's geometry-instance manager is unavailable, the RT "
              "vertex-data set is skipped");
        vertexDataCreationFailed = true;
    }

    if (!vertexDataCreationFailed)
    {
        staticVertexData = pDevice->createHandleForNativeBuffer(
            nvrhi::ObjectTypes::VK_Buffer,
            nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(staticCollector->GetVertexBuffer()))),
            MakeVertexDataBufferDesc(staticCollector->GetVertexBufferSize(), sizeof(ShVertex),
                                     true, false,
                                     "RHI static vertices (vertex data)"));

        staticIndexData = pDevice->createHandleForNativeBuffer(
            nvrhi::ObjectTypes::VK_Buffer,
            nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(staticCollector->GetIndexBuffer()))),
            MakeVertexDataBufferDesc(staticCollector->GetIndexBufferSize(), sizeof(uint32_t),
                                     false, true,
                                     "RHI static indices (vertex data)"));

        if (staticVertexData == nullptr || staticIndexData == nullptr)
        {
            print("Warning: RHI: failed to wrap the engine's static geometry buffers for the "
                  "vertex-data set");
            vertexDataCreationFailed = true;
        }
    }

    // Slot 0 and the other slots share one geometry buffer, but each slot has its own staging buffer
    // (AutoBuffer.cpp:44-55 creates one per frame), and the RHI copies read the slot's own (dynamic
    // records are written to the current slot's staging only, WriteGeomInfo's frameBegin/frameEnd).
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT && !vertexDataCreationFailed; i++)
    {
        const VkBuffer engineStaging = geomInfoMgr->GetStagingBuffer(i);
        if (engineStaging == VK_NULL_HANDLE)
        {
            print("Warning: RHI: the engine's geometry staging buffer is unavailable, the RT "
                  "vertex-data set is skipped");
            vertexDataCreationFailed = true;
            break;
        }

        geometryStaging[i] = pDevice->createHandleForNativeBuffer(
            nvrhi::ObjectTypes::VK_Buffer,
            nvrhi::Object(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(engineStaging))),
            MakeCopySourceBufferDesc(geomInfoMgr->GetBufferSize(),
                                     "RHI geometry records staging slot " + std::to_string(i) + " (copy source)"));

        if (geometryStaging[i] == nullptr)
        {
            print("Warning: RHI: failed to wrap the engine's geometry staging buffer, the RT "
                  "vertex-data set is skipped");
            vertexDataCreationFailed = true;
        }
    }

    // Every RHI-side vertex-data copy is created here, with a minimal size (the match table with its
    // full one), so that GetVertexDataBuffers answers with eight non-null handles from the first
    // frame on and the caller never has to track which of them has been needed before; the first
    // frame that brings more grows them under the doubling policy above. The contents are undefined
    // until the frame that copies or writes them, and no instance references an undefined range.
    if (!vertexDataCreationFailed)
    {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT && !vertexDataCreationFailed; i++)
        {
            const std::shared_ptr<VertexCollector> &dynamicCollector = pAsManager->GetDynamicCollector(i);

            vertexDataCreationFailed =
                !EnsureDynamicCopyBuffer(dynamicCopies[i].vertex, dynamicCopies[i].vertexCapacity,
                                         sizeof(ShVertex), uint64_t(dynamicCollector->GetVertexBufferSize()),
                                         sizeof(ShVertex), true, false, i, "vertex") ||
                !EnsureDynamicCopyBuffer(dynamicCopies[i].index, dynamicCopies[i].indexCapacity,
                                         sizeof(uint32_t), uint64_t(dynamicCollector->GetIndexBufferSize()),
                                         sizeof(uint32_t), false, true, i, "index") ||
                !EnsureVertexDataCopyBuffer(vertexDataCopies[i].geometryInstances,
                                            vertexDataCopies[i].geometryInstancesCapacity,
                                            sizeof(ShGeometryInstance), uint64_t(geomInfoMgr->GetBufferSize()),
                                            sizeof(ShGeometryInstance), i, "geometry records") ||
                !EnsureVertexDataCopyBuffer(vertexDataCopies[i].matchPrev, vertexDataCopies[i].matchPrevCapacity,
                                            uint64_t(geomInfoMgr->GetMatchPrevSize()),
                                            uint64_t(geomInfoMgr->GetMatchPrevSize()), sizeof(int32_t), i,
                                            "geometry match");
        }

        if (!vertexDataCreationFailed)
        {
            const std::shared_ptr<VertexCollector> &dynamicCollector = pAsManager->GetDynamicCollector(0);

            vertexDataCreationFailed =
                !EnsureVertexDataCopyBuffer(previousVertexBuffer, previousVertexCapacity, sizeof(ShVertex),
                                            uint64_t(dynamicCollector->GetVertexBufferSize()), sizeof(ShVertex), 0,
                                            "previous dynamic vertices") ||
                !EnsureVertexDataCopyBuffer(previousIndexBuffer, previousIndexCapacity, sizeof(uint32_t),
                                            uint64_t(dynamicCollector->GetIndexBufferSize()), sizeof(uint32_t), 0,
                                            "previous dynamic indices");
        }
    }

    vertexBuffer = std::move(vertex);
    indexBuffer = std::move(index);
    staticVertexDataBuffer = std::move(staticVertexData);
    staticIndexDataBuffer = std::move(staticIndexData);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        stagingVertexBuffer[i] = std::move(stagingVertex[i]);
        stagingIndexBuffer[i] = std::move(stagingIndex[i]);
        geometryStagingBuffer[i] = std::move(geometryStaging[i]);
    }

    // The generation the "no static set built yet" state belongs to. A later value means the engine
    // submitted a static set (see BuildStatic); aligning the counter here keeps that decision
    // correct when the first level was submitted before this object existed.
    staticGeneration = pAsManager->GetStaticGeneration();
    staticMovableRevision = pAsManager->GetStaticMovableRevision();

    return true;
}

bool RhiAccelStructs::CreateVertexPreprocessing(const char *pShaderFolderPath)
{
    if (vertexPreprocessPipeline != nullptr)
    {
        return true;
    }

    if (device == nullptr || vertexPreprocessCreationFailed)
    {
        return false;
    }

    const std::string folder = pShaderFolderPath != nullptr ? pShaderFolderPath : "";
    const std::string blobPath = folder + VERTEX_PREPROCESS_SHADER_FILE_NAME;

    // The engine's own blob, so the RHI pass and the legacy pass are the same shader. The load
    // helper stays silent about a missing or unreadable blob (RhiPipeline.h); the warning here is
    // the module's, and it is the only one - RecordVertexPreprocessing then records nothing.
    vertexPreprocessShader = rhi::loadShader(device, blobPath, nvrhi::ShaderType::Compute,
                                             "RHI vertex preprocessing");

    if (vertexPreprocessShader == nullptr)
    {
        vertexPreprocessCreationFailed = true;
        print(("Warning: RHI: cannot load the engine's vertex-preprocessing shader \"" + blobPath +
               "\", the dynamic geometry keeps the uploaded normals").c_str());
        return false;
    }

    // Set 0: the global uniform at raw binding 0, the engine's own set 0 (GlobalUniform.cpp:55-59).
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM));

        vertexPreprocessUniformLayout = device->createBindingLayout(desc);
    }

    // Set 1: the five buffers the compiled blob statically references, at their raw bindings: the
    // two vertex buffers as UAVs (the engine's own preprocessing layout binds them as storage
    // buffers, ASManager.cpp:155-195) and the two index buffers plus the geometry-instance buffer as
    // SRVs. Bindings 5..7 of the engine's layout are not in the blob's interface - the dynamic
    // branch never reads the match table or the previous-frame pair - so the layout is partial, like
    // the traced passes' set 3.
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(0)
                                   .setUnorderedAccessViewOffset(0));

        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(BINDING_VERTEX_BUFFER_STATIC));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(BINDING_VERTEX_BUFFER_DYNAMIC));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_INDEX_BUFFER_STATIC));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_INDEX_BUFFER_DYNAMIC));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_GEOMETRY_INSTANCES));

        vertexPreprocessVertexDataLayout = device->createBindingLayout(desc);
    }

    // The engine's push constant: one compute range at offset 0 of sizeof(ShVertPreprocessing)
    // (VertexPreprocessing.cpp:100-118). The descriptor-less layout carries it; the backend takes
    // the pipeline's single VkPushConstantRange from this item and creates an empty descriptor set
    // layout for the position (vulkan-resource-bindings.cpp:90-94, :1110-1140), the same shape the
    // god-rays pass uses for its push-constant layout.
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShVertPreprocessing)));

        vertexPreprocessPushLayout = device->createBindingLayout(desc);
    }

    if (vertexPreprocessUniformLayout == nullptr || vertexPreprocessVertexDataLayout == nullptr ||
        vertexPreprocessPushLayout == nullptr)
    {
        vertexPreprocessCreationFailed = true;
        print("Warning: RHI: failed to create a vertex-preprocessing binding layout, the dynamic "
              "geometry keeps the uploaded normals");
        return false;
    }

    // Specialize and build both pipelines over the same blob and layouts: dynamic-only for steady-
    // state frames, and ALL (dynamic + static) for the first traced frame after a static scene
    // upload (matching VertexPreprocessing::Preprocess).
    struct ModeSpec
    {
        uint32_t mode;
        nvrhi::ShaderHandle *specializedShader;
        nvrhi::ComputePipelineHandle *pipeline;
        const char *debugName;
    };

    const ModeSpec specs[2] =
    {
        { VERT_PREPROC_MODE_ONLY_DYNAMIC, std::addressof(vertexPreprocessSpecializedShader),
          std::addressof(vertexPreprocessPipeline), "RhiAccelStructs vertex preprocessing" },
        { VERT_PREPROC_MODE_ALL, std::addressof(vertexPreprocessAllSpecializedShader),
          std::addressof(vertexPreprocessAllPipeline), "RhiAccelStructs vertex preprocessing (all)" },
    };

    for (const ModeSpec &spec : specs)
    {
        const nvrhi::ShaderSpecialization specialization[2] =
        {
            nvrhi::ShaderSpecialization::UInt32(VERTEX_PREPROCESS_MODE_SPEC_ID, spec.mode),
            nvrhi::ShaderSpecialization::UInt32(VERTEX_PREPROCESS_CHUNK_SPEC_ID, VERTEX_PREPROCESS_DYNAMIC_CHUNKS),
        };

        *spec.specializedShader =
            device->createShaderSpecialization(vertexPreprocessShader.Get(), specialization, 2);
        if (*spec.specializedShader == nullptr)
        {
            vertexPreprocessCreationFailed = true;
            print("Warning: RHI: failed to specialize the engine's vertex-preprocessing shader, the "
                  "dynamic geometry keeps the uploaded normals");
            return false;
        }

        // The engine's set order: set 0 uniform, set 1 buffers, then the descriptor-less push-constant
        // layout. The backend places the regular layouts into descriptor sets in list order
        // (vulkan-resource-bindings.cpp:1090-1099), so the two real sets keep their engine numbers.
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(*spec.specializedShader);
        desc.addBindingLayout(vertexPreprocessUniformLayout);
        desc.addBindingLayout(vertexPreprocessVertexDataLayout);
        desc.addBindingLayout(vertexPreprocessPushLayout);

        *spec.pipeline = rhi::createComputePipeline(device, desc, spec.debugName);
        if (*spec.pipeline == nullptr)
        {
            vertexPreprocessCreationFailed = true;
            print("Warning: RHI: failed to create the vertex-preprocessing compute pipeline, the dynamic "
                  "geometry keeps the uploaded normals");
            return false;
        }
    }

    return true;
}

void RhiAccelStructs::RetireStaticSet()
{
    for (StaticBlas &blas : staticBlas)
    {
        if (blas.handle != nullptr)
        {
            frameContext->Retire(std::move(blas.handle));
        }
    }

    staticBlas.clear();
}

static bool StaticGeometryMatchesPrev(const VkAccelerationStructureGeometryKHR &srcGeom,
                                      const VkAccelerationStructureBuildRangeInfoKHR &srcRange,
                                      const nvrhi::rt::GeometryDesc &prevDesc,
                                      VkDeviceAddress vertexBufferAddress,
                                      VkDeviceAddress indexBufferAddress,
                                      VkDeviceAddress transformsBufferAddress)
{
    const VkAccelerationStructureGeometryTrianglesDataKHR &srcTri = srcGeom.geometry.triangles;
    const nvrhi::rt::GeometryTriangles &prevTri = prevDesc.geometryData.triangles;
    const bool srcIndexed = srcTri.indexType == VK_INDEX_TYPE_UINT32;
    const nvrhi::rt::GeometryFlags expectedFlag = (srcGeom.flags & VK_GEOMETRY_OPAQUE_BIT_KHR) != 0
        ? nvrhi::rt::GeometryFlags::Opaque
        : nvrhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation;
    const bool expectedTransform = srcTri.transformData.deviceAddress >= transformsBufferAddress;

    return prevDesc.geometryType == nvrhi::rt::GeometryType::Triangles &&
           prevDesc.flags == expectedFlag &&
           prevTri.vertexOffset == (srcTri.vertexData.deviceAddress - vertexBufferAddress) &&
           prevTri.vertexStride == srcTri.vertexStride &&
           prevTri.vertexCount == srcTri.maxVertex &&
           (prevTri.indexFormat != nvrhi::Format::UNKNOWN) == srcIndexed &&
           (!srcIndexed || (prevTri.indexOffset == (srcTri.indexData.deviceAddress - indexBufferAddress) &&
                            prevTri.indexCount == srcRange.primitiveCount * 3)) &&
           prevDesc.useTransform == expectedTransform;
}

RhiAccelStructs::StaticBlas *RhiAccelStructs::FindMutableStaticBlas(uint32_t filter)
{
    for (StaticBlas &blas : staticBlas)
    {
        if (blas.filter == filter)
        {
            return &blas;
        }
    }
    return nullptr;
}

bool RhiAccelStructs::CanRefitStaticMovableInPlace(const VertexCollector &collector,
                                                   const StaticCollectorMemory &mem) const
{
    for (const std::unique_ptr<BLASComponent> &component : asManager->GetStaticBlasComponents())
    {
        if (component == nullptr)
        {
            continue;
        }

        const VertexCollectorFilterTypeFlags filter = component->GetFilter();
        if ((filter & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC) ||
            !(filter & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE))
        {
            continue;
        }

        const std::vector<VkAccelerationStructureGeometryKHR> &geoms = collector.GetASGeometries(filter);
        const std::vector<VkAccelerationStructureBuildRangeInfoKHR> &ranges =
            collector.GetASBuildRangeInfos(filter);
        assert(geoms.size() == ranges.size());
        if (geoms.empty() || geoms.size() != ranges.size())
        {
            continue;
        }

        const StaticBlas *existing = FindStaticBlas(filter);
        if (existing == nullptr || existing->handle == nullptr ||
            existing->geometries.size() != geoms.size() || existing->lastBuildId.empty())
        {
            return false;
        }

        for (size_t i = 0; i < geoms.size(); i++)
        {
            if (!StaticGeometryMatchesPrev(geoms[i], ranges[i], existing->geometries[i],
                                           mem.vertexBufferAddress, mem.indexBufferAddress,
                                           mem.transformsBufferAddress))
            {
                return false;
            }
        }
    }
    return true;
}

void RhiAccelStructs::RefitStaticMovableInPlace(nvrhi::ICommandList *pCommandList,
                                                const VertexCollector &collector,
                                                const StaticCollectorMemory &mem)
{
    for (const std::unique_ptr<BLASComponent> &component : asManager->GetStaticBlasComponents())
    {
        if (component == nullptr)
        {
            continue;
        }

        const VertexCollectorFilterTypeFlags filter = component->GetFilter();
        if ((filter & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC) ||
            !(filter & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE))
        {
            continue;
        }

        const std::vector<VkAccelerationStructureGeometryKHR> &geoms = collector.GetASGeometries(filter);
        StaticBlas *existing = FindMutableStaticBlas(filter);
        if (geoms.empty() || existing == nullptr)
        {
            continue;
        }

        for (size_t i = 0; i < geoms.size(); i++)
        {
            const VkAccelerationStructureGeometryTrianglesDataKHR &srcTri = geoms[i].geometry.triangles;
            if (srcTri.transformData.deviceAddress >= mem.transformsBufferAddress)
            {
                const uint32_t transformIndex = uint32_t(
                    (srcTri.transformData.deviceAddress - mem.transformsBufferAddress) /
                    sizeof(VkTransformMatrixKHR));
                nvrhi::rt::AffineTransform transform;
                memcpy(transform, &mem.transforms[transformIndex], sizeof(transform));
                existing->geometries[i].setTransform(transform);
            }
        }

        pCommandList->buildBottomLevelAccelStruct(
            existing->handle.Get(), existing->geometries.data(), existing->geometries.size(),
            STATIC_MOVABLE_BLAS_BUILD_FLAGS | nvrhi::rt::AccelStructBuildFlags::PerformUpdate);
    }
}

void RhiAccelStructs::GatherStaticBlasCandidates(const VertexCollector &collector,
                                                 const StaticCollectorMemory &mem,
                                                 bool rebuildMovable,
                                                 std::vector<StaticBlas> &candidates,
                                                 StaticBlasCounts &counts) const
{
    for (const std::unique_ptr<BLASComponent> &component : asManager->GetStaticBlasComponents())
    {
        if (component == nullptr)
        {
            continue;
        }

        const VertexCollectorFilterTypeFlags filter = component->GetFilter();

        if (filter & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC)
        {
            continue;
        }

        if (rebuildMovable && !(filter & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE))
        {
            continue;
        }

        const std::vector<VkAccelerationStructureGeometryKHR> &geoms = collector.GetASGeometries(filter);
        const std::vector<VkAccelerationStructureBuildRangeInfoKHR> &ranges =
            collector.GetASBuildRangeInfos(filter);

        assert(geoms.size() == ranges.size());
        if (geoms.empty() || geoms.size() != ranges.size())
        {
            continue;
        }

        StaticBlas blas;
        blas.filter = filter;
        blas.debugName = MakeBlasDebugName(filter);
        blas.geometries.reserve(geoms.size());

        for (size_t i = 0; i < geoms.size(); i++)
        {
            blas.geometries.push_back(MakeGeometryDesc(geoms[i], ranges[i], mem.vertexBufferAddress,
                                                       mem.indexBufferAddress, mem.transformsBufferAddress,
                                                       mem.transforms, vertexBuffer, indexBuffer));
            counts.geometryCount++;
            counts.vertexCount += geoms[i].geometry.triangles.maxVertex;
            counts.primitiveCount += ranges[i].primitiveCount;
        }

        blas.lastBuildId.reserve(geoms.size() * 12);

        for (size_t i = 0; i < geoms.size(); i++)
        {
            AppendGeometryIdentity(blas.lastBuildId, geoms[i], ranges[i],
                                   mem.vertexBufferAddress, mem.indexBufferAddress,
                                   vertexBuffer, indexBuffer);
        }

        candidates.push_back(std::move(blas));
    }
}

void RhiAccelStructs::BuildStatic(nvrhi::ICommandList *pCommandList)
{
    if (device == nullptr || pCommandList == nullptr || asManager == nullptr || staticCreationFailed)
    {
        return;
    }

    // ASManager::SubmitStaticGeometry destroys every static component and rebuilds the non-empty
    // ones (and it runs for an empty new level too), so every handle and address this module
    // mirrored belongs to the previous level once the counter moved. Retire the whole set and fall
    // through to the rebuild; the vkDeviceWaitIdle inside SubmitStaticGeometry already drained the
    // queue, and Retire keeps the ordering contract with the frame context anyway.
    const uint32_t generation = asManager->GetStaticGeneration();
    if (generation != staticGeneration)
    {
        staticGeneration = generation;
        RetireStaticSet();
        staticBuilt = false;
        staticGeometryCount = 0;
        staticVertexCount = 0;
        staticPrimitiveCount = 0;
    }

    const uint32_t movableRevision = asManager->GetStaticMovableRevision();
    const bool rebuildMovable = staticBuilt && movableRevision != staticMovableRevision;

    if (staticBuilt && !rebuildMovable)
    {
        return;
    }

    const std::shared_ptr<VertexCollector> &collector = asManager->GetStaticCollector();
    if (collector == nullptr)
    {
        return;
    }

    const VkDeviceAddress vertexBufferAddress = collector->GetVertexBufferAddress();
    const VkDeviceAddress indexBufferAddress = collector->GetIndexBufferAddress();
    const VkDeviceAddress transformsBufferAddress = collector->GetTransformsBufferAddress();
    const VkTransformMatrixKHR *transforms = collector->GetTransformsStaging();

    if (vertexBufferAddress == 0 || indexBufferAddress == 0 || transformsBufferAddress == 0 ||
        transforms == nullptr)
    {
        // The engine's buffers carry SHADER_DEVICE_ADDRESS by construction (VertexCollector.cpp:
        // 66-84), so this is a configuration error, not a state that fixes itself.
        print("Warning: RHI: the engine's static geometry buffers have no device address, no static "
              "BLAS is built");
        staticCreationFailed = true;
        return;
    }

    const StaticCollectorMemory staticMem = {
        vertexBufferAddress, indexBufferAddress, transformsBufferAddress, transforms
    };
    if (rebuildMovable && shapeRefit && CanRefitStaticMovableInPlace(*collector, staticMem))
    {
        RefitStaticMovableInPlace(pCommandList, *collector, staticMem);
        staticMovableRevision = movableRevision;
        return;
    }

    // Gather first, create second: the create-time descriptors of an NVRHI acceleration structure
    // decide the size of its allocation (vulkan-raytracing.cpp:376-386) and the build re-queries the
    // size from the descriptors it is given (:803-815), so both calls have to describe the same
    // geometry. Collecting a frame's geometry once and using that one list for both is what makes
    // the sizes match by construction.
    std::vector<StaticBlas> candidates;
    StaticBlasCounts counts;
    GatherStaticBlasCandidates(*collector, staticMem, rebuildMovable, candidates, counts);
    const uint32_t geometryCount = counts.geometryCount;
    const uint64_t vertexCount = counts.vertexCount;
    const uint64_t primitiveCount = counts.primitiveCount;

    if (candidates.empty())
    {
        // No decision: static geometry arrives with a level load, so a later frame may still bring
        // it and the build stays pending.
        staticMovableRevision = movableRevision;
        return;
    }

    // One structure per non-empty static component, created and recorded in the same call, so that
    // the first frame that follows a level load is also the frame that builds. A movable rebuild
    // whose topology identity matches the last build refits the existing structure instead
    // (PerformUpdate), so a moving mover costs a refit rather than a recreation and a full build.
    for (StaticBlas &candidate : candidates)
    {
        const bool isMovableFilter =
            (candidate.filter & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE) != 0;
        const nvrhi::rt::AccelStructBuildFlags buildFlags =
            (rebuildMovable || isMovableFilter) ? STATIC_MOVABLE_BLAS_BUILD_FLAGS : STATIC_BLAS_BUILD_FLAGS;

        StaticBlas *existing = nullptr;

        if (rebuildMovable)
        {
            for (StaticBlas &blas : staticBlas)
            {
                if (blas.filter == candidate.filter)
                {
                    existing = &blas;
                    break;
                }
            }
        }

        if (rebuildMovable && shapeRefit && existing != nullptr && existing->handle != nullptr &&
            !existing->lastBuildId.empty() && existing->lastBuildId == candidate.lastBuildId)
        {
            pCommandList->buildBottomLevelAccelStruct(
                existing->handle.Get(), candidate.geometries.data(), candidate.geometries.size(),
                buildFlags | nvrhi::rt::AccelStructBuildFlags::PerformUpdate);

            existing->geometries = std::move(candidate.geometries);
            existing->lastBuildId = std::move(candidate.lastBuildId);
            continue;
        }

        nvrhi::rt::AccelStructDesc desc;
        desc.bottomLevelGeometries = candidate.geometries;
        desc.setBuildFlags(buildFlags);
        desc.setDebugName(candidate.debugName);

        nvrhi::rt::AccelStructHandle handle = device->createAccelStruct(desc);
        if (handle == nullptr)
        {
            print(("Warning: RHI: failed to create the BLAS \"" + candidate.debugName +
                   "\", the static acceleration structures are skipped").c_str());
            staticCreationFailed = true;
            return;
        }

        pCommandList->buildBottomLevelAccelStruct(handle.Get(), candidate.geometries.data(),
                                                  candidate.geometries.size(), buildFlags);

        candidate.handle = std::move(handle);

        if (!rebuildMovable)
        {
            staticBlas.push_back(std::move(candidate));
            continue;
        }

        if (existing == nullptr)
        {
            staticBlas.push_back(std::move(candidate));
            continue;
        }

        frameContext->Retire(std::move(existing->handle));
        existing->handle = std::move(candidate.handle);
        existing->geometries = std::move(candidate.geometries);
        existing->lastBuildId = std::move(candidate.lastBuildId);
    }

    staticMovableRevision = movableRevision;

    if (!rebuildMovable)
    {
        staticGeometryCount = geometryCount;
        staticVertexCount = vertexCount;
        staticPrimitiveCount = primitiveCount;
        staticBuilt = true;
    }
}

const RhiAccelStructs::StaticBlas *RhiAccelStructs::FindStaticBlas(uint32_t filter) const
{
    for (const StaticBlas &blas : staticBlas)
    {
        if (blas.filter == filter)
        {
            return &blas;
        }
    }

    return nullptr;
}

void RhiAccelStructs::AppendStaticInstances(uint32_t rayCullMaskWorld,
                                            bool allowGeometryWithSkyFlag,
                                            std::vector<nvrhi::rt::InstanceDesc> &instances)
{
    const std::shared_ptr<VertexCollector> &collector = asManager->GetStaticCollector();
    if (collector == nullptr)
    {
        return;
    }

    // GetStaticBlasComponents() enumerates the static components in construction order, which is the
    // filter grid order (ASManager.cpp:55-68 iterates VertexCollectorFilterTypeFlags_IterateOverFlags)
    // - the same order the engine's own prepared instance list has, and the order this module built
    // staticBlas in. Iterating the components (rather than staticBlas) keeps that order authoritative
    // and makes a non-empty component without a handle visible.
    for (const std::unique_ptr<BLASComponent> &component : asManager->GetStaticBlasComponents())
    {
        if (component == nullptr)
        {
            continue;
        }

        const VertexCollectorFilterTypeFlags filter = component->GetFilter();
        if (filter & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC)
        {
            continue;
        }

        const StaticBlas *blas = FindStaticBlas(filter);
        if (blas == nullptr || blas->handle == nullptr)
        {
            // Only a create failure (already reported) or a frame that skipped BuildStatic can get
            // here; an empty component is not an error and is simply not an instance.
            if (!collector->GetASGeometries(filter).empty())
            {
                WarnUnresolvedFilter(filter);
            }
            continue;
        }

        // The engine's own attribute rules - the mask against the cull mask, the first-person /
        // viewer / sky custom-index bits, the refract rewrite, the alpha-tested SBT offset and the
        // instance flags (ASManager.cpp:877-995). The reference is this module's BLAS.
        VkAccelerationStructureInstanceKHR record = {};
        if (!ASManager::GetTLASInstanceForFilter(filter, rayCullMaskWorld, allowGeometryWithSkyFlag, record))
        {
            continue;
        }

        // The uniform's per-instance geometry information for this instance (see
        // GetInstanceGeometryInfo): the filter's global-array offset and its geometry count - the
        // values the engine's WriteInstanceGeomInfo writes for the same filter
        // (ASManager.cpp:1016-1028). One instance per static filter, so the count is the number of
        // geometries this module's BLAS was built from, which equals the component's geometry count
        // (BLASComponent::GetGeomCount, ASManager.cpp:450).
        const uint32_t instanceIndex = static_cast<uint32_t>(instances.size());
        if (instanceIndex < MAX_TLAS_INSTANCES)
        {
            instanceGeomInfo[instanceIndex].offset =
                static_cast<int32_t>(VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(filter));
            instanceGeomInfo[instanceIndex].count = static_cast<int32_t>(blas->geometries.size());
            instanceGeomInfo[instanceIndex].filterId = VertexCollectorFilterTypeFlags_GetID(filter);
            instanceGeomInfo[instanceIndex].movable =
                (filter & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE) != 0;
            instanceGeomInfo[instanceIndex].isStatic = true;
        }

        instances.push_back(MakeInstanceDesc(record, blas->handle.Get()));
        hasGlassInstances |= (filter & uint32_t(VertexCollectorFilterTypeFlagBits::PT_GLASS)) == uint32_t(VertexCollectorFilterTypeFlagBits::PT_GLASS);
    }
}

bool RhiAccelStructs::EnsureDynamicCopyBuffer(nvrhi::BufferHandle &buffer,
                                              uint64_t &capacity,
                                              uint64_t needed,
                                              uint64_t maxCapacity,
                                              uint32_t structStride,
                                              bool isVertexBuffer,
                                              bool isIndexBuffer,
                                              uint32_t frameIndex,
                                              const char *kind)
{
    if (needed == 0 || (buffer != nullptr && capacity >= needed))
    {
        return true;
    }

    // Doubling bounds the number of replacements over a run; the cap is the collector's staging
    // size, which is also the largest prefix a frame can have (the collector asserts its counters
    // against the same buffer size, VertexCollector.cpp:236-246). The replaced buffer goes through
    // the retire queue: a submission of this slot may still be reading it.
    uint64_t newCapacity = std::max(needed, capacity * 2);
    newCapacity = std::min(newCapacity, maxCapacity);

    // The copy destination: an RHI-created buffer (so it carries TRANSFER_SRC | TRANSFER_DST,
    // vulkan-buffer.cpp:48-49, and the build-input usage the flag adds, :72-73) that copyBuffer
    // writes, the BLAS build reads and the RT shaders read as a structured buffer in the same list.
    // structStride makes the same handle bindable as a StructuredBuffer_SRV of the vertex-data set
    // (validation-device.cpp:1693-1699 and vulkan-resource-bindings.cpp:535-536 both require it);
    // adding it does not change the build, which addresses the buffer by device address. CopyDest is
    // claimed as the initial state, so the copy itself is transition-free and NVRHI emits exactly the
    // barriers the build (CopyDest -> AccelStructBuildInput, vulkan-raytracing.cpp:708-711) and the
    // shader read (-> the list's shader-resource state) need. The next list starts the buffer at
    // CopyDest again; the shared queue, the per-slot buffers and the frame context's wait for the
    // slot's previous submission (RhiFrameContext.h) cover the write-after-read against that
    // submission.
    nvrhi::BufferDesc desc;
    desc.byteSize = newCapacity;
    desc.structStride = structStride;
    desc.isAccelStructBuildInput = true;

    // The raster consumer of the same copies: the RHI shadow-map pass draws the dynamic geometry
    // from them as vertex/index buffers, so the flags (and the usage bits they add on an
    // RHI-created buffer, vulkan-buffer.cpp:51-55) follow the kind of data the call site passes.
    desc.isVertexBuffer = isVertexBuffer;
    desc.isIndexBuffer = isIndexBuffer;

    // The RHI vertex-preprocessing pass writes the dynamic vertex copy as a UAV (binding 1 of its
    // set 1), so that copy has to declare the capability (validation-device.cpp:1709-1715); on an
    // RHI-created buffer the flag adds no usage bit of its own, because structStride already
    // requests VK_BUFFER_USAGE_STORAGE_BUFFER_BIT (vulkan-buffer.cpp:63). The index copy - an SRV in
    // the same set - stays without it.
    desc.canHaveUAVs = isVertexBuffer;

    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    desc.debugName = std::string("RHI dynamic ") + kind + " slot " + std::to_string(frameIndex) + " (AS input)";

    nvrhi::BufferHandle created = device->createBuffer(desc);
    if (created == nullptr)
    {
        print(("Warning: RHI: failed to create the slot " + std::to_string(frameIndex) + " dynamic " + kind +
               " copy buffer, the dynamic acceleration structures are skipped").c_str());
        return false;
    }

    if (buffer != nullptr)
    {
        frameContext->Retire(std::move(buffer));
    }

    buffer = std::move(created);
    capacity = newCapacity;
    return true;
}

bool RhiAccelStructs::EnsureVertexDataCopyBuffer(nvrhi::BufferHandle &buffer,
                                                 uint64_t &capacity,
                                                 uint64_t needed,
                                                 uint64_t maxCapacity,
                                                 uint32_t structStride,
                                                 uint32_t frameIndex,
                                                 const char *kind)
{
    if (needed == 0 || (buffer != nullptr && capacity >= needed))
    {
        return true;
    }

    if (needed > maxCapacity)
    {
        // The engine's own bound is the maximum a frame can produce; a larger request means the
        // engine's invariant broke, and a clamp here would silently drop records.
        print(("Warning: RHI: the " + std::string(kind) + " copy needs " + std::to_string(needed) +
               " bytes, more than the engine's " + std::to_string(maxCapacity) +
               " byte buffer; the RT vertex-data set is skipped").c_str());
        return false;
    }

    uint64_t newCapacity = std::max(needed, capacity * 2);
    newCapacity = std::min(newCapacity, maxCapacity);

    // A buffer only the RT shaders read: CopyDest claimed as the initial state so the frame's own
    // copy (and the writeBuffer of the match table) is transition-free, and structStride for the
    // StructuredBuffer binding. The replaced buffer is retired - a submission of this slot may still
    // be reading it - and the slot's wait in BeginSlot covers the write-after-read.
    nvrhi::BufferDesc desc;
    desc.byteSize = newCapacity;
    desc.structStride = structStride;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    desc.debugName = std::string("RHI ") + kind + " slot " + std::to_string(frameIndex) + " (vertex data)";

    nvrhi::BufferHandle created = device->createBuffer(desc);
    if (created == nullptr)
    {
        print(("Warning: RHI: failed to create the slot " + std::to_string(frameIndex) + " " + kind +
               " copy buffer, the RT vertex-data set is skipped").c_str());
        return false;
    }

    if (buffer != nullptr)
    {
        frameContext->Retire(std::move(buffer));
    }

    buffer = std::move(created);
    capacity = newCapacity;
    return true;
}

bool RhiAccelStructs::CopyDynamicPrefixBuffers(nvrhi::ICommandList *pCommandList,
                                               uint32_t frameIndex,
                                               const VertexCollector &collector,
                                               DynamicCopies &copies)
{
    const uint64_t vertexBytes = uint64_t(collector.GetCurrentVertexCount()) * sizeof(ShVertex);
    const uint64_t indexBytes = uint64_t(collector.GetCurrentIndexCount()) * sizeof(uint32_t);

    copies.vertexBytes = vertexBytes;
    copies.indexBytes = indexBytes;

    // The used prefix starts at 0 and is contiguous: AddGeometry aligns each geometry up from the
    // running counters starting at 0 (VertexCollector.cpp:221-232) and BeginDynamicGeometry resets
    // the slot's collector before the game fills it (ASManager.cpp:736-737), so one copy per buffer
    // describes the frame exactly.
    if (vertexBytes > 0)
    {
        if (!EnsureDynamicCopyBuffer(copies.vertex, copies.vertexCapacity, vertexBytes,
                                     uint64_t(collector.GetVertexBufferSize()), sizeof(ShVertex),
                                     true, false, frameIndex, "vertex"))
        {
            dynamicCreationFailed = true;
            return false;
        }

        pCommandList->copyBuffer(copies.vertex.Get(), 0, stagingVertexBuffer[frameIndex].Get(), 0, vertexBytes);
        dynamicCopyBytes += vertexBytes;
    }

    if (indexBytes > 0)
    {
        if (!EnsureDynamicCopyBuffer(copies.index, copies.indexCapacity, indexBytes,
                                     uint64_t(collector.GetIndexBufferSize()), sizeof(uint32_t),
                                     false, true, frameIndex, "index"))
        {
            dynamicCreationFailed = true;
            return false;
        }

        pCommandList->copyBuffer(copies.index.Get(), 0, stagingIndexBuffer[frameIndex].Get(), 0, indexBytes);
        dynamicCopyBytes += indexBytes;
    }

    return true;
}

void RhiAccelStructs::AppendDynamicSlot(nvrhi::ICommandList *pCommandList,
                                        uint32_t frameIndex,
                                        uint32_t rayCullMaskWorld,
                                        bool allowGeometryWithSkyFlag,
                                        std::vector<nvrhi::rt::InstanceDesc> &instances)
{
    // Only a filter built in this frame is active: a skipped or failed dynamic frame must produce no
    // instances, while the handles of the earlier frames stay alive for TLAS builds still in flight.
    for (DynamicBlas &blas : dynamicBlas[frameIndex])
    {
        blas.active = false;
    }

    if (dynamicCreationFailed)
    {
        return;
    }

    const std::shared_ptr<VertexCollector> &collector = asManager->GetDynamicCollector(frameIndex);
    if (collector == nullptr)
    {
        return;
    }

    DynamicCopies &copies = dynamicCopies[frameIndex];

    // The used prefixes of this frame, and what the next frame's previous-frame copies must read
    // from this slot. Reset before every early return, so a frame that fails or brings nothing does
    // not leave the previous frame's lengths behind.
    copies.vertexBytes = 0;
    copies.indexBytes = 0;

    const VkDeviceAddress vertexBufferAddress = collector->GetVertexBufferAddress();
    const VkDeviceAddress indexBufferAddress = collector->GetIndexBufferAddress();
    const VkDeviceAddress transformsBufferAddress = collector->GetTransformsBufferAddress();
    const VkTransformMatrixKHR *transforms = collector->GetTransformsStaging();

    if (vertexBufferAddress == 0 || indexBufferAddress == 0 || transformsBufferAddress == 0 ||
        transforms == nullptr)
    {
        // The engine's buffers carry SHADER_DEVICE_ADDRESS by construction, so this is a
        // configuration error, not a state that fixes itself.
        dynamicCreationFailed = true;
        print("Warning: RHI: the engine's dynamic geometry buffers have no device address, no "
              "dynamic BLAS is built");
        return;
    }

    if (!CopyDynamicPrefixBuffers(pCommandList, frameIndex, *collector, copies))
    {
        return;
    }

    // The public filter iteration is the authoritative grid order (VertexCollectorFilterType.cpp:
    // 45-57), which the instance ordering and the engine's per-instance uniform arrays rely on.
    VertexCollectorFilterTypeFlags_IterateOverFlags(
        [&](VertexCollectorFilterTypeFlags filter)
        {
            if (!(filter & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC))
            {
                return;
            }

            const std::vector<VkAccelerationStructureGeometryKHR> &geoms = collector->GetASGeometries(filter);
            const std::vector<VkAccelerationStructureBuildRangeInfoKHR> &ranges =
                collector->GetASBuildRangeInfos(filter);

            assert(geoms.size() == ranges.size());
            if (geoms.empty() || geoms.size() != ranges.size())
            {
                return;
            }

            // Skip culled dynamic filters before allocating or building a BLAS.
            VkAccelerationStructureInstanceKHR record = {};
            if (!ASManager::GetTLASInstanceForFilter(filter, rayCullMaskWorld, allowGeometryWithSkyFlag, record))
            {
                return;
            }

            dynamicActiveFilterCount++;

            DynamicBlas *blas = nullptr;
            for (DynamicBlas &candidate : dynamicBlas[frameIndex])
            {
                if (candidate.filter == filter)
                {
                    blas = &candidate;
                    break;
                }
            }

            if (blas == nullptr)
            {
                dynamicBlas[frameIndex].push_back(DynamicBlas{});
                blas = &dynamicBlas[frameIndex].back();
                blas->filter = filter;
                blas->debugName = MakeBlasDebugName(filter);
            }

            // The descriptors of this frame's geometry, built from this module's copy buffers. The
            // engine's addresses only serve the offset arithmetic (copy starts at 0, same layout),
            // and the transforms come from the collector's CPU staging array, which NVRHI copies into
            // its own upload buffer at build time.
            std::vector<nvrhi::rt::GeometryDesc> &geometries = blas->geometries;
            geometries.clear();
            geometries.reserve(geoms.size());

            // The shape the handle has to cover. A size query is unreachable from here (no native
            // VkDevice in the pinned NVRHI, no ASManager wrapper), so the module keeps the handle
            // while this fits the create-time envelope and recreates it only when it does not
            // (see RhiAccelStructs.h).
            std::vector<uint32_t> &shape = blas->shapeScratch;
            shape.clear();
            shape.reserve(geoms.size() * 5);

            uint32_t listPrimitives = 0;

            for (size_t i = 0; i < geoms.size(); i++)
            {
                geometries.push_back(MakeGeometryDesc(geoms[i], ranges[i], vertexBufferAddress,
                                                      indexBufferAddress, transformsBufferAddress,
                                                      transforms, copies.vertex.Get(), copies.index.Get()));

                const VkAccelerationStructureGeometryTrianglesDataKHR &triangles = geoms[i].geometry.triangles;
                shape.push_back(ranges[i].primitiveCount);
                shape.push_back(triangles.maxVertex);
                shape.push_back(triangles.vertexStride);
                shape.push_back(triangles.indexType == VK_INDEX_TYPE_UINT32 ? 1u : 0u);
                shape.push_back(triangles.transformData.deviceAddress != 0 ? 1u : 0u);
                listPrimitives += ranges[i].primitiveCount;
            }

            std::vector<uint64_t> &buildId = blas->buildIdScratch;
            buildId.clear();
            if (shapeRefit)
            {
                buildId.reserve(geoms.size() * 12);

                for (size_t i = 0; i < geoms.size(); i++)
                {
                    AppendGeometryIdentity(buildId, geoms[i], ranges[i], vertexBufferAddress, indexBufferAddress,
                                           copies.vertex.Get(), copies.index.Get());
                }
            }

            const bool replacingHandle = blas->handle != nullptr;
            const bool shapeFits =
                replacingHandle && (sortedShapeReuse
                                        ? DynamicShapeSortedFitsCached(blas->sortedEnvelopeCounts, shape,
                                                                       sortedShapeScratch)
                                        : DynamicShapeFits(blas->shape, shape));

            DynamicFilterCounters &predecessor = FilterCountersFor(blas->filter);
            predecessor.envelopeCount = uint32_t(blas->shape.size() / 5);
            predecessor.envelopePrimitives = 0;
            for (size_t k = 0; k + 4 < blas->shape.size(); k += 5)
            {
                predecessor.envelopePrimitives += blas->shape[k];
            }
            predecessor.listPrimitives = listPrimitives;

            if (replacingHandle && !shapeFits)
            {
                DynamicFilterCounters &failures = FilterCountersFor(blas->filter);

                failures.positionalFails++;

                const int64_t failDelta = int64_t(shape.size()) - int64_t(blas->shape.size());
                failures.failDeltaSum += failDelta;
                if (failDelta > failures.failDeltaMax)
                {
                    failures.failDeltaMax = int32_t(failDelta);
                }
                if (failDelta < failures.failDeltaMin)
                {
                    failures.failDeltaMin = int32_t(failDelta);
                }

                if (!sortedShapeReuse && DynamicShapeSortedFitsCached(blas->sortedEnvelopeCounts, shape, sortedShapeScratch))
                {
                    failures.sortedPassPositionalFails++;
                }
                else
                {
                    failures.sortedFails++;

                    if (shape.size() != blas->shape.size())
                    {
                        failures.sortedCountFails++;
                    }
                    else
                    {
                        failures.sortedShapeFails++;
                    }
                }
            }

            bool createdThisFrame = false;

            if (blas->handle == nullptr || !shapeFits)
            {
                createdThisFrame = true;

                const std::chrono::steady_clock::time_point createStart = std::chrono::steady_clock::now();

                // Create from the padded list (see "Dynamic BLAS sizing and replacement" in the
                // header): the allocation covers this frame's build and every later build whose
                // shape the padded envelope accepts, and the same padded shape becomes that
                // envelope, so the reuse test and the allocation cannot disagree. With the headroom
                // off the padded list is the build list itself, which is the historical create from
                // the very list this build uses (vulkan-raytracing.cpp:376-386 against :839-851).
                // The replaced handle is not dropped while the queue may still read it - it goes
                // through the frame context's retire queue (rt::AccelStructHandle is a ref-counted
                // NVRHI handle, nvrhi.h:1835).
                std::vector<nvrhi::rt::GeometryDesc> headroomGeometries;
                std::vector<uint32_t> headroomShape;

                if (dynamicHeadroomPercent > 0)
                {
                    BuildDynamicHeadroom(geometries, shape, dynamicHeadroomPercent, headroomGeometries,
                                         headroomShape);
                }

                nvrhi::rt::AccelStructDesc desc;
                desc.bottomLevelGeometries =
                    headroomGeometries.empty() ? geometries : headroomGeometries;
                desc.setBuildFlags(DYNAMIC_BLAS_BUILD_FLAGS);
                desc.setDebugName(blas->debugName);

                nvrhi::rt::AccelStructHandle handle = device->createAccelStruct(desc);
                if (handle == nullptr)
                {
                    dynamicCreationFailed = true;
                    print(("Warning: RHI: failed to create the BLAS \"" + blas->debugName +
                           "\", the dynamic acceleration structures are skipped").c_str());
                    return;
                }

                if (blas->handle != nullptr)
                {
                    frameContext->Retire(std::move(blas->handle));
                }

                blas->handle = std::move(handle);

                if (headroomShape.empty())
                {
                    blas->shape.swap(shape);
                }
                else
                {
                    blas->shape.swap(headroomShape);
                }
                BuildSortedShapeCounts(blas->shape, blas->sortedEnvelopeCounts);

                dynamicFrameCreateMs +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createStart)
                        .count();

                if (replacingHandle)
                {
                    dynamicFrameRecreations++;
                    FilterCountersFor(blas->filter).recreates++;
                }
            }

            blas->active = true;

            nvrhi::rt::AccelStructBuildFlags buildFlags = DYNAMIC_BLAS_BUILD_FLAGS;

            if (shapeRefit && !createdThisFrame && !blas->lastBuildId.empty() &&
                blas->lastBuildId == blas->buildIdScratch)
            {
                buildFlags = buildFlags | nvrhi::rt::AccelStructBuildFlags::PerformUpdate;
            }

            pCommandList->buildBottomLevelAccelStruct(blas->handle.Get(), blas->geometries.data(),
                                                      blas->geometries.size(), buildFlags);

            if (shapeRefit)
            {
                blas->lastBuildId = blas->buildIdScratch;
            }
            else
            {
                blas->lastBuildId.clear();
            }

            dynamicFrameBuilds++;
            DynamicFilterCounters &builtCounters = FilterCountersFor(blas->filter);
            builtCounters.builds++;
            builtCounters.geometryCount = uint32_t(blas->geometries.size());
            dynamicBlasCount++;
            dynamicGeometryCount += uint32_t(blas->geometries.size());
            for (size_t i = 0; i < geoms.size(); i++)
            {
                dynamicVertexCount += geoms[i].geometry.triangles.maxVertex;
                dynamicPrimitiveCount += ranges[i].primitiveCount;
            }

            // The uniform's per-instance geometry information for this instance (see
            // GetInstanceGeometryInfo): the filter's global-array offset and its geometry count -
            // the values the engine's WriteInstanceGeomInfo writes for the same filter
            // (ASManager.cpp:1016-1028).
            const uint32_t instanceIndex = static_cast<uint32_t>(instances.size());
            if (instanceIndex < MAX_TLAS_INSTANCES)
            {
                instanceGeomInfo[instanceIndex].offset =
                    static_cast<int32_t>(VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(filter));
                instanceGeomInfo[instanceIndex].count = static_cast<int32_t>(blas->geometries.size());
                instanceGeomInfo[instanceIndex].filterId = VertexCollectorFilterTypeFlags_GetID(filter);
                instanceGeomInfo[instanceIndex].movable = false;
                instanceGeomInfo[instanceIndex].isStatic = false;

                // The vertex-preprocessing mask for this instance (see RecordVertexPreprocessing):
                // the shader reads word tlasInstanceIndex / 32 and bit tlasInstanceIndex % 32, so the
                // bits use exactly that packing. (The engine's own writer packs against
                // MAX_TOP_LEVEL_INSTANCE_COUNT instead, which misplaces every bit from instance 32
                // on; this module marks its own instance list, so it follows what the shader reads.)
                vertexPreprocessPush[frameIndex]
                    .tlasInstanceIsDynamicBits[instanceIndex / 32] |= 1u << (instanceIndex % 32);
            }

            instances.push_back(MakeInstanceDesc(record, blas->handle.Get()));
            hasGlassInstances |= (filter & uint32_t(VertexCollectorFilterTypeFlagBits::PT_GLASS)) == uint32_t(VertexCollectorFilterTypeFlagBits::PT_GLASS);
        });
}

static uint64_t WriteMatchPrevBucketSlices(nvrhi::ICommandList *pCommandList,
                                             nvrhi::IBuffer *dstBuffer,
                                             const int32_t *matchPrev,
                                             VertexCollectorFilterTypeFlagBits cf,
                                             uint32_t upperCount)
{
    uint64_t copiedBytes = 0;
    for (auto pt : VertexCollectorFilterGroup_PassThrough)
    {
        for (auto pm : VertexCollectorFilterGroup_PrimaryVisibility)
        {
            const VertexCollectorFilterTypeFlags flags = cf | pt | pm;
            const uint32_t baseIndex = VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(flags);
            const uint32_t count =
                std::min(upperCount, VertexCollectorFilterTypeFlags_GetAmountInGlobalArray(flags));
            if (count == 0)
            {
                continue;
            }

            const uint64_t byteOffset = uint64_t(baseIndex) * sizeof(int32_t);
            const size_t byteSize = size_t(count) * sizeof(int32_t);
            pCommandList->writeBuffer(dstBuffer, matchPrev + baseIndex, byteSize, byteOffset);
            copiedBytes += byteSize;
        }
    }
    return copiedBytes;
}

void RhiAccelStructs::RecordStaticTexCoordUpdates(nvrhi::ICommandList *pCommandList)
{
    if (staticVertexDataBuffer == nullptr)
    {
        return;
    }

    const std::shared_ptr<VertexCollector> &staticCollector = asManager->GetStaticCollector();
    if (staticCollector == nullptr)
    {
        return;
    }

    std::vector<VertexCollector::TexCoordUpdateRange> uvRanges;
    staticCollector->ConsumePendingTexCoordUpdates(uvRanges);
    const ShVertex *stagingVerts = staticCollector->GetStagingVertexData();
    if (uvRanges.empty() || stagingVerts == nullptr)
    {
        return;
    }

    static_assert(offsetof(ShVertex, texCoordLayer1) == offsetof(ShVertex, texCoord) + 2 * sizeof(float) &&
                  offsetof(ShVertex, texCoordLayer2) == offsetof(ShVertex, texCoord) + 4 * sizeof(float),
                  "ShVertex texCoord layers must be contiguous");
    constexpr size_t kTexCoordsOffset = offsetof(ShVertex, texCoord);
    constexpr size_t kTexCoordsSize = 6 * sizeof(float);

    for (const VertexCollector::TexCoordUpdateRange &range : uvRanges)
    {
        if (range.vertexCount == 0)
        {
            continue;
        }

        if (!range.preserveGpuNormals)
        {
            const uint64_t byteOffset = uint64_t(range.firstVertex) * sizeof(ShVertex);
            const size_t byteSize = size_t(range.vertexCount) * sizeof(ShVertex);
            pCommandList->writeBuffer(staticVertexDataBuffer.Get(),
                                      stagingVerts + range.firstVertex,
                                      byteSize, byteOffset);
            vertexDataCopyBytes += byteSize;
        }
        else
        {
            // Preserve GPU-generated ShVertex::normal written by CmVertexPreprocess.comp.
            for (uint32_t i = 0; i < range.vertexCount; i++)
            {
                const uint32_t vertIdx = range.firstVertex + i;
                const uint64_t byteOffset = uint64_t(vertIdx) * sizeof(ShVertex) + kTexCoordsOffset;
                pCommandList->writeBuffer(staticVertexDataBuffer.Get(),
                                          stagingVerts[vertIdx].texCoord,
                                          kTexCoordsSize, byteOffset);
            }
            vertexDataCopyBytes += uint64_t(range.vertexCount) * kTexCoordsSize;
        }
    }
}

void RhiAccelStructs::RecordGeometryInstanceCopies(nvrhi::ICommandList *pCommandList,
                                                   uint32_t frameIndex,
                                                   GeomInfoManager &geomInfoMgr,
                                                   VertexDataCopies &copies,
                                                   uint64_t geometryInstancesNeeded)
{
    const nvrhi::IBuffer *prevGeoPtr = copies.geometryInstances.Get();
    if (!EnsureVertexDataCopyBuffer(copies.geometryInstances, copies.geometryInstancesCapacity,
                                    geometryInstancesNeeded, uint64_t(geomInfoMgr.GetBufferSize()),
                                    sizeof(ShGeometryInstance), frameIndex, "geometry records"))
    {
        vertexDataCreationFailed = true;
        return;
    }

    if (copies.geometryInstances.Get() != prevGeoPtr)
    {
        copies.uploadedStaticFilterMask = 0;
    }

    const uint64_t staticContentRev = geomInfoMgr.GetStaticContentRevision();
    if (copies.uploadedStaticGeneration != staticGeneration ||
        copies.uploadedStaticContentRevision != staticContentRev)
    {
        copies.uploadedStaticFilterMask = 0;
        copies.uploadedStaticGeneration = staticGeneration;
        copies.uploadedStaticContentRevision = staticContentRev;
    }

    const ShGeometryInstance *stagingRecords = geomInfoMgr.GetStagingData(frameIndex);

    for (uint32_t i = 0; i < tlasInstanceCount; i++)
    {
        const uint64_t offset = uint64_t(instanceGeomInfo[i].offset) * sizeof(ShGeometryInstance);
        const uint64_t size = uint64_t(instanceGeomInfo[i].count) * sizeof(ShGeometryInstance);
        if (size == 0)
        {
            continue;
        }

        if (instanceGeomInfo[i].movable && stagingRecords != nullptr)
        {
            pCommandList->writeBuffer(copies.geometryInstances.Get(),
                                      stagingRecords + instanceGeomInfo[i].offset, size_t(size), offset);
        }
        else
        {
            const uint64_t staticBit =
                (instanceGeomInfo[i].isStatic && instanceGeomInfo[i].filterId < 64u)
                    ? (1ull << instanceGeomInfo[i].filterId)
                    : 0ull;
            if (staticBit != 0u && (copies.uploadedStaticFilterMask & staticBit) != 0u)
            {
                continue;
            }

            pCommandList->copyBuffer(copies.geometryInstances.Get(), offset,
                                     geometryStagingBuffer[frameIndex].Get(), offset, size);
            copies.uploadedStaticFilterMask |= staticBit;
        }

        vertexDataCopyBytes += size;
    }
}

void RhiAccelStructs::RecordMatchPrevUpdates(nvrhi::ICommandList *pCommandList,
                                             uint32_t frameIndex,
                                             const GeomInfoManager &geomInfoMgr,
                                             VertexDataCopies &copies)
{
    const int32_t *matchPrev = geomInfoMgr.GetMatchPrevData();
    const VkDeviceSize matchPrevSize = geomInfoMgr.GetMatchPrevSize();
    if (matchPrev == nullptr || matchPrevSize <= 0)
    {
        return;
    }

    const nvrhi::IBuffer *prevMatchPtr = copies.matchPrev.Get();
    if (!EnsureVertexDataCopyBuffer(copies.matchPrev, copies.matchPrevCapacity, uint64_t(matchPrevSize),
                                    uint64_t(matchPrevSize), sizeof(int32_t), frameIndex, "geometry match"))
    {
        vertexDataCreationFailed = true;
        return;
    }

    if (copies.matchPrev.Get() != prevMatchPtr)
    {
        copies.matchPrevInitialized = false;
        copies.uploadedMatchPrevStaticRevision = UINT64_MAX;
    }

    const uint64_t staticMatchRev = geomInfoMgr.GetStaticMatchPrevRevision();
    if (!copies.matchPrevInitialized)
    {
        pCommandList->writeBuffer(copies.matchPrev.Get(), matchPrev, size_t(matchPrevSize), 0);
        vertexDataCopyBytes += matchPrevSize;
        copies.matchPrevInitialized = true;
        copies.uploadedMatchPrevStaticRevision = staticMatchRev;
        return;
    }

    uint32_t maxStatic = 0;
    uint32_t maxDynamic = 0;
    geomInfoMgr.GetMatchPrevCopyBounds(maxStatic, maxDynamic);
    const bool uploadStaticMatch = (copies.uploadedMatchPrevStaticRevision != staticMatchRev);

    for (auto cf : VertexCollectorFilterGroup_ChangeFrequency)
    {
        const bool isDynamicCf = (cf & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC) != 0;
        if (!isDynamicCf && !uploadStaticMatch)
        {
            continue;
        }

        const uint32_t upperCount = isDynamicCf ? maxDynamic : maxStatic;
        if (upperCount > 0)
        {
            vertexDataCopyBytes +=
                WriteMatchPrevBucketSlices(pCommandList, copies.matchPrev.Get(), matchPrev, cf, upperCount);
        }
    }

    if (uploadStaticMatch)
    {
        copies.uploadedMatchPrevStaticRevision = staticMatchRev;
    }
}

void RhiAccelStructs::RecordVertexDataCopies(nvrhi::ICommandList *pCommandList, uint32_t frameIndex)
{
    if (vertexDataCreationFailed)
    {
        return;
    }

    RecordStaticTexCoordUpdates(pCommandList);

    if (tlasInstanceCount == 0)
    {
        return;
    }

    const std::shared_ptr<GeomInfoManager> &geomInfoMgr = asManager->GetGeomInfoManager();
    if (geomInfoMgr == nullptr || geometryStagingBuffer[frameIndex] == nullptr)
    {
        return;
    }

    VertexDataCopies &copies = vertexDataCopies[frameIndex];

    uint64_t geometryInstancesNeeded = 0;
    for (uint32_t i = 0; i < tlasInstanceCount; i++)
    {
        geometryInstancesNeeded =
            std::max(geometryInstancesNeeded,
                     (uint64_t(instanceGeomInfo[i].offset) + uint64_t(instanceGeomInfo[i].count)) *
                         sizeof(ShGeometryInstance));
    }

    if (geometryInstancesNeeded > 0)
    {
        RecordGeometryInstanceCopies(pCommandList, frameIndex, *geomInfoMgr, copies, geometryInstancesNeeded);
    }

    RecordMatchPrevUpdates(pCommandList, frameIndex, *geomInfoMgr, copies);
}

bool RhiAccelStructs::EnsureVertexPreprocessUniformSet(nvrhi::IBuffer *pUniformBuffer)
{
    if (vertexPreprocessUniformSet != nullptr && vertexPreprocessUniformBuffer == pUniformBuffer)
    {
        return true;
    }

    if (vertexPreprocessUniformSet != nullptr)
    {
        frameContext->Retire(vertexPreprocessUniformSet);
    }
    vertexPreprocessUniformSet = nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM, pUniformBuffer));

    vertexPreprocessUniformSet = device->createBindingSet(setDesc, vertexPreprocessUniformLayout);
    if (vertexPreprocessUniformSet == nullptr)
    {
        vertexPreprocessCreationFailed = true;
        print("Warning: RHI: failed to create the vertex-preprocessing uniform set, the dynamic "
              "geometry keeps the uploaded normals");
        return false;
    }

    vertexPreprocessUniformBuffer = pUniformBuffer;
    return true;
}

void RhiAccelStructs::RecordVertexPreprocessing(nvrhi::ICommandList *pCommandList,
                                                uint32_t frameIndex,
                                                nvrhi::IBuffer *pUniformBuffer,
                                                bool includeStatic)
{
    if (device == nullptr || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT ||
        vertexPreprocessPipeline == nullptr || vertexPreprocessCreationFailed || vertexDataCreationFailed)
    {
        return;
    }

    // The slot's push: the instance count and the dynamic-instance mask of its last BuildTopLevel.
    // No instance list (a disabled or all-culled frame) or no dynamic instance in it is nothing to
    // do unless includeStatic requested VERT_PREPROC_MODE_ALL on static geometry submission.
    const ShVertPreprocessing &push = vertexPreprocessPush[frameIndex];
    const bool hasDynamicInstance =
        (push.tlasInstanceIsDynamicBits[0] | push.tlasInstanceIsDynamicBits[1]) != 0;

    if (push.tlasInstanceCount == 0 || (!hasDynamicInstance && !includeStatic) || pUniformBuffer == nullptr)
    {
        return;
    }

    // Set 0: the frame's uniform, already written on this list by the caller (NvrhiFrameSkeleton
    // writes its patched CPU copy right before this call), so the per-instance geometry ranges the
    // pass reads describe this frame's instance list. The validation device requires
    // isConstantBuffer of a ConstantBuffer item and refuses a volatile one
    // (validation-device.cpp:1717-1730).
    if (!pUniformBuffer->getDesc().isConstantBuffer || pUniformBuffer->getDesc().isVolatile)
    {
        if (!warnedBadVertexPreprocessUniform)
        {
            warnedBadVertexPreprocessUniform = true;
            print("Warning: RHI: the vertex preprocessing needs the global uniform as a static "
                  "constant-buffer wrap, the dynamic geometry keeps the uploaded normals");
        }
        return;
    }

    // The very buffers the TLAS describes and the traced passes read: the two vertex buffers as
    // UAVs, the dynamic index buffer and the geometry records as SRVs. GetVertexDataBuffers answers
    // with the slot's current handles (a grown copy buffer is a new one), and the static vertex
    // buffer is the engine's own - the dynamic-only mode never writes it, but the shader declares it.
    const VertexDataBuffers vertexData = GetVertexDataBuffers(frameIndex);

    nvrhi::IBuffer *const buffers[VERTEX_PREPROCESS_BUFFER_COUNT] =
    {
        vertexData.staticVertices,
        vertexData.dynamicVertices,
        vertexData.staticIndices,
        vertexData.dynamicIndices,
        vertexData.geometryInstances,
    };

    for (uint32_t i = 0; i < VERTEX_PREPROCESS_BUFFER_COUNT; i++)
    {
        if (buffers[i] == nullptr)
        {
            return;
        }
    }

    if (!EnsureVertexPreprocessUniformSet(pUniformBuffer))
    {
        return;
    }

    // Set 1: rebuilt when a copy buffer was replaced (a growth), the pointer keys the same way the
    // traced passes' vertex-data set does. The replaced set goes through the retire queue, so a
    // submission that still reads its descriptors keeps them alive.
    VertexPreprocessTarget &target = vertexPreprocessTargets[frameIndex];

    bool sameBuffers = target.set != nullptr;
    for (uint32_t i = 0; i < VERTEX_PREPROCESS_BUFFER_COUNT; i++)
    {
        sameBuffers = sameBuffers && target.buffers[i] == buffers[i];
    }

    if (!sameBuffers)
    {
        if (target.set != nullptr)
        {
            frameContext->Retire(target.set);
        }
        target.set = nullptr;

        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            BINDING_VERTEX_BUFFER_STATIC, buffers[0]));
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            BINDING_VERTEX_BUFFER_DYNAMIC, buffers[1]));
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            BINDING_INDEX_BUFFER_STATIC, buffers[2]));
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            BINDING_INDEX_BUFFER_DYNAMIC, buffers[3]));
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            BINDING_GEOMETRY_INSTANCES, buffers[4]));

        target.set = device->createBindingSet(setDesc, vertexPreprocessVertexDataLayout);

        if (target.set == nullptr)
        {
            // Same as the uniform set above: a failed set creation is permanent.
            vertexPreprocessCreationFailed = true;
            print("Warning: RHI: failed to create the vertex-preprocessing binding set, the dynamic "
                  "geometry keeps the uploaded normals");
            return;
        }

        for (uint32_t i = 0; i < VERTEX_PREPROCESS_BUFFER_COUNT; i++)
        {
            target.buffers[i] = buffers[i];
        }
    }

    // One dispatch, one workgroup per (TLAS instance, geometry slice): the slice comes from the
    // group's Y index and the dynamicChunkCount specialization the RHI's pipeline sets; the
    // engine's own path keeps Y = 1, the constant's default and the historical shape
    // (VertexPreprocessing.cpp:88). The push constant is the range the descriptor-less layout
    // declared, and it has to follow the state, which setPushConstants requires (nvrhi.h:3430-3440).
    nvrhi::IComputePipeline *pipeline =
        (includeStatic && vertexPreprocessAllPipeline != nullptr)
            ? vertexPreprocessAllPipeline.Get()
            : vertexPreprocessPipeline.Get();

    nvrhi::ComputeState state;
    state.setPipeline(pipeline);
    state.addBindingSet(vertexPreprocessUniformSet);
    state.addBindingSet(target.set);

    pCommandList->setComputeState(state);
    pCommandList->setPushConstants(&push, sizeof(push));
    pCommandList->dispatch(push.tlasInstanceCount, VERTEX_PREPROCESS_DYNAMIC_CHUNKS, 1);
}

void RhiAccelStructs::WarnUnresolvedFilter(uint32_t filter)
{
    if (warnedUnresolvedInstance || print == nullptr)
    {
        return;
    }

    warnedUnresolvedInstance = true;

    print(("Warning: RHI: " + MakeBlasDebugName(filter) +
           " has geometry but no RHI BLAS; its instances are missing from the top-level structure").c_str());
}

bool RhiAccelStructs::BuildParticleProxies(nvrhi::ICommandList *pCommandList,
                                           uint32_t frameIndex,
                                           const ParticleProxy *pProxies,
                                           uint32_t proxyCount)
{
    if (pProxies == nullptr || proxyCount == 0 || proxyCount > MAX_PARTICLE_PROXY_COUNT ||
        frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return false;
    }

    // The two buffers follow the module's vertex-data copy policy: CopyDest as the claimed initial
    // state so the frame's writeBuffer is transition-free, doubles up to the bound the proxy cap
    // gives, and a replaced handle goes through the retire queue. The AABB buffer additionally
    // carries the AS build-input flag the validation device requires.
    const auto ensureBuffer = [&](nvrhi::BufferHandle &buffer, uint64_t &capacity, uint64_t needed,
                                  uint32_t stride, bool buildInput, const char *kind) -> bool {
        if (buffer != nullptr && capacity >= needed)
        {
            return true;
        }

        const uint64_t maxCapacity = uint64_t(MAX_PARTICLE_PROXY_COUNT) * stride;
        uint64_t newCapacity = std::min(std::max(needed, capacity * 2), maxCapacity);

        nvrhi::BufferDesc desc;
        desc.byteSize = newCapacity;
        desc.structStride = stride;
        desc.isAccelStructBuildInput = buildInput;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        desc.debugName = std::string("RHI ") + kind + " slot " + std::to_string(frameIndex);

        nvrhi::BufferHandle created = device->createBuffer(desc);
        if (created == nullptr)
        {
            print(("Warning: RHI: failed to create the slot " + std::to_string(frameIndex) + " " +
                   kind + " buffer, the traced particle proxies are skipped").c_str());
            return false;
        }

        if (buffer != nullptr)
        {
            frameContext->Retire(std::move(buffer));
        }

        buffer = std::move(created);
        capacity = newCapacity;
        return true;
    };

    if (!ensureBuffer(particleProxyBuffer[frameIndex], particleProxyCapacity[frameIndex],
                      uint64_t(proxyCount) * sizeof(ParticleProxy), sizeof(ParticleProxy), false,
                      "particle proxy") ||
        !ensureBuffer(particleAabbBuffer[frameIndex], particleAabbCapacity[frameIndex],
                      uint64_t(proxyCount) * sizeof(ParticleAabb), sizeof(ParticleAabb), true,
                      "particle AABB"))
    {
        return false;
    }

    pCommandList->writeBuffer(particleProxyBuffer[frameIndex].Get(), pProxies,
                              size_t(proxyCount) * sizeof(ParticleProxy), 0);

    particleAabbScratch.resize(proxyCount);
    for (uint32_t i = 0; i < proxyCount; i++)
    {
        const ParticleProxy &proxy = pProxies[i];
        for (int c = 0; c < 3; c++)
        {
            particleAabbScratch[i].min[c] = proxy.center[c] - proxy.radius;
            particleAabbScratch[i].max[c] = proxy.center[c] + proxy.radius;
        }
    }

    pCommandList->writeBuffer(particleAabbBuffer[frameIndex].Get(), particleAabbScratch.data(),
                              particleAabbScratch.size() * sizeof(ParticleAabb), 0);

    // The structure is created once with room for the whole cap and only rebuilt afterwards, so a
    // frame that brings more (or fewer) sprites never pays for a create.
    if (particleBlas[frameIndex] == nullptr)
    {
        nvrhi::rt::GeometryAABBs boxes;
        boxes.setBuffer(particleAabbBuffer[frameIndex].Get()).setOffset(0)
             .setCount(MAX_PARTICLE_PROXY_COUNT).setStride(sizeof(ParticleAabb));

        nvrhi::rt::GeometryDesc geometry;
        geometry.setAABBs(boxes);
        geometry.setFlags(nvrhi::rt::GeometryFlags::None);

        nvrhi::rt::AccelStructDesc desc;
        desc.bottomLevelGeometries = { geometry };
        desc.setBuildFlags(DYNAMIC_BLAS_BUILD_FLAGS);
        desc.setDebugName("RHI particle proxies slot " + std::to_string(frameIndex));

        particleBlas[frameIndex] = device->createAccelStruct(desc);
        if (particleBlas[frameIndex] == nullptr)
        {
            print(("Warning: RHI: failed to create the slot " + std::to_string(frameIndex) +
                   " particle proxy BLAS, the traced particle proxies are skipped").c_str());
            return false;
        }
    }

    nvrhi::rt::GeometryAABBs boxes;
    boxes.setBuffer(particleAabbBuffer[frameIndex].Get()).setOffset(0)
         .setCount(proxyCount).setStride(sizeof(ParticleAabb));

    nvrhi::rt::GeometryDesc geometry;
    geometry.setAABBs(boxes);
    geometry.setFlags(nvrhi::rt::GeometryFlags::None);

    pCommandList->buildBottomLevelAccelStruct(particleBlas[frameIndex].Get(), &geometry, 1,
                                              DYNAMIC_BLAS_BUILD_FLAGS);
    return true;
}

void RhiAccelStructs::BuildTopLevel(nvrhi::ICommandList *pCommandList,
                                    uint32_t frameIndex,
                                    uint32_t rayCullMaskWorld,
                                    bool allowGeometryWithSkyFlag,
                                    bool disableRayTracedGeometry,
                                    const ParticleProxy *pParticleProxies,
                                    uint32_t particleProxyCount)
{
    if (device == nullptr || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    // The summary is due on this frame or not; decided before the work so that its grace period
    // ticks on every frame the TLAS build is asked for, and printed after the work so that its
    // figures are this frame's.
    const bool summaryDue = IsSummaryDue();

    // This frame's figures, for the summary.
    dynamicBlasCount = 0;
    dynamicActiveFilterCount = 0;
    dynamicGeometryCount = 0;
    dynamicVertexCount = 0;
    dynamicPrimitiveCount = 0;
    dynamicCopyBytes = 0;
    vertexDataCopyBytes = 0;
    tlasInstanceCount = 0;
    hasGlassInstances = false;
    particleProxyActive[frameIndex] = false;

    // The vertex-preprocessing push describes this frame's instance list only; a frame that records
    // none (disabled or all culled) leaves a zero count, and RecordVertexPreprocessing then does
    // nothing.
    vertexPreprocessPush[frameIndex] = {};

    std::vector<nvrhi::rt::InstanceDesc> instances;
    instances.reserve(MAX_TLAS_INSTANCES);

    if (!disableRayTracedGeometry)
    {
        // The engine's order: the static components first, then the slot's dynamic filters, each in
        // filter-grid order (ASManager.cpp:1045-1049 walks allStaticBlas then allDynamicBlas[slot]).
        AppendStaticInstances(rayCullMaskWorld, allowGeometryWithSkyFlag, instances);
        AppendDynamicSlot(pCommandList, frameIndex, rayCullMaskWorld, allowGeometryWithSkyFlag, instances);
    }

    // The frame's traced particle stand-ins: the reserved slot after the engine's instances, so
    // every engine instance keeps its index in the uniform's per-instance ranges. Its BLAS and
    // buffers are built here, before the TLAS build records them, and the instance carries the
    // particle mask alone - no scene ray's cull mask includes it, only the reflect/refract raygen's
    // inline query opts in with its own mask.
    if (!disableRayTracedGeometry && pParticleProxies != nullptr && particleProxyCount > 0 &&
        instances.size() < MAX_TLAS_INSTANCES &&
        BuildParticleProxies(pCommandList, frameIndex, pParticleProxies, particleProxyCount))
    {
        nvrhi::rt::InstanceDesc instance;
        instance.setBLAS(particleBlas[frameIndex].Get());
        instance.setInstanceMask(INSTANCE_MASK_PARTICLE);
        instance.setInstanceID(0);
        instance.setInstanceContributionToHitGroupIndex(0);
        instance.setFlags(nvrhi::rt::InstanceFlags::None);
        instances.push_back(instance);

        instanceGeomInfo[instances.size() - 1] = InstanceGeomInfo{};
        particleProxyActive[frameIndex] = true;
    }

    if (instances.size() > MAX_TLAS_INSTANCES)
    {
        if (!warnedInstanceOverflow)
        {
            warnedInstanceOverflow = true;
            print(("Warning: RHI: the instance synthesis produced " + std::to_string(instances.size()) +
                   " instances, more than the " + std::to_string(MAX_TLAS_INSTANCES) +
                   " the engine's filter grid can contribute; the build is clamped").c_str());
        }

        instances.resize(MAX_TLAS_INSTANCES);
    }

    tlasInstanceCount = static_cast<uint32_t>(instances.size());

    // The instance count of the push the pass dispatches with (the shader's workgroup count); the
    // dynamic bits were set on the same instance indices by AppendDynamicSlot.
    vertexPreprocessPush[frameIndex].tlasInstanceCount = tlasInstanceCount;

    // The vertex-data set's per-frame copies (set 3): the geometry records of the instances above
    // and the previous-frame dynamic data. Recorded before the TLAS build so the pass that follows
    // in the same list finds them current; a frame without an instance list copies nothing and keeps
    // the previous frame's content, which is exactly what the TLAS it still binds describes.
    RecordVertexDataCopies(pCommandList, frameIndex);

    if (tlasInstanceCount > 0)
    {
        if (topLevel[frameIndex] == nullptr || topLevelCapacity[frameIndex] < tlasInstanceCount)
        {
            // The first build creates the structure for the instance count it is actually used with;
            // a later frame that needs more grows the slot once, straight to the module's maximum.
            // Either way a replaced structure is not dropped while the queue may still read it - it
            // goes through the frame context's retire queue.
            const uint32_t capacity =
                topLevel[frameIndex] == nullptr ? tlasInstanceCount : MAX_TLAS_INSTANCES;

            nvrhi::rt::AccelStructDesc desc;
            desc.setTopLevelMaxInstances(capacity);
            desc.setBuildFlags(TOP_LEVEL_BUILD_FLAGS);
            desc.setDebugName("RHI TLAS slot " + std::to_string(frameIndex));

            nvrhi::rt::AccelStructHandle handle = device->createAccelStruct(desc);
            if (handle == nullptr)
            {
                // Keep the previous structure (if any) and skip the build; the frame still reaches
                // the summary below.
                print("Warning: RHI: failed to create the top-level acceleration structure");
            }
            else
            {
                if (topLevel[frameIndex] != nullptr)
                {
                    frameContext->Retire(std::move(topLevel[frameIndex]));
                }

                topLevel[frameIndex] = std::move(handle);
                topLevelCapacity[frameIndex] = capacity;
            }
        }

        if (topLevel[frameIndex] != nullptr)
        {
            // The instances were synthesized on the CPU this frame (their references are this
            // module's BLAS handles), so NVRHI copies the array into its own upload buffer and the
            // TLAS of frame N describes frame N - no engine instance buffer, no one-frame lag, no
            // warm-up.
            pCommandList->buildTopLevelAccelStruct(topLevel[frameIndex].Get(), instances.data(),
                                                   instances.size(), TOP_LEVEL_BUILD_FLAGS);
        }
    }

    if (summaryDue)
    {
        PrintSummary();
    }
}

nvrhi::rt::IAccelStruct *RhiAccelStructs::GetTopLevel(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    return topLevel[frameIndex].Get();
}

nvrhi::IBuffer *RhiAccelStructs::GetParticleProxyBuffer(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || !particleProxyActive[frameIndex])
    {
        return nullptr;
    }

    return particleProxyBuffer[frameIndex].Get();
}

RhiAccelStructs::VertexDataBuffers RhiAccelStructs::GetVertexDataBuffers(uint32_t frameIndex) const
{
    VertexDataBuffers result;

    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || vertexDataCreationFailed)
    {
        return result;
    }

    const DynamicCopies &dynamic = dynamicCopies[frameIndex];
    const DynamicCopies &other = dynamicCopies[(frameIndex + 1) % MAX_FRAMES_IN_FLIGHT];
    const VertexDataCopies &copies = vertexDataCopies[frameIndex];

    result.staticVertices = staticVertexDataBuffer.Get();
    result.dynamicVertices = dynamic.vertex.Get();
    result.staticIndices = staticIndexDataBuffer.Get();
    result.dynamicIndices = dynamic.index.Get();
    result.geometryInstances = copies.geometryInstances.Get();
    result.geometryInstancesMatchPrev = copies.matchPrev.Get();
    result.previousDynamicVertices =
        (other.vertex != nullptr && other.vertexBytes > 0) ? other.vertex.Get() : previousVertexBuffer.Get();
    result.previousDynamicIndices =
        (other.index != nullptr && other.indexBytes > 0) ? other.index.Get() : previousIndexBuffer.Get();

    return result;
}

uint32_t RhiAccelStructs::GetInstanceGeometryInfo(int32_t *pInstanceGeomInfoOffset,
                                                  int32_t *pInstanceGeomInfoCount) const
{
    if (pInstanceGeomInfoOffset == nullptr || pInstanceGeomInfoCount == nullptr)
    {
        return 0;
    }

    for (uint32_t i = 0; i < tlasInstanceCount; i++)
    {
        pInstanceGeomInfoOffset[i] = instanceGeomInfo[i].offset;
        pInstanceGeomInfoCount[i] = instanceGeomInfo[i].count;
    }

    return tlasInstanceCount;
}

bool RhiAccelStructs::IsSummaryDue()
{
    if (summaryPrinted || print == nullptr)
    {
        return false;
    }

    // The line is the gate's evidence, so it must not claim a "0 static BLAS" state that a level
    // load is about to change: it waits for the static decision, with a grace period that covers a
    // scene that never brings static geometry.
    if (!staticBuilt && !staticCreationFailed)
    {
        if (summaryFrameGrace > 0)
        {
            summaryFrameGrace--;
            return false;
        }
    }

    summaryPrinted = true;
    return true;
}

void RhiAccelStructs::PrintSummary()
{
    if (print == nullptr)
    {
        return;
    }

    const uint32_t componentCount = asManager != nullptr
                                        ? static_cast<uint32_t>(asManager->GetStaticBlasComponents().size())
                                        : 0;

    char buffer[512];
    snprintf(buffer, sizeof(buffer) / sizeof(buffer[0]),
             "RHI: acceleration structures: %u/%u static BLAS, %u geometries, %llu triangles, "
             "%llu vertices; %u/%u dynamic BLAS, %u geometries, %llu triangles, %llu vertices; "
             "%llu KiB dynamic copies and %llu KiB vertex-data copies this frame; TLAS instances %u\n",
             static_cast<uint32_t>(staticBlas.size()), componentCount, staticGeometryCount,
             static_cast<unsigned long long>(staticPrimitiveCount),
             static_cast<unsigned long long>(staticVertexCount),
             dynamicBlasCount, dynamicActiveFilterCount, dynamicGeometryCount,
             static_cast<unsigned long long>(dynamicPrimitiveCount),
             static_cast<unsigned long long>(dynamicVertexCount),
             static_cast<unsigned long long>(dynamicCopyBytes / 1024),
             static_cast<unsigned long long>(vertexDataCopyBytes / 1024),
             tlasInstanceCount);
    print(buffer);
}

void RhiAccelStructs::GetDynamicFrameCounters(uint32_t *builds, uint32_t *recreations, float *createMs) const
{
    if (builds != nullptr)
    {
        *builds = dynamicFrameBuilds;
    }
    if (recreations != nullptr)
    {
        *recreations = dynamicFrameRecreations;
    }
    if (createMs != nullptr)
    {
        *createMs = static_cast<float>(dynamicFrameCreateMs);
    }
}

void RhiAccelStructs::ResetDynamicFrameCounters()
{
    dynamicFrameBuilds = 0;
    dynamicFrameRecreations = 0;
    dynamicFrameCreateMs = 0.0;
    dynamicFrameFilterCount = 0;
}

const RhiAccelStructs::DynamicFilterCounters *RhiAccelStructs::GetDynamicFilterCounters(uint32_t *count) const
{
    if (count != nullptr)
    {
        *count = dynamicFrameFilterCount;
    }
    return dynamicFrameFilters;
}

void RhiAccelStructs::SetShapeRefit(bool enable)
{
    shapeRefit = enable;
}

void RhiAccelStructs::SetSortedShapeReuse(bool enable)
{
    sortedShapeReuse = enable;
}

void RhiAccelStructs::SetDynamicHeadroom(uint32_t percent)
{
    if (percent > 100)
    {
        percent = 100;
    }

    dynamicHeadroomPercent = percent;
}

RhiAccelStructs::DynamicFilterCounters &RhiAccelStructs::FilterCountersFor(uint32_t filter)
{
    for (uint32_t i = 0; i < dynamicFrameFilterCount; i++)
    {
        if (dynamicFrameFilters[i].filter == filter)
        {
            return dynamicFrameFilters[i];
        }
    }

    if (dynamicFrameFilterCount < MAX_DYNAMIC_FILTER_COUNTERS)
    {
        dynamicFrameFilters[dynamicFrameFilterCount] = DynamicFilterCounters{};
        dynamicFrameFilters[dynamicFrameFilterCount].filter = filter;
        return dynamicFrameFilters[dynamicFrameFilterCount++];
    }

    return dynamicFrameFilterOverflow;
}

}
