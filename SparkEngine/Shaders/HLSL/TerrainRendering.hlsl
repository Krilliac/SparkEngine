// =============================================================================
// TerrainRendering.hlsl — Clipmap terrain with height displacement
//
// Vertex shader displaces terrain mesh using heightmap. Pixel shader uses
// tri-planar texturing with slope-based material blending (grass/rock/snow).
// =============================================================================

cbuffer TerrainConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 World;
    float3 CameraPosition;
    float HeightmapSize;
    float3 WorldOffset;
    float TerrainScale;
    float3 SunDirection;
    float SunIntensity;
    float2 TerrainSize; // world-space XZ size
    float HeightScale;
    float TexelSize; // 1.0 / HeightmapSize
    float4 MaterialWeights; // x=grass height, y=rock slope, z=snow height, w=blend sharpness
};

Texture2D<float> Heightmap : register(t0);
Texture2D GrassAlbedo : register(t1);
Texture2D RockAlbedo : register(t2);
Texture2D SnowAlbedo : register(t3);
Texture2D GrassNormal : register(t4);
Texture2D RockNormal : register(t5);
SamplerState TerrainSampler : register(s0);

struct VS_INPUT
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
    float Height : TEXCOORD3;
};

// Sample heightmap with bilinear filtering
float SampleHeight(float2 uv)
{
    return Heightmap.SampleLevel(TerrainSampler, uv, 0).r * HeightScale;
}

// Compute normal from heightmap using central differences
float3 ComputeNormalFromHeightmap(float2 uv)
{
    float hL = SampleHeight(uv - float2(TexelSize, 0));
    float hR = SampleHeight(uv + float2(TexelSize, 0));
    float hD = SampleHeight(uv - float2(0, TexelSize));
    float hU = SampleHeight(uv + float2(0, TexelSize));

    float3 normal;
    normal.x = (hL - hR) / (2.0 * TexelSize * TerrainSize.x);
    normal.z = (hD - hU) / (2.0 * TexelSize * TerrainSize.y);
    normal.y = 1.0;

    return normalize(normal);
}

PS_INPUT VS_Terrain(VS_INPUT input)
{
    PS_INPUT output;

    // World position with height displacement
    float3 worldPos = input.Position + WorldOffset;
    float height = SampleHeight(input.TexCoord);
    worldPos.y += height;

    output.WorldPos = worldPos;
    output.Position = mul(float4(worldPos, 1.0), ViewProjection);
    output.Normal = ComputeNormalFromHeightmap(input.TexCoord);
    output.TexCoord = input.TexCoord;
    output.Height = height;

    return output;
}

// Tri-planar texture mapping (avoids stretching on steep surfaces)
float4 TriPlanarSample(Texture2D tex, float3 worldPos, float3 normal, float scale)
{
    float3 blendWeights = abs(normal);
    blendWeights = pow(blendWeights, 4.0);
    blendWeights /= (blendWeights.x + blendWeights.y + blendWeights.z);

    float4 xProj = tex.Sample(TerrainSampler, worldPos.yz * scale);
    float4 yProj = tex.Sample(TerrainSampler, worldPos.xz * scale);
    float4 zProj = tex.Sample(TerrainSampler, worldPos.xy * scale);

    return xProj * blendWeights.x + yProj * blendWeights.y + zProj * blendWeights.z;
}

float4 PS_Terrain(PS_INPUT input) : SV_Target
{
    float3 normal = normalize(input.Normal);
    float slope = 1.0 - normal.y; // 0 = flat, 1 = vertical
    float height = input.Height;

    // Material blending based on slope and height
    float grassWeight = saturate(1.0 - slope * MaterialWeights.w) *
                        saturate(1.0 - (height - MaterialWeights.x) * 0.1);
    float rockWeight = saturate(slope * MaterialWeights.w - MaterialWeights.y);
    float snowWeight = saturate((height - MaterialWeights.z) * 0.05);

    // Normalize weights
    float totalWeight = grassWeight + rockWeight + snowWeight + 0.001;
    grassWeight /= totalWeight;
    rockWeight /= totalWeight;
    snowWeight /= totalWeight;

    // Tri-planar sampling for each material
    float texScale = 0.1; // world-space texture tiling
    float4 grass = TriPlanarSample(GrassAlbedo, input.WorldPos, normal, texScale);
    float4 rock = TriPlanarSample(RockAlbedo, input.WorldPos, normal, texScale * 0.5);
    float4 snow = TriPlanarSample(SnowAlbedo, input.WorldPos, normal, texScale);

    float4 albedo = grass * grassWeight + rock * rockWeight + snow * snowWeight;

    // Normal mapping (grass layer only, blended)
    float3 grassNorm = TriPlanarSample(GrassNormal, input.WorldPos, normal, texScale).xyz * 2.0 - 1.0;
    float3 rockNorm = TriPlanarSample(RockNormal, input.WorldPos, normal, texScale * 0.5).xyz * 2.0 - 1.0;
    float3 detailNormal = normalize(lerp(grassNorm, rockNorm, rockWeight));
    float3 finalNormal = normalize(normal + detailNormal * 0.3);

    // Simple directional lighting
    float NdotL = saturate(dot(finalNormal, SunDirection));
    float3 ambient = float3(0.15, 0.18, 0.25);
    float3 diffuse = albedo.rgb * NdotL * SunIntensity;

    // Distance fog
    float dist = length(input.WorldPos - CameraPosition);
    float fogFactor = saturate((dist - 200.0) / 800.0);
    float3 fogColor = float3(0.7, 0.8, 0.95);

    float3 finalColor = ambient * albedo.rgb + diffuse;
    finalColor = lerp(finalColor, fogColor, fogFactor);

    return float4(finalColor, 1.0);
}
