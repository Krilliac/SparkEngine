// =============================================================================
// ShadowAtlas.hlsl — Shadow map atlas rendering and sampling
//
// Depth-only vertex shader for shadow map rendering with geometry shader
// for atlas tile selection. Includes PCF shadow sampling utility.
// =============================================================================

#ifndef MAX_SHADOW_CASCADES
#define MAX_SHADOW_CASCADES 4
#endif

cbuffer ShadowConstants : register(b0)
{
    float4x4 LightViewProjection[MAX_SHADOW_CASCADES];
    float4 CascadeSplits; // view-space Z splits for each cascade
    float2 AtlasTileSize;
    float2 AtlasSize;
    float ShadowBias;
    float NormalBias;
    uint CascadeCount;
    float ShadowSoftness;
};

struct VS_INPUT
{
    float3 Position : POSITION;
};

struct GS_INPUT
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
};

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    uint RTIndex : SV_RenderTargetArrayIndex;
};

// =============================================================================
// Shadow Map Rendering (Depth-only)
// =============================================================================

GS_INPUT VS_Shadow(VS_INPUT input)
{
    GS_INPUT output;
    output.WorldPos = input.Position;
    output.Position = float4(input.Position, 1.0);
    return output;
}

// Geometry shader selects cascade/atlas tile
[maxvertexcount(3 * MAX_SHADOW_CASCADES)]
void GS_Shadow(triangle GS_INPUT input[3], inout TriangleStream<PS_INPUT> stream)
{
    for (uint cascade = 0; cascade < CascadeCount; cascade++)
    {
        PS_INPUT output;
        output.RTIndex = cascade;

        [unroll]
        for (uint v = 0; v < 3; v++)
        {
            output.Position = mul(float4(input[v].WorldPos, 1.0), LightViewProjection[cascade]);
            stream.Append(output);
        }
        stream.RestartStrip();
    }
}

float PS_ShadowDepth(PS_INPUT input) : SV_Depth
{
    return input.Position.z;
}

// =============================================================================
// Shadow Sampling Utilities (used by lighting passes)
// =============================================================================

Texture2DArray<float> ShadowAtlas : register(t10);
SamplerComparisonState ShadowSampler : register(s10);

// Select cascade based on view-space depth
uint SelectCascade(float viewDepth)
{
    uint cascade = 0;
    if (viewDepth > CascadeSplits.x) cascade = 1;
    if (viewDepth > CascadeSplits.y) cascade = 2;
    if (viewDepth > CascadeSplits.z) cascade = 3;
    return min(cascade, CascadeCount - 1);
}

// PCF shadow sampling with configurable kernel
float SampleShadowPCF(float3 worldPos, float viewDepth, float3 normal)
{
    uint cascade = SelectCascade(viewDepth);
    float4 shadowCoord = mul(float4(worldPos, 1.0), LightViewProjection[cascade]);
    shadowCoord.xyz /= shadowCoord.w;

    // NDC to texture space
    float2 shadowUV = shadowCoord.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;
    float compareDepth = shadowCoord.z - ShadowBias;

    // 4x4 PCF kernel
    float shadow = 0.0;
    float2 texelSize = 1.0 / AtlasSize;

    [unroll]
    for (int y = -1; y <= 2; y++)
    {
        [unroll]
        for (int x = -1; x <= 2; x++)
        {
            float2 offset = float2(x, y) * texelSize * ShadowSoftness;
            shadow += ShadowAtlas.SampleCmpLevelZero(
                ShadowSampler, float3(shadowUV + offset, cascade), compareDepth);
        }
    }
    shadow /= 16.0;

    // Cascade edge blending
    float cascadeEdge = max(abs(shadowCoord.x), abs(shadowCoord.y));
    float edgeFade = saturate((0.95 - cascadeEdge) * 20.0);

    // Fade shadow at far cascade boundary
    float cascadeFade = 1.0;
    if (cascade == CascadeCount - 1)
    {
        float depthFade = saturate((CascadeSplits.w - viewDepth) / (CascadeSplits.w * 0.1));
        cascadeFade = depthFade;
    }

    return lerp(1.0, shadow, edgeFade * cascadeFade);
}
