// NOT the shader the Windows renderer compiles at runtime.
//
// GraphicsEngine::InitializeBasicShaders compiles the copy embedded in
// GraphicsDeviceResourcesWindowsShaders.cpp, so editing this file changes
// nothing on Windows. This file is (a) what the Linux/Vulkan RHI bridge
// registers as "basic_ps" from Shaders/HLSL/BasicPS.hlsl, and (b) what
// TestShaderCompilerReal compiles to prove the layouts below still match
// PerFrameConstants/PerObjectConstants. Change the embedded source and this
// file together.
//
// Basic pixel shader.
//
// b1 MUST mirror Spark::PerFrameConstants and b0 Spark::PerObjectConstants
// (Graphics/Shader.h), matching the embedded fallback in
// GraphicsDeviceResourcesWindowsShaders.cpp. Declaring a shorter cbuffer here
// silently shifts every field the engine writes.

cbuffer PerFrameConstants : register(b1)
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

    float3 DirectionalLightDir;
    float  DirectionalLightIntensity;
    float3 DirectionalLightColor;
    float  AmbientIntensity;
    float3 AmbientColor;
    float  _padding1;
};

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
    float4 UVTiling;
};

Texture2D MainTexture : register(t0);
SamplerState MainSampler : register(s0);

struct PixelInput
{
    float4 Position : SV_POSITION;
    float3 WorldPos : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Color    : COLOR;
};

float4 main(PixelInput input) : SV_TARGET
{
    float4 texColor = MainTexture.Sample(MainSampler, input.TexCoord);

    float3 normal = normalize(input.Normal);
    float3 lightDir = normalize(-DirectionalLightDir);
    float NdotL = max(0.0f, dot(normal, lightDir));

    float3 diffuse = DirectionalLightColor * DirectionalLightIntensity * NdotL;
    float3 ambient = AmbientColor * AmbientIntensity;

    // Soft camera-facing fill, matching the embedded shader so the on-disk and
    // embedded variants render identically.
    float3 viewDir = normalize(CameraPosition - input.WorldPos);
    float fill = 0.35f * max(0.0f, dot(normal, viewDir));

    float3 lighting = diffuse + ambient + float3(fill, fill * 0.95f, fill * 0.88f);

    float4 finalColor = texColor * input.Color;
    finalColor.rgb *= lighting;

    // Emissive (MaterialProperties.z, default 0) adds the surface color back
    // unlit; alpha (MaterialProperties.w, default 1) is a no-op for opaques.
    finalColor.rgb += texColor.rgb * input.Color.rgb * MaterialProperties.z;
    finalColor.a = texColor.a * input.Color.a * MaterialProperties.w;

    return finalColor;
}
