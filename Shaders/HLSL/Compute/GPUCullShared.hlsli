/**
 * @file GPUCullShared.hlsli
 * @brief Shared C++/HLSL ABI for GPU-driven culling.
 *
 * Resource contract:
 *   b0 = GPUCullConstants
 *   t0 = StructuredBuffer<GPUInstanceAABB> (world-space min/max)
 *   t1 = Texture2D<float> HiZ (optional when enableHiZCull == 0)
 *   u0 = RWStructuredBuffer<uint> visibility flags, indexed by input instance
 *   u1 = RWByteAddressBuffer D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS
 */
#ifndef SPARK_GPU_CULL_SHARED_HLSLI
#define SPARK_GPU_CULL_SHARED_HLSLI

#ifdef __cplusplus

#include <cstddef>
#include <cstdint>

namespace Spark::Graphics
{
    struct alignas(16) GPUInstanceAABB
    {
        float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
        float padding0 = 0.0f;
        float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
        float padding1 = 0.0f;
    };

    struct IndirectDrawArgs
    {
        uint32_t indexCountPerInstance = 0;
        uint32_t instanceCount = 0;
        uint32_t startIndexLocation = 0;
        int32_t baseVertexLocation = 0;
        uint32_t startInstanceLocation = 0;
    };

    struct alignas(16) GPUCullConstants
    {
        float viewProjection[16] = {};
        float frustumPlanes[6][4] = {};
        uint32_t instanceCount = 0;
        uint32_t hiZWidth = 0;
        uint32_t hiZHeight = 0;
        uint32_t hiZMipCount = 0;
        uint32_t enableFrustumCull = 0;
        uint32_t enableHiZCull = 0;
        uint32_t padding0 = 0;
        uint32_t padding1 = 0;
    };

    inline constexpr uint32_t GPUCullIndirectInstanceCountOffset = sizeof(uint32_t);
    inline constexpr uint32_t GPUCullThreadGroupSize = 64;

    static_assert(sizeof(GPUInstanceAABB) == 32);
    static_assert(sizeof(IndirectDrawArgs) == 20);
    static_assert(offsetof(IndirectDrawArgs, instanceCount) == GPUCullIndirectInstanceCountOffset);
    static_assert(sizeof(GPUCullConstants) == 192);
    static_assert(offsetof(GPUCullConstants, instanceCount) == 160);
    static_assert(offsetof(GPUCullConstants, enableFrustumCull) == 176);
}

#else

struct GPUInstanceAABB
{
    float3 minimum;
    float  padding0;
    float3 maximum;
    float  padding1;
};

cbuffer GPUCullConstants : register(b0)
{
    float4x4 viewProjection;
    float4 frustumPlanes[6];
    uint instanceCount;
    uint hiZWidth;
    uint hiZHeight;
    uint hiZMipCount;
    uint enableFrustumCull;
    uint enableHiZCull;
    uint2 cullPadding;
};

#define GPU_CULL_INDIRECT_INSTANCE_COUNT_OFFSET 4
#define GPU_CULL_THREAD_GROUP_SIZE 64

#endif

#endif // SPARK_GPU_CULL_SHARED_HLSLI
