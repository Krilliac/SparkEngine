// GTAOPS.hlsl
//
// Ground Truth Ambient Occlusion — pixel shader variant of the
// compute shader reference implementation in GTAOEffect.h.
// Consumed by PostProcessingPipeline as the "GTAO" pass.
//
// The runtime path uses the identical source embedded as a string
// literal inside PostProcessingPipeline::CompileEffectShaders so every
// effect follows the same inline-literal pattern. This standalone .hlsl
// file exists so the build-time shader compile step (Phase D DXC on
// DXR shaders, Phase G FXC on foliage shaders, and Phase H's unified
// compile coverage when it lands) can validate the shader source
// offline — a typo would otherwise only surface on the first Windows
// scene load.
//
// Inputs:
//   t0  sceneTexture  — HDR ping-pong source
//   t1  depthTexture  — linearized scene depth
//   s0  linearSampler
//   s1  pointSampler
//   b0  PostProcessParams:
//     params0 = (radius, power, projScale, directions)
//     params1 = (width, height, stepsPerDir, totalTime)
//     params2 = (falloffStart, falloffEnd, depthThreshold, normalBias)
//     params3 = unused
//
// Output: float4(scene.rgb * ao, 1) — multiplicative composite so the
// pass is self-contained and does not need a separate composite stage.

Texture2D sceneTexture : register(t0);
Texture2D depthTexture : register(t1);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);

cbuffer PostProcessParams : register(b0)
{
    float4 params0;
    float4 params1;
    float4 params2;
    float4 params3;
};

static const float GTAO_PI = 3.14159265;

float3 ReconstructNormalFromDepth(float2 uv, float2 texelSize, float centerDepth)
{
    float rd = depthTexture.Sample(pointSampler, uv + float2(texelSize.x, 0)).r;
    float dd = depthTexture.Sample(pointSampler, uv + float2(0, texelSize.y)).r;
    float3 dx = float3(texelSize.x, 0.0, rd - centerDepth);
    float3 dy = float3(0.0, texelSize.y, dd - centerDepth);
    return normalize(cross(dy, dx));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 scene = sceneTexture.Sample(linearSampler, uv).rgb;
    float depth = depthTexture.Sample(linearSampler, uv).r;
    if (depth <= 0.0001)
        return float4(scene, 1);

    float2 texelSize = 1.0 / float2(params1.x, params1.y);
    int directions = (int)params0.w;
    int steps = (int)params1.z;
    float radius = params0.x;
    float power = params0.y;
    float projScale = max(params0.z, 0.001);
    float falloffStart = params2.x * radius;
    float falloffEnd = params2.y * radius;

    float3 normal = ReconstructNormalFromDepth(uv, texelSize, depth);
    float screenRadius = clamp(radius * projScale / max(depth, 0.001), 1.0, 256.0);
    float totalAO = 0.0;

    [loop]
    for (int d = 0; d < directions; d++)
    {
        float angle = GTAO_PI * (float)d / max((float)directions, 1.0);
        float2 dir = float2(cos(angle), sin(angle));
        float maxH = -1.0;
        float maxHNeg = -1.0;

        [loop]
        for (int s = 1; s <= steps; s++)
        {
            float stepFrac = (float)s / max((float)steps, 1.0);
            float2 pixOffset = dir * stepFrac * screenRadius;
            float2 uvStep = pixOffset * texelSize;

            float2 spUV = uv + uvStep;
            if (all(spUV >= 0.0) && all(spUV <= 1.0))
            {
                float sd = depthTexture.Sample(linearSampler, spUV).r;
                if (sd > 0.0001)
                {
                    float3 delta = float3(pixOffset / projScale * depth, sd - depth);
                    float len = length(delta);
                    if (len > 0.0001)
                    {
                        float hc = dot(delta, normal) / len;
                        float falloff = 1.0 - saturate((len - falloffStart) /
                                                       max(falloffEnd - falloffStart, 0.001));
                        maxH = max(maxH, hc * falloff);
                    }
                }
            }

            float2 spUVneg = uv - uvStep;
            if (all(spUVneg >= 0.0) && all(spUVneg <= 1.0))
            {
                float sdn = depthTexture.Sample(linearSampler, spUVneg).r;
                if (sdn > 0.0001)
                {
                    float3 deltaN = float3(-pixOffset / projScale * depth, sdn - depth);
                    float lenN = length(deltaN);
                    if (lenN > 0.0001)
                    {
                        float hcN = dot(deltaN, normal) / lenN;
                        float falloffN = 1.0 - saturate((lenN - falloffStart) /
                                                        max(falloffEnd - falloffStart, 0.001));
                        maxHNeg = max(maxHNeg, hcN * falloffN);
                    }
                }
            }
        }

        float h1 = acos(clamp(maxH, -1.0, 1.0));
        float h2 = acos(clamp(maxHNeg, -1.0, 1.0));
        float vis = 0.25 * (-cos(2.0 * h1) + 2.0 * h1 + -cos(2.0 * h2) + 2.0 * h2) / GTAO_PI;
        totalAO += saturate(vis);
    }

    float ao = pow(saturate(totalAO / max((float)directions, 1.0)), max(power, 0.01));
    return float4(scene * ao, 1);
}
