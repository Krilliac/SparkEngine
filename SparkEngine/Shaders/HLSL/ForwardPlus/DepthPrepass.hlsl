// DepthPrepass.hlsl - Forward+ depth pre-pass vertex shader
// Renders geometry to the depth buffer only (no color output).
// Used as Phase 1 of the Forward+ pipeline to populate the depth buffer
// before the light culling compute pass.

cbuffer PerFrame : register(b0)
{
    float4x4 ViewProjection;
};

cbuffer PerObject : register(b1)
{
    float4x4 World;
};

struct VS_INPUT
{
    float3 Position : POSITION;
};

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    float4 worldPos = mul(float4(input.Position, 1.0), World);
    output.Position = mul(worldPos, ViewProjection);
    return output;
}
