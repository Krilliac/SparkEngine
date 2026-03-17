/**
 * @file DXRShadows.hlsl
 * @brief DXR 1.1 soft shadow ray tracing — multi-sample jittered shadow rays
 *
 * Extracted from DXRSupport.cpp inline HLSL. Requires SM 6.5+ and DXR pipeline.
 */

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float> OutputShadows : register(u0);
Texture2D<float> GBufferDepth : register(t1);
Texture2D<float4> GBufferNormals : register(t2);

cbuffer ShadowConstants : register(b0)
{
    float4x4 InvViewProj;
    float3 LightDirection;
    float SoftShadowRadius;
    int SamplesPerPixel;
    float3 _Padding;
};

struct ShadowPayload
{
    float visibility;
};

float Random(uint2 seed, uint frame)
{
    uint h = seed.x * 1597334677u ^ seed.y * 3812015801u ^ frame * 2654435761u;
    h = ((h >> 16u) ^ h) * 0x45d9f3bu;
    h = ((h >> 16u) ^ h) * 0x45d9f3bu;
    h = (h >> 16u) ^ h;
    return float(h) / 4294967295.0;
}

[shader("raygeneration")]
void ShadowRayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

    float depth = GBufferDepth[launchIndex];
    if (depth >= 1.0)
    {
        OutputShadows[launchIndex] = 1.0;
        return;
    }

    float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = mul(InvViewProj, clipPos);
    worldPos /= worldPos.w;

    float totalVisibility = 0.0;
    for (int s = 0; s < SamplesPerPixel; s++)
    {
        float r1 = Random(launchIndex, s * 2);
        float r2 = Random(launchIndex, s * 2 + 1);
        float3 jitter = float3(r1 - 0.5, r2 - 0.5, r1 * r2 - 0.25) * SoftShadowRadius;
        float3 dir = normalize(-LightDirection + jitter);

        RayDesc ray;
        ray.Origin = worldPos.xyz + normal * 0.01;
        ray.Direction = dir;
        ray.TMin = 0.001;
        ray.TMax = 1000.0;

        ShadowPayload payload;
        payload.visibility = 1.0;

        TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF, 0, 0, 0, ray, payload);

        totalVisibility += payload.visibility;
    }

    OutputShadows[launchIndex] = totalVisibility / float(SamplesPerPixel);
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.visibility = 1.0;
}
