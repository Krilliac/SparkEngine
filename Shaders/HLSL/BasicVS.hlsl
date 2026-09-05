// NOT the shader the Windows renderer compiles at runtime.
//
// GraphicsEngine::InitializeBasicShaders compiles the copy embedded in
// GraphicsDeviceResourcesWindowsShaders.cpp, so editing this file changes
// nothing on Windows. This file is (a) what the Linux/Vulkan RHI bridge
// registers as "basic_vs" from Shaders/HLSL/BasicVS.hlsl, and (b) what
// TestShaderCompilerReal compiles to prove the layout below still matches
// PerObjectConstants. Change the embedded source and this file together.
//
// Basic vertex shader.
//
// The b0 layout MUST mirror Spark::PerObjectConstants (Graphics/Shader.h) and
// the embedded fallback in GraphicsDeviceResourcesWindowsShaders.cpp exactly.
// PreviousWorld was once missing here, which shifted every following member by
// 64 bytes and made ObjectColor read as rgba(0,0,0,0) - the black-screen bug.

cbuffer PerObjectConstants : register(b0)
{
    matrix World;
    matrix WorldViewProjection;
    matrix WorldInverseTranspose;
    matrix PreviousWorld;
    float3 ObjectPosition;
    float  ObjectScale;
    float4 ObjectColor;
    float4 MaterialProperties; // x: metallic, y: roughness, z: emissive, w: alpha
    float4 UVTiling;           // xy: tiling, zw: offset
};

struct VertexInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Color    : COLOR;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output = (VertexOutput)0;

    output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
    output.WorldPos = mul(float4(input.Position, 1.0f), World).xyz;
    output.Normal = normalize(mul(input.Normal, (float3x3)WorldInverseTranspose));
    output.TexCoord = input.TexCoord * UVTiling.xy + UVTiling.zw;
    output.Color = ObjectColor;

    return output;
}
