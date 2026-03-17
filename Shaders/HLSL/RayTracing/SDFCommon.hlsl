/**
 * @file SDFCommon.hlsl
 * @brief Signed Distance Field primitive functions for SDFGI sphere tracing
 *
 * CS 5.0 compatible — works on DX11, DX12, and Vulkan compute.
 * References: Inigo Quilez SDF functions (https://iquilezles.org/articles/distfunctions/)
 * Extended for SparkEngine hybrid RT system (March 2026).
 */

#ifndef SDF_COMMON_HLSL
#define SDF_COMMON_HLSL

// ============================================================================
// SDF Primitive Structure (must match C++ SDFPrimitive layout — 64 bytes)
// ============================================================================

struct SDFPrimitive
{
    float3 center;
    float radius;
    float3 halfExtents;
    uint type; // 0=sphere, 1=box, 2=capsule, 3=rounded box
    float4 albedo;
    float roughness;
    float metallic;
    float emissive;
    float padding;
};

// ============================================================================
// Core SDF Distance Functions
// ============================================================================

/// Signed distance to a sphere
float SDFSphere(float3 p, float3 center, float radius)
{
    return length(p - center) - radius;
}

/// Signed distance to an axis-aligned box
float SDFBox(float3 p, float3 center, float3 halfExtents)
{
    float3 d = abs(p - center) - halfExtents;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

/// Signed distance to a vertical capsule (Y-axis aligned)
float SDFCapsule(float3 p, float3 center, float radius, float halfHeight)
{
    float3 local = p - center;
    local.y -= clamp(local.y, -halfHeight, halfHeight);
    return length(local) - radius;
}

/// Signed distance to a rounded box
float SDFRoundedBox(float3 p, float3 center, float3 halfExtents, float rounding)
{
    float3 d = abs(p - center) - halfExtents + rounding;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0) - rounding;
}

// ============================================================================
// SDF Operations
// ============================================================================

/// Smooth minimum for blending SDF shapes
float SmoothMin(float a, float b, float k)
{
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

/// Sharp union (standard min)
float SDFUnion(float a, float b)
{
    return min(a, b);
}

/// Sharp subtraction
float SDFSubtract(float a, float b)
{
    return max(a, -b);
}

/// Sharp intersection
float SDFIntersect(float a, float b)
{
    return max(a, b);
}

// ============================================================================
// Primitive Dispatcher
// ============================================================================

/// Evaluate distance to a single SDF primitive based on its type
float SDFPrimitiveDistance(float3 p, SDFPrimitive prim)
{
    switch (prim.type)
    {
    case 0: // Sphere
        return SDFSphere(p, prim.center, prim.radius);
    case 1: // Box
        return SDFBox(p, prim.center, prim.halfExtents);
    case 2: // Capsule
        return SDFCapsule(p, prim.center, prim.radius, prim.halfExtents.y);
    case 3: // Rounded box
        return SDFRoundedBox(p, prim.center, prim.halfExtents, prim.radius * 0.1);
    default:
        return SDFSphere(p, prim.center, prim.radius);
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

/// Estimate normal at point p via central differences on the SDF
/// Caller must provide the scene evaluation function externally;
/// this version uses a single-primitive normal (for per-hit refinement).
float3 SDFPrimitiveNormal(float3 p, SDFPrimitive prim)
{
    float e = 0.001;
    return normalize(float3(
        SDFPrimitiveDistance(p + float3(e, 0, 0), prim) - SDFPrimitiveDistance(p - float3(e, 0, 0), prim),
        SDFPrimitiveDistance(p + float3(0, e, 0), prim) - SDFPrimitiveDistance(p - float3(0, e, 0), prim),
        SDFPrimitiveDistance(p + float3(0, 0, e), prim) - SDFPrimitiveDistance(p - float3(0, 0, e), prim)));
}

/// Reconstruct world position from depth buffer + inverse view-projection
float3 ReconstructWorldPos(float2 uv, float depth, float4x4 invViewProj)
{
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y; // D3D convention: Y flipped
    float4 worldPos = mul(clipPos, invViewProj);
    return worldPos.xyz / worldPos.w;
}

/// Soft shadow factor using SDF sphere tracing toward light
/// k controls softness (higher = sharper shadows)
float SDFSoftShadow(float3 origin, float3 dir, float maxDist, float k,
                    StructuredBuffer<SDFPrimitive> scene, int primitiveCount)
{
    float t = 0.02;
    float shadow = 1.0;
    for (int i = 0; i < 32; i++)
    {
        float3 p = origin + dir * t;
        float d = 1e10;
        for (int j = 0; j < primitiveCount; j++)
        {
            d = min(d, SDFPrimitiveDistance(p, scene[j]));
        }
        shadow = min(shadow, k * d / t);
        if (d < 0.001)
            return 0.0;
        t += max(d, 0.01);
        if (t > maxDist)
            break;
    }
    return saturate(shadow);
}

/// Ambient occlusion via short-range SDF sampling
float SDFAmbientOcclusion(float3 p, float3 n, StructuredBuffer<SDFPrimitive> scene,
                          int primitiveCount, int steps, float maxDist)
{
    float ao = 0.0;
    float weight = 1.0;
    for (int i = 0; i < steps; i++)
    {
        float dist = maxDist * (float(i + 1) / float(steps));
        float3 samplePos = p + n * dist;
        float d = 1e10;
        for (int j = 0; j < primitiveCount; j++)
        {
            d = min(d, SDFPrimitiveDistance(samplePos, scene[j]));
        }
        ao += weight * (dist - d);
        weight *= 0.5;
    }
    return saturate(1.0 - ao);
}

#endif // SDF_COMMON_HLSL
