/**
 * @file ProbeInterpolate.hlsl
 * @brief Trilinear interpolation of irradiance probe grid for indirect lighting
 *
 * CS 5.0 compatible. For each pixel, finds the 8 surrounding probes in the
 * grid and trilinearly interpolates their SH coefficients, then evaluates
 * the SH to produce an irradiance value in the surface normal direction.
 *
 * Dispatch: ceil(width/8) x ceil(height/8) x 1
 */

// ============================================================================
// Resources
// ============================================================================

struct ProbeData
{
    float3 position;
    float padding;
    float4 shCoeffs[3]; // RGB channels, 4 SH coefficients each (L0 + L1)
};

StructuredBuffer<ProbeData> g_Probes : register(t0);
Texture2D<float4> g_GBufferNormals : register(t1);
Texture2D<float> g_GBufferDepth : register(t2);

RWTexture2D<float4> g_OutputGI : register(u0);

cbuffer ProbeInterpolateConstants : register(b0)
{
    float4x4 InvViewProj;
    float3 GridOrigin;     ///< World-space origin of probe grid
    float GridSpacing;     ///< Distance between probes
    int3 GridDimensions;   ///< Number of probes in X, Y, Z
    float GIIntensity;
    float ScreenWidth;
    float ScreenHeight;
    float2 _Pad;
};

// ============================================================================
// SH Evaluation
// ============================================================================

static const float SH_C0 = 0.282095;
static const float SH_C1 = 0.488603;

float3 EvaluateSH(float4 shR, float4 shG, float4 shB, float3 normal)
{
    float4 sh = float4(SH_C0, SH_C1 * normal.y, SH_C1 * normal.z, SH_C1 * normal.x);
    return float3(dot(shR, sh), dot(shG, sh), dot(shB, sh));
}

// ============================================================================
// Grid Lookup
// ============================================================================

int ProbeIndex(int3 gridPos)
{
    gridPos = clamp(gridPos, int3(0, 0, 0), GridDimensions - int3(1, 1, 1));
    return gridPos.x + gridPos.y * GridDimensions.x + gridPos.z * GridDimensions.x * GridDimensions.y;
}

// ============================================================================
// Main Kernel
// ============================================================================

[numthreads(8, 8, 1)]
void CSInterpolateProbes(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)ScreenWidth || dtid.y >= (uint)ScreenHeight)
        return;

    float depth = g_GBufferDepth[dtid.xy];
    if (depth >= 1.0)
    {
        g_OutputGI[dtid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float2 uv = (dtid.xy + 0.5) / float2(ScreenWidth, ScreenHeight);
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos4 = mul(clipPos, InvViewProj);
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    float3 normal = normalize(g_GBufferNormals[dtid.xy].xyz * 2.0 - 1.0);

    // Find grid cell containing this world position
    float3 gridCoord = (worldPos - GridOrigin) / GridSpacing;
    int3 baseCell = int3(floor(gridCoord));
    float3 frac3 = gridCoord - float3(baseCell);

    // Trilinear interpolation of 8 surrounding probes
    float3 irradiance = float3(0, 0, 0);
    for (int dz = 0; dz < 2; dz++)
    {
        for (int dy = 0; dy < 2; dy++)
        {
            for (int dx = 0; dx < 2; dx++)
            {
                int3 cell = baseCell + int3(dx, dy, dz);
                int idx = ProbeIndex(cell);
                ProbeData probe = g_Probes[idx];

                float3 shIrradiance = EvaluateSH(probe.shCoeffs[0], probe.shCoeffs[1],
                                                  probe.shCoeffs[2], normal);

                float wx = dx == 0 ? (1.0 - frac3.x) : frac3.x;
                float wy = dy == 0 ? (1.0 - frac3.y) : frac3.y;
                float wz = dz == 0 ? (1.0 - frac3.z) : frac3.z;
                float weight = wx * wy * wz;

                irradiance += shIrradiance * weight;
            }
        }
    }

    g_OutputGI[dtid.xy] = float4(max(irradiance * GIIntensity, float3(0, 0, 0)), 1.0);
}
