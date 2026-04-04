// =============================================================================
// VolumetricFog.hlsl — Froxel-based volumetric fog compute shader
//
// Dispatched per-froxel: accumulates density, applies Henyey-Greenstein
// phase function, integrates light contribution along view ray.
// =============================================================================

cbuffer FogConstants : register(b0)
{
    float4x4 InverseViewProjection;
    float3 FogColor;
    float FogDensity;
    float3 LightDirection;
    float ScatteringCoeff;
    float3 LightColor;
    float AbsorptionCoeff;
    float3 CameraPosition;
    float PhaseG; // Henyey-Greenstein asymmetry parameter
    float NearPlane;
    float FarPlane;
    float2 ScreenSize;
    uint3 FroxelDimensions; // e.g., 160x90x64
    float TimeSeconds;
    float HeightFogStart;
    float HeightFogEnd;
    float NoiseScale;
    float NoiseIntensity;
};

Texture3D<float> NoiseTex : register(t0);
Texture2D<float> ShadowMap : register(t1);
SamplerState LinearSampler : register(s0);

RWTexture3D<float4> FogVolume : register(u0);

// Henyey-Greenstein phase function
float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(max(denom, 0.0001)));
}

// Convert froxel coordinates to world space
float3 FroxelToWorld(uint3 froxelCoord)
{
    float2 uv = (float2(froxelCoord.xy) + 0.5) / float2(FroxelDimensions.xy);
    float zSlice = float(froxelCoord.z) / float(FroxelDimensions.z);

    // Exponential depth distribution for better near-camera precision
    float depth = NearPlane * pow(FarPlane / NearPlane, zSlice);

    float2 ndc = uv * 2.0 - 1.0;
    ndc.y *= -1.0;
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldPos = mul(clipPos, InverseViewProjection);
    float3 rayDir = normalize(worldPos.xyz / worldPos.w - CameraPosition);

    return CameraPosition + rayDir * depth;
}

// Sample procedural fog density with 3D noise
float SampleFogDensity(float3 worldPos)
{
    float baseDensity = FogDensity;

    // Height-based fog attenuation
    float heightFactor = 1.0;
    if (HeightFogEnd > HeightFogStart)
    {
        heightFactor = saturate((HeightFogEnd - worldPos.y) / (HeightFogEnd - HeightFogStart));
        heightFactor = heightFactor * heightFactor; // quadratic falloff
    }

    // Animated noise for natural-looking fog
    float3 noiseUV = worldPos * NoiseScale * 0.01 + float3(TimeSeconds * 0.02, 0, TimeSeconds * 0.01);
    float noise = NoiseTex.SampleLevel(LinearSampler, noiseUV, 0).r;
    float noiseFactor = lerp(1.0, noise, NoiseIntensity);

    return baseDensity * heightFactor * noiseFactor;
}

[numthreads(8, 8, 1)]
void CS_VolumetricFog(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId >= FroxelDimensions))
        return;

    float3 worldPos = FroxelToWorld(dispatchId);
    float density = SampleFogDensity(worldPos);

    // View ray direction for phase function
    float3 viewDir = normalize(worldPos - CameraPosition);
    float cosTheta = dot(viewDir, -LightDirection);
    float phase = HenyeyGreenstein(cosTheta, PhaseG);

    // In-scattering: light contribution through fog
    float3 inScatter = LightColor * ScatteringCoeff * phase * density;

    // Absorption
    float extinction = (ScatteringCoeff + AbsorptionCoeff) * density;

    // Store scattering (rgb) and extinction (a) for front-to-back compositing
    float4 result = float4(inScatter * FogColor, extinction);

    FogVolume[dispatchId] = result;
}
