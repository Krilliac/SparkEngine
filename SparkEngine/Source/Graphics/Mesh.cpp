#include "Mesh.h"
#include "../Core/Platform.h"
#include "../Utils/MathUtils.h"
/**
 * @file Mesh.cpp
 * @brief CPU-side mesh geometry and D3D11 GPU buffer management
 *
 * Supports loading OBJ files via tinyobjloader, procedural primitive generation
 * (cube, sphere, plane, triangle, pyramid), automatic normal calculation via
 * cross-product accumulation, and D3D11 vertex/index buffer creation.
 * Dual implementation: Windows uses DirectXMath + D3D11; Linux stores CPU-side
 * data for the RHI abstraction layer.
 */
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include <tiny_obj_loader.h>
#if SPARK_HAS_CGLTF
#include <cgltf.h>
#endif
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <fstream>
#include <filesystem>
#include <cmath>

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

Mesh::Mesh() {}
Mesh::~Mesh()
{
    Shutdown();
}

HRESULT Mesh::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);
    m_device = device;
    m_context = context;
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Mesh initialized with device/context");
    return S_OK;
}

void Mesh::Shutdown()
{
    m_ib.Reset();
    m_vb.Reset();
    m_vertices.clear();
    m_indices.clear();
    m_vertexCount = m_indexCount = 0;
    m_device = nullptr;
    m_context = nullptr;
}

/// Loads an OBJ mesh from disk using tinyobjloader. Converts wide path to UTF-8,
/// extracts positions/normals/UVs per-index (expanding shared vertices), recalculates
/// normals if any are zero, then uploads to GPU via CreateBuffers().
bool Mesh::LoadFromFile(const std::wstring& path)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !path.empty(), "Mesh::LoadFromFile — empty path");

    // Convert wide path to UTF-8 for tinyobjloader
    auto u8path_u8 = std::filesystem::path(path).u8string(); // basic_string<char8_t>
    std::string u8Path(u8path_u8.begin(), u8path_u8.end());  // convert to std::string

    // Reader config
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig cfg;
    {
        auto parent_u8 = std::filesystem::path(path).parent_path().u8string();
        std::string mtlSearch(parent_u8.begin(), parent_u8.end());
        cfg.mtl_search_path = mtlSearch; // now a std::string
    }

    if (!reader.ParseFromFile(u8Path, cfg))
    {
        if (!reader.Error().empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "tinyobj error: %s", reader.Error().c_str());
        }
        return false;
    }

    if (!reader.Warning().empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "tinyobj warning: %s", reader.Warning().c_str());
    }

    // Parse geometry
    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    std::vector<Vertex> verts;
    std::vector<unsigned int> inds;
    verts.reserve(attrib.vertices.size() / 3);

    for (const auto& shape : shapes)
    {
        for (const auto& idx : shape.mesh.indices)
        {
            Vertex v{};
            // Position
            v.Position = {attrib.vertices[3 * idx.vertex_index + 0], attrib.vertices[3 * idx.vertex_index + 1],
                          attrib.vertices[3 * idx.vertex_index + 2]};
            // Normal
            if (idx.normal_index >= 0)
            {
                v.Normal = {attrib.normals[3 * idx.normal_index + 0], attrib.normals[3 * idx.normal_index + 1],
                            attrib.normals[3 * idx.normal_index + 2]};
            }
            else
            {
                v.Normal = {0, 1, 0};
            }
            // TexCoord
            if (idx.texcoord_index >= 0)
            {
                v.TexCoord = {attrib.texcoords[2 * idx.texcoord_index + 0],
                              1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]};
            }
            else
            {
                v.TexCoord = {0, 0};
            }

            verts.push_back(v);
            inds.push_back(static_cast<unsigned int>(inds.size()));
        }
    }

    // Ensure we got geometry
    ASSERT_ALWAYS_MSG(!verts.empty() && !inds.empty(), "tinyobj produced empty mesh data");

    m_vertices = std::move(verts);
    m_indices = std::move(inds);
    m_vertexCount = static_cast<UINT>(m_vertices.size());
    m_indexCount = static_cast<UINT>(m_indices.size());

    // Recompute normals if any were zero
    bool anyZero = std::any_of(m_vertices.begin(), m_vertices.end(),
                               [](const Vertex& v) { return v.Normal.x == 0 && v.Normal.y == 0 && v.Normal.z == 0; });
    if (anyZero)
        CalculateNormals();

    // Create GPU buffers
    HRESULT hr = CreateBuffers();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "CreateBuffers failed in LoadFromFile");
        return false;
    }

    return true;
}

