/**
 * @file DXRReflections.hlsl
 * @brief DXR 1.1 ray-traced reflections — ray generation, closest hit, and miss shaders
 *
 * Extracted from DXRSupport.cpp inline HLSL. Requires SM 6.5+ and DXR pipeline.
 * Dispatched via DispatchRays() through DXRManager::TraceReflections().
 */

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> OutputReflections : register(u0);
Texture2D<float4> GBufferNormals : register(t1);
Texture2D<float> GBufferDepth : register(t2);
Texture2D<float4> GBufferAlbedo : register(t3);

cbuffer RTConstants : register(b0)
{
    float4x4 InvViewProj;
    float3 CameraPosition;
    float MaxDistance;
    int MaxBounces;
    int SamplesPerPixel;
    float RoughnessThreshold;
    float _Padding;
};

struct RayPayload
{
    float4 color;
    int depth;
};

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);

    float depth = GBufferDepth[launchIndex];
    if (depth >= 1.0)
    {
        OutputReflections[launchIndex] = float4(0, 0, 0, 0);
        return;
    }

    float3 normal = normalize(GBufferNormals[launchIndex].xyz * 2.0 - 1.0);
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = mul(InvViewProj, clipPos);
    worldPos /= worldPos.w;

    float3 viewDir = normalize(worldPos.xyz - CameraPosition);
    float3 reflectDir = reflect(viewDir, normal);

    RayDesc ray;
    ray.Origin = worldPos.xyz + normal * 0.01;
    ray.Direction = reflectDir;
    ray.TMin = 0.001;
    ray.TMax = MaxDistance;

    RayPayload payload;
    payload.color = float4(0, 0, 0, 0);
    payload.depth = 0;

    TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
             0xFF, 0, 0, 0, ray, payload);

    OutputReflections[launchIndex] = payload.color;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.color = float4(0.5, 0.5, 0.5, 1.0);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    float3 skyColor = float3(0.4, 0.6, 0.9);
    payload.color = float4(skyColor, 0.0);
}
