// LightCull.hlsl - Forward+ clustered light culling compute shader (Godot-inspired)
//
// Subdivides the screen into tiles and determines which lights affect each tile.
// Uses the depth buffer to compute per-tile min/max depth for tighter culling.
//
// Dispatch: (ceil(screenWidth/TILE_SIZE), ceil(screenHeight/TILE_SIZE), 1)
// Each thread group processes one tile.

#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 256
#define MAX_LIGHTS 1024

// --------------------------------------------------------------------------
// Constant Buffers
// --------------------------------------------------------------------------

cbuffer CullConstants : register(b0)
{
    float4x4 View;
    float4x4 Projection;
    float4x4 InvProjection;
    uint NumLights;
    uint ScreenWidth;
    uint ScreenHeight;
    float NearPlane;
    float FarPlane;
    float3 _Padding;
};

// --------------------------------------------------------------------------
// Light Data
// --------------------------------------------------------------------------

struct LightData
{
    float3 PositionVS; // View-space position
    float Range;
    float3 Color;
    float Intensity;
    float3 Direction;  // View-space direction (for spot lights)
    uint Type;         // 0 = Directional, 1 = Point, 2 = Spot
};

StructuredBuffer<LightData> LightBuffer : register(t0);
Texture2D<float> DepthTexture : register(t1);

// --------------------------------------------------------------------------
// Output: per-tile light index list
// --------------------------------------------------------------------------

struct TileLightData
{
    uint LightCount;
    uint LightIndices[MAX_LIGHTS_PER_TILE];
};

RWStructuredBuffer<TileLightData> TileLightBuffer : register(u0);

// --------------------------------------------------------------------------
// Shared memory for per-tile min/max depth
// --------------------------------------------------------------------------

groupshared uint tileMinDepthInt;
groupshared uint tileMaxDepthInt;
groupshared uint tileLightCount;
groupshared uint tileLightIndices[MAX_LIGHTS_PER_TILE];

// --------------------------------------------------------------------------
// Helper: reconstruct view-space position from depth
// --------------------------------------------------------------------------

float LinearizeDepth(float d)
{
    return NearPlane * FarPlane / (FarPlane - d * (FarPlane - NearPlane));
}

// --------------------------------------------------------------------------
// Helper: test sphere (light) against tile frustum planes
// --------------------------------------------------------------------------

bool SphereFrustumTest(float3 center, float radius, float4 frustumPlanes[4])
{
    for (uint i = 0; i < 4; ++i)
    {
        float dist = dot(frustumPlanes[i].xyz, center) + frustumPlanes[i].w;
        if (dist < -radius)
            return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// Main compute shader
// --------------------------------------------------------------------------

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void CSMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint groupIndex : SV_GroupIndex)
{
    uint tileX = groupId.x;
    uint tileY = groupId.y;
    uint tilesX = (ScreenWidth + TILE_SIZE - 1) / TILE_SIZE;
    uint tileIndex = tileY * tilesX + tileX;

    // Initialize shared memory
    if (groupIndex == 0)
    {
        tileMinDepthInt = 0x7F7FFFFF; // FLT_MAX as uint
        tileMaxDepthInt = 0;
        tileLightCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Step 1: Compute per-tile min/max depth from depth buffer
    uint2 pixelCoord = uint2(tileX * TILE_SIZE + groupThreadId.x,
                              tileY * TILE_SIZE + groupThreadId.y);

    if (pixelCoord.x < ScreenWidth && pixelCoord.y < ScreenHeight)
    {
        float depth = DepthTexture.Load(int3(pixelCoord, 0));
        float linearDepth = LinearizeDepth(depth);
        uint depthInt = asuint(linearDepth);

        InterlockedMin(tileMinDepthInt, depthInt);
        InterlockedMax(tileMaxDepthInt, depthInt);
    }
    GroupMemoryBarrierWithGroupSync();

    float tileMinDepth = asfloat(tileMinDepthInt);
    float tileMaxDepth = asfloat(tileMaxDepthInt);

    // Step 2: Build tile frustum planes in view space
    float2 tileMin = float2(tileX * TILE_SIZE, tileY * TILE_SIZE);
    float2 tileMax = float2((tileX + 1) * TILE_SIZE, (tileY + 1) * TILE_SIZE);

    // Convert tile corners to NDC
    float2 ndcMin = tileMin / float2(ScreenWidth, ScreenHeight) * 2.0 - 1.0;
    float2 ndcMax = tileMax / float2(ScreenWidth, ScreenHeight) * 2.0 - 1.0;
    ndcMin.y = -ndcMin.y;
    ndcMax.y = -ndcMax.y;

    // Build 4 side planes in view space (left, right, bottom, top)
    float4 frustumPlanes[4];
    frustumPlanes[0] = float4( 1, 0, ndcMin.x * InvProjection._11, 0); // Left
    frustumPlanes[1] = float4(-1, 0, -ndcMax.x * InvProjection._11, 0); // Right
    frustumPlanes[2] = float4(0,  1, ndcMax.y * InvProjection._22, 0); // Bottom
    frustumPlanes[3] = float4(0, -1, -ndcMin.y * InvProjection._22, 0); // Top

    // Normalize planes
    for (uint p = 0; p < 4; ++p)
    {
        float len = length(frustumPlanes[p].xyz);
        frustumPlanes[p] /= len;
    }

    // Step 3: Test each light against the tile (distribute across threads)
    for (uint lightIdx = groupIndex; lightIdx < NumLights; lightIdx += TILE_SIZE * TILE_SIZE)
    {
        LightData light = LightBuffer[lightIdx];

        bool inTile = false;

        if (light.Type == 0) // Directional
        {
            inTile = true;
        }
        else // Point or Spot
        {
            // Depth range check
            float lightDepth = light.PositionVS.z;
            if (lightDepth + light.Range >= tileMinDepth &&
                lightDepth - light.Range <= tileMaxDepth)
            {
                // Frustum plane check
                inTile = SphereFrustumTest(light.PositionVS, light.Range, frustumPlanes);
            }
        }

        if (inTile)
        {
            uint slot;
            InterlockedAdd(tileLightCount, 1, slot);
            if (slot < MAX_LIGHTS_PER_TILE)
            {
                tileLightIndices[slot] = lightIdx;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Step 4: Write tile results to output buffer
    if (groupIndex == 0)
    {
        uint count = min(tileLightCount, MAX_LIGHTS_PER_TILE);
        TileLightBuffer[tileIndex].LightCount = count;
    }
    GroupMemoryBarrierWithGroupSync();

    uint count = min(tileLightCount, MAX_LIGHTS_PER_TILE);
    for (uint i = groupIndex; i < count; i += TILE_SIZE * TILE_SIZE)
    {
        TileLightBuffer[tileIndex].LightIndices[i] = tileLightIndices[i];
    }
}
