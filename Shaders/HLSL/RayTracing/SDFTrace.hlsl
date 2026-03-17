/**
 * @file SDFTrace.hlsl
 * @brief SDFGI sphere tracing compute shader — reflections, GI, and shadows
 *
 * CS 5.0 compatible — works on DX11, DX12, and Vulkan compute.
 * This is the primary software fallback for the hybrid ray tracing system.
 *
 * Three entry points:
 *   CSTraceReflections  — Sphere-traced reflections from GBuffer
 *   CSTraceGI           — Diffuse global illumination via SDF bounces
 *   CSTraceShadows      — Soft shadows via SDF sphere tracing toward light
 *
 * Dispatch: ceil(width/8) x ceil(height/8) x 1
 */

#include "SDFCommon.hlsl"

// ============================================================================
// Resources
// ============================================================================

StructuredBuffer<SDFPrimitive> g_SDFScene : register(t0);
Texture2D<float4> g_GBufferNormals : register(t1);
Texture2D<float> g_GBufferDepth : register(t2);
Texture2D<float4> g_GBufferAlbedo : register(t3);

RWTexture2D<float4> g_Output : register(u0);

cbuffer TraceConstants : register(b0)
{
    float4x4 InvViewProj;
    float3 CameraPos;
    float MaxDistance;
    float3 LightDir;
    int MaxSteps;
    int PrimitiveCount;
    int TraceMode; // 0=reflections, 1=GI, 2=shadows
    float OutputWidth;
    float OutputHeight;
    float SDFBias;
    float ShadowSoftness;
    float GIIntensity;
    float TemporalBlend;
};

// ============================================================================
// Scene Evaluation
// ============================================================================

/// Evaluate minimum distance to entire SDF scene, returning nearest hit material
float SceneSDF(float3 p, out float3 hitAlbedo, out float hitRoughness, out float hitMetallic)
{
    float d = 1e10;
    hitAlbedo = float3(0.5, 0.5, 0.5);
    hitRoughness = 0.5;
    hitMetallic = 0.0;

    for (int i = 0; i < PrimitiveCount; i++)
    {
        SDFPrimitive prim = g_SDFScene[i];
        float pd = SDFPrimitiveDistance(p, prim);
        if (pd < d)
        {
            d = pd;
            hitAlbedo = prim.albedo.rgb;
            hitRoughness = prim.roughness;
            hitMetallic = prim.metallic;
        }
    }
    return d;
}

/// Scene SDF distance only (no material output — for normals/shadows)
float SceneSDFDistance(float3 p)
{
    float d = 1e10;
    for (int i = 0; i < PrimitiveCount; i++)
    {
        d = min(d, SDFPrimitiveDistance(p, g_SDFScene[i]));
    }
    return d;
}

/// Estimate scene normal via central differences
float3 EstimateSceneNormal(float3 p)
{
    float e = SDFBias;
    return normalize(float3(SceneSDFDistance(p + float3(e, 0, 0)) - SceneSDFDistance(p - float3(e, 0, 0)),
                            SceneSDFDistance(p + float3(0, e, 0)) - SceneSDFDistance(p - float3(0, e, 0)),
                            SceneSDFDistance(p + float3(0, 0, e)) - SceneSDFDistance(p - float3(0, 0, e))));
}

// ============================================================================
// Sphere Tracing Core
// ============================================================================

struct TraceResult
{
    float4 color;
    float distance;
    float3 hitNormal;
    bool hit;
};

TraceResult SphereTrace(float3 origin, float3 dir, int maxSteps, float maxDist)
{
    TraceResult result;
    result.color = float4(0, 0, 0, 0);
    result.distance = maxDist;
    result.hitNormal = float3(0, 1, 0);
    result.hit = false;

    float t = 0.02; // Start with small offset to avoid self-intersection
    for (int i = 0; i < maxSteps; i++)
    {
        float3 p = origin + dir * t;
        float3 albedo;
        float roughness, metallic;
        float d = SceneSDF(p, albedo, roughness, metallic);

        if (d < SDFBias)
        {
            result.hit = true;
            result.distance = t;
            result.hitNormal = EstimateSceneNormal(p);

            // Simple Lambertian shading at hit point
            float ndotl = saturate(dot(result.hitNormal, -LightDir));
            float ao = SDFAmbientOcclusion(p, result.hitNormal, g_SDFScene,
                                           PrimitiveCount, 5, 1.0);
            float shadow = SDFSoftShadow(p + result.hitNormal * 0.02, -LightDir,
                                         MaxDistance * 0.5, ShadowSoftness,
                                         g_SDFScene, PrimitiveCount);

            float3 ambient = albedo * 0.15 * ao;
            float3 diffuse = albedo * ndotl * shadow;

            // Fresnel-Schlick approximation for metallic surfaces
            float3 viewDir = -dir;
            float3 halfVec = normalize(viewDir + (-LightDir));
            float ndoth = saturate(dot(result.hitNormal, halfVec));
            float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
            float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - ndoth, 5.0);
            float3 specular = fresnel * pow(ndoth, lerp(32.0, 256.0, 1.0 - roughness));

            result.color = float4(ambient + diffuse + specular, 1.0);
            return result;
        }

        t += max(d, SDFBias * 2.0);
        if (t > maxDist)
            break;
    }

    return result;
}

