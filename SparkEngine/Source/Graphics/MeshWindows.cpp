/**
 * @file MeshWindows.cpp
 * @brief Windows/D3D11 implementation — split from Mesh.cpp
 */
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
#include <map>
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
    m_submeshes.clear();
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

    // Parse geometry, grouped by MTL material so each material becomes a
    // contiguous index range (submesh) that can be drawn with its own
    // diffuse color / texture. Faces with no material get bucket -1.
    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& materials = reader.GetMaterials();

    auto makeVertex = [&](const tinyobj::index_t& idx)
    {
        Vertex v{};
        v.Position = {attrib.vertices[3 * idx.vertex_index + 0], attrib.vertices[3 * idx.vertex_index + 1],
                      attrib.vertices[3 * idx.vertex_index + 2]};
        if (idx.normal_index >= 0)
        {
            v.Normal = {attrib.normals[3 * idx.normal_index + 0], attrib.normals[3 * idx.normal_index + 1],
                        attrib.normals[3 * idx.normal_index + 2]};
        }
        else
        {
            v.Normal = {0, 1, 0};
        }
        if (idx.texcoord_index >= 0)
        {
            v.TexCoord = {attrib.texcoords[2 * idx.texcoord_index + 0],
                          1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]};
        }
        else
        {
            v.TexCoord = {0, 0};
        }
        return v;
    };

    // Bucket triangles by material id (ObjReader triangulates by default).
    std::map<int, std::vector<Vertex>> buckets;
    for (const auto& shape : shapes)
    {
        const size_t faceCount = shape.mesh.indices.size() / 3;
        for (size_t f = 0; f < faceCount; ++f)
        {
            int matId = -1;
            if (f < shape.mesh.material_ids.size())
                matId = shape.mesh.material_ids[f];
            auto& bucket = buckets[matId];
            bucket.push_back(makeVertex(shape.mesh.indices[3 * f + 0]));
            bucket.push_back(makeVertex(shape.mesh.indices[3 * f + 1]));
            bucket.push_back(makeVertex(shape.mesh.indices[3 * f + 2]));
        }
    }

    std::vector<Vertex> verts;
    std::vector<unsigned int> inds;
    std::vector<MeshSubmesh> submeshes;
    const std::filesystem::path objDir = std::filesystem::path(path).parent_path();

    for (auto& [matId, bucket] : buckets)
    {
        MeshSubmesh sm{};
        sm.indexStart = static_cast<unsigned int>(inds.size());
        sm.indexCount = static_cast<unsigned int>(bucket.size());
        if (matId >= 0 && matId < static_cast<int>(materials.size()))
        {
            const auto& mat = materials[matId];
            sm.diffuseColor = {mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], 1.0f};
            if (!mat.diffuse_texname.empty())
            {
                // Resolve map_Kd relative to the OBJ's directory
                std::filesystem::path texPath(mat.diffuse_texname);
                if (texPath.is_relative())
                    texPath = objDir / texPath;
                sm.diffuseTexture = texPath.generic_string();
            }
        }
        for (const Vertex& v : bucket)
        {
            verts.push_back(v);
            inds.push_back(static_cast<unsigned int>(inds.size()));
        }
        submeshes.push_back(std::move(sm));
    }

    // Ensure we got geometry
    ASSERT_ALWAYS_MSG(!verts.empty() && !inds.empty(), "tinyobj produced empty mesh data");

    m_vertices = std::move(verts);
    m_indices = std::move(inds);
    m_submeshes = std::move(submeshes);
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
    m_submeshes.clear(); // procedural geometry has no per-material ranges
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
    m_submeshes.clear();

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
    // Proper 0..1 UVs per corner — previously all four corners were (0,0),
    // which collapsed any texture to a single texel (untextured-looking plane).
    XMFLOAT2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    XMFLOAT3 n{0, 1, 0};
    unsigned int idxs[6] = {0, 1, 2, 0, 2, 3};

    for (int i = 0; i < 6; ++i)
    {
        md.vertices.emplace_back(pts[idxs[i]], n, uvs[idxs[i]]);
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

/// Binds buffers and draws a contiguous index range (one submesh).
void Mesh::RenderRange(ID3D11DeviceContext* ctx, unsigned int indexStart, unsigned int indexCount)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, ctx && m_vb && m_ib && indexCount > 0,
                      "Mesh::RenderRange — invalid render state");
    if (indexStart + indexCount > m_indexCount)
        return;

    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = m_vb.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->DrawIndexed(indexCount, indexStart, 0);
}


#endif // SPARK_PLATFORM_WINDOWS