HRESULT Mesh::CreateFromVertices(const std::vector<Vertex>& verts, const std::vector<unsigned int>& inds)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !verts.empty() && !inds.empty(),
                      "CreateFromVertices — empty vertex or index data");

    m_vertices = verts;
    m_indices = inds;
    m_vertexCount = static_cast<UINT>(verts.size());
    m_indexCount = static_cast<UINT>(inds.size());

    CalculateNormals();
    return CreateBuffers();
}

HRESULT Mesh::CreateCube(float size)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, size > 0.0f, "Cube size must be positive");

    // Clear existing data
    m_vertices.clear();
    m_indices.clear();

    float h = size * 0.5f;

    // 24 vertices (4 per face, 6 faces)
    m_vertices = {
        // Front face (z = +h)
        {{-h, -h, h}, {0, 0, 1}, {0, 1}}, // 0
        {{h, -h, h}, {0, 0, 1}, {1, 1}},  // 1
        {{h, h, h}, {0, 0, 1}, {1, 0}},   // 2
        {{-h, h, h}, {0, 0, 1}, {0, 0}},  // 3

        // Back face (z = -h)
        {{h, -h, -h}, {0, 0, -1}, {0, 1}},  // 4
        {{-h, -h, -h}, {0, 0, -1}, {1, 1}}, // 5
        {{-h, h, -h}, {0, 0, -1}, {1, 0}},  // 6
        {{h, h, -h}, {0, 0, -1}, {0, 0}},   // 7

        // Left face (x = -h)
        {{-h, -h, -h}, {-1, 0, 0}, {0, 1}}, // 8
        {{-h, -h, h}, {-1, 0, 0}, {1, 1}},  // 9
        {{-h, h, h}, {-1, 0, 0}, {1, 0}},   // 10
        {{-h, h, -h}, {-1, 0, 0}, {0, 0}},  // 11

        // Right face (x = +h)
        {{h, -h, h}, {1, 0, 0}, {0, 1}},  // 12
        {{h, -h, -h}, {1, 0, 0}, {1, 1}}, // 13
        {{h, h, -h}, {1, 0, 0}, {1, 0}},  // 14
        {{h, h, h}, {1, 0, 0}, {0, 0}},   // 15

        // Bottom face (y = -h)
        {{-h, -h, -h}, {0, -1, 0}, {0, 1}}, // 16
        {{h, -h, -h}, {0, -1, 0}, {1, 1}},  // 17
        {{h, -h, h}, {0, -1, 0}, {1, 0}},   // 18
        {{-h, -h, h}, {0, -1, 0}, {0, 0}},  // 19

        // Top face (y = +h)
        {{-h, h, h}, {0, 1, 0}, {0, 1}}, // 20
        {{h, h, h}, {0, 1, 0}, {1, 1}},  // 21
        {{h, h, -h}, {0, 1, 0}, {1, 0}}, // 22
        {{-h, h, -h}, {0, 1, 0}, {0, 0}} // 23
    };

    // 36 indices (2 triangles per face, 6 faces)
    m_indices = {// Front face
                 0, 1, 2, 0, 2, 3,
                 // Back face
                 4, 5, 6, 4, 6, 7,
                 // Left face
                 8, 9, 10, 8, 10, 11,
                 // Right face
                 12, 13, 14, 12, 14, 15,
                 // Bottom face
                 16, 17, 18, 16, 18, 19,
                 // Top face
                 20, 21, 22, 20, 22, 23};

    m_vertexCount = static_cast<UINT>(m_vertices.size());
    m_indexCount = static_cast<UINT>(m_indices.size());

    ASSERT_ALWAYS_MSG(m_vertexCount > 0 && m_indexCount > 0, "CreateCube produced empty mesh");

    return CreateBuffers();
}

