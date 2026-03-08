#include "Core/Platform.h"
// Terrain.cpp
#include "Terrain.h"
#include "Utils/Assert.h"
#include <fstream>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#endif // SPARK_PLATFORM_WINDOWS

using namespace DirectX;

#ifdef SPARK_PLATFORM_WINDOWS

HRESULT Terrain::Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx, const wchar_t* file, UINT w, UINT h,
                            float s)
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
    if (FAILED(hr))
        return hr;

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

void Terrain::BuildMesh(const std::vector<uint8_t>& h, UINT w, UINT hgt, float s)
{
    ASSERT_MSG(h.size() == w * hgt, "Height data size mismatch");

    m_vertices.resize(static_cast<std::vector<TerrainVertex, std::allocator<TerrainVertex>>::size_type>(w) * hgt);
    m_indices.clear();

    // Vertices
    for (UINT z = 0; z < hgt; ++z)
    {
        for (UINT x = 0; x < w; ++x)
        {
            float height = h[static_cast<std::vector<uint8_t, std::allocator<uint8_t>>::size_type>(z) * w + x] *
                           0.1f; // scale factor
            m_vertices[static_cast<std::vector<TerrainVertex, std::allocator<TerrainVertex>>::size_type>(z) * w + x] = {
                {(x - w / 2) * s, height, (z - hgt / 2) * s}, {0, 1, 0}, {float(x) / (w - 1), float(z) / (hgt - 1)}};
        }
    }

    // Indices (two tris per quad)
    for (UINT z = 0; z < hgt - 1; ++z)
    {
        for (UINT x = 0; x < w - 1; ++x)
        {
            UINT tl = z * w + x;
            UINT tr = tl + 1;
            UINT bl = (z + 1) * w + x;
            UINT br = bl + 1;
            m_indices.insert(m_indices.end(), {tl, bl, tr, tr, bl, br});
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
        v.Normal = {0, 0, 0};

    // Accumulate face normals
    for (UINT i = 0; i + 2 < m_indices.size(); i += 3)
    {
        auto& v0 = m_vertices[m_indices[i]];
        auto& v1 = m_vertices[m_indices[i + 1]];
        auto& v2 = m_vertices[m_indices[i + 2]];

        XMVECTOR p0 = XMLoadFloat3(&v0.Position);
        XMVECTOR p1 = XMLoadFloat3(&v1.Position);
        XMVECTOR p2 = XMLoadFloat3(&v2.Position);

        XMVECTOR fn = XMVector3Normalize(XMVector3Cross(p1 - p0, p2 - p0));

        XMFLOAT3 nf;
        XMStoreFloat3(&nf, fn);
        v0.Normal.x += nf.x;
        v0.Normal.y += nf.y;
        v0.Normal.z += nf.z;
        v1.Normal.x += nf.x;
        v1.Normal.y += nf.y;
        v1.Normal.z += nf.z;
        v2.Normal.x += nf.x;
        v2.Normal.y += nf.y;
        v2.Normal.z += nf.z;
    }

    // Normalize
    for (auto& v : m_vertices)
    {
        XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.Normal));
        XMFLOAT3 nf;
        XMStoreFloat3(&nf, n);
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
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->DrawIndexed(m_indexCount, 0, 0);
}

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// Linux implementation — CPU-side terrain with no GPU resources
// ============================================================================

#include <cmath>
#include <sstream>
#include <algorithm>

Terrain::Terrain() {}

HRESULT Terrain::Initialize(ID3D11Device* /*device*/, ID3D11DeviceContext* /*ctx*/, const wchar_t* file,
                            const TerrainDesc& desc)
{
    m_desc = desc;

    // Read raw 8-bit BMP after 54-byte header
    std::vector<uint8_t> data(desc.width * desc.height);
    if (file)
    {
        std::string path(wcslen(file) * 4, '\0');
        // Simple wchar_t to char conversion for file path
        for (size_t i = 0; i < wcslen(file); ++i)
            path[i] = static_cast<char>(file[i] & 0x7F);
        path.resize(strlen(path.c_str()));

        std::ifstream in(path, std::ios::binary);
        if (in)
        {
            in.seekg(54);
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(desc.width) * desc.height);
        }
    }

    BuildMesh(data, desc.width, desc.height, desc.cellSpacing);
    CalculateNormals();
    CalculateSplatWeights();
    return S_OK;
}

