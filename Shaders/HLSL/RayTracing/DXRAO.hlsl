/**
 * @file DXRAO.hlsl
 * @brief DXR 1.1 ray-traced ambient occlusion — cosine-weighted hemisphere sampling
 *
 * Extracted from DXRSupport.cpp inline HLSL. Requires SM 6.5+ and DXR pipeline.
 */

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float> OutputAO : register(u0);
Texture2D<float> GBufferDepth : register(t1);
Texture2D<float4> GBufferNormals : register(t2);

cbuffer AOConstants : register(b0)
{
    float4x4 InvViewProj;
    float3 CameraPosition;
    float AORadius;
    float AOPower;
    int SamplesPerPixel;
    float2 _Padding;
};

struct AOPayload
{
    float occlusion;
};

float Random(uint2 seed, uint frame)
{
    uint h = seed.x * 1597334677u ^ seed.y * 3812015801u ^ frame * 2654435761u;
    h = ((h >> 16u) ^ h) * 0x45d9f3bu;
    return float(h) / 4294967295.0;
}

float3 CosineWeightedHemisphere(float r1, float r2, float3 normal)
{
    float phi = 2.0 * 3.14159265 * r1;
    float cosTheta = sqrt(r2);
    float sinTheta = sqrt(1.0 - r2);

    float3 tangent = abs(normal.x) > 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 bitangent = normalize(cross(normal, tangent));
    tangent = cross(bitangent, normal);

    return normalize(tangent * cos(phi) * sinTheta + bitangent * sin(phi) * sinTheta + normal * cosTheta);
}

[shader("raygeneration")]
void AORayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

    float depth = GBufferDepth[launchIndex];
    if (depth >= 1.0)
    {
        OutputAO[launchIndex] = 1.0;
        return;
    }

    float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = mul(InvViewProj, clipPos);
    worldPos /= worldPos.w;

    float totalOcclusion = 0.0;
    for (int s = 0; s < SamplesPerPixel; s++)
    {
        float r1 = Random(launchIndex, s * 2);
        float r2 = Random(launchIndex, s * 2 + 1);
        float3 dir = CosineWeightedHemisphere(r1, r2, normal);

        RayDesc ray;
        ray.Origin = worldPos.xyz + normal * 0.01;
        ray.Direction = dir;
        ray.TMin = 0.001;
        ray.TMax = AORadius;

        AOPayload payload;
        payload.occlusion = 0.0;

        TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF, 0, 0, 0, ray, payload);

        totalOcclusion += payload.occlusion;
    }

    float ao = 1.0 - pow(totalOcclusion / float(SamplesPerPixel), AOPower);
    OutputAO[launchIndex] = ao;
}

[shader("miss")]
void AOMiss(inout AOPayload payload)
{
    payload.occlusion = 0.0;
}

[shader("closesthit")]
void AOClosestHit(inout AOPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.occlusion = 1.0;
}
