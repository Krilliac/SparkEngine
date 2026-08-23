/**
 * @file GPUCull.hlsl
 * @brief GPU compute shader for frustum + HiZ occlusion culling
 *
 * Tests each instance AABB against the view frustum and previous frame's
 * hierarchical Z-buffer. Visible instances set per-input visibility flags and
 * atomically update DrawIndexedInstancedIndirect's instance count.
 *
 * Dispatch: ceil(instanceCount / GPU_CULL_THREAD_GROUP_SIZE) groups.
 */

#include "GPUCullShared.hlsli"

StructuredBuffer<GPUInstanceAABB> boundingBoxes : register(t0);
Texture2D<float>                   hiZTexture : register(t1);

// One flag per input instance. C++ clears the buffer before each dispatch.
RWStructuredBuffer<uint> visibilityFlags : register(u0);

// D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS. The shader atomically updates
// instanceCount at byte offset 4; C++ initializes the other four fields.
RWByteAddressBuffer indirectArgs : register(u1);

// Test AABB against 6 frustum planes
bool FrustumTest(float3 center, float3 extents)
{
    [unroll]
    for (uint i = 0; i < 6; i++)
    {
        float3 normal = frustumPlanes[i].xyz;
        float  dist = frustumPlanes[i].w;

        // Effective radius: project extents onto plane normal
        float r = dot(extents, abs(normal));

        // Signed distance from center to plane
        float d = dot(normal, center) + dist;

        // If the nearest point is behind the plane, AABB is fully outside
        if (d + r < 0.0)
            return false;
    }
    return true;
}

// Transform AABB to screen-space and test against HiZ
bool HiZOcclusionTest(float3 center, float3 extents)
{
    // Project all 8 AABB corners to clip space
    float2 minScreen = float2(1.0, 1.0);
    float2 maxScreen = float2(0.0, 0.0);
    float  nearestZ = 1.0;

    [unroll]
    for (uint i = 0; i < 8; i++)
    {
        float3 corner = center + extents * float3(
            (i & 1) ? 1.0 : -1.0,
            (i & 2) ? 1.0 : -1.0,
            (i & 4) ? 1.0 : -1.0);

        float4 clip = mul(float4(corner, 1.0), viewProjection);

        // Behind camera
        if (clip.w <= 0.0)
            return true; // Conservative: visible if any corner behind camera

        float3 ndc = clip.xyz / clip.w;

        // NDC to UV [0,1]
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;

        minScreen = min(minScreen, uv);
        maxScreen = max(maxScreen, uv);
        nearestZ = min(nearestZ, ndc.z);
    }

    // Clamp to screen
    minScreen = saturate(minScreen);
    maxScreen = saturate(maxScreen);

    // Select HiZ mip based on screen-space extent
    float2 screenExtent = (maxScreen - minScreen) * float2(hiZWidth, hiZHeight);
    uint mipLevel = (uint)ceil(log2(max(max(screenExtent.x, screenExtent.y), 1.0)));
    mipLevel = min(mipLevel, hiZMipCount - 1);

    uint2 mipSize = max(uint2(1, 1), uint2(hiZWidth, hiZHeight) >> mipLevel);
    uint2 maxCoord = mipSize - 1;

    // Texture.Load avoids a sampler-state dependency in this compute pass.
    #define SAMPLE_HIZ(uv) hiZTexture.Load(int3(min((uint2)(saturate(uv) * mipSize), maxCoord), mipLevel))

    // Sample HiZ at 4 corners of the AABB's screen rect
    float2 uvCenter = (minScreen + maxScreen) * 0.5;
    float hiZDepth = SAMPLE_HIZ(uvCenter);

    // Also sample corners for robustness
    hiZDepth = max(hiZDepth, SAMPLE_HIZ(minScreen));
    hiZDepth = max(hiZDepth, SAMPLE_HIZ(maxScreen));
    hiZDepth = max(hiZDepth, SAMPLE_HIZ(float2(minScreen.x, maxScreen.y)));
    hiZDepth = max(hiZDepth, SAMPLE_HIZ(float2(maxScreen.x, minScreen.y)));

    #undef SAMPLE_HIZ

    // Object is occluded if its nearest depth is behind all HiZ samples
    return nearestZ <= hiZDepth;
}

[numthreads(GPU_CULL_THREAD_GROUP_SIZE, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= instanceCount)
        return;

    GPUInstanceAABB aabb = boundingBoxes[index];
    float3 worldCenter = (aabb.minimum + aabb.maximum) * 0.5;
    float3 worldExtents = max((aabb.maximum - aabb.minimum) * 0.5, 0.0);

    // Frustum cull
    if (enableFrustumCull != 0 && !FrustumTest(worldCenter, worldExtents))
        return;

    // HiZ occlusion cull (optional)
    if (enableHiZCull != 0)
    {
        if (!HiZOcclusionTest(worldCenter, worldExtents))
            return;
    }

    visibilityFlags[index] = 1;
    uint ignored;
    indirectArgs.InterlockedAdd(GPU_CULL_INDIRECT_INSTANCE_COUNT_OFFSET, 1, ignored);
}
