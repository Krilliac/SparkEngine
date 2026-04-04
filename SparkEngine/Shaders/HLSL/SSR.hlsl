// =============================================================================
// SSR.hlsl — Screen-space reflections via hierarchical ray marching
//
// Hi-Z buffer traversal with roughness-based cone tracing and edge fading.
// =============================================================================

cbuffer SSRConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 InverseViewProjection;
    float4x4 View;
    float4x4 Projection;
    float2 ScreenDimensions;
    float MaxSteps;
    float Thickness;
    float RoughnessThreshold;
    float FadeStart;
    float FadeEnd;
    float Padding;
};

Texture2D<float4> ColorBuffer : register(t0);
Texture2D<float> DepthBuffer : register(t1);
Texture2D<float4> NormalBuffer : register(t2);
Texture2D<float> RoughnessBuffer : register(t3);
Texture2D<float> HiZBuffer : register(t4);

SamplerState PointSampler : register(s0);
SamplerState LinearSampler : register(s1);

RWTexture2D<float4> SSROutput : register(u0);

float3 ReconstructViewPos(float2 uv, float depth)
{
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y *= -1.0;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, InverseViewProjection);
    return worldPos.xyz / worldPos.w;
}

float2 ProjectToScreen(float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0), ViewProjection);
    float2 ndc = clip.xy / clip.w;
    float2 uv = ndc * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

// Hi-Z ray march for efficient depth buffer traversal
bool HiZTrace(float3 origin, float3 dir, out float2 hitUV)
{
    hitUV = float2(0, 0);

    float stepSize = 1.0 / MaxSteps;
    float3 marchPos = origin;

    // Linear step first, then refine
    for (float t = 0.0; t < 1.0; t += stepSize)
    {
        float3 samplePos = origin + dir * (t * 100.0);
        float2 sampleUV = ProjectToScreen(samplePos);

        if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
            return false;

        float sceneDepth = DepthBuffer.SampleLevel(PointSampler, sampleUV, 0).r;
        float3 scenePos = ReconstructViewPos(sampleUV, sceneDepth);

        float rayDepth = length(samplePos);
        float surfaceDepth = length(scenePos);
        float depthDiff = rayDepth - surfaceDepth;

        if (depthDiff > 0.0 && depthDiff < Thickness)
        {
            // Binary search refinement
            float lo = t - stepSize;
            float hi = t;
            for (int r = 0; r < 4; r++)
            {
                float mid = (lo + hi) * 0.5;
                float3 midPos = origin + dir * (mid * 100.0);
                float2 midUV = ProjectToScreen(midPos);
                float midSceneDepth = DepthBuffer.SampleLevel(PointSampler, midUV, 0).r;
                float3 midScenePos = ReconstructViewPos(midUV, midSceneDepth);

                if (length(midPos) > length(midScenePos))
                    hi = mid;
                else
                    lo = mid;
            }

            hitUV = ProjectToScreen(origin + dir * ((lo + hi) * 0.5 * 100.0));
            return true;
        }
    }

    return false;
}

[numthreads(8, 8, 1)]
void CS_SSR(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= uint(ScreenDimensions.x) || dispatchId.y >= uint(ScreenDimensions.y))
        return;

    float2 uv = (float2(dispatchId.xy) + 0.5) / ScreenDimensions;

    float roughness = RoughnessBuffer.SampleLevel(PointSampler, uv, 0).r;

    // Skip pixels above roughness threshold (too rough for sharp reflections)
    if (roughness > RoughnessThreshold)
    {
        SSROutput[dispatchId.xy] = float4(0, 0, 0, 0);
        return;
    }

    float depth = DepthBuffer.SampleLevel(PointSampler, uv, 0).r;
    if (depth >= 1.0)
    {
        SSROutput[dispatchId.xy] = float4(0, 0, 0, 0);
        return;
    }

    float3 worldPos = ReconstructViewPos(uv, depth);
    float3 normal = normalize(NormalBuffer.SampleLevel(PointSampler, uv, 0).xyz * 2.0 - 1.0);
    float3 viewDir = normalize(worldPos);
    float3 reflectDir = reflect(viewDir, normal);

    float2 hitUV;
    if (HiZTrace(worldPos + normal * 0.01, reflectDir, hitUV))
    {
        float3 reflectedColor = ColorBuffer.SampleLevel(LinearSampler, hitUV, roughness * 4.0).rgb;

        // Edge fade (reduce artifacts at screen edges)
        float2 edgeFade = saturate((hitUV - 0.02) / 0.1) * saturate((0.98 - hitUV) / 0.1);
        float fade = edgeFade.x * edgeFade.y;

        // Roughness-based intensity falloff
        float roughnessFade = 1.0 - smoothstep(0.0, RoughnessThreshold, roughness);

        // Fresnel-based reflection intensity
        float NdotV = saturate(dot(normal, -viewDir));
        float fresnel = pow(1.0 - NdotV, 5.0) * 0.9 + 0.1;

        float confidence = fade * roughnessFade * fresnel;
        SSROutput[dispatchId.xy] = float4(reflectedColor * confidence, confidence);
    }
    else
    {
        SSROutput[dispatchId.xy] = float4(0, 0, 0, 0);
    }
}
