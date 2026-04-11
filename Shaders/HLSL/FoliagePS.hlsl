// ============================================================================
// FoliagePS.hlsl
// ----------------------------------------------------------------------------
// Pixel shader for instanced foliage meshes. Alpha-tested for leaves/grass
// and shaded with a simple wrapped-diffuse model so backlit leaves keep a
// warm fill rather than going flat-black.
//
// Pairs with FoliageVS.hlsl. The impostor path uses a separate, even simpler
// shader (FoliageImpostorPS.hlsl — follow-up).
// ============================================================================

cbuffer PerFrame : register(b0)
{
    matrix View;
    matrix Projection;
    float3 CameraPos;
    float  Time;
};

cbuffer FoliageLighting : register(b3)
{
    // Main directional light
    float4 SunDirection;   // xyz: direction (from sun to surface), w: unused
    float4 SunColor;       // rgb: linear color, a: intensity
    float4 AmbientColor;   // rgb: sky/ambient fill, a: unused
    float4 FoliageParams;  // x: alphaRef, y: wrapAmount, z: backlit, w: aoStrength
};

Texture2D AlbedoMap       : register(t1); // leaf/bark albedo with alpha
Texture2D ImpostorAtlas   : register(t2); // foliage impostor atlas (Phase E)
StructuredBuffer<float4> ImpostorCells : register(t3); // per-species cell rects
SamplerState SamplerLinear : register(s0);

// Flag bit for PS_INPUT.Flags — must match
// Spark::Graphics::InstanceFlags::FoliageImpostor (GPUSceneBuffer.h).
#define FOLIAGE_IMPOSTOR_FLAG 0x40u

struct PS_INPUT
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : POSITION1;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float  WindSway : TEXCOORD1;
    nointerpolation uint  MaterialId : TEXCOORD2;
    nointerpolation float AngleU     : TEXCOORD3;
    nointerpolation uint  Flags      : TEXCOORD4;
};

float4 main(PS_INPUT i) : SV_TARGET
{
    bool isImpostor = (i.Flags & FOLIAGE_IMPOSTOR_FLAG) != 0u;

    float4 albedo;
    if (isImpostor)
    {
        // Sample the pre-baked impostor atlas. The VS packed the angle-bin
        // U offset into AngleU; `cell` gives the atlas cell origin and
        // per-angle cell size in UV space. The vertex UVs are in [0,1]
        // canonical unit-quad space (TEXCOORD0).
        float4 cell = ImpostorCells[i.MaterialId];
        float2 uv = float2(cell.x + i.AngleU + i.UV.x * cell.z,
                            cell.y +            i.UV.y * cell.w);
        albedo = ImpostorAtlas.Sample(SamplerLinear, uv);
    }
    else
    {
        albedo = AlbedoMap.Sample(SamplerLinear, i.UV);
    }

    // Alpha-test: grass blades and leaf cards.
    if (albedo.a < FoliageParams.x)
    {
        discard;
    }

    // Impostors come out of the baker already lit, so short-circuit the
    // wrapped-diffuse model and return their sampled colour directly.
    if (isImpostor)
    {
        return float4(albedo.rgb, 1.0);
    }

    float3 N = normalize(i.Normal);
    float3 L = normalize(-SunDirection.xyz);

    // Wrapped diffuse: lets backlit foliage retain ambient warmth.
    float wrap = FoliageParams.y;
    float NdotL = saturate((dot(N, L) + wrap) / (1.0 + wrap));

    // Rim backlight for translucency fake.
    float3 V = normalize(CameraPos - i.WorldPos);
    float backlit = saturate(dot(-N, L)) * FoliageParams.z;

    float3 diffuse = albedo.rgb * SunColor.rgb * SunColor.a * NdotL;
    float3 ambient = albedo.rgb * AmbientColor.rgb;
    float3 rim     = albedo.rgb * SunColor.rgb * backlit;

    // Small sway-to-AO modulation: vertices that moved a lot are usually at
    // the canopy edge so they receive more sky light — reduce AO there.
    float sway = saturate(i.WindSway * 2.0);
    float ao   = 1.0 - (FoliageParams.w * (1.0 - sway));

    float3 color = (diffuse + ambient + rim) * ao;
    return float4(color, 1.0);
}
