/**
 * @file MeshletMS.hlsl
 * @brief Mesh shader for meshlet-based rendering (D3D12 / Vulkan)
 *
 * Processes one meshlet per thread group. Each meshlet contains up to
 * 64 vertices and 124 triangles, matching GPU hardware limits.
 * Replaces the traditional vertex + index buffer pipeline with
 * direct meshlet dispatch for better GPU occupancy and culling.
 *
 * Requires: SM 6.5 (D3D12) or VK_EXT_mesh_shader (Vulkan)
 */

struct MeshletVertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
};

struct Meshlet
{
    uint vertexOffset;    // offset into mesh vertex buffer
    uint triangleOffset;  // offset into mesh triangle buffer
    uint vertexCount;     // number of unique vertices
    uint triangleCount;   // number of triangles

    float3 boundCenter;   // bounding sphere center
    float  boundRadius;   // bounding sphere radius

    // Normal cone for backface culling
    float3 coneAxis;
    float  coneCutoff;    // cos(half_angle + 90), negative = valid cone
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 texCoord : TEXCOORD0;
    uint   meshletId : COLOR0; // debug: which meshlet
};

cbuffer MeshletConstants : register(b0)
{
    float4x4 worldViewProjection;
    float4x4 worldMatrix;
    float3   cameraPosition;
    uint     meshletCount;
};

// Mesh data
StructuredBuffer<MeshletVertex> vertices : register(t0);
StructuredBuffer<Meshlet>       meshlets : register(t1);
ByteAddressBuffer               uniqueVertexIndices : register(t2);
ByteAddressBuffer               primitiveIndices : register(t3);

// Frustum planes for per-meshlet culling
cbuffer CullData : register(b1)
{
    float4 frustumPlanes[6];
};

// Backface cone cull test
bool IsMeshletBackface(Meshlet m)
{
    if (m.coneCutoff >= 0.0)
        return false; // degenerate cone, skip test

    float3 coneApex = m.boundCenter;
    float3 viewDir = normalize(cameraPosition - coneApex);
    return dot(viewDir, m.coneAxis) < m.coneCutoff;
}

// Frustum sphere test
bool IsMeshletOutsideFrustum(Meshlet m)
{
    [unroll]
    for (uint i = 0; i < 6; i++)
    {
        float dist = dot(frustumPlanes[i].xyz, m.boundCenter) + frustumPlanes[i].w;
        if (dist < -m.boundRadius)
            return true;
    }
    return false;
}

#define MAX_VERTICES  64
#define MAX_PRIMITIVES 124

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(
    uint groupId : SV_GroupID,
    uint threadId : SV_GroupThreadID,
    out vertices VertexOutput verts[MAX_VERTICES],
    out indices uint3 tris[MAX_PRIMITIVES])
{
    // Each thread group processes one meshlet
    if (groupId >= meshletCount)
    {
        SetMeshOutputCounts(0, 0);
        return;
    }

    Meshlet m = meshlets[groupId];

    // Per-meshlet culling: backface cone + frustum
    if (IsMeshletBackface(m) || IsMeshletOutsideFrustum(m))
    {
        SetMeshOutputCounts(0, 0);
        return;
    }

    SetMeshOutputCounts(m.vertexCount, m.triangleCount);

    // Output vertices (threads cooperatively process vertices)
    if (threadId < m.vertexCount)
    {
        uint vertexIndex = uniqueVertexIndices.Load((m.vertexOffset + threadId) * 4);
        MeshletVertex v = vertices[vertexIndex];

        VertexOutput output;
        output.position = mul(float4(v.position, 1.0), worldViewProjection);
        output.normal = mul(float4(v.normal, 0.0), worldMatrix).xyz;
        output.texCoord = v.texCoord;
        output.meshletId = groupId;

        verts[threadId] = output;
    }

    // Output triangles (threads cooperatively process triangles)
    if (threadId < m.triangleCount)
    {
        // Each triangle is stored as 3 packed bytes (local indices within meshlet)
        uint packedIndex = primitiveIndices.Load((m.triangleOffset + threadId) * 4);
        uint3 tri;
        tri.x = (packedIndex >> 0) & 0xFF;
        tri.y = (packedIndex >> 8) & 0xFF;
        tri.z = (packedIndex >> 16) & 0xFF;

        tris[threadId] = tri;
    }
}