HRESULT Terrain::Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx, const wchar_t* file, UINT w, UINT h,
                            float s)
{
    TerrainDesc desc;
    desc.width = w;
    desc.height = h;
    desc.cellSpacing = s;
    return Initialize(device, ctx, file, desc);
}

void Terrain::BuildMesh(const std::vector<uint8_t>& h, UINT w, UINT hgt, float s)
{
    m_vertices.resize(static_cast<size_t>(w) * hgt);
    m_heightData.resize(static_cast<size_t>(w) * hgt);
    m_indices.clear();

    float heightScale = m_desc.heightScale;

    for (UINT z = 0; z < hgt; ++z)
    {
        for (UINT x = 0; x < w; ++x)
        {
            size_t idx = static_cast<size_t>(z) * w + x;
            float normalizedHeight = h[idx] / 255.0f;
            m_heightData[idx] = normalizedHeight;
            float height = normalizedHeight * heightScale;

            m_vertices[idx] = {{static_cast<float>(x) - w / 2.0f, height, static_cast<float>(z) - hgt / 2.0f},
                               {0, 1, 0},
                               {static_cast<float>(x) / (w - 1), static_cast<float>(z) / (hgt - 1)},
                               {1, 0, 0, 0}};
            m_vertices[idx].Position.x *= s;
            m_vertices[idx].Position.z *= s;
        }
    }

    for (UINT z = 0; z < hgt - 1; ++z)
    {
        for (UINT x = 0; x < w - 1; ++x)
        {
            UINT tl = z * w + x;
            UINT tr = tl + 1;
            UINT bl = (z + 1) * w + x;
            UINT br = bl + 1;
            m_indices.insert(m_indices.end(), {tl, bl, tr, tr, bl, br});
        }
    }
    m_indexCount = static_cast<UINT>(m_indices.size());
}

void Terrain::CalculateNormals()
{
    if (m_vertices.empty() || m_indices.empty())
        return;

    for (auto& v : m_vertices)
        v.Normal = {0, 0, 0};

    for (UINT i = 0; i + 2 < static_cast<UINT>(m_indices.size()); i += 3)
    {
        auto& v0 = m_vertices[m_indices[i]];
        auto& v1 = m_vertices[m_indices[i + 1]];
        auto& v2 = m_vertices[m_indices[i + 2]];

        XMVECTOR p0 = XMLoadFloat3(&v0.Position);
        XMVECTOR p1 = XMLoadFloat3(&v1.Position);
        XMVECTOR p2 = XMLoadFloat3(&v2.Position);

        XMVECTOR e1 = XMVectorSubtract(p1, p0);
        XMVECTOR e2 = XMVectorSubtract(p2, p0);
        XMVECTOR fn = XMVector3Normalize(XMVector3Cross(e1, e2));

        XMFLOAT3 nf;
        XMStoreFloat3(&nf, fn);
        v0.Normal.x += nf.x;
        v0.Normal.y += nf.y;
        v0.Normal.z += nf.z;
        v1.Normal.x += nf.x;
        v1.Normal.y += nf.y;
        v1.Normal.z += nf.z;
        v2.Normal.x += nf.x;
        v2.Normal.y += nf.y;
        v2.Normal.z += nf.z;
    }

    for (auto& v : m_vertices)
    {
        XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.Normal));
        XMStoreFloat3(&v.Normal, n);
    }
}

void Terrain::CalculateSplatWeights()
{
    if (m_vertices.empty() || m_heightData.empty())
        return;

    for (size_t i = 0; i < m_vertices.size(); ++i)
    {
        float h = m_heightData[i];
        // Auto-splat by height: layer0 = low, layer1 = mid-low, layer2 = mid-high, layer3 = high
        float w0 = std::max(0.0f, 1.0f - h * 4.0f);
        float w1 = std::max(0.0f, 1.0f - std::abs(h - 0.33f) * 4.0f);
        float w2 = std::max(0.0f, 1.0f - std::abs(h - 0.66f) * 4.0f);
        float w3 = std::max(0.0f, h * 4.0f - 3.0f);
        float total = w0 + w1 + w2 + w3;
        if (total > 0.0f)
        {
            w0 /= total;
            w1 /= total;
            w2 /= total;
            w3 /= total;
        }
        else
        {
            w0 = 1.0f;
        }
        m_vertices[i].SplatWeights = {w0, w1, w2, w3};
    }
}

