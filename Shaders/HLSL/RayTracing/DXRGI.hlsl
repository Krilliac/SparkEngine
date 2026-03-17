/**
 * @file DXRGI.hlsl
 * @brief DXR 1.1 ray-traced global illumination — cosine-weighted hemisphere sampling for indirect light
 *
 * Dispatched via DXRManager::TraceGlobalIllumination(). Requires SM 6.5+ and DXR pipeline.
 * Uses multi-bounce cosine-weighted hemisphere sampling for diffuse indirect illumination.
 */

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> OutputGI : register(u0);
Texture2D<float> GBufferDepth : register(t1);
Texture2D<float4> GBufferNormals : register(t2);
Texture2D<float4> GBufferAlbedo : register(t3);

cbuffer GIConstants : register(b0)
{
    float4x4 InvViewProj;
    float3 CameraPosition;
    float MaxDistance;
    float3 LightDirection;
    float LightIntensity;
    int SamplesPerPixel;
    int MaxBounces;
    float2 _Padding;
};

struct GIPayload
{
    float3 radiance;
    float hitT;
    int bounceDepth;
    float _pad;
};

float Random(uint2 seed, uint frame)
{
    uint h = seed.x * 1597334677u ^ seed.y * 3812015801u ^ frame * 2654435761u;
    h = ((h >> 16u) ^ h) * 0x45d9f3bu;
    h = ((h >> 16u) ^ h) * 0x45d9f3bu;
    h = (h >> 16u) ^ h;
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
void GIRayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

    float depth = GBufferDepth[launchIndex];
    if (depth >= 1.0)
    {
        OutputGI[launchIndex] = float4(0, 0, 0, 0);
        return;
    }

    float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
    float3 albedo = GBufferAlbedo[launchIndex].rgb;
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = mul(InvViewProj, clipPos);
    worldPos /= worldPos.w;

    float3 totalRadiance = float3(0, 0, 0);
    for (int s = 0; s < SamplesPerPixel; s++)
    {
        float r1 = Random(launchIndex, s * 2);
        float r2 = Random(launchIndex, s * 2 + 1);
        float3 dir = CosineWeightedHemisphere(r1, r2, normal);

        RayDesc ray;
        ray.Origin = worldPos.xyz + normal * 0.01;
        ray.Direction = dir;
        ray.TMin = 0.001;
        ray.TMax = MaxDistance;

        GIPayload payload;
        payload.radiance = float3(0, 0, 0);
        payload.hitT = -1.0;
        payload.bounceDepth = 0;

        TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

        totalRadiance += payload.radiance;
    }

    // Cosine-weighted hemisphere sampling has implicit 1/pi PDF, multiply by albedo
    float3 gi = albedo * totalRadiance / float(SamplesPerPixel);
    OutputGI[launchIndex] = float4(gi, 1.0);
}

[shader("closesthit")]
void GIClosestHit(inout GIPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    // Simple diffuse bounce: approximate as direct light contribution at hit point
    float3 hitNormal = float3(0, 1, 0); // Simplified — full implementation reads vertex normals
    float ndotl = saturate(dot(hitNormal, -LightDirection));
    payload.radiance = float3(0.5, 0.5, 0.5) * ndotl * LightIntensity;
    payload.hitT = RayTCurrent();

    // Multi-bounce: trace secondary rays if budget allows
    if (payload.bounceDepth < MaxBounces - 1)
    {
        float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        float r1 = Random(uint2(asuint(hitPos.x), asuint(hitPos.z)), payload.bounceDepth);
        float r2 = Random(uint2(asuint(hitPos.y), asuint(hitPos.x)), payload.bounceDepth + 1);
        float3 bounceDir = CosineWeightedHemisphere(r1, r2, hitNormal);

        RayDesc bounceRay;
        bounceRay.Origin = hitPos + hitNormal * 0.01;
        bounceRay.Direction = bounceDir;
        bounceRay.TMin = 0.001;
        bounceRay.TMax = MaxDistance * 0.5;

        GIPayload bouncePayload;
        bouncePayload.radiance = float3(0, 0, 0);
        bouncePayload.hitT = -1.0;
        bouncePayload.bounceDepth = payload.bounceDepth + 1;

        TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, bounceRay, bouncePayload);

        // Accumulate bounce contribution with energy conservation
        payload.radiance += bouncePayload.radiance * 0.5;
    }
}

[shader("miss")]
void GIMiss(inout GIPayload payload)
{
    // Sky contribution for indirect illumination
    float3 skyColor = float3(0.3, 0.5, 0.8) * LightIntensity * 0.2;
    payload.radiance = skyColor;
    payload.hitT = -1.0;
}
