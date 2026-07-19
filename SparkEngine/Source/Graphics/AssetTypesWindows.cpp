/**
 * @file AssetTypesWindows.cpp
 * @brief Windows/D3D11 mesh asset implementation (MeshAsset)
 *
 * Contains the OBJ parser and D3D11 GPU buffer creation for meshes.
 * Split from AssetTypes.cpp for platform isolation; the texture/audio/cache
 * implementations live in AssetTypesWindowsMedia.cpp. The Linux counterpart
 * lives in AssetTypesLinux.cpp.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

// ============================================================================
// MESH ASSET IMPLEMENTATION (Windows / D3D11)
// ============================================================================

HRESULT MeshAsset::Load(ID3D11Device* device)
{
    ASSERT(device);

    // Attempt to load from file if the path exists
    if (!m_path.empty() && std::filesystem::exists(m_path))
    {
        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".obj")
        {
            std::ifstream file(m_path);
            if (file.is_open())
            {
                std::vector<XMFLOAT3> positions;
                std::vector<XMFLOAT3> normals;
                std::vector<XMFLOAT2> texCoords;
                std::string line;

                XMFLOAT3 bbMin = {FLT_MAX, FLT_MAX, FLT_MAX};
                XMFLOAT3 bbMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

                while (std::getline(file, line))
                {
                    std::istringstream iss(line);
                    std::string prefix;
                    iss >> prefix;

                    if (prefix == "v")
                    {
                        XMFLOAT3 pos;
                        iss >> pos.x >> pos.y >> pos.z;
                        positions.push_back(pos);
                        bbMin.x = std::min(bbMin.x, pos.x);
                        bbMin.y = std::min(bbMin.y, pos.y);
                        bbMin.z = std::min(bbMin.z, pos.z);
                        bbMax.x = std::max(bbMax.x, pos.x);
                        bbMax.y = std::max(bbMax.y, pos.y);
                        bbMax.z = std::max(bbMax.z, pos.z);
                    }
                    else if (prefix == "vn")
                    {
                        XMFLOAT3 n;
                        iss >> n.x >> n.y >> n.z;
                        normals.push_back(n);
                    }
                    else if (prefix == "vt")
                    {
                        XMFLOAT2 tc;
                        iss >> tc.x >> tc.y;
                        texCoords.push_back(tc);
                    }
                    else if (prefix == "f")
                    {
                        std::string token;
                        std::vector<uint32_t> faceIndices;
                        while (iss >> token)
                        {
                            MeshAssetData::Vertex vert = {};
                            int vi = 0, vti = 0, vni = 0;
                            if (sscanf(token.c_str(), "%d/%d/%d", &vi, &vti, &vni) >= 1)
                            {
                                if (vi > 0 && vi <= static_cast<int>(positions.size()))
                                    vert.position = positions[vi - 1];
                                if (vti > 0 && vti <= static_cast<int>(texCoords.size()))
                                    vert.texCoord0 = texCoords[vti - 1];
                                if (vni > 0 && vni <= static_cast<int>(normals.size()))
                                    vert.normal = normals[vni - 1];
                            }
                            vert.color = {1.0f, 1.0f, 1.0f, 1.0f};
                            faceIndices.push_back(static_cast<uint32_t>(m_meshData.vertices.size()));
                            m_meshData.vertices.push_back(vert);
                        }
                        for (size_t i = 2; i < faceIndices.size(); ++i)
                        {
                            m_meshData.indices.push_back(faceIndices[0]);
                            m_meshData.indices.push_back(faceIndices[i - 1]);
                            m_meshData.indices.push_back(faceIndices[i]);
                        }
                    }
                }
                m_meshData.boundingBoxMin = bbMin;
                m_meshData.boundingBoxMax = bbMax;
                m_meshData.boundingSphereCenter = {(bbMin.x + bbMax.x) * 0.5f, (bbMin.y + bbMax.y) * 0.5f,
                                                   (bbMin.z + bbMax.z) * 0.5f};
                float dx = bbMax.x - bbMin.x, dy = bbMax.y - bbMin.y, dz = bbMax.z - bbMin.z;
                m_meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
                file.close();

                if (!m_meshData.vertices.empty())
                {
                    Spark::SimpleConsole::GetInstance().LogSuccess(
                        "Loaded OBJ: " + m_path + " (" + std::to_string(m_meshData.vertices.size()) + " verts, " +
                        std::to_string(m_meshData.indices.size() / 3) + " tris)");
                }
            }
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
