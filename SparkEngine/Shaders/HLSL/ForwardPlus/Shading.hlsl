// Shading.hlsl - Forward+ lit shading pass
// Reads the per-tile light list produced by LightCull.hlsl and evaluates
// lighting for each fragment using only the lights assigned to its tile.

#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 256

// --------------------------------------------------------------------------
// Constant Buffers
// --------------------------------------------------------------------------

cbuffer PerFrame : register(b0)
{
    float4x4 View;
    float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
    uint ScreenWidth;
    uint ScreenHeight;
    float2 _FramePad;
};

cbuffer PerObject : register(b1)
{
    float4x4 World;
    float4x4 WorldInvTranspose;
};

cbuffer PerMaterial : register(b2)
{
    float3 Albedo;
    float Metallic;
    float Roughness;
    float AO;
    float2 _MatPad;
};

// --------------------------------------------------------------------------
// Light Data (must match LightCull.hlsl layout)
// --------------------------------------------------------------------------

struct LightData
{
    float3 PositionVS;
    float Range;
    float3 Color;
    float Intensity;
    float3 Direction;
    uint Type; // 0 = Directional, 1 = Point, 2 = Spot
};

struct TileLightData
{
    uint LightCount;
    uint LightIndices[MAX_LIGHTS_PER_TILE];
};

StructuredBuffer<LightData> LightBuffer : register(t0);
StructuredBuffer<TileLightData> TileLightBuffer : register(t1);
Texture2D DiffuseTexture : register(t2);
SamplerState LinearSampler : register(s0);

// --------------------------------------------------------------------------
// Vertex Shader
// --------------------------------------------------------------------------

struct VS_INPUT
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position  : SV_POSITION;
    float3 WorldPos  : TEXCOORD0;
    float3 Normal    : TEXCOORD1;
    float2 TexCoord  : TEXCOORD2;
    float3 ViewPos   : TEXCOORD3;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    float4 worldPos = mul(float4(input.Position, 1.0), World);
    output.Position = mul(worldPos, ViewProjection);
    output.WorldPos = worldPos.xyz;
    output.Normal = normalize(mul(input.Normal, (float3x3)WorldInvTranspose));
    output.TexCoord = input.TexCoord;
    output.ViewPos = mul(worldPos, View).xyz;
    return output;
}

// --------------------------------------------------------------------------
// PBR Helpers
// --------------------------------------------------------------------------

static const float PI = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// --------------------------------------------------------------------------
// Pixel Shader
// --------------------------------------------------------------------------

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    // Determine which tile this fragment belongs to
    uint2 pixelCoord = uint2(input.Position.xy);
    uint tileX = pixelCoord.x / TILE_SIZE;
    uint tileY = pixelCoord.y / TILE_SIZE;
    uint tilesX = (ScreenWidth + TILE_SIZE - 1) / TILE_SIZE;
    uint tileIndex = tileY * tilesX + tileX;

    // Sample material
    float3 albedo = Albedo * DiffuseTexture.Sample(LinearSampler, input.TexCoord).rgb;
    float3 N = normalize(input.Normal);
    float3 V = normalize(CameraPosition - input.WorldPos);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, Metallic);

    // Accumulate lighting from tile's light list
    float3 Lo = float3(0, 0, 0);
    TileLightData tile = TileLightBuffer[tileIndex];

    for (uint i = 0; i < tile.LightCount; ++i)
    {
        LightData light = LightBuffer[tile.LightIndices[i]];

        float3 L;
        float attenuation = 1.0;

        if (light.Type == 0) // Directional
        {
            L = normalize(-light.Direction);
        }
        else // Point or Spot
        {
            float3 toLight = light.PositionVS - input.ViewPos;
            float dist = length(toLight);
            L = normalize(toLight);
            attenuation = saturate(1.0 - (dist * dist) / (light.Range * light.Range));
            attenuation *= attenuation;
        }

        float3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        float NDF = DistributionGGX(N, H, Roughness);
        float G = GeometrySchlickGGX(max(dot(N, V), 0.0), Roughness)
                * GeometrySchlickGGX(NdotL, Roughness);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

        float3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.001;
        float3 specular = numerator / denominator;

        float3 kD = (1.0 - F) * (1.0 - Metallic);
        Lo += (kD * albedo / PI + specular) * light.Color * light.Intensity * attenuation * NdotL;
    }

    // Ambient
    float3 ambient = float3(0.03, 0.03, 0.03) * albedo * AO;
    float3 color = ambient + Lo;

    // Tone mapping (Reinhard)
    color = color / (color + 1.0);

    return float4(color, 1.0);
}
