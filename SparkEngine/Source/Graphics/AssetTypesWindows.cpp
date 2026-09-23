/**
 * @file AssetTypesWindows.cpp
 * @brief Windows/D3D11 mesh asset implementation (MeshAsset)
 *
 * Converts geometry from the shared OBJ/glTF CPU loaders and creates the D3D11
 * GPU buffers for meshes.
 * Split from AssetTypes.cpp for platform isolation; the texture/audio/cache
 * implementations live in AssetTypesWindowsMedia.cpp. The Linux counterpart
 * lives in AssetTypesLinux.cpp.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "GLTFStaticMeshLoader.h"
#include "OBJStaticMeshLoader.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

// ============================================================================
// MESH ASSET IMPLEMENTATION (Windows / D3D11)
// ============================================================================

HRESULT MeshAsset::Load(ID3D11Device* device)
{
    ASSERT(device);

    m_meshData.vertices.clear();
    m_meshData.indices.clear();
    m_meshData.submeshes.clear();

    if (!m_path.empty() && !std::filesystem::exists(m_path))
    {
        std::string extension = std::filesystem::path(m_path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".gltf" || extension == ".glb")
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "glTF source does not exist: %s", m_path.c_str());
            return E_FAIL;
        }
    }

    // Attempt to load from file if the path exists
    if (!m_path.empty() && std::filesystem::exists(m_path))
    {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".obj")
        {
            // Shared with the portable MeshAsset path so D3D11 imports the same
            // corners: every OBJ face form (including `v//vn`), UVs converted to
            // the engine's top-left origin, generated normals for corners that
            // lack one, and a hard failure (never the placeholder cube below)
            // when an existing OBJ yields no triangles.
            Spark::Graphics::Detail::OBJStaticMeshData imported;
            std::string error;
            if (!Spark::Graphics::Detail::LoadOBJStaticMesh(std::filesystem::path(m_path), imported, error))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load OBJ '%s': %s", m_path.c_str(),
                                error.c_str());
                Spark::SimpleConsole::GetInstance().LogError("Failed to load OBJ: " + m_path + " (" + error + ")");
                return E_FAIL;
            }

            XMFLOAT3 bbMin = {FLT_MAX, FLT_MAX, FLT_MAX};
            XMFLOAT3 bbMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            m_meshData.vertices.reserve(imported.vertices.size());
            for (const auto& source : imported.vertices)
            {
                MeshAssetData::Vertex vertex{};
                vertex.position = {source.position[0], source.position[1], source.position[2]};
                vertex.normal = {source.normal[0], source.normal[1], source.normal[2]};
                vertex.texCoord0 = {source.texCoord[0], source.texCoord[1]};
                vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                m_meshData.vertices.push_back(vertex);

                bbMin.x = std::min(bbMin.x, vertex.position.x);
                bbMin.y = std::min(bbMin.y, vertex.position.y);
                bbMin.z = std::min(bbMin.z, vertex.position.z);
                bbMax.x = std::max(bbMax.x, vertex.position.x);
                bbMax.y = std::max(bbMax.y, vertex.position.y);
                bbMax.z = std::max(bbMax.z, vertex.position.z);
            }
            m_meshData.indices = std::move(imported.indices);
            m_meshData.submeshes.reserve(imported.submeshes.size());
            for (const auto& submesh : imported.submeshes)
            {
                m_meshData.submeshes.push_back(submesh.indexStart);
            }

            m_meshData.boundingBoxMin = bbMin;
            m_meshData.boundingBoxMax = bbMax;
            m_meshData.boundingSphereCenter = {(bbMin.x + bbMax.x) * 0.5f, (bbMin.y + bbMax.y) * 0.5f,
                                               (bbMin.z + bbMax.z) * 0.5f};
            const float dx = bbMax.x - bbMin.x;
            const float dy = bbMax.y - bbMin.y;
            const float dz = bbMax.z - bbMin.z;
            m_meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

            Spark::SimpleConsole::GetInstance().LogSuccess("Loaded OBJ: " + m_path + " (" +
                                                           std::to_string(m_meshData.vertices.size()) + " verts, " +
                                                           std::to_string(m_meshData.indices.size() / 3) + " tris)");
        }
        else if (ext == ".gltf" || ext == ".glb")
        {
            Spark::Graphics::Detail::GLTFStaticMeshData imported;
            std::string error;
            if (!Spark::Graphics::Detail::LoadGLTFStaticMesh(std::filesystem::path(m_path), imported, error))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load glTF '%s': %s", m_path.c_str(),
                                error.c_str());
                return E_FAIL;
            }

            m_meshData.vertices.reserve(imported.vertices.size());
            m_meshData.indices = std::move(imported.indices);
            m_meshData.submeshes.reserve(imported.primitives.size());

            XMFLOAT3 bbMin = {FLT_MAX, FLT_MAX, FLT_MAX};
            XMFLOAT3 bbMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (const auto& source : imported.vertices)
            {
                MeshAssetData::Vertex vertex{};
                vertex.position = {source.position[0], source.position[1], source.position[2]};
                vertex.normal = {source.normal[0], source.normal[1], source.normal[2]};
                vertex.texCoord0 = {source.texCoord[0], source.texCoord[1]};
                vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                m_meshData.vertices.push_back(vertex);

                bbMin.x = std::min(bbMin.x, vertex.position.x);
                bbMin.y = std::min(bbMin.y, vertex.position.y);
                bbMin.z = std::min(bbMin.z, vertex.position.z);
                bbMax.x = std::max(bbMax.x, vertex.position.x);
                bbMax.y = std::max(bbMax.y, vertex.position.y);
                bbMax.z = std::max(bbMax.z, vertex.position.z);
            }
            for (const auto& primitive : imported.primitives)
            {
                m_meshData.submeshes.push_back(primitive.indexStart);
            }

            m_meshData.boundingBoxMin = bbMin;
            m_meshData.boundingBoxMax = bbMax;
            m_meshData.boundingSphereCenter = {(bbMin.x + bbMax.x) * 0.5f, (bbMin.y + bbMax.y) * 0.5f,
                                               (bbMin.z + bbMax.z) * 0.5f};
            const float dx = bbMax.x - bbMin.x;
            const float dy = bbMax.y - bbMin.y;
            const float dz = bbMax.z - bbMin.z;
            m_meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

            Spark::SimpleConsole::GetInstance().LogSuccess("Loaded glTF: " + m_path + " (" +
                                                           std::to_string(m_meshData.vertices.size()) + " verts, " +
                                                           std::to_string(m_meshData.indices.size() / 3) + " tris)");
        }
    }

    // Fallback: unit cube if no file was loaded
    if (m_meshData.vertices.empty())
    {
        m_meshData.vertices = {
            {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 0, 0}, {0, 1}, {0, 0}, {1, 1, 1, 1}},
            {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 0, 0}, {1, 1}, {0, 0}, {1, 1, 1, 1}},
            {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0, 0}, {1, 0}, {0, 0}, {1, 1, 1, 1}},
            {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0, 0}, {0, 0}, {0, 0}, {1, 1, 1, 1}},
            {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {-1, 0, 0}, {1, 1}, {0, 0}, {1, 1, 1, 1}},
            {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {-1, 0, 0}, {0, 1}, {0, 0}, {1, 1, 1, 1}},
            {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {-1, 0, 0}, {0, 0}, {0, 0}, {1, 1, 1, 1}},
            {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {-1, 0, 0}, {1, 0}, {0, 0}, {1, 1, 1, 1}},
        };
        m_meshData.indices = {0, 1, 2, 2, 3, 0, 4, 6, 5, 6, 4, 7, 4, 0, 3, 3, 7, 4,
                              1, 5, 6, 6, 2, 1, 3, 2, 6, 6, 7, 3, 4, 1, 0, 1, 4, 5};
    }

    if (m_meshData.vertices.size() > std::numeric_limits<UINT>::max() / sizeof(MeshAssetData::Vertex) ||
        m_meshData.indices.size() > std::numeric_limits<UINT>::max() / sizeof(uint32_t))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Mesh is too large for D3D11 buffers: %s", m_path.c_str());
        return E_FAIL;
    }

    // Create GPU buffers
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = static_cast<UINT>(m_meshData.vertices.size() * sizeof(MeshAssetData::Vertex));
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = m_meshData.vertices.data();
    HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    if (FAILED(hr))
        return hr;

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.Usage = D3D11_USAGE_DEFAULT;
    ibDesc.ByteWidth = static_cast<UINT>(m_meshData.indices.size() * sizeof(uint32_t));
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = m_meshData.indices.data();
    hr = device->CreateBuffer(&ibDesc, &ibData, &m_indexBuffer);
    if (FAILED(hr))
        return hr;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MeshAsset loaded: %zu verts, %zu indices, GPU buffers created",
                   m_meshData.vertices.size(), m_meshData.indices.size());
    m_loaded = true;
    return S_OK;
}

void MeshAsset::Unload()
{
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_meshData.vertices.clear();
    m_meshData.indices.clear();
    m_loaded = false;
}

size_t MeshAsset::GetMemoryUsage() const
{
    return m_meshData.vertices.size() * sizeof(MeshAssetData::Vertex) + m_meshData.indices.size() * sizeof(uint32_t);
}

#endif // SPARK_PLATFORM_WINDOWS
