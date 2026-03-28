/**
 * @file SkinningCS.hlsl
 * @brief GPU compute shader for skeletal mesh vertex skinning
 *
 * Transforms source vertices by bone matrices, writing pre-skinned
 * positions/normals/tangents to an output buffer. The vertex shader
 * then reads already-skinned data with no per-vertex bone work.
 *
 * Dispatch: ceil(vertexCount / 256) groups of 256 threads.
 */

// Source vertex layout (unskinned mesh data)
struct SourceVertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float3 tangent;
    float  pad0;
    uint4  boneIndices; // up to 4 bones per vertex
    float4 boneWeights; // corresponding weights (sum = 1.0)
};

// Output vertex layout (skinned, ready for VS passthrough)
struct SkinnedVertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float3 tangent;
    float  pad0;
};

cbuffer SkinningConstants : register(b0)
{
    uint vertexCount;
    uint boneCount;
    uint vertexOffset;  // offset into source buffer for this mesh
    uint outputOffset;  // offset into output buffer for this mesh
};

// Bone matrices for current frame (4x3 row-major for compact storage)
// Each bone is stored as 3 float4s: row0, row1, row2 (implicit 0,0,0,1 fourth row)
StructuredBuffer<float4>     boneMatrices : register(t0); // boneCount * 3 entries
StructuredBuffer<SourceVertex> sourceVertices : register(t1);

RWStructuredBuffer<SkinnedVertex> skinnedVertices : register(u0);

// Reconstruct 4x4 matrix from 3 packed float4 rows
float4x4 LoadBoneMatrix(uint boneIndex)
{
    uint base = boneIndex * 3;
    float4 row0 = boneMatrices[base + 0];
    float4 row1 = boneMatrices[base + 1];
    float4 row2 = boneMatrices[base + 2];

    return float4x4(
        row0.x, row0.y, row0.z, row0.w,
        row1.x, row1.y, row1.z, row1.w,
        row2.x, row2.y, row2.z, row2.w,
        0.0, 0.0, 0.0, 1.0);
}

// Transform position by bone matrix
float3 TransformPosition(float4x4 m, float3 pos)
{
    float4 result = mul(float4(pos, 1.0), m);
    return result.xyz;
}

// Transform direction by bone matrix (no translation)
float3 TransformDirection(float4x4 m, float3 dir)
{
    float4 result = mul(float4(dir, 0.0), m);
    return result.xyz;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint vertexId = dispatchThreadId.x;
    if (vertexId >= vertexCount)
        return;

    SourceVertex src = sourceVertices[vertexOffset + vertexId];

    float3 skinnedPos = float3(0, 0, 0);
    float3 skinnedNormal = float3(0, 0, 0);
    float3 skinnedTangent = float3(0, 0, 0);

    // Blend up to 4 bone influences
    [unroll]
    for (uint i = 0; i < 4; i++)
    {
        float weight = src.boneWeights[i];
        if (weight <= 0.0)
            continue;

        uint boneIdx = src.boneIndices[i];
        if (boneIdx >= boneCount)
            continue;

        float4x4 boneMat = LoadBoneMatrix(boneIdx);

        skinnedPos += TransformPosition(boneMat, src.position) * weight;
        skinnedNormal += TransformDirection(boneMat, src.normal) * weight;
        skinnedTangent += TransformDirection(boneMat, src.tangent) * weight;
    }

    // Write skinned vertex
    SkinnedVertex output;
    output.position = skinnedPos;
    output.normal = normalize(skinnedNormal);
    output.texCoord = src.texCoord;
    output.tangent = normalize(skinnedTangent);
    output.pad0 = 0.0;

    skinnedVertices[outputOffset + vertexId] = output;
}
