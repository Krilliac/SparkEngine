cbuffer ConstantBuffer : register(b1)
{
    matrix World;
    matrix View;
    matrix Projection;
};

struct VS_INPUT
{
    float4 Pos     : POSITION;
    float3 Normal  : NORMAL;
    float2 Tex     : TEXCOORD0;
};

struct PS_INPUT
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : POSITION1;
    float3 Normal   : NORMAL;
    float2 Tex      : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;

    // World-space position
    float4 worldPos = mul(input.Pos, World);
    output.WorldPos = worldPos.xyz;

    // Clip-space position
    output.Pos = mul(mul(worldPos, View), Projection);

    // Transform normal
    output.Normal = normalize(mul(input.Normal, (float3x3)World));

    // Texture coordinates
    output.Tex = input.Tex;

    return output;
}