HRESULT Mesh::CreateTriangle(float size)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, size > 0.0f, "Triangle size must be positive");

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    float h = size * 0.5f;

    // Create a simple triangle
    vertices.emplace_back(XMFLOAT3(0.0f, h, 0.0f),    // Top vertex
                          XMFLOAT3(0.0f, 0.0f, 1.0f), // Normal pointing forward
                          XMFLOAT2(0.5f, 0.0f)        // Texture coordinate
    );

    vertices.emplace_back(XMFLOAT3(-h, -h, 0.0f),     // Bottom left
                          XMFLOAT3(0.0f, 0.0f, 1.0f), // Normal pointing forward
                          XMFLOAT2(0.0f, 1.0f)        // Texture coordinate
    );

    vertices.emplace_back(XMFLOAT3(h, -h, 0.0f),      // Bottom right
                          XMFLOAT3(0.0f, 0.0f, 1.0f), // Normal pointing forward
                          XMFLOAT2(1.0f, 1.0f)        // Texture coordinate
    );

    // Triangle indices
    indices = {0, 1, 2};

    ASSERT_ALWAYS_MSG(!vertices.empty() && !indices.empty(), "CreateTriangle produced empty mesh");

    return CreateFromVertices(vertices, indices);
}

HRESULT Mesh::CreatePlane(float width, float depth)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, width > 0.0f && depth > 0.0f, "Plane dimensions must be positive");

    MeshData md;
    float hw = width * 0.5f, hd = depth * 0.5f;
    XMFLOAT3 pts[4] = {{-hw, 0, -hd}, {+hw, 0, -hd}, {+hw, 0, +hd}, {-hw, 0, +hd}};
    XMFLOAT3 n{0, 1, 0};
    unsigned int idxs[6] = {0, 1, 2, 0, 2, 3};

    for (int i = 0; i < 6; ++i)
    {
        md.vertices.emplace_back(pts[idxs[i]], n, XMFLOAT2(0, 0));
        md.indices.push_back(static_cast<unsigned int>(md.indices.size()));
    }

    ASSERT_ALWAYS_MSG(!md.vertices.empty() && !md.indices.empty(), "CreatePlane produced empty mesh");

    return CreateFromVertices(md.vertices, md.indices);
}

HRESULT Mesh::CreateSphere(float radius, int slices, int stacks)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, radius > 0.0f, "Sphere radius must be positive");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, slices >= 3 && stacks >= 2,
                      "Sphere needs at least 3 slices and 2 stacks");

    MeshData md;
    for (int i = 0; i <= stacks; ++i)
    {
        float v = i / static_cast<float>(stacks);
        float phi = v * XM_PI;
        for (int j = 0; j <= slices; ++j)
        {
            float u = j / static_cast<float>(slices);
            float theta = u * XM_2PI;

            XMFLOAT3 pos{radius * sinf(phi) * cosf(theta), radius * cosf(phi), radius * sinf(phi) * sinf(theta)};
            XMVECTOR nVec = XMVector3Normalize(XMLoadFloat3(&pos));
            XMFLOAT3 n;
            XMStoreFloat3(&n, nVec);

            md.vertices.emplace_back(pos, n, XMFLOAT2(u, v));
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            int a = i * (slices + 1) + j;
            int b = a + slices + 1;
            md.indices.insert(md.indices.end(), {static_cast<unsigned int>(a), static_cast<unsigned int>(b),
                                                 static_cast<unsigned int>(a + 1), static_cast<unsigned int>(b),
                                                 static_cast<unsigned int>(b + 1), static_cast<unsigned int>(a + 1)});
        }
    }

    ASSERT_ALWAYS_MSG(!md.vertices.empty() && !md.indices.empty(), "CreateSphere produced empty mesh");

    return CreateFromVertices(md.vertices, md.indices);
}

