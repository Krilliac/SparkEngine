/**
 * @file ProbeUpdate.hlsl
 * @brief Irradiance probe SH coefficient update via SDFGI traces
 *
 * CS 5.0 compatible. Each thread updates one probe's spherical harmonics
 * by tracing rays from the probe position through the SDF scene and
 * accumulating radiance into SH bands (L0 + L1 = 4 coefficients per channel).
 *
 * Dispatch: ceil(probeCount / 64) x 1 x 1
 */

#include "SDFCommon.hlsl"

// ============================================================================
// Resources
// ============================================================================

StructuredBuffer<SDFPrimitive> g_SDFScene : register(t0);

struct ProbeData
{
    float3 position;
    float padding;
    float4 shCoeffs[3]; ///< 3 channels (RGB) x 4 SH coefficients (L0, L1xyz)
};

RWStructuredBuffer<ProbeData> g_Probes : register(u0);

cbuffer ProbeUpdateConstants : register(b0)
{
    int ProbeCount;
    int PrimitiveCount;
    int RaysPerProbe;
    float MaxTraceDistance;
    float3 LightDir;
    float LightIntensity;
    float Hysteresis;   ///< Temporal smoothing (0.97 = slow update, 0.8 = fast)
    float3 _Pad;
};

// ============================================================================
// SH Basis Functions (L0 + L1 band)
// ============================================================================

/// Y_00 = 0.5 * sqrt(1/pi)
static const float SH_C0 = 0.282095;
/// Y_1m = 0.5 * sqrt(3/pi) * {y, z, x}
static const float SH_C1 = 0.488603;

float4 SHBasis(float3 dir)
{
    return float4(SH_C0, SH_C1 * dir.y, SH_C1 * dir.z, SH_C1 * dir.x);
}

// ============================================================================
// Scene Trace for Probes
// ============================================================================

float3 TraceProbeRay(float3 origin, float3 dir)
{
    float t = 0.05;
    for (int i = 0; i < 32; i++)
    {
        float3 p = origin + dir * t;
        float d = 1e10;
        float3 hitAlbedo = float3(0, 0, 0);

        for (int j = 0; j < PrimitiveCount; j++)
        {
            float pd = SDFPrimitiveDistance(p, g_SDFScene[j]);
            if (pd < d)
            {
                d = pd;
                hitAlbedo = g_SDFScene[j].albedo.rgb;
            }
        }

        if (d < 0.01)
        {
            // Hit — compute simple lighting at hit point
            float3 n = float3(0, 1, 0); // Approximate up-normal for speed
            for (int k = 0; k < PrimitiveCount; k++)
            {
                if (SDFPrimitiveDistance(p, g_SDFScene[k]) < 0.02)
                {
                    n = SDFPrimitiveNormal(p, g_SDFScene[k]);
                    break;
                }
            }
            float ndotl = saturate(dot(n, -LightDir));
            return hitAlbedo * ndotl * LightIntensity;
        }

        t += max(d, 0.02);
        if (t > MaxTraceDistance)
            break;
    }

    // Sky contribution (simple gradient)
    float skyFactor = saturate(dir.y * 0.5 + 0.5);
    return lerp(float3(0.1, 0.1, 0.15), float3(0.4, 0.6, 1.0), skyFactor) * 0.3;
}

// ============================================================================
// Fibonacci Sphere for Uniform Direction Sampling
// ============================================================================

static const float GOLDEN_RATIO = 1.6180339887;

float3 FibonacciSphereDirection(int index, int total)
{
    float theta = 2.0 * 3.14159265 * float(index) / GOLDEN_RATIO;
    float phi = acos(1.0 - 2.0 * (float(index) + 0.5) / float(total));
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}

// ============================================================================
// Main Kernel
// ============================================================================

[numthreads(64, 1, 1)]
void CSUpdateProbes(uint3 dtid : SV_DispatchThreadID)
{
    uint probeIndex = dtid.x;
    if (probeIndex >= (uint)ProbeCount)
        return;

    ProbeData probe = g_Probes[probeIndex];

    // Accumulate new SH coefficients from traced rays
    float4 newSH_R = float4(0, 0, 0, 0);
    float4 newSH_G = float4(0, 0, 0, 0);
    float4 newSH_B = float4(0, 0, 0, 0);

    for (int i = 0; i < RaysPerProbe; i++)
    {
        float3 dir = FibonacciSphereDirection(i, RaysPerProbe);
        float3 radiance = TraceProbeRay(probe.position, dir);
        float4 sh = SHBasis(dir);

        float weight = 4.0 * 3.14159265 / float(RaysPerProbe); // Solid angle weight
        newSH_R += sh * radiance.r * weight;
        newSH_G += sh * radiance.g * weight;
        newSH_B += sh * radiance.b * weight;
    }

    // Temporal smoothing: blend with previous SH coefficients
    probe.shCoeffs[0] = lerp(newSH_R, probe.shCoeffs[0], Hysteresis);
    probe.shCoeffs[1] = lerp(newSH_G, probe.shCoeffs[1], Hysteresis);
    probe.shCoeffs[2] = lerp(newSH_B, probe.shCoeffs[2], Hysteresis);

    g_Probes[probeIndex] = probe;
}
