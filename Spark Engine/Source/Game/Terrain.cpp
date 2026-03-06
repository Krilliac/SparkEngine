#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// Terrain.cpp
#include "Terrain.h"
#include "Utils/Assert.h"
#include <fstream>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

using namespace DirectX;

HRESULT Terrain::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    const wchar_t* file,
    UINT w, UINT h, float s)
{
    ASSERT(device != nullptr);
    ASSERT(ctx != nullptr);
    ASSERT_MSG(file != nullptr, "Heightmap file path null");
    ASSERT_MSG(w > 1 && h > 1, "Terrain dimensions must be >1");
    ASSERT_MSG(s > 0, "Cell spacing must be positive");

    // 1) Read raw 8-bit BMP after 54-byte header
    std::vector<uint8_t> data(w * h);
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return E_FAIL;
    in.seekg(54);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(w) * h);
    in.close();

    // 2) Build mesh
    BuildMesh(data, w, h, s);
    CalculateNormals();

    // Ensure we have vertices and indices
    ASSERT_ALWAYS_MSG(!m_vertices.empty(), "No terrain vertices generated");
    ASSERT_ALWAYS_MSG(!m_indices.empty(), "No terrain indices generated");

    // 3) Create VB
    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = UINT(m_vertices.size() * sizeof(TerrainVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;
    sd.pSysMem = m_vertices.data();

    HRESULT hr = device->CreateBuffer(&bd, &sd, &m_vb);
    ASSERT_MSG(SUCCEEDED(hr), "Terrain vertex buffer creation failed");
    if (FAILED(hr)) return hr;

    // 4) Create IB
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = UINT(m_indices.size() * sizeof(UINT));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = m_indices.data();

    hr = device->CreateBuffer(&bd, &sd, &m_ib);
    ASSERT_MSG(SUCCEEDED(hr), "Terrain index buffer creation failed");
    m_indexCount = UINT(m_indices.size());
    return hr;
}

void Terrain::BuildMesh(const std::vector<uint8_t>& h,
    UINT w, UINT hgt, float s)
{
    ASSERT_MSG(h.size() == w * hgt, "Height data size mismatch");

    m_vertices.resize(static_cast<std::vector<TerrainVertex, std::allocator<TerrainVertex>>::size_type>(w) * hgt);
    m_indices.clear();

    // Vertices
    for (UINT z = 0; z < hgt; ++z) {
        for (UINT x = 0; x < w; ++x) {
            float height = h[static_cast<std::vector<uint8_t, std::allocator<uint8_t>>::size_type>(z) * w + x] * 0.1f; // scale factor
            m_vertices[static_cast<std::vector<TerrainVertex, std::allocator<TerrainVertex>>::size_type>(z) * w + x] = {
                { (x - w / 2) * s, height, (z - hgt / 2) * s },
                { 0,1,0 },
                { float(x) / (w - 1), float(z) / (hgt - 1) }
            };
        }
    }

    // Indices (two tris per quad)
    for (UINT z = 0; z < hgt - 1; ++z) {
        for (UINT x = 0; x < w - 1; ++x) {
            UINT tl = z * w + x;
            UINT tr = tl + 1;
            UINT bl = (z + 1) * w + x;
            UINT br = bl + 1;
            m_indices.insert(m_indices.end(),
                { tl, bl, tr, tr, bl, br });
        }
    }

    ASSERT_ALWAYS_MSG(!m_vertices.empty(), "BuildMesh: no vertices");
    ASSERT_ALWAYS_MSG(!m_indices.empty(), "BuildMesh: no indices");
}

void Terrain::CalculateNormals()
{
    ASSERT(!m_vertices.empty() && !m_indices.empty());

    // Zero normals
    for (auto& v : m_vertices)
        v.Normal = { 0,0,0 };

    // Accumulate face normals
    for (UINT i = 0; i + 2 < m_indices.size(); i += 3) {
        auto& v0 = m_vertices[m_indices[i]];
        auto& v1 = m_vertices[m_indices[i + 1]];
        auto& v2 = m_vertices[m_indices[i + 2]];

        XMVECTOR p0 = XMLoadFloat3(&v0.Position);
        XMVECTOR p1 = XMLoadFloat3(&v1.Position);
        XMVECTOR p2 = XMLoadFloat3(&v2.Position);

        XMVECTOR fn = XMVector3Normalize(
            XMVector3Cross(p1 - p0, p2 - p0));

        XMFLOAT3 nf; XMStoreFloat3(&nf, fn);
        v0.Normal.x += nf.x; v0.Normal.y += nf.y; v0.Normal.z += nf.z;
        v1.Normal.x += nf.x; v1.Normal.y += nf.y; v1.Normal.z += nf.z;
        v2.Normal.x += nf.x; v2.Normal.y += nf.y; v2.Normal.z += nf.z;
    }

    // Normalize
    for (auto& v : m_vertices) {
        XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.Normal));
        XMFLOAT3 nf; XMStoreFloat3(&nf, n);
        v.Normal = nf;
    }
}

void Terrain::Render(ID3D11DeviceContext* ctx)
{
    ASSERT(ctx != nullptr);
    ASSERT(m_vb != nullptr);
    ASSERT(m_ib != nullptr);
    ASSERT(m_indexCount > 0);

    UINT stride = sizeof(TerrainVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->DrawIndexed(m_indexCount, 0, 0);
}
#endif // SPARK_PLATFORM_WINDOWS
