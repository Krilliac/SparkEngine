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

// Per-species impostor atlas records, packed as 2 float4s per species:
//   record[speciesIndex*2 + 0] = uvRect: (minU, minV, cellDU, cellDV)
//   record[speciesIndex*2 + 1] = meta:   (billboardHeight, 0, 0, 0)
// Indexed by InstanceData.materialId. Built by FoliageImpostorAtlas::
// UploadCellBuffer. Bound by FoliageRenderer::RenderFoliagePass; null-
// bound (skip) when no impostors are in the batch.
StructuredBuffer<float4> ImpostorCells : register(t3);

// Phase E: fixed angle-cell count. Matches the default in
// FoliageImpostorAtlas::BakeAllRegisteredSpecies(angleSteps=8). Move to a
// cbuffer once per-species overrides are needed.
#define FOLIAGE_IMPOSTOR_ANGLE_STEPS 8u

// Flag bit for InstanceData.flags — must match
// Spark::Graphics::InstanceFlags::FoliageImpostor (GPUSceneBuffer.h).
#define FOLIAGE_IMPOSTOR_FLAG 0x40u

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
    nointerpolation float AngleU    : TEXCOORD3; // per-instance impostor U offset
    nointerpolation uint  Flags     : TEXCOORD4; // InstanceData.flags copy
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

    bool isImpostor = (inst.flags & FOLIAGE_IMPOSTOR_FLAG) != 0u;

    float4 worldPos;
    float3 worldNormal;
    float  swayLen = 0.0;
    float  angleU  = 0.0;

    if (isImpostor)
    {
        // ----- Impostor billboard path -----
        // The impostor sub-pass binds a unit-quad VB with positions in
        // y=[0,1], xz=[-0.5,0.5]. We rebuild the quad as a camera-aligned
        // billboard anchored at the instance's world translation.
        float3 worldTranslation = mul(float4(0.0, 0.0, 0.0, 1.0), inst.worldMatrix).xyz;

        // Y-axis-aligned billboard: trees / tall foliage look best pinned
        // to world-up while facing the camera around the horizontal axis.
        float3 toCam = CameraPos - worldTranslation;
        float3 worldUp = float3(0.0, 1.0, 0.0);
        float3 right = normalize(cross(worldUp, float3(toCam.x, 0.0, toCam.z) + float3(1e-6, 0.0, 0.0)));

        // Phase F: per-species billboard height comes from the cell meta
        // float4 (element [materialId*2 + 1].x). Horizontal half-width
        // is half the height — good default for tree-like foliage.
        float4 meta = ImpostorCells[inst.materialId * 2u + 1u];
        float billboardHeight = max(meta.x, 0.01);
        float billboardHalfWidth = billboardHeight * 0.5;

        float3 offset = right * (input.Pos.x * billboardHalfWidth * 2.0)
                      + worldUp * (input.Pos.y * billboardHeight);
        worldPos = float4(worldTranslation + offset, 1.0);

        // Normal faces back at the camera — keeps wrapped-diffuse lighting
        // consistent across yaws without making the impostor go dark.
        worldNormal = normalize(toCam - worldUp * toCam.y);

        // Angle bin: map view-relative yaw to one of ANGLE_STEPS cells and
        // convert to a U offset into the atlas row for this species.
        float yaw = atan2(toCam.x, toCam.z); // [-pi, pi]
        float normalised = (yaw + 3.14159265) * (0.5 / 3.14159265); // [0, 1]
        uint angleBin = (uint)floor(normalised * (float)FOLIAGE_IMPOSTOR_ANGLE_STEPS);
        if (angleBin >= FOLIAGE_IMPOSTOR_ANGLE_STEPS)
            angleBin = FOLIAGE_IMPOSTOR_ANGLE_STEPS - 1u;

        // uvRect: (minU, minV, cellDU, cellDV). Impostor atlas stores
        // angles horizontally so the U offset is the only per-angle
        // adjustment. The PS reads uvRect separately.
        float4 uvRect = ImpostorCells[inst.materialId * 2u + 0u];
        angleU = (float)angleBin * uvRect.z;
    }
    else
    {
        // ----- Mesh path (existing) -----
        worldPos = mul(input.Pos, inst.worldMatrix);

        float3 sway = ComputeWindSway(worldPos.xyz, input.Pos.y, inst.windPhase);
        worldPos.xyz += sway;
        swayLen = length(sway);

        worldNormal = normalize(mul(input.Normal, (float3x3)inst.normalMatrix));
    }

    output.WorldPos = worldPos.xyz;
    output.Pos = mul(mul(worldPos, View), Projection);
    output.Normal = worldNormal;
    output.UV = input.UV;
    output.WindSway = swayLen;
    output.MaterialId = inst.materialId;
    output.AngleU = angleU;
    output.Flags = inst.flags;

    return output;
}