HRESULT Mesh::CreatePyramid(float size, float height)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, size > 0.0f && height > 0.0f,
                      "Pyramid size and height must be positive");

    // Clear existing data
    m_vertices.clear();
    m_indices.clear();

    float h = size * 0.5f;

    // 5 vertices: 4 for the base + 1 for the apex
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    // Base vertices (square base in XZ plane at y=0)
    vertices.emplace_back(XMFLOAT3(-h, 0.0f, -h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f)); // 0: bottom-left
    vertices.emplace_back(XMFLOAT3(h, 0.0f, -h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f));  // 1: bottom-right
    vertices.emplace_back(XMFLOAT3(h, 0.0f, h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f));   // 2: top-right
    vertices.emplace_back(XMFLOAT3(-h, 0.0f, h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f));  // 3: top-left

    // Apex vertex (at the top)
    vertices.emplace_back(XMFLOAT3(0.0f, height, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.5f, 0.5f)); // 4: apex

    // Base (bottom face) - 2 triangles
    indices.insert(indices.end(), {0, 2, 1}); // Triangle 1
    indices.insert(indices.end(), {0, 3, 2}); // Triangle 2

    // Side faces - 4 triangles
    // Front face (negative Z)
    indices.insert(indices.end(), {0, 1, 4});
    // Right face (positive X)
    indices.insert(indices.end(), {1, 2, 4});
    // Back face (positive Z)
    indices.insert(indices.end(), {2, 3, 4});
    // Left face (negative X)
    indices.insert(indices.end(), {3, 0, 4});

    // Compute proper normals for the side faces
    // For each triangle, calculate face normal and assign to all vertices of that triangle
    for (size_t i = 6; i < indices.size(); i += 3) // Skip base triangles (indices 0-5)
    {
        unsigned int i0 = indices[i];
        unsigned int i1 = indices[i + 1];
        unsigned int i2 = indices[i + 2];

        XMVECTOR v0 = XMLoadFloat3(&vertices[i0].Position);
        XMVECTOR v1 = XMLoadFloat3(&vertices[i1].Position);
        XMVECTOR v2 = XMLoadFloat3(&vertices[i2].Position);

        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));

        XMFLOAT3 n;
        XMStoreFloat3(&n, normal);

        // Assign the same normal to all vertices of this triangle
        vertices[i0].Normal = n;
        vertices[i1].Normal = n;
        vertices[i2].Normal = n;
    }

    ASSERT_ALWAYS_MSG(!vertices.empty() && !indices.empty(), "CreatePyramid produced empty mesh");

    return CreateFromVertices(vertices, indices);
}

/// Computes per-face normals via cross product and assigns to all three triangle vertices.
/// Note: this produces flat shading; smooth normals would require accumulation + normalization.
void Mesh::CalculateNormals()
{
    ASSERT(!m_vertices.empty() && !m_indices.empty());

    for (size_t i = 0; i + 2 < m_indices.size(); i += 3)
    {
        unsigned i0 = m_indices[i], i1 = m_indices[i + 1], i2 = m_indices[i + 2];
        XMVECTOR v0 = XMLoadFloat3(&m_vertices[i0].Position);
        XMVECTOR v1 = XMLoadFloat3(&m_vertices[i1].Position);
        XMVECTOR v2 = XMLoadFloat3(&m_vertices[i2].Position);

        XMVECTOR n = XMVector3Normalize(XMVector3Cross(v1 - v0, v2 - v0));
        XMFLOAT3 nf;
        XMStoreFloat3(&nf, n);

        m_vertices[i0].Normal = nf;
        m_vertices[i1].Normal = nf;
        m_vertices[i2].Normal = nf;
    }
}

/// Creates D3D11 vertex and index buffers from CPU-side data. Releases any
/// previously allocated buffers first to prevent COM object leaks.
HRESULT Mesh::CreateBuffers()
{
    ASSERT(m_device);
    ASSERT(!m_vertices.empty() && !m_indices.empty());

    // Release existing buffers before creating new ones
    m_vb.Reset();
    m_ib.Reset();

    // Vertex buffer
    D3D11_BUFFER_DESC vbd{};
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.ByteWidth = static_cast<UINT>(m_vertices.size() * sizeof(Vertex));
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{m_vertices.data(), 0, 0};

    HRESULT hr = m_device->CreateBuffer(&vbd, &vsd, m_vb.GetAddressOf());
    ASSERT_MSG(SUCCEEDED(hr), "CreateBuffer (VB) failed");
    if (FAILED(hr))
        return hr;

    // Index buffer
    D3D11_BUFFER_DESC ibd{};
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.ByteWidth = static_cast<UINT>(m_indices.size() * sizeof(unsigned int));
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA isd{};
    isd.pSysMem = m_indices.data();

    hr = m_device->CreateBuffer(&ibd, &isd, m_ib.GetAddressOf());
    ASSERT_MSG(SUCCEEDED(hr), "CreateBuffer (IB) failed");
    if (FAILED(hr))
        return hr;

    return S_OK;
}

