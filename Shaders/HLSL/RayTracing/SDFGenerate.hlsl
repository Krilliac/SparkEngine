/**
 * @file SDFGenerate.hlsl
 * @brief Compute shader for generating SDF primitives from mesh bounding volumes
 *
 * CS 5.0 compatible. Processes mesh AABB data and outputs SDF primitive
 * descriptions to a structured buffer for use by SDFTrace.hlsl.
 *
 * Each mesh is approximated as the best-fitting SDF primitive (sphere, box,
 * or capsule) based on its aspect ratio and bounding volume shape.
 *
 * Dispatch: ceil(meshCount / 64) x 1 x 1
 */

#include "SDFCommon.hlsl"

// ============================================================================
// Input: Mesh bounding data (uploaded from CPU)
// ============================================================================

struct MeshBoundsInput
{
    float3 aabbMin;
    float padding0;
    float3 aabbMax;
    float padding1;
    float4 albedo;
    float roughness;
    float metallic;
    float emissive;
    float padding2;
    float4x4 worldTransform;
};

StructuredBuffer<MeshBoundsInput> g_MeshBounds : register(t0);
RWStructuredBuffer<SDFPrimitive> g_OutputPrimitives : register(u0);

cbuffer GenerateConstants : register(b0)
{
    int MeshCount;
    float SizeScale;    ///< Scale factor for SDF radius (1.0 = exact bounds, 1.1 = 10% margin)
    float2 _Padding;
};

// ============================================================================
// Primitive Fitting
// ============================================================================

/// Determine best SDF primitive type for an AABB based on aspect ratio
uint FitPrimitiveType(float3 halfExtents)
{
    float maxE = max(halfExtents.x, max(halfExtents.y, halfExtents.z));
    float minE = min(halfExtents.x, min(halfExtents.y, halfExtents.z));
    float aspectRatio = maxE / max(minE, 0.001);

    // Near-cubic: use box
    if (aspectRatio < 1.5)
        return 1; // Box

    // Elongated along Y: use capsule
    if (halfExtents.y > halfExtents.x * 1.5 && halfExtents.y > halfExtents.z * 1.5)
        return 2; // Capsule

    // Near-spherical (small size or uniform): use sphere
    if (aspectRatio < 2.0)
        return 0; // Sphere

    // Default: rounded box
    return 3;
}

// ============================================================================
// Main Kernel
// ============================================================================

[numthreads(64, 1, 1)]
void CSGenerateSDF(uint3 dtid : SV_DispatchThreadID)
{
    uint meshIndex = dtid.x;
    if (meshIndex >= (uint)MeshCount)
        return;

    MeshBoundsInput mesh = g_MeshBounds[meshIndex];

    // Compute world-space center and half-extents
    float3 localCenter = (mesh.aabbMin + mesh.aabbMax) * 0.5;
    float3 localHalfExtents = (mesh.aabbMax - mesh.aabbMin) * 0.5 * SizeScale;

    // Transform center to world space
    float4 worldCenter = mul(float4(localCenter, 1.0), mesh.worldTransform);

    // Approximate world-space scale from transform (assumes uniform-ish scale)
    float3 scaleX = float3(mesh.worldTransform[0][0], mesh.worldTransform[0][1], mesh.worldTransform[0][2]);
    float3 scaleY = float3(mesh.worldTransform[1][0], mesh.worldTransform[1][1], mesh.worldTransform[1][2]);
    float3 scaleZ = float3(mesh.worldTransform[2][0], mesh.worldTransform[2][1], mesh.worldTransform[2][2]);
    float3 worldScale = float3(length(scaleX), length(scaleY), length(scaleZ));
    float3 worldHalfExtents = localHalfExtents * worldScale;

    // Fit best primitive type
    uint primType = FitPrimitiveType(worldHalfExtents);

    // Build output primitive
    SDFPrimitive prim;
    prim.center = worldCenter.xyz;
    prim.halfExtents = worldHalfExtents;
    prim.type = primType;
    prim.albedo = mesh.albedo;
    prim.roughness = mesh.roughness;
    prim.metallic = mesh.metallic;
    prim.emissive = mesh.emissive;
    prim.padding = 0.0;

    // Compute bounding sphere radius for the primitive
    switch (primType)
    {
    case 0: // Sphere — use max half-extent
        prim.radius = max(worldHalfExtents.x, max(worldHalfExtents.y, worldHalfExtents.z));
        break;
    case 1: // Box — half-extents used directly
        prim.radius = 0.0;
        break;
    case 2: // Capsule — radius from XZ, height from Y
        prim.radius = max(worldHalfExtents.x, worldHalfExtents.z);
        break;
    case 3: // Rounded box — small rounding
        prim.radius = min(worldHalfExtents.x, min(worldHalfExtents.y, worldHalfExtents.z)) * 0.1;
        break;
    }

    g_OutputPrimitives[meshIndex] = prim;
}
