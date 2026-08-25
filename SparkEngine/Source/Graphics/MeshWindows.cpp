/**
 * @file MeshWindows.cpp
 * @brief Windows/D3D11 implementation — split from Mesh.cpp
 *
 * Keeps lifecycle (Initialize/Shutdown), OBJ/glTF loading, normal calculation,
 * buffer creation, and rendering. Procedural primitive generation
 * (CreateCube/CreateTriangle/CreatePlane/CreateSphere/CreatePyramid) lives in
 * MeshWindowsPrimitives.cpp. The Linux counterpart lives in MeshLinux.cpp.
 */
#include "Mesh.h"
#include "GLTFStaticMeshLoader.h"
#include "../Core/Platform.h"
#include "../Utils/MathUtils.h"
/**
 * @file Mesh.cpp
 * @brief CPU-side mesh geometry and D3D11 GPU buffer management
 *
 * Supports loading OBJ files via tinyobjloader and static glTF meshes via cgltf,
 * procedural primitive generation
 * (cube, sphere, plane, triangle, pyramid), automatic normal calculation via
 * cross-product accumulation, and D3D11 vertex/index buffer creation.
 * Dual implementation: Windows uses DirectXMath + D3D11; Linux stores CPU-side
 * data for the RHI abstraction layer.
 */
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include <tiny_obj_loader.h>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <fstream>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <map>
#include <limits>
#include <utility>
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

/// Loads an OBJ or core static glTF mesh from disk, validates its CPU geometry,
/// recalculates missing normals, then uploads to GPU via CreateBuffers().
bool Mesh::LoadFromFile(const std::wstring& path)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !path.empty(), "Mesh::LoadFromFile — empty path");

    // Convert wide path to UTF-8 for tinyobjloader
    auto u8path_u8 = std::filesystem::path(path).u8string(); // basic_string<char8_t>
    std::string u8Path(u8path_u8.begin(), u8path_u8.end());  // convert to std::string

    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    if (extension == ".gltf" || extension == ".glb")
    {
        Spark::Graphics::Detail::GLTFStaticMeshData imported;
        std::string error;
        if (!Spark::Graphics::Detail::LoadGLTFStaticMesh(std::filesystem::path(path), imported, error))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "glTF load failed for '%s': %s", u8Path.c_str(),
                            error.c_str());
            return false;
        }
        if (imported.vertices.size() > std::numeric_limits<UINT>::max() ||
            imported.indices.size() > std::numeric_limits<UINT>::max())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "glTF mesh exceeds D3D11 count limits: %s", u8Path.c_str());
            return false;
        }

        std::vector<Vertex> vertices;
        vertices.reserve(imported.vertices.size());
        for (const auto& source : imported.vertices)
        {
            vertices.emplace_back(XMFLOAT3{source.position[0], source.position[1], source.position[2]},
                                  XMFLOAT3{source.normal[0], source.normal[1], source.normal[2]},
                                  XMFLOAT2{source.texCoord[0], source.texCoord[1]});
        }

        std::vector<MeshSubmesh> submeshes;
        submeshes.reserve(imported.primitives.size());
        for (const auto& primitive : imported.primitives)
        {
            MeshSubmesh submesh{};
            submesh.indexStart = primitive.indexStart;
            submesh.indexCount = primitive.indexCount;
            submeshes.push_back(std::move(submesh));
        }

        m_vertices = std::move(vertices);
        m_indices.assign(imported.indices.begin(), imported.indices.end());
        m_submeshes = std::move(submeshes);
        m_vertexCount = static_cast<UINT>(m_vertices.size());
        m_indexCount = static_cast<UINT>(m_indices.size());

        const HRESULT hr = CreateBuffers();
        if (FAILED(hr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "CreateBuffers failed for glTF: %s", u8Path.c_str());
            return false;
        }
        return true;
    }

    if (extension != ".obj")
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Unsupported mesh format '%s': %s", extension.c_str(),
                        u8Path.c_str());
        return false;
    }

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
