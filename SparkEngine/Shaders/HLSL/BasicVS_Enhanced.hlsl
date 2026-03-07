// BasicVS.hlsl - Enhanced basic vertex shader with modern features
// SparkEngine - Professional shader system with console integration

cbuffer PerFrame : register(b0)
{
    matrix ViewMatrix;
    matrix ProjectionMatrix;
    matrix ViewProjectionMatrix;
    float3 CameraPosition;
    float  Time;
    float3 CameraDirection;
    float  DeltaTime;
    float2 ScreenResolution;
    float2 InvScreenResolution;
    
    // Lighting parameters
    float3 DirectionalLightDir;
    float  DirectionalLightIntensity;
    float3 DirectionalLightColor;
    float  AmbientIntensity;
    float3 AmbientColor;
    float  _padding1;
};

cbuffer PerObject : register(b1)
{
    matrix WorldMatrix;
    matrix WorldViewProjectionMatrix;
    matrix WorldInverseTransposeMatrix;
    matrix PreviousWorldMatrix;
    float3 ObjectPosition;
    float  ObjectScale;
    float4 ObjectColor;
    float4 MaterialProperties; // x: metallic, y: roughness, z: emissive, w: alpha
    float4 UVTiling;           // xy: tiling, zw: offset
};

cbuffer PerMaterial : register(b2)
{
    float4 AlbedoColor;
    float  MetallicFactor;
    float  RoughnessFactor;
    float  NormalScale;
    float  OcclusionStrength;
    float  EmissiveFactor;
    float  AlphaCutoff;
    float2 _materialPadding;
};

// Input vertex structure
struct VS_INPUT
{
    float3 Position  : POSITION;
    float3 Normal    : NORMAL;
    float2 TexCoord  : TEXCOORD0;
    float4 Color     : COLOR0;
};

// Output structure for pixel shader
struct PS_INPUT
{
    float4 Position      : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal        : NORMAL;
    float2 TexCoord0     : TEXCOORD0;
    float4 Color         : COLOR0;
};

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    
    // Transform to world space
    float4 worldPosition = mul(float4(input.Position, 1.0), WorldMatrix);
    output.WorldPosition = worldPosition.xyz;
    
    // Transform to clip space
    output.Position = mul(worldPosition, ViewProjectionMatrix);
    
    // Transform normal to world space
    output.Normal = normalize(mul(input.Normal, (float3x3)WorldInverseTransposeMatrix));
    
    // Apply texture coordinate transformations
    output.TexCoord0 = input.TexCoord * UVTiling.xy + UVTiling.zw;
    
    // Pass through color with object color modulation
    output.Color = input.Color * ObjectColor;
    
    return output;
}