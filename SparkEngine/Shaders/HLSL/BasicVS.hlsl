// Legacy copy - superseded by <repo>/Shaders/HLSL/BasicVS.hlsl.
//
// The cbuffer below is the old World/View/Projection block at b1; the engine
// binds PerObjectConstants at b0 (Graphics/Shader.h), so this source does not
// match what the renderer writes. No in-tree code path loads it: Windows
// compiles the source embedded in GraphicsDeviceResourcesWindowsShaders.cpp,
// and the Linux RHI bridge registers "Shaders/HLSL/BasicVS.hlsl", which CMake
// stages from the repo-root Shaders/HLSL tree. This file is staged to
// bin/Shaders/BasicVS.hlsl, where nothing reads it. Edit the repo-root copy.

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