void Terrain::Update(const XMFLOAT3& /*cameraPosition*/)
{
    // LOD selection would go here; no-op on Linux
}

void Terrain::Render(ID3D11DeviceContext* /*ctx*/)
{
    // No GPU rendering on Linux without D3D11
}

void Terrain::Render(ID3D11DeviceContext* /*ctx*/, const XMMATRIX& /*view*/, const XMMATRIX& /*projection*/)
{
    // No GPU rendering on Linux without D3D11
}

void Terrain::ModifyHeight(const XMFLOAT3& worldPos, float radius, float strength)
{
    if (m_vertices.empty())
        return;
    int cx, cz;
    if (!WorldToHeightmap(worldPos.x, worldPos.z, cx, cz))
        return;

    int r = static_cast<int>(radius / m_desc.cellSpacing) + 1;
    for (int z = cz - r; z <= cz + r; ++z)
    {
        for (int x = cx - r; x <= cx + r; ++x)
        {
            if (x < 0 || x >= (int)m_desc.width || z < 0 || z >= (int)m_desc.height)
                continue;
            float dx = (x - cx) * m_desc.cellSpacing;
            float dz = (z - cz) * m_desc.cellSpacing;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > radius)
                continue;
            float falloff = 1.0f - (dist / radius);
            size_t idx = static_cast<size_t>(z) * m_desc.width + x;
            m_heightData[idx] += strength * falloff / m_desc.heightScale;
            m_heightData[idx] = std::max(0.0f, std::min(1.0f, m_heightData[idx]));
            m_vertices[idx].Position.y = m_heightData[idx] * m_desc.heightScale;
        }
    }
    m_meshDirty = true;
}

void Terrain::SmoothHeight(const XMFLOAT3& worldPos, float radius, float strength)
{
    if (m_vertices.empty())
        return;
    int cx, cz;
    if (!WorldToHeightmap(worldPos.x, worldPos.z, cx, cz))
        return;

    int r = static_cast<int>(radius / m_desc.cellSpacing) + 1;
    for (int z = cz - r; z <= cz + r; ++z)
    {
        for (int x = cx - r; x <= cx + r; ++x)
        {
            if (x < 0 || x >= (int)m_desc.width || z < 0 || z >= (int)m_desc.height)
                continue;
            float avg = 0.0f;
            int count = 0;
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    int nx = x + dx, nz = z + dz;
                    if (nx >= 0 && nx < (int)m_desc.width && nz >= 0 && nz < (int)m_desc.height)
                    {
                        avg += m_heightData[static_cast<size_t>(nz) * m_desc.width + nx];
                        count++;
                    }
                }
            }
            if (count > 0)
            {
                avg /= count;
                size_t idx = static_cast<size_t>(z) * m_desc.width + x;
                m_heightData[idx] += (avg - m_heightData[idx]) * strength;
                m_vertices[idx].Position.y = m_heightData[idx] * m_desc.heightScale;
            }
        }
    }
    m_meshDirty = true;
}

void Terrain::FlattenHeight(const XMFLOAT3& worldPos, float radius, float targetHeight)
{
    if (m_vertices.empty())
        return;
    int cx, cz;
    if (!WorldToHeightmap(worldPos.x, worldPos.z, cx, cz))
        return;
    float normalizedTarget = targetHeight / m_desc.heightScale;

    int r = static_cast<int>(radius / m_desc.cellSpacing) + 1;
    for (int z = cz - r; z <= cz + r; ++z)
    {
        for (int x = cx - r; x <= cx + r; ++x)
        {
            if (x < 0 || x >= (int)m_desc.width || z < 0 || z >= (int)m_desc.height)
                continue;
            float dx = (x - cx) * m_desc.cellSpacing;
            float dz = (z - cz) * m_desc.cellSpacing;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > radius)
                continue;
            float falloff = 1.0f - (dist / radius);
            size_t idx = static_cast<size_t>(z) * m_desc.width + x;
            m_heightData[idx] += (normalizedTarget - m_heightData[idx]) * falloff;
            m_vertices[idx].Position.y = m_heightData[idx] * m_desc.heightScale;
        }
    }
    m_meshDirty = true;
}

