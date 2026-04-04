// =============================================================================
// GlobalIllumination.hlsl — Screen-space global illumination compute shader
//
// Samples normal+depth buffer, traces screen-space rays, and accumulates
// indirect light bounces for approximate GI.
// =============================================================================

cbuffer GIConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 InverseViewProjection;
    float4x4 View;
    float3 CameraPosition;
    float GIIntensity;
    float2 ScreenDimensions;
    float MaxRaySteps;
    float RayThickness;
    float RayMaxDistance;
    float IndirectBounceFactor;
    float TemporalBlend;
    float Padding;
};

Texture2D<float4> NormalBuffer : register(t0);
Texture2D<float> DepthBuffer : register(t1);
Texture2D<float4> AlbedoBuffer : register(t2);
Texture2D<float4> PreviousGI : register(t3);

SamplerState PointSampler : register(s0);

RWTexture2D<float4> GIOutput : register(u0);

// Reconstruct view-space position from depth
float3 ReconstructPosition(float2 uv, float depth)
{
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y *= -1.0;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, InverseViewProjection);
    return worldPos.xyz / worldPos.w;
}

// Hash function for random sampling directions
float Hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Generate cosine-weighted hemisphere sample
float3 CosineHemisphere(float2 rand, float3 normal)
{
    float r = sqrt(rand.x);
    float theta = 2.0 * 3.14159265 * rand.y;

    float3 tangent = abs(normal.x) > 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    tangent = normalize(cross(normal, tangent));
    float3 bitangent = cross(normal, tangent);

    return normalize(tangent * r * cos(theta) + bitangent * r * sin(theta) + normal * sqrt(1.0 - rand.x));
}

// Screen-space ray march
bool TraceScreenSpaceRay(float3 origin, float3 dir, out float2 hitUV, out float3 hitPos)
{
    hitUV = float2(0, 0);
    hitPos = float3(0, 0, 0);

    float3 endPoint = origin + dir * RayMaxDistance;

    // Project start and end to screen space
    float4 startClip = mul(float4(origin, 1.0), ViewProjection);
    float4 endClip = mul(float4(endPoint, 1.0), ViewProjection);

    float2 startScreen = (startClip.xy / startClip.w) * 0.5 + 0.5;
    float2 endScreen = (endClip.xy / endClip.w) * 0.5 + 0.5;
    startScreen.y = 1.0 - startScreen.y;
    endScreen.y = 1.0 - endScreen.y;

    float2 delta = endScreen - startScreen;
    int stepCount = min(int(MaxRaySteps), max(abs(delta.x * ScreenDimensions.x),
                                               abs(delta.y * ScreenDimensions.y)));
    stepCount = max(stepCount, 1);

    float2 stepDir = delta / float(stepCount);

    for (int i = 1; i <= stepCount; i++)
    {
        float2 sampleUV = startScreen + stepDir * float(i);

        if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
            return false;

        float sceneDepth = DepthBuffer.SampleLevel(PointSampler, sampleUV, 0).r;
        float3 scenePos = ReconstructPosition(sampleUV, sceneDepth);

        float t = float(i) / float(stepCount);
        float3 rayPos = lerp(origin, endPoint, t);

        float depthDiff = length(rayPos - CameraPosition) - length(scenePos - CameraPosition);

        if (depthDiff > 0.0 && depthDiff < RayThickness)
        {
            hitUV = sampleUV;
            hitPos = scenePos;
            return true;
        }
    }

    return false;
}

[numthreads(8, 8, 1)]
void CS_GlobalIllumination(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= uint(ScreenDimensions.x) || dispatchId.y >= uint(ScreenDimensions.y))
        return;

    float2 uv = (float2(dispatchId.xy) + 0.5) / ScreenDimensions;

    float depth = DepthBuffer.SampleLevel(PointSampler, uv, 0).r;
    if (depth >= 1.0)
    {
        GIOutput[dispatchId.xy] = float4(0, 0, 0, 0);
        return;
    }

    float3 worldPos = ReconstructPosition(uv, depth);
    float3 normal = normalize(NormalBuffer.SampleLevel(PointSampler, uv, 0).xyz * 2.0 - 1.0);

    // Multi-sample indirect lighting with random ray directions
    const int NUM_SAMPLES = 4;
    float3 indirectLight = float3(0, 0, 0);

    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        float2 rand;
        rand.x = Hash(uv * ScreenDimensions + float2(i * 7.13, i * 11.31));
        rand.y = Hash(uv * ScreenDimensions + float2(i * 3.71, i * 5.97));

        float3 sampleDir = CosineHemisphere(rand, normal);

        float2 hitUV;
        float3 hitPos;
        if (TraceScreenSpaceRay(worldPos + normal * 0.05, sampleDir, hitUV, hitPos))
        {
            float3 hitAlbedo = AlbedoBuffer.SampleLevel(PointSampler, hitUV, 0).rgb;
            float3 hitNormal = normalize(NormalBuffer.SampleLevel(PointSampler, hitUV, 0).xyz * 2.0 - 1.0);

            float NdotL = saturate(dot(hitNormal, -sampleDir));
            indirectLight += hitAlbedo * NdotL * IndirectBounceFactor;
        }
    }

    indirectLight /= float(NUM_SAMPLES);
    indirectLight *= GIIntensity;

    // Temporal accumulation
    float4 prevGI = PreviousGI.SampleLevel(PointSampler, uv, 0);
    float3 result = lerp(prevGI.rgb, indirectLight, TemporalBlend);

    GIOutput[dispatchId.xy] = float4(result, 1.0);
}
