/**
 * @file SkinningCS.hlsl
 * @brief GPU compute shader for skeletal mesh vertex skinning (GPUSkinning).
 *
 * Linear blend skinning of up to four influences per vertex. The structure layouts,
 * register slots and thread-group size below are the ABI GPUSkinning.cpp binds:
 * SkinningSourceVertex (80 bytes), SkinningOutputVertex (48 bytes) and one
 * DirectX::XMFLOAT4X4 (64 bytes) per bone, all declared in GPUSkinning.h.
 *
 * Bone matrices are engine skinning matrices (AnimationEvaluator::ComputeSkinningMatrices):
 * DirectXMath row-vector convention, so p' = p.x * row0 + p.y * row1 + p.z * row2 + row3.
 *
 * Dispatch: ceil(vertexCount / 64) groups of 64 threads.
 */

struct SourceVertex
{
    float3 position;
    float pad0;
    float3 normal;
    float pad1;
    float2 texCoord;
    uint4 boneIndices; // up to 4 bones per vertex
    float4 boneWeights; // corresponding weights (sum = 1.0)
    float2 pad2;
};

struct SkinnedVertex
{
    float3 position;
    float pad0;
    float3 normal;
    float pad1;
    float2 texCoord;
    float2 pad2;
};

// One DirectX::XMFLOAT4X4 per bone, read row by row so no matrix packing rule applies.
struct BoneMatrix
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
};

cbuffer SkinningConstants : register(b0)
{
    uint vertexCount;
    uint boneCount;
    uint2 padding;
};

StructuredBuffer<SourceVertex> sourceVertices : register(t0);
StructuredBuffer<BoneMatrix> boneMatrices : register(t1);

RWStructuredBuffer<SkinnedVertex> skinnedVertices : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint vertexId = dispatchThreadId.x;
    if (vertexId >= vertexCount)
        return;

    SourceVertex src = sourceVertices[vertexId];

    float3 skinnedPos = float3(0, 0, 0);
    float3 skinnedNormal = float3(0, 0, 0);
    float appliedWeight = 0.0;

    [unroll]
    for (uint i = 0; i < 4; i++)
    {
        float weight = src.boneWeights[i];
        uint boneIdx = src.boneIndices[i];
        // Zero or negative weights and indices past the uploaded palette contribute nothing. A real
        // branch, not a select: 0 * (non-finite matrix) would still poison the sum.
        [branch] if (weight > 0.0 && boneIdx < boneCount)
        {
            BoneMatrix bone = boneMatrices[boneIdx];
            float3 position = src.position.x * bone.row0.xyz + src.position.y * bone.row1.xyz +
                              src.position.z * bone.row2.xyz + bone.row3.xyz;
            float3 normal =
                src.normal.x * bone.row0.xyz + src.normal.y * bone.row1.xyz + src.normal.z * bone.row2.xyz;

            skinnedPos += position * weight;
            skinnedNormal += normal * weight;
            appliedWeight += weight;
        }
    }

    SkinnedVertex output;
    if (appliedWeight > 0.0)
    {
        output.position = skinnedPos;
        float normalLengthSq = dot(skinnedNormal, skinnedNormal);
        output.normal = normalLengthSq > 0.0 ? skinnedNormal * rsqrt(normalLengthSq) : src.normal;
    }
    else
    {
        // No usable influence: leave the vertex unskinned rather than collapsing it to the origin.
        output.position = src.position;
        output.normal = src.normal;
    }
    output.pad0 = 0.0;
    output.pad1 = 0.0;
    output.texCoord = src.texCoord;
    output.pad2 = float2(0.0, 0.0);

    skinnedVertices[vertexId] = output;
}