/// Binds vertex/index buffers to the input assembler and issues an indexed draw call.
/// Uses triangle list topology with 32-bit indices.
void Mesh::Render(ID3D11DeviceContext* ctx)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, ctx && m_vb && m_ib && m_indexCount > 0,
                      "Mesh::Render — invalid render state");

    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = m_vb.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->DrawIndexed(m_indexCount, 0, 0);
}

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// Linux implementation — stores CPU-side geometry; GPU upload deferred to RHI
// ============================================================================
#include "Mesh.h"
#include "../Utils/Validate.h"
#include <cstring>
#include <unordered_map>
#include <tiny_obj_loader.h>

Mesh::Mesh() {}
Mesh::~Mesh()
{
    Shutdown();
}

HRESULT Mesh::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT_MSG(device != nullptr, "Mesh::Initialize — device must not be null");
    ASSERT_MSG(context != nullptr, "Mesh::Initialize — context must not be null");
    m_device = device;
    m_context = context;
    return S_OK;
}

void Mesh::Shutdown()
{
    m_vertices.clear();
    m_indices.clear();
    m_vertexCount = 0;
    m_indexCount = 0;
    // On Linux, m_vb/m_ib are ComPtr stubs — Reset() sets to nullptr
    m_vb.Reset();
    m_ib.Reset();
}

HRESULT Mesh::CreateBuffers()
{
    if (m_vertices.empty())
        return E_FAIL;
    m_vertexCount = static_cast<unsigned int>(m_vertices.size());
    m_indexCount = static_cast<unsigned int>(m_indices.size());
    // On Linux, the actual GPU buffers are created through the RHI layer
    // when the GraphicsEngine renders. We store CPU-side data here.
    return S_OK;
}

void Mesh::CalculateNormals()
{
    // Zero out normals
    for (auto& v : m_vertices)
        v.Normal = {0.0f, 0.0f, 0.0f};

    // Calculate face normals and accumulate
    for (size_t i = 0; i + 2 < m_indices.size(); i += 3)
    {
        auto& v0 = m_vertices[m_indices[i]];
        auto& v1 = m_vertices[m_indices[i + 1]];
        auto& v2 = m_vertices[m_indices[i + 2]];

        float e1x = v1.Position.x - v0.Position.x;
        float e1y = v1.Position.y - v0.Position.y;
        float e1z = v1.Position.z - v0.Position.z;
        float e2x = v2.Position.x - v0.Position.x;
        float e2y = v2.Position.y - v0.Position.y;
        float e2z = v2.Position.z - v0.Position.z;

        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;

        v0.Normal.x += nx;
        v0.Normal.y += ny;
        v0.Normal.z += nz;
        v1.Normal.x += nx;
        v1.Normal.y += ny;
        v1.Normal.z += nz;
        v2.Normal.x += nx;
        v2.Normal.y += ny;
        v2.Normal.z += nz;
    }

    // Normalize
    for (auto& v : m_vertices)
    {
        float len = sqrtf(v.Normal.x * v.Normal.x + v.Normal.y * v.Normal.y + v.Normal.z * v.Normal.z);
        if (len > 1e-8f)
        {
            v.Normal.x /= len;
            v.Normal.y /= len;
            v.Normal.z /= len;
        }
    }
}

