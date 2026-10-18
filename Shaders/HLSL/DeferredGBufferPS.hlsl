// D3D11 deferred geometry pixel shader. The runtime loads this file through
// GraphicsEngine::CompileShaderFromFile during basic shader initialization.

cbuffer PerObjectConstants : register(b0)
{
    matrix World;
    matrix WorldViewProjection;
    matrix WorldInverseTranspose;
    matrix PreviousWorld;
    float3 ObjectPosition;
    float ObjectScale;
    float4 ObjectColor;
    float4 MaterialProperties;
    float4 UVTiling;
};

Texture2D MainTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D RoughnessTexture : register(t2);
SamplerState MainSampler : register(s0);

struct PixelInput
{
    float4 Position : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR;
};

struct GBufferOutput
{
    float4 Albedo : SV_TARGET0;
    float4 Normal : SV_TARGET1;
    float4 Material : SV_TARGET2;
};

float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    // Co-vectors perpendicular to N: projecting the edge vectors
    // through these solves for T (du direction) and B (dv direction)
    // while keeping both in the surface plane.
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // Scale-invariant normalization (max keeps the T:B aspect so
    // mirrored/anisotropic UVs stay correct). Guard degenerate UVs:
    // constant texcoords across the pixel quad make T = B = 0, and
    // rsqrt(0) = INF would turn the frame into NaNs; forcing 0 makes
    // the tangential components drop out so N passes through instead.
    float maxLen2 = max(dot(T, T), dot(B, B));
    float invmax = (maxLen2 > 1e-10f) ? rsqrt(maxLen2) : 0.0f;
    return float3x3(T * invmax, B * invmax, N);
}

GBufferOutput main(PixelInput input)
{
    GBufferOutput output = (GBufferOutput)0;
    float4 albedo = MainTexture.Sample(MainSampler, input.TexCoord) * input.Color;
    float3 geometricNormal = normalize(input.Normal);
    float3 tangentNormal = NormalTexture.Sample(MainSampler, input.TexCoord).xyz * 2.0f - 1.0f;
    float3 normal = normalize(mul(tangentNormal, CotangentFrame(geometricNormal, input.WorldPos, input.TexCoord)));

    output.Albedo = albedo;
    output.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    output.Material = MaterialProperties;
    output.Material.y = saturate(RoughnessTexture.Sample(MainSampler, input.TexCoord).r);
    return output;
}
