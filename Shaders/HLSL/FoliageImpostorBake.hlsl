/**
 * @file FoliageImpostorBake.hlsl
 * @brief Vertex+pixel shader pair used to bake a foliage mesh into an impostor atlas slot.
 *
 * Driven by FoliageImpostorAtlas::BakeSlot. The CPU side fits an
 * orthographic projection around the mesh's bounding box, builds one
 * view matrix per yaw angle, and renders the mesh into the cell at
 * (slot.atlasX + angleIndex * cellSize, slot.atlasY) using a viewport
 * the size of one cell. The PS approximates lit foliage with a simple
 * lambertian green tint so the impostor reads correctly even when no
 * diffuse texture is bound.
 */

cbuffer ImpostorBakeConstants : register(b0)
{
    float4x4 ViewProj;     ///< Combined ortho view-proj for the current angle.
    float4 FoliageTint;    ///< Per-species base colour (rgb) + alpha.
    float3 LightDirection; ///< World-space sun direction (normalised).
    float Padding0;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR;
    uint4 bones : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.svPosition = mul(float4(input.position, 1.0), ViewProj);
    output.worldNormal = normalize(input.normal);
    output.uv = input.uv0;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // Lambertian shading with a fixed half-lambert wrap so the impostor
    // never goes fully black on back-facing leaves. Foliage tint is
    // multiplied in so each species can use its own colour without a
    // separate texture binding during the bake.
    float ndotl = saturate(dot(input.worldNormal, -LightDirection));
    float wrapped = ndotl * 0.5 + 0.5;
    float3 color = FoliageTint.rgb * wrapped;
    return float4(color, FoliageTint.a);
}