HRESULT Mesh::CreateCube(float size)
{
    ASSERT_MSG(size > 0.0f, "Cube size must be positive");
    float h = size * 0.5f;
    m_vertices.clear();
    m_indices.clear();

    // 6 faces, 4 vertices each = 24 vertices
    Vertex verts[] = {
        // Front face
        {{-h, -h, -h}, {0, 0, -1}, {0, 1}},
        {{h, -h, -h}, {0, 0, -1}, {1, 1}},
        {{h, h, -h}, {0, 0, -1}, {1, 0}},
        {{-h, h, -h}, {0, 0, -1}, {0, 0}},
        // Back face
        {{h, -h, h}, {0, 0, 1}, {0, 1}},
        {{-h, -h, h}, {0, 0, 1}, {1, 1}},
        {{-h, h, h}, {0, 0, 1}, {1, 0}},
        {{h, h, h}, {0, 0, 1}, {0, 0}},
        // Top face
        {{-h, h, -h}, {0, 1, 0}, {0, 1}},
        {{h, h, -h}, {0, 1, 0}, {1, 1}},
        {{h, h, h}, {0, 1, 0}, {1, 0}},
        {{-h, h, h}, {0, 1, 0}, {0, 0}},
        // Bottom face
        {{-h, -h, h}, {0, -1, 0}, {0, 1}},
        {{h, -h, h}, {0, -1, 0}, {1, 1}},
        {{h, -h, -h}, {0, -1, 0}, {1, 0}},
        {{-h, -h, -h}, {0, -1, 0}, {0, 0}},
        // Left face
        {{-h, -h, h}, {-1, 0, 0}, {0, 1}},
        {{-h, -h, -h}, {-1, 0, 0}, {1, 1}},
        {{-h, h, -h}, {-1, 0, 0}, {1, 0}},
        {{-h, h, h}, {-1, 0, 0}, {0, 0}},
        // Right face
        {{h, -h, -h}, {1, 0, 0}, {0, 1}},
        {{h, -h, h}, {1, 0, 0}, {1, 1}},
        {{h, h, h}, {1, 0, 0}, {1, 0}},
        {{h, h, -h}, {1, 0, 0}, {0, 0}},
    };
    unsigned int inds[] = {0,  2,  1,  0,  3,  2,  4,  6,  5,  4,  7,  6,  8,  10, 9,  8,  11, 10,
                           12, 14, 13, 12, 15, 14, 16, 18, 17, 16, 19, 18, 20, 22, 21, 20, 23, 22};
    m_vertices.assign(verts, verts + 24);
    m_indices.assign(inds, inds + 36);
    return CreateBuffers();
}

HRESULT Mesh::CreateTriangle(float size)
{
    ASSERT_MSG(size > 0.0f, "Triangle size must be positive");
    float h = size * 0.5f;
    m_vertices = {
        {{0, h, 0}, {0, 0, -1}, {0.5f, 0}}, {{h, -h, 0}, {0, 0, -1}, {1, 1}}, {{-h, -h, 0}, {0, 0, -1}, {0, 1}}};
    m_indices = {0, 1, 2};
    return CreateBuffers();
}

HRESULT Mesh::CreatePlane(float width, float depth)
{
    ASSERT_MSG(width > 0.0f && depth > 0.0f, "Plane dimensions must be positive");
    float hw = width * 0.5f, hd = depth * 0.5f;
    m_vertices = {{{-hw, 0, -hd}, {0, 1, 0}, {0, 0}},
                  {{hw, 0, -hd}, {0, 1, 0}, {1, 0}},
                  {{hw, 0, hd}, {0, 1, 0}, {1, 1}},
                  {{-hw, 0, hd}, {0, 1, 0}, {0, 1}}};
    m_indices = {0, 1, 2, 0, 2, 3};
    return CreateBuffers();
}

HRESULT Mesh::CreateSphere(float radius, int slices, int stacks)
{
    ASSERT_MSG(radius > 0.0f, "Sphere radius must be positive");
    ASSERT_MSG(slices >= 3 && stacks >= 2, "Sphere needs at least 3 slices and 2 stacks");
    m_vertices.clear();
    m_indices.clear();

    for (int i = 0; i <= stacks; ++i)
    {
        float phi = MathUtils::PI * i / stacks;
        float sinPhi = sinf(phi), cosPhi = cosf(phi);
        for (int j = 0; j <= slices; ++j)
        {
            float theta = MathUtils::TWO_PI * j / slices;
            float sinTheta = sinf(theta), cosTheta = cosf(theta);
            XMFLOAT3 normal = {sinPhi * cosTheta, cosPhi, sinPhi * sinTheta};
            XMFLOAT3 pos = {radius * normal.x, radius * normal.y, radius * normal.z};
            XMFLOAT2 uv = {(float)j / slices, (float)i / stacks};
            m_vertices.push_back({pos, normal, uv});
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            unsigned int a = i * (slices + 1) + j;
            unsigned int b = a + slices + 1;
            m_indices.push_back(a);
            m_indices.push_back(b);
            m_indices.push_back(a + 1);
            m_indices.push_back(a + 1);
            m_indices.push_back(b);
            m_indices.push_back(b + 1);
        }
    }
    return CreateBuffers();
}