void Terrain::PaintSplatMap(const XMFLOAT3& worldPos, float radius, int layerIndex, float strength)
{
    if (m_vertices.empty() || layerIndex < 0 || layerIndex > 3)
        return;
    int cx, cz;
    if (!WorldToHeightmap(worldPos.x, worldPos.z, cx, cz))
        return;

    int r = static_cast<int>(radius / m_desc.cellSpacing) + 1;
    for (int z = cz - r; z <= cz + r; ++z)
    {
        for (int x = cx - r; x <= cx + r; ++x)
        {
            if (x < 0 || x >= (int)m_desc.width || z < 0 || z >= (int)m_desc.height)
                continue;
            float dx = (x - cx) * m_desc.cellSpacing;
            float dz = (z - cz) * m_desc.cellSpacing;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > radius)
                continue;
            float falloff = (1.0f - dist / radius) * strength;
            size_t idx = static_cast<size_t>(z) * m_desc.width + x;
            float* w = &m_vertices[idx].SplatWeights.x;
            w[layerIndex] += falloff;
            // Renormalize
            float total = w[0] + w[1] + w[2] + w[3];
            if (total > 0.0f)
            {
                w[0] /= total;
                w[1] /= total;
                w[2] /= total;
                w[3] /= total;
            }
        }
    }
    m_meshDirty = true;
}

float Terrain::GetHeightAt(float worldX, float worldZ) const
{
    int hx, hz;
    if (!WorldToHeightmap(worldX, worldZ, hx, hz))
        return 0.0f;
    return SampleHeight(hx, hz) * m_desc.heightScale;
}

XMFLOAT3 Terrain::GetNormalAt(float worldX, float worldZ) const
{
    int hx, hz;
    if (!WorldToHeightmap(worldX, worldZ, hx, hz))
        return {0, 1, 0};
    if (hx < 0 || hx >= (int)m_desc.width || hz < 0 || hz >= (int)m_desc.height)
        return {0, 1, 0};
    return m_vertices[static_cast<size_t>(hz) * m_desc.width + hx].Normal;
}

void Terrain::GetBounds(XMFLOAT3& minBounds, XMFLOAT3& maxBounds) const
{
    float halfW = (m_desc.width / 2.0f) * m_desc.cellSpacing;
    float halfH = (m_desc.height / 2.0f) * m_desc.cellSpacing;
    minBounds = {-halfW, 0.0f, -halfH};
    maxBounds = {halfW, m_desc.heightScale, halfH};
}

std::string Terrain::Console_GetInfo() const
{
    std::ostringstream ss;
    ss << "=== Terrain Info (Linux) ===\n";
    ss << "  Size: " << m_desc.width << " x " << m_desc.height << "\n";
    ss << "  Cell Spacing: " << m_desc.cellSpacing << "\n";
    ss << "  Height Scale: " << m_desc.heightScale << "\n";
    ss << "  Vertices: " << m_vertices.size() << "\n";
    ss << "  Triangles: " << (m_indexCount / 3) << "\n";
    ss << "  Current LOD: " << m_currentLOD << "\n";
    return ss.str();
}

bool Terrain::WorldToHeightmap(float worldX, float worldZ, int& hx, int& hz) const
{
    if (m_vertices.empty())
        return false;
    float halfW = (m_desc.width / 2.0f) * m_desc.cellSpacing;
    float halfH = (m_desc.height / 2.0f) * m_desc.cellSpacing;
    hx = static_cast<int>((worldX + halfW) / m_desc.cellSpacing);
    hz = static_cast<int>((worldZ + halfH) / m_desc.cellSpacing);
    return (hx >= 0 && hx < (int)m_desc.width && hz >= 0 && hz < (int)m_desc.height);
}

float Terrain::SampleHeight(int x, int z) const
{
    if (x < 0 || x >= (int)m_desc.width || z < 0 || z >= (int)m_desc.height)
        return 0.0f;
    return m_heightData[static_cast<size_t>(z) * m_desc.width + x];
}

void Terrain::UpdateVertexBuffer()
{
    // No GPU vertex buffer on Linux
    m_meshDirty = false;
}

void Terrain::BuildQuadTree()
{
    // Quadtree LOD not implemented on Linux
}

#endif // SPARK_PLATFORM_WINDOWS