// ============================================================================
// Entry Point: Reflections
// ============================================================================

[numthreads(8, 8, 1)] void CSTraceReflections(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)OutputWidth || dtid.y >= (uint)OutputHeight)
        return;

    float depth = g_GBufferDepth[dtid.xy];
    if (depth >= 1.0)
    {
        g_Output[dtid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float2 uv = (dtid.xy + 0.5) / float2(OutputWidth, OutputHeight);
    float3 worldPos = ReconstructWorldPos(uv, depth, InvViewProj);
    float3 normal = normalize(g_GBufferNormals[dtid.xy].xyz * 2.0 - 1.0);
    float3 viewDir = normalize(worldPos - CameraPos);
    float3 reflectDir = reflect(viewDir, normal);

    // Offset ray origin along normal to prevent self-intersection
    float3 rayOrigin = worldPos + normal * 0.05;
    TraceResult result = SphereTrace(rayOrigin, reflectDir, MaxSteps, MaxDistance);

    g_Output[dtid.xy] = result.color;
}

// ============================================================================
// Entry Point: Global Illumination
// ============================================================================

/// Simple hash for per-pixel jitter (avoids banding in GI)
float Hash(uint2 pixel)
{
    uint h = pixel.x * 374761393u + pixel.y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h) / 4294967295.0;
}

[numthreads(8, 8, 1)] void CSTraceGI(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)OutputWidth || dtid.y >= (uint)OutputHeight)
        return;

    float depth = g_GBufferDepth[dtid.xy];
    if (depth >= 1.0)
    {
        g_Output[dtid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float2 uv = (dtid.xy + 0.5) / float2(OutputWidth, OutputHeight);
    float3 worldPos = ReconstructWorldPos(uv, depth, InvViewProj);
    float3 normal = normalize(g_GBufferNormals[dtid.xy].xyz * 2.0 - 1.0);
    float3 surfaceAlbedo = g_GBufferAlbedo[dtid.xy].rgb;

    // Cosine-weighted hemisphere sample using per-pixel hash
    float r1 = Hash(dtid.xy);
    float r2 = Hash(dtid.xy + uint2(17, 31));
    float cosTheta = sqrt(1.0 - r1);
    float sinTheta = sqrt(r1);
    float phi = 6.28318530718 * r2;

    // Build tangent space from normal
    float3 tangent = abs(normal.y) < 0.999 ? normalize(cross(float3(0, 1, 0), normal))
                                            : normalize(cross(float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float3 sampleDir = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + normal * cosTheta;

    float3 rayOrigin = worldPos + normal * 0.05;
    TraceResult result = SphereTrace(rayOrigin, sampleDir, MaxSteps / 2, MaxDistance * 0.5);

    // Indirect lighting contribution: hit color modulated by surface albedo
    float3 gi = result.hit ? result.color.rgb * surfaceAlbedo * GIIntensity : float3(0, 0, 0);
    g_Output[dtid.xy] = float4(gi, result.hit ? 1.0 : 0.0);
}

// ============================================================================
// Entry Point: Shadows
// ============================================================================

[numthreads(8, 8, 1)] void CSTraceShadows(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)OutputWidth || dtid.y >= (uint)OutputHeight)
        return;

    float depth = g_GBufferDepth[dtid.xy];
    if (depth >= 1.0)
    {
        g_Output[dtid.xy] = float4(1, 1, 1, 1);
        return;
    }

    float2 uv = (dtid.xy + 0.5) / float2(OutputWidth, OutputHeight);
    float3 worldPos = ReconstructWorldPos(uv, depth, InvViewProj);
    float3 normal = normalize(g_GBufferNormals[dtid.xy].xyz * 2.0 - 1.0);

    float3 rayOrigin = worldPos + normal * 0.02;
    float shadow = SDFSoftShadow(rayOrigin, -LightDir, MaxDistance * 0.5, ShadowSoftness,
                                 g_SDFScene, PrimitiveCount);

    g_Output[dtid.xy] = float4(shadow, shadow, shadow, 1.0);
}