HRESULT Mesh::CreatePyramid(float size, float height)
{
    ASSERT_MSG(size > 0.0f && height > 0.0f, "Pyramid size and height must be positive");
    float h = size * 0.5f;
    XMFLOAT3 top = {0, height, 0};
    XMFLOAT3 bl = {-h, 0, -h}, br = {h, 0, -h};
    XMFLOAT3 fl = {-h, 0, h}, fr = {h, 0, h};

    m_vertices.clear();
    m_indices.clear();

    auto addFace = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c)
    {
        float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
        float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
        XMFLOAT3 n = {e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x};
        float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-8f)
        {
            n.x /= len;
            n.y /= len;
            n.z /= len;
        }
        unsigned int base = (unsigned int)m_vertices.size();
        m_vertices.push_back({a, n, {0, 1}});
        m_vertices.push_back({b, n, {1, 1}});
        m_vertices.push_back({c, n, {0.5f, 0}});
        m_indices.push_back(base);
        m_indices.push_back(base + 1);
        m_indices.push_back(base + 2);
    };

    addFace(bl, br, top); // Front
    addFace(br, fr, top); // Right
    addFace(fr, fl, top); // Back
    addFace(fl, bl, top); // Left

    // Base
    XMFLOAT3 dn = {0, -1, 0};
    unsigned int base = (unsigned int)m_vertices.size();
    m_vertices.push_back({bl, dn, {0, 0}});
    m_vertices.push_back({fl, dn, {0, 1}});
    m_vertices.push_back({fr, dn, {1, 1}});
    m_vertices.push_back({br, dn, {1, 0}});
    m_indices.push_back(base);
    m_indices.push_back(base + 1);
    m_indices.push_back(base + 2);
    m_indices.push_back(base);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 3);

    return CreateBuffers();
}

HRESULT Mesh::CreateFromVertices(const std::vector<Vertex>& verts, const std::vector<unsigned int>& inds)
{
    ASSERT_MSG(!verts.empty() && !inds.empty(), "CreateFromVertices — empty vertex or index data");
    m_vertices = verts;
    m_indices = inds;
    return CreateBuffers();
}

bool Mesh::LoadFromFile(const std::wstring& path)
{
    ASSERT_ALWAYS_MSG(!path.empty(), "Mesh::LoadFromFile — empty path");

    // Convert wide string to narrow for tinyobj on Linux
    std::string narrowPath(path.begin(), path.end());

    // Check file extension for OBJ support
    std::string ext;
    auto dotPos = narrowPath.rfind('.');
    if (dotPos != std::string::npos)
    {
        ext = narrowPath.substr(dotPos);
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    m_vertices.clear();
    m_indices.clear();

#if SPARK_HAS_CGLTF
    if (ext == ".gltf" || ext == ".glb")
    {
        cgltf_options options = {};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&options, narrowPath.c_str(), &data) != cgltf_result_success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "LoadFromFile: cgltf parse failed: %s", narrowPath.c_str());
            return false;
        }
        cgltf_load_buffers(&options, data, narrowPath.c_str());

        for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
        {
            const cgltf_mesh& mesh = data->meshes[mi];
            for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
            {
                const cgltf_primitive& prim = mesh.primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles)
                    continue;

                auto vertexOffset = static_cast<unsigned int>(m_vertices.size());
                const cgltf_accessor* posAcc = nullptr;
                const cgltf_accessor* normAcc = nullptr;
                const cgltf_accessor* texAcc = nullptr;

                for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
                {
                    if (prim.attributes[ai].type == cgltf_attribute_type_position)
                        posAcc = prim.attributes[ai].data;
                    else if (prim.attributes[ai].type == cgltf_attribute_type_normal)
                        normAcc = prim.attributes[ai].data;
                    else if (prim.attributes[ai].type == cgltf_attribute_type_texcoord)
                        texAcc = prim.attributes[ai].data;
                }

                if (!posAcc)
                    continue;

                cgltf_size vertCount = posAcc->count;
                std::vector<float> positions(vertCount * 3);
                cgltf_accessor_unpack_floats(posAcc, positions.data(), vertCount * 3);

                std::vector<float> normals;
                if (normAcc)
                {
                    normals.resize(vertCount * 3);
                    cgltf_accessor_unpack_floats(normAcc, normals.data(), vertCount * 3);
                }

                std::vector<float> texcoords;
                if (texAcc)
                {
                    texcoords.resize(vertCount * 2);
                    cgltf_accessor_unpack_floats(texAcc, texcoords.data(), vertCount * 2);
                }

                for (cgltf_size vi = 0; vi < vertCount; ++vi)
                {
                    Vertex vertex;
                    vertex.Position = {positions[vi * 3], positions[vi * 3 + 1], positions[vi * 3 + 2]};
                    if (!normals.empty())
                        vertex.Normal = {normals[vi * 3], normals[vi * 3 + 1], normals[vi * 3 + 2]};
                    if (!texcoords.empty())
                        vertex.TexCoord = {texcoords[vi * 2], texcoords[vi * 2 + 1]};
                    m_vertices.push_back(vertex);
                }

                if (prim.indices)
                {
                    for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
                    {
                        cgltf_uint idx = 0;
                        cgltf_accessor_read_uint(prim.indices, ii, &idx, 1);
                        m_indices.push_back(vertexOffset + idx);
                    }
                }
                else
                {
                    for (unsigned int vi = 0; vi < static_cast<unsigned int>(vertCount); ++vi)
                        m_indices.push_back(vertexOffset + vi);
                }
            }
        }

        bool needsNormals = true;
        for (cgltf_size mi = 0; mi < data->meshes_count && needsNormals; ++mi)
            for (cgltf_size pi = 0; pi < data->meshes[mi].primitives_count; ++pi)
                for (cgltf_size ai = 0; ai < data->meshes[mi].primitives[pi].attributes_count; ++ai)
                    if (data->meshes[mi].primitives[pi].attributes[ai].type == cgltf_attribute_type_normal)
                        needsNormals = false;

        cgltf_free(data);

        if (needsNormals)
            CalculateNormals();

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loaded glTF: %s (%zu vertices, %zu triangles)",
                       narrowPath.c_str(), m_vertices.size(), m_indices.size() / 3);
        return CreateBuffers() == S_OK;
    }
