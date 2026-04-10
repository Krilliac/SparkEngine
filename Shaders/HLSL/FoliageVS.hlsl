// ============================================================================
// FoliageVS.hlsl
// ----------------------------------------------------------------------------
// Vertex shader for instanced foliage rendering with per-instance wind sway.
//
// Inputs:
//   - Standard per-vertex position / normal / uv
//   - Per-instance data read from StructuredBuffer<InstanceData> (t0) —
//     matches SparkEngine::Graphics::GPUInstanceData layout, with the
//     `padding` field repurposed as per-instance wind phase (radians).
//
// Wind model:
//   Each instance sways sinusoidally around its base position. The sway
//   amplitude scales with the vertex's local Y so the trunk stays anchored
//   while leaves move the most. Per-instance phase prevents a uniform-looking
//   "one field, one sway" artifact.
//
//   offset.xz = sin(Time*Freq + Phase + worldY*verticalFreq) * amplitude
//             * saturate(localY / referenceHeight)
//             * instance.windStrength
//
// The wind coefficients live in a dedicated constant buffer so the render
// frontend can tune them without touching the shader.
// ============================================================================

cbuffer PerFrame : register(b0)
{
    matrix View;
    matrix Projection;
    float3 CameraPos;
    float  Time;
};

cbuffer FoliageWindParams : register(b2)
{
    // x: global wind strength [0,2]
    // y: horizontal frequency (Hz)
    // z: vertical frequency (cycles per metre, adds a swirl with height)
    // w: reference height (metres, controls trunk-to-leaf falloff)
    float4 WindParams;

    // xyz: wind direction (should be normalised)
    // w:   time-of-day gust multiplier [0,2]
    float4 WindDirection;
};

struct InstanceData
{
    float4x4 worldMatrix;
    float4x4 prevWorldMatrix;
    float4x4 normalMatrix;
    uint     materialId;
    uint     flags;
    float    lodDistance;
    float    windPhase; // repurposed padding field
};

StructuredBuffer<InstanceData> InstanceBuffer : register(t0);

struct VS_INPUT
{
    float4 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    uint   InstanceId : SV_InstanceID;
};

struct PS_INPUT
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : POSITION1;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float  WindSway : TEXCOORD1; // exposed for the pixel shader (debug / AO)
    nointerpolation uint MaterialId : TEXCOORD2;
};

// ----------------------------------------------------------------------------
// ComputeWindSway
// Returns the world-space horizontal offset to apply to a vertex, scaled by
// the falloff so the trunk base stays in place.
// ----------------------------------------------------------------------------
float3 ComputeWindSway(float3 worldPos, float localY, float instancePhase)
{
    float strength = WindParams.x * WindDirection.w;
    float hFreq    = WindParams.y;
    float vFreq    = WindParams.z;
    float refH     = max(WindParams.w, 0.001);

    // Phase combines: global time, per-instance phase, and a world-space
    // swirl so adjacent blades sway in a coherent wave.
    float phase = Time * hFreq * 6.28318530 + instancePhase
                + worldPos.x * vFreq + worldPos.z * vFreq;

    // Trunk anchor: vertex at localY=0 has zero sway; leaves at refH get
    // full amplitude. saturate clamps tall foliage to refH max.
    float anchor = saturate(localY / refH);

    // Amplitude in metres. 0.15 is a comfortable default for grass/leaves.
    float amp = 0.15 * strength * anchor * anchor;

    float sway = sin(phase) * amp;

    // Apply along the dominant wind direction; a small orthogonal jitter
    // breaks up straight-line motion without needing a second sin() call.
    float3 dir = WindDirection.xyz;
    float3 ortho = float3(-dir.z, 0.0, dir.x);
    float  jitter = cos(phase * 0.5) * 0.25;

    return dir * sway + ortho * (sway * jitter);
}

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;

    InstanceData inst = InstanceBuffer[input.InstanceId];

    // Transform the vertex into world space using the instance's matrix.
    float4 worldPos = mul(input.Pos, inst.worldMatrix);

    // Apply wind deformation in world space; the local Y is read from the
    // untransformed vertex so trunk position is independent of instance
    // rotation/scale.
    float3 sway = ComputeWindSway(worldPos.xyz, input.Pos.y, inst.windPhase);
    worldPos.xyz += sway;

    output.WorldPos = worldPos.xyz;
    output.Pos = mul(mul(worldPos, View), Projection);

    // Transform the normal by the instance's normal matrix.
    output.Normal = normalize(mul(input.Normal, (float3x3)inst.normalMatrix));

    output.UV = input.UV;
    output.WindSway = length(sway);
    output.MaterialId = inst.materialId;

    return output;
}
