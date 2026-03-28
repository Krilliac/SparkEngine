/**
 * @file MeshletAS.hlsl
 * @brief Amplification shader for meshlet LOD selection and dispatch
 *
 * Runs before the mesh shader to determine which meshlets to render.
 * Performs coarse-level frustum culling and LOD selection, then dispatches
 * mesh shader thread groups only for visible meshlets.
 *
 * Requires: SM 6.5 (D3D12) or VK_EXT_mesh_shader (Vulkan)
 */

struct Meshlet
{
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;

    float3 boundCenter;
    float  boundRadius;

    float3 coneAxis;
    float  coneCutoff;
};

struct MeshletGroup
{
    uint   meshletOffset;    // first meshlet index in this group
    uint   meshletCount;     // number of meshlets in this group
    float  lodError;         // screen-space error for this LOD level
    float  parentLodError;   // screen-space error of parent group
    float3 boundCenter;      // group bounding sphere
    float  boundRadius;
};

struct ASPayload
{
    uint meshletIndices[32]; // meshlet indices to dispatch
};

cbuffer ASConstants : register(b0)
{
    float4x4 viewProjection;
    float3   cameraPosition;
    float    lodErrorThreshold; // screen-space pixel error threshold
    float4   frustumPlanes[6];
    uint     groupCount;
    float    screenHeight;      // for screen-space error calculation
    float    fovY;              // vertical FOV in radians
    float    pad0;
};

StructuredBuffer<MeshletGroup> groups : register(t0);
StructuredBuffer<Meshlet>      meshlets : register(t1);

groupshared ASPayload payload;
groupshared uint payloadCount;

// Calculate screen-space error from world-space error and distance
float ScreenSpaceError(float worldError, float3 boundCenter, float boundRadius)
{
    float dist = length(cameraPosition - boundCenter);
    dist = max(dist - boundRadius, 0.001); // prevent divide-by-zero

    // Project error to screen pixels
    float projFactor = screenHeight / (2.0 * tan(fovY * 0.5));
    return (worldError / dist) * projFactor;
}

// Frustum sphere cull
bool IsGroupOutsideFrustum(MeshletGroup g)
{
    [unroll]
    for (uint i = 0; i < 6; i++)
    {
        float dist = dot(frustumPlanes[i].xyz, g.boundCenter) + frustumPlanes[i].w;
        if (dist < -g.boundRadius)
            return true;
    }
    return false;
}

[numthreads(32, 1, 1)]
void ASMain(uint groupId : SV_GroupID, uint threadId : SV_GroupThreadID)
{
    // Initialize shared count
    if (threadId == 0)
        payloadCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint groupIndex = groupId * 32 + threadId;
    if (groupIndex >= groupCount)
    {
        GroupMemoryBarrierWithGroupSync();
        DispatchMesh(payloadCount, 1, 1, payload);
        return;
    }

    MeshletGroup group = groups[groupIndex];

    // Frustum cull the entire group
    if (IsGroupOutsideFrustum(group))
    {
        GroupMemoryBarrierWithGroupSync();
        DispatchMesh(payloadCount, 1, 1, payload);
        return;
    }

    // LOD selection: render this group if its error is below threshold
    // but its parent's error would be above threshold
    float screenError = ScreenSpaceError(group.lodError, group.boundCenter, group.boundRadius);
    float parentScreenError = ScreenSpaceError(group.parentLodError, group.boundCenter, group.boundRadius);

    bool shouldRender = (screenError <= lodErrorThreshold) && (parentScreenError > lodErrorThreshold);

    // Special case: finest LOD always renders if parent is too coarse
    if (group.parentLodError == 0.0)
        shouldRender = (screenError <= lodErrorThreshold);

    if (shouldRender)
    {
        // Add all meshlets in this group to the dispatch payload
        for (uint i = 0; i < group.meshletCount && i < 32; i++)
        {
            uint slot;
            InterlockedAdd(payloadCount, 1, slot);
            if (slot < 32)
            {
                payload.meshletIndices[slot] = group.meshletOffset + i;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(payloadCount, 1, 1, payload);
}
