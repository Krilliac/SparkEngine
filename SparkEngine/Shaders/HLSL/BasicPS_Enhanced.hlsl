// BasicPS.hlsl - Enhanced basic pixel shader with modern features
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

// Textures
Texture2D AlbedoTexture    : register(t0);
Texture2D NormalTexture    : register(t1);
Texture2D MetallicTexture  : register(t2);
Texture2D RoughnessTexture : register(t3);

// Samplers
SamplerState LinearSampler : register(s0);
SamplerState PointSampler  : register(s1);

// Input from vertex shader
struct PS_INPUT
{
    float4 Position      : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal        : NORMAL;
    float2 TexCoord0     : TEXCOORD0;
    float4 Color         : COLOR0;
};

// Simple Blinn-Phong lighting for basic shader
float3 CalculateBlinnPhong(float3 normal, float3 lightDir, float3 viewDir, float3 albedo)
{
    // Ambient
    float3 ambient = AmbientColor * AmbientIntensity * albedo;
    
    // Diffuse
    float NdotL = max(dot(normal, lightDir), 0.0);
    float3 diffuse = DirectionalLightColor * DirectionalLightIntensity * NdotL * albedo;
    
    // Specular (Blinn-Phong)
    float3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    float specularPower = 32.0; // Fixed shininess for basic shader
    float specular = pow(NdotH, specularPower);
    float3 specularColor = DirectionalLightColor * DirectionalLightIntensity * specular * 0.3;
    
    return ambient + diffuse + specularColor;
}

float4 main(PS_INPUT input) : SV_Target
{
    // Sample albedo texture
    float4 albedoSample = AlbedoTexture.Sample(LinearSampler, input.TexCoord0);
    float3 albedo = albedoSample.rgb * AlbedoColor.rgb * input.Color.rgb * ObjectColor.rgb;
    float alpha = albedoSample.a * AlbedoColor.a * input.Color.a * ObjectColor.a;
    
    // Alpha testing
    if (alpha < AlphaCutoff)
        discard;
    
    // Normalize interpolated normal
    float3 normal = normalize(input.Normal);
    
    // Calculate lighting
    float3 lightDir = normalize(-DirectionalLightDir);
    float3 viewDir = normalize(CameraPosition - input.WorldPosition);
    
    float3 finalColor = CalculateBlinnPhong(normal, lightDir, viewDir, albedo);
    
    // Add simple emissive if available
    float emissiveStrength = MetallicTexture.Sample(LinearSampler, input.TexCoord0).b * EmissiveFactor;
    finalColor += albedo * emissiveStrength;
    
    // Simple exposure and gamma correction
    finalColor *= 1.0; // Exposure
    finalColor = pow(abs(finalColor), 1.0/2.2); // Gamma correction
    
    return float4(finalColor, alpha);
}