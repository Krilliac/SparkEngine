/**
 * @file ClusterCull.hlsl
 * @brief GPU compute shader for clustered light assignment
 *
 * Assigns lights to 3D frustum-space clusters for Forward+ rendering.
 * Each thread group handles one cluster, testing all lights against the
 * cluster's AABB. Results are written to a per-cluster light index list.
 *
 * Dispatch: (gridX, gridY, gridZ) groups, one per cluster.
 */

struct LightData
{
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
    float3 direction;    // spot light direction
    float  spotAngle;    // spot light half-angle (radians), 0 = point light
    uint   lightType;    // 0 = point, 1 = spot, 2 = directional
    uint   castShadows;
    float2 pad0;
};

struct ClusterAABB
{
    float3 minPoint;
    float  pad0;
    float3 maxPoint;
    float  pad1;
};

cbuffer ClusterConstants : register(b0)
{
    uint   gridX;
    uint   gridY;
    uint   gridZ;
    uint   maxLightsPerCluster;

    uint   activeLightCount;
    float  nearPlane;
    float  farPlane;
    float  pad0;

    float4x4 inverseProjection;
};

StructuredBuffer<LightData>   lights : register(t0);
StructuredBuffer<ClusterAABB> clusters : register(t1);

// Per-cluster light index list: [cluster0_light0, cluster0_light1, ..., cluster1_light0, ...]
RWStructuredBuffer<uint> clusterLightIndices : register(u0);

// Per-cluster light count
RWStructuredBuffer<uint> clusterLightCounts : register(u1);

groupshared LightData sharedLights[64]; // cache lights in shared memory

// Sphere-AABB intersection test
bool SphereAABBIntersect(float3 sphereCenter, float sphereRadius, float3 aabbMin, float3 aabbMax)
{
    // Find closest point on AABB to sphere center
    float3 closest = clamp(sphereCenter, aabbMin, aabbMax);
    float3 delta = sphereCenter - closest;
    float distSq = dot(delta, delta);
    return distSq <= (sphereRadius * sphereRadius);
}

// Spot light-AABB intersection (conservative: use bounding sphere of spot cone)
bool SpotAABBIntersect(float3 position, float3 direction, float range, float halfAngle,
                       float3 aabbMin, float3 aabbMax)
{
    // Bounding sphere of the spot cone
    float sinAngle = sin(halfAngle);
    float cosAngle = cos(halfAngle);

    float sphereRadius;
    float3 sphereCenter;

    if (halfAngle > 0.7854) // > 45 degrees: sphere encompasses entire cone
    {
        sphereRadius = range * sinAngle;
        sphereCenter = position + direction * (range * cosAngle);
    }
    else
    {
        sphereRadius = range / (2.0 * cosAngle);
        sphereCenter = position + direction * sphereRadius;
    }

    return SphereAABBIntersect(sphereCenter, sphereRadius, aabbMin, aabbMax);
}

[numthreads(1, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID)
{
    uint clusterIndex = groupId.x + groupId.y * gridX + groupId.z * gridX * gridY;
    uint totalClusters = gridX * gridY * gridZ;
    if (clusterIndex >= totalClusters)
        return;

    ClusterAABB cluster = clusters[clusterIndex];
    uint baseOffset = clusterIndex * maxLightsPerCluster;
    uint lightCount = 0;

    // Process lights in batches to leverage shared memory
    uint batchCount = (activeLightCount + 63) / 64;

    for (uint batch = 0; batch < batchCount; batch++)
    {
        uint batchStart = batch * 64;
        uint batchEnd = min(batchStart + 64, activeLightCount);

        for (uint i = batchStart; i < batchEnd; i++)
        {
            if (lightCount >= maxLightsPerCluster)
                break;

            LightData light = lights[i];

            bool intersects = false;

            if (light.lightType == 0) // Point light
            {
                intersects = SphereAABBIntersect(
                    light.position, light.radius,
                    cluster.minPoint, cluster.maxPoint);
            }
            else if (light.lightType == 1) // Spot light
            {
                intersects = SpotAABBIntersect(
                    light.position, light.direction, light.radius, light.spotAngle,
                    cluster.minPoint, cluster.maxPoint);
            }
            else // Directional (always affects all clusters)
            {
                intersects = true;
            }

            if (intersects)
            {
                clusterLightIndices[baseOffset + lightCount] = i;
                lightCount++;
            }
        }
    }

    clusterLightCounts[clusterIndex] = lightCount;
}
