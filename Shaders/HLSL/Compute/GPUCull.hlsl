/**
 * @file GPUCull.hlsl
 * @brief GPU compute shader for frustum + HiZ occlusion culling
 *
 * Tests each instance AABB against the view frustum and previous frame's
 * hierarchical Z-buffer. Visible instances are compacted into an indirect
 * draw argument buffer for ExecuteIndirect rendering.
 *
 * Dispatch: ceil(instanceCount / 256) groups of 256 threads.
 */

struct InstanceData
{
    float4x4 worldMatrix;
    float4x4 prevWorldMatrix;
    float4x4 normalMatrix;
    uint     materialId;
    uint     flags;        // see InstanceFlags
    float    lodDistance;
    float    pad0;
};

struct AABB
{
    float3 center;
    float  pad0;
    float3 extents;
    float  pad1;
};

// Indirect draw arguments (D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)
struct IndirectArgs
{
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int  baseVertexLocation;
    uint startInstanceLocation;
};

cbuffer CullConstants : register(b0)
{
    float4x4 viewProjection;
    float4   frustumPlanes[6]; // xyz = normal, w = distance
    float2   hiZSize;          // HiZ texture dimensions
    float    hiZMipCount;
    uint     instanceCount;
    float    nearPlane;
    float    farPlane;
    uint     enableHiZ;       // 0 = frustum only, 1 = frustum + HiZ
    uint     pad0;
};

StructuredBuffer<InstanceData> instances : register(t0);
StructuredBuffer<AABB>         boundingBoxes : register(t1);
Texture2D<float>               hiZTexture : register(t2);
SamplerState                   hiZSampler : register(s0);

// Output: compacted visible instance indices
RWStructuredBuffer<uint>  visibleInstances : register(u0);

// Output: indirect draw args (atomically incremented)
RWStructuredBuffer<IndirectArgs> indirectArgsBuffer : register(u1);

// Output: visible instance count
RWBuffer<uint> visibleCount : register(u2);

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
    float2 screenExtent = (maxScreen - minScreen) * hiZSize;
    float mipLevel = ceil(log2(max(screenExtent.x, screenExtent.y)));
    mipLevel = clamp(mipLevel, 0.0, hiZMipCount - 1.0);

    // Sample HiZ at 4 corners of the AABB's screen rect
    float2 uvCenter = (minScreen + maxScreen) * 0.5;
    float hiZDepth = hiZTexture.SampleLevel(hiZSampler, uvCenter, mipLevel);

    // Also sample corners for robustness
    hiZDepth = max(hiZDepth, hiZTexture.SampleLevel(hiZSampler, minScreen, mipLevel));
    hiZDepth = max(hiZDepth, hiZTexture.SampleLevel(hiZSampler, maxScreen, mipLevel));
    hiZDepth = max(hiZDepth, hiZTexture.SampleLevel(hiZSampler, float2(minScreen.x, maxScreen.y), mipLevel));
    hiZDepth = max(hiZDepth, hiZTexture.SampleLevel(hiZSampler, float2(maxScreen.x, minScreen.y), mipLevel));

    // Object is occluded if its nearest depth is behind all HiZ samples
    return nearestZ <= hiZDepth;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= instanceCount)
        return;

    AABB aabb = boundingBoxes[index];

    // Transform AABB center to world space
    float3 worldCenter = mul(float4(aabb.center, 1.0), instances[index].worldMatrix).xyz;

    // Scale extents by max axis scale (conservative)
    float3 row0 = float3(instances[index].worldMatrix[0][0],
                         instances[index].worldMatrix[0][1],
                         instances[index].worldMatrix[0][2]);
    float3 row1 = float3(instances[index].worldMatrix[1][0],
                         instances[index].worldMatrix[1][1],
                         instances[index].worldMatrix[1][2]);
    float3 row2 = float3(instances[index].worldMatrix[2][0],
                         instances[index].worldMatrix[2][1],
                         instances[index].worldMatrix[2][2]);

    float3 worldExtents = float3(
        dot(abs(aabb.extents), float3(abs(row0.x), abs(row1.x), abs(row2.x))),
        dot(abs(aabb.extents), float3(abs(row0.y), abs(row1.y), abs(row2.y))),
        dot(abs(aabb.extents), float3(abs(row0.z), abs(row1.z), abs(row2.z))));

    // Frustum cull
    if (!FrustumTest(worldCenter, worldExtents))
        return;

    // HiZ occlusion cull (optional)
    if (enableHiZ > 0)
    {
        if (!HiZOcclusionTest(worldCenter, worldExtents))
            return;
    }

    // Instance is visible - add to compacted list
    uint visIdx;
    InterlockedAdd(visibleCount[0], 1, visIdx);
    visibleInstances[visIdx] = index;
}