#endif // SPARK_HAS_CGLTF

    if (ext != ".obj")
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "LoadFromFile: unsupported format '%s' on Linux: %s", ext.c_str(),
                        narrowPath.c_str());
        return false;
    }

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, narrowPath.c_str());

    if (!warn.empty())
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "tinyobj warning: %s", warn.c_str());
    if (!err.empty())
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "tinyobj error: %s", err.c_str());
    if (!ret)
        return false;

    // Deduplicate vertices using a hash combining vertex/normal/texcoord indices.
    std::unordered_map<size_t, unsigned int> uniqueVertices;

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            Vertex vertex;

            if (index.vertex_index >= 0)
            {
                vertex.Position = {attrib.vertices[3 * index.vertex_index + 0],
                                   attrib.vertices[3 * index.vertex_index + 1],
                                   attrib.vertices[3 * index.vertex_index + 2]};
            }

            if (index.normal_index >= 0 && !attrib.normals.empty())
            {
                vertex.Normal = {attrib.normals[3 * index.normal_index + 0], attrib.normals[3 * index.normal_index + 1],
                                 attrib.normals[3 * index.normal_index + 2]};
            }

            if (index.texcoord_index >= 0 && !attrib.texcoords.empty())
            {
                vertex.TexCoord = {attrib.texcoords[2 * index.texcoord_index + 0],
                                   1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};
            }

            size_t h = std::hash<int>()(index.vertex_index);
            h ^= std::hash<int>()(index.normal_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(index.texcoord_index) + 0x9e3779b9 + (h << 6) + (h >> 2);

            auto it = uniqueVertices.find(h);
            if (it == uniqueVertices.end())
            {
                unsigned int newIndex = static_cast<unsigned int>(m_vertices.size());
                uniqueVertices[h] = newIndex;
                m_vertices.push_back(vertex);
                m_indices.push_back(newIndex);
            }
            else
            {
                m_indices.push_back(it->second);
            }
        }
    }

    if (attrib.normals.empty())
        CalculateNormals();

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loaded OBJ: %s (%zu vertices, %zu triangles)", narrowPath.c_str(),
                   m_vertices.size(), m_indices.size() / 3);

    return CreateBuffers() == S_OK;
}

void Mesh::Render(ID3D11DeviceContext* ctx)
{
    // On Linux, rendering goes through the RHI layer in GraphicsEngine
    // This function stores the draw intent; actual GPU calls happen via RHI
    (void)ctx;
}

#endif // SPARK_PLATFORM_WINDOWS
