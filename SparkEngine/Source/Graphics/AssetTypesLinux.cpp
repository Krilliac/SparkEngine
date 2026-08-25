/**
 * @file AssetTypesLinux.cpp
 * @brief Linux mesh asset implementation (MeshAsset)
 *
 * Uses tinyobjloader, cgltf, and FBXImporter for mesh loading without D3D11
 * dependencies. TextureAsset, AudioAsset, and AssetCache live in
 * AssetTypesLinuxMedia.cpp; the Windows counterpart lives in AssetTypesWindows.cpp.
 * Split from AssetTypes.cpp for platform isolation.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "FBXImporter.h"
#include "GLTFStaticMeshLoader.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#include "Utils/LogMacros.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cfloat>
#include <cstring>
#include <cmath>
#include <utility>
#include <sys/stat.h>
#include <tiny_obj_loader.h>

// ============================================================================
// Asset implementations (Linux)
// ============================================================================

namespace
{
    void ComputeBounds(MeshAssetData& meshData)
    {
        if (meshData.vertices.empty())
        {
            meshData.boundingBoxMin = {0.0f, 0.0f, 0.0f};
            meshData.boundingBoxMax = {0.0f, 0.0f, 0.0f};
            meshData.boundingSphereCenter = {0.0f, 0.0f, 0.0f};
            meshData.boundingSphereRadius = 0.0f;
            return;
        }

        XMFLOAT3 minV = {FLT_MAX, FLT_MAX, FLT_MAX};
        XMFLOAT3 maxV = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto& v : meshData.vertices)
        {
            minV.x = std::min(minV.x, v.position.x);
            minV.y = std::min(minV.y, v.position.y);
            minV.z = std::min(minV.z, v.position.z);
            maxV.x = std::max(maxV.x, v.position.x);
            maxV.y = std::max(maxV.y, v.position.y);
            maxV.z = std::max(maxV.z, v.position.z);
        }

        meshData.boundingBoxMin = minV;
        meshData.boundingBoxMax = maxV;
        meshData.boundingSphereCenter = {(minV.x + maxV.x) * 0.5f, (minV.y + maxV.y) * 0.5f, (minV.z + maxV.z) * 0.5f};
        float dx = maxV.x - minV.x;
        float dy = maxV.y - minV.y;
        float dz = maxV.z - minV.z;
        meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
    }

    void GenerateTangents(MeshAssetData& meshData)
    {
        if (meshData.vertices.empty() || meshData.indices.size() < 3)
        {
            return;
        }

        std::vector<XMFLOAT3> accum(meshData.vertices.size(), XMFLOAT3{0.0f, 0.0f, 0.0f});
        for (size_t i = 0; i + 2 < meshData.indices.size(); i += 3)
        {
            uint32_t i0 = meshData.indices[i + 0];
            uint32_t i1 = meshData.indices[i + 1];
            uint32_t i2 = meshData.indices[i + 2];
            if (i0 >= meshData.vertices.size() || i1 >= meshData.vertices.size() || i2 >= meshData.vertices.size())
            {
                continue;
            }

            const auto& v0 = meshData.vertices[i0];
            const auto& v1 = meshData.vertices[i1];
            const auto& v2 = meshData.vertices[i2];

            float e1x = v1.position.x - v0.position.x;
            float e1y = v1.position.y - v0.position.y;
            float e1z = v1.position.z - v0.position.z;
            float e2x = v2.position.x - v0.position.x;
            float e2y = v2.position.y - v0.position.y;
            float e2z = v2.position.z - v0.position.z;

            float du1 = v1.texCoord0.x - v0.texCoord0.x;
            float dv1 = v1.texCoord0.y - v0.texCoord0.y;
            float du2 = v2.texCoord0.x - v0.texCoord0.x;
            float dv2 = v2.texCoord0.y - v0.texCoord0.y;

            float det = du1 * dv2 - du2 * dv1;
            if (std::abs(det) < 1e-8f)
            {
                continue;
            }

            float invDet = 1.0f / det;
            XMFLOAT3 tangent{(dv2 * e1x - dv1 * e2x) * invDet, (dv2 * e1y - dv1 * e2y) * invDet,
                             (dv2 * e1z - dv1 * e2z) * invDet};

            accum[i0].x += tangent.x;
            accum[i0].y += tangent.y;
            accum[i0].z += tangent.z;
            accum[i1].x += tangent.x;
            accum[i1].y += tangent.y;
            accum[i1].z += tangent.z;
            accum[i2].x += tangent.x;
            accum[i2].y += tangent.y;
            accum[i2].z += tangent.z;
        }

        for (size_t i = 0; i < meshData.vertices.size(); ++i)
        {
            auto& t = accum[i];
            float len = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
            if (len > 1e-6f)
            {
                meshData.vertices[i].tangent.x = t.x / len;
                meshData.vertices[i].tangent.y = t.y / len;
                meshData.vertices[i].tangent.z = t.z / len;
            }
            else
            {
                meshData.vertices[i].tangent.x = 1.0f;
                meshData.vertices[i].tangent.y = 0.0f;
                meshData.vertices[i].tangent.z = 0.0f;
            }
        }
    }
} // namespace

HRESULT MeshAsset::Load(ID3D11Device* /*device*/)
{
    m_metadata.filePath = m_path;
    m_metadata.name = std::filesystem::path(m_path).stem().string();
    m_metadata.type = AssetType::Mesh;
    m_metadata.state = StreamingState::Loaded;
    if (std::filesystem::exists(m_path))
    {
        m_metadata.fileSize = std::filesystem::file_size(m_path);

        std::string ext = std::filesystem::path(m_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".obj")
        {
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;
            std::string warn, err;
            if (tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, m_path.c_str()))
            {
                m_meshData.vertices.clear();
                m_meshData.indices.clear();
                for (const auto& shape : shapes)
                {
                    for (const auto& index : shape.mesh.indices)
                    {
                        MeshAssetData::Vertex vertex{};
                        if (index.vertex_index >= 0)
                        {
                            vertex.position = {attrib.vertices[3 * index.vertex_index + 0],
                                               attrib.vertices[3 * index.vertex_index + 1],
                                               attrib.vertices[3 * index.vertex_index + 2]};
                        }
                        if (index.normal_index >= 0 && !attrib.normals.empty())
                        {
                            vertex.normal = {attrib.normals[3 * index.normal_index + 0],
                                             attrib.normals[3 * index.normal_index + 1],
                                             attrib.normals[3 * index.normal_index + 2]};
                        }
                        if (index.texcoord_index >= 0 && !attrib.texcoords.empty())
                        {
                            vertex.texCoord0 = {attrib.texcoords[2 * index.texcoord_index + 0],
                                                1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};
                        }
                        vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                        m_meshData.indices.push_back(static_cast<uint32_t>(m_meshData.vertices.size()));
                        m_meshData.vertices.push_back(vertex);
                    }
                }
            }
        }
        else if (ext == ".fbx")
        {
            Spark::Graphics::FBXImportOptions importOptions{};
            importOptions.targetCoordSystem = Spark::Graphics::CoordinateSystem::LeftHanded;
            importOptions.scaleFactor = 1.0f;
            importOptions.importAnimations = true;
            importOptions.flipUVs = false;

            Spark::Graphics::FBXImportResult result =
                Spark::Graphics::FBXImporter::GetInstance().Import(m_path, importOptions);
            std::string validationError;
            if (Spark::Graphics::FBXImporter::GetInstance().ValidateForPipeline(result, false, false, &validationError))
            {
                m_meshData.vertices.clear();
                m_meshData.indices.clear();
                m_meshData.submeshes.clear();

                uint32_t vertexBase = 0;
                for (const auto& mesh : result.meshes)
                {
                    if (mesh.vertices.empty() || mesh.indices.empty())
                    {
                        continue;
                    }

                    m_meshData.submeshes.push_back(static_cast<uint32_t>(m_meshData.indices.size()));
                    const size_t vertexCount = mesh.vertices.size() / 3;
                    const size_t normalCount = mesh.normals.size() / 3;
                    const size_t uvCount = mesh.uvs.size() / 2;

                    for (size_t i = 0; i < vertexCount; ++i)
                    {
                        MeshAssetData::Vertex vertex{};
                        vertex.position.x = mesh.vertices[i * 3 + 0];
                        vertex.position.y = mesh.vertices[i * 3 + 1];
                        vertex.position.z = mesh.vertices[i * 3 + 2];
                        if (i < normalCount)
                        {
                            vertex.normal.x = mesh.normals[i * 3 + 0];
                            vertex.normal.y = mesh.normals[i * 3 + 1];
                            vertex.normal.z = mesh.normals[i * 3 + 2];
                        }
                        if (i < uvCount)
                        {
                            vertex.texCoord0.x = mesh.uvs[i * 2 + 0];
                            vertex.texCoord0.y = mesh.uvs[i * 2 + 1];
                        }
                        vertex.color.x = 1.0f;
                        vertex.color.y = 1.0f;
                        vertex.color.z = 1.0f;
                        vertex.color.w = 1.0f;
                        m_meshData.vertices.push_back(vertex);
                    }

                    for (uint32_t idx : mesh.indices)
                    {
                        m_meshData.indices.push_back(vertexBase + idx);
                    }
                    vertexBase += static_cast<uint32_t>(vertexCount);
                }

                GenerateTangents(m_meshData);
                ComputeBounds(m_meshData);

                m_metadata.customProperties["fbx.meshCount"] = std::to_string(result.meshes.size());
                m_metadata.customProperties["fbx.boneCount"] = std::to_string(result.bones.size());
                m_metadata.customProperties["fbx.animationCount"] = std::to_string(result.animations.size());
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics, "FBX validation failed for '%s': %s", m_path.c_str(),
                               validationError.c_str());
            }
        }
        else if (ext == ".gltf" || ext == ".glb")
        {
            Spark::Graphics::Detail::GLTFStaticMeshData imported;
            std::string error;
            if (Spark::Graphics::Detail::LoadGLTFStaticMesh(std::filesystem::path(m_path), imported, error))
            {
                m_meshData.vertices.clear();
                m_meshData.indices.clear();
                m_meshData.submeshes.clear();
                m_meshData.vertices.reserve(imported.vertices.size());
                for (const auto& source : imported.vertices)
                {
                    MeshAssetData::Vertex vertex{};
                    vertex.position = {source.position[0], source.position[1], source.position[2]};
                    vertex.normal = {source.normal[0], source.normal[1], source.normal[2]};
                    vertex.texCoord0 = {source.texCoord[0], source.texCoord[1]};
                    vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                    m_meshData.vertices.push_back(vertex);
                }
                m_meshData.indices = std::move(imported.indices);
                for (const auto& primitive : imported.primitives)
                {
                    m_meshData.submeshes.push_back(primitive.indexStart);
                }
                GenerateTangents(m_meshData);
                ComputeBounds(m_meshData);
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics, "glTF validation failed for '%s': %s", m_path.c_str(),
                               error.c_str());
                m_metadata.state = StreamingState::Failed;
                return E_FAIL;
            }
        }
    }
    if (!m_meshData.vertices.empty())
    {
        ComputeBounds(m_meshData);
    }
    m_metadata.memorySize = GetMemoryUsage();
    m_loaded = true;
    return S_OK;
}

void MeshAsset::Unload()
{
    m_meshData.vertices.clear();
    m_meshData.indices.clear();
    m_meshData.submeshes.clear();
    m_metadata.state = StreamingState::Unloaded;
    m_metadata.memorySize = 0;
    m_loaded = false;
}

size_t MeshAsset::GetMemoryUsage() const
{
    return m_meshData.vertices.size() * sizeof(MeshAssetData::Vertex) + m_meshData.indices.size() * sizeof(uint32_t) +
           m_meshData.submeshes.size() * sizeof(uint32_t);
}

#endif // !SPARK_PLATFORM_WINDOWS
