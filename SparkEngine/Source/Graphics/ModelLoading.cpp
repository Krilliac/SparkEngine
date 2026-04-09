#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file ModelLoading.cpp
 * @brief Model file parsing, GPU buffer upload, and rendering helpers
 *
 * Contains format-specific loaders (OBJ, FBX, glTF), file-to-asset creation
 * helpers, and D3D11 mesh/material binding for draw calls.
 * Split from AssetPipeline.cpp for maintainability.
 */

#include "AssetPipeline.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// ============================================================================
// FILE-TO-ASSET CREATION HELPERS (Windows)
// ============================================================================

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loading mesh from file: %s", path.c_str());
    auto meshAsset = std::make_shared<MeshAsset>(path);
    HRESULT hr = meshAsset->Load(m_device);

    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load mesh: %s", path.c_str());
        Spark::SimpleConsole::GetInstance().LogError("Failed to load mesh: " + path);
        return nullptr;
    }

    return meshAsset;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTextureFromFile(const std::string& path)
{
    auto textureAsset = std::make_shared<TextureAsset>(path);
    HRESULT hr = textureAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + path);
        return nullptr;
    }

    return textureAsset;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudioFromFile(const std::string& path)
{
    auto audioAsset = std::make_shared<AudioAsset>(path);
    HRESULT hr = audioAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load audio: " + path);
        return nullptr;
    }

    return audioAsset;
}

#endif // inner SPARK_PLATFORM_WINDOWS
#else  // !SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "FBXImporter.h"
#include "../Utils/LogMacros.h"
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <tiny_obj_loader.h>

#if SPARK_HAS_CGLTF
#include <cgltf.h>
#endif

// ============================================================================
// FILE-TO-ASSET CREATION HELPERS (Linux)
// ============================================================================

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    auto mesh = std::make_shared<MeshAsset>(path);
    if (mesh->Load(m_device) == S_OK)
    {
        return mesh;
    }
    return nullptr;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTextureFromFile(const std::string& path)
{
    auto texture = std::make_shared<TextureAsset>(path);
    if (texture->Load(m_device) == S_OK)
    {
        return texture;
    }
    return nullptr;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudioFromFile(const std::string& path)
{
    auto audio = std::make_shared<AudioAsset>(path);
    if (audio->Load(m_device) == S_OK)
    {
        return audio;
    }
    return nullptr;
}

// ============================================================================
// FORMAT-SPECIFIC LOADERS (Linux)
// ============================================================================

HRESULT AssetPipeline::LoadOBJ(const std::string& path, MeshAssetData& meshData)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str());

    if (!ret)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "OBJ load failed: %s (%s)", path.c_str(), err.c_str());
        fprintf(stderr, "[AssetPipeline] OBJ load failed: %s %s\n", path.c_str(), err.c_str());
        return E_FAIL;
    }

    meshData.vertices.clear();
    meshData.indices.clear();
    meshData.submeshes.clear();

    XMFLOAT3 bboxMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 bboxMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    std::unordered_map<size_t, uint32_t> uniqueVertices;

    for (const auto& shape : shapes)
    {
        meshData.submeshes.push_back(static_cast<uint32_t>(meshData.indices.size()));

        for (const auto& index : shape.mesh.indices)
        {
            MeshAssetData::Vertex vertex{};

            if (index.vertex_index >= 0)
            {
                vertex.position = {attrib.vertices[3 * index.vertex_index + 0],
                                   attrib.vertices[3 * index.vertex_index + 1],
                                   attrib.vertices[3 * index.vertex_index + 2]};

                // Update bounding box
                bboxMin.x = std::min(bboxMin.x, vertex.position.x);
                bboxMin.y = std::min(bboxMin.y, vertex.position.y);
                bboxMin.z = std::min(bboxMin.z, vertex.position.z);
                bboxMax.x = std::max(bboxMax.x, vertex.position.x);
                bboxMax.y = std::max(bboxMax.y, vertex.position.y);
                bboxMax.z = std::max(bboxMax.z, vertex.position.z);
            }

            if (index.normal_index >= 0 && !attrib.normals.empty())
            {
                vertex.normal = {attrib.normals[3 * index.normal_index + 0], attrib.normals[3 * index.normal_index + 1],
                                 attrib.normals[3 * index.normal_index + 2]};
            }

            if (index.texcoord_index >= 0 && !attrib.texcoords.empty())
            {
                vertex.texCoord0 = {attrib.texcoords[2 * index.texcoord_index + 0],
                                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};
            }

            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};

            size_t h = std::hash<int>()(index.vertex_index);
            h ^= std::hash<int>()(index.normal_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(index.texcoord_index) + 0x9e3779b9 + (h << 6) + (h >> 2);

            auto it = uniqueVertices.find(h);
            if (it == uniqueVertices.end())
            {
                uint32_t newIndex = static_cast<uint32_t>(meshData.vertices.size());
                uniqueVertices[h] = newIndex;
                meshData.vertices.push_back(vertex);
                meshData.indices.push_back(newIndex);
            }
            else
            {
                meshData.indices.push_back(it->second);
            }
        }
    }

    meshData.boundingBoxMin = bboxMin;
    meshData.boundingBoxMax = bboxMax;

    // Calculate bounding sphere
    meshData.boundingSphereCenter = {(bboxMin.x + bboxMax.x) * 0.5f, (bboxMin.y + bboxMax.y) * 0.5f,
                                     (bboxMin.z + bboxMax.z) * 0.5f};
    float dx = bboxMax.x - bboxMin.x;
    float dy = bboxMax.y - bboxMin.y;
    float dz = bboxMax.z - bboxMin.z;
    meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loaded OBJ: %s (%zu verts, %zu tris, %zu submeshes)", path.c_str(),
                   meshData.vertices.size(), meshData.indices.size() / 3, meshData.submeshes.size());
    fprintf(stderr, "[AssetPipeline] Loaded OBJ: %s (%zu verts, %zu tris)\n", path.c_str(), meshData.vertices.size(),
            meshData.indices.size() / 3);
    return S_OK;
}

HRESULT AssetPipeline::LoadFBX(const std::string& path, MeshAssetData& meshData)
{
    Spark::Graphics::FBXImportOptions options{};
    options.targetCoordSystem = Spark::Graphics::CoordinateSystem::LeftHanded;
    options.scaleFactor = 1.0f;
    options.importAnimations = true;
    options.triangulate = true;

    Spark::Graphics::FBXImportResult result = Spark::Graphics::FBXImporter::GetInstance().Import(path, options);
    std::string validationError;
    if (!Spark::Graphics::FBXImporter::GetInstance().ValidateForPipeline(result, false, false, &validationError))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FBX import failed: %s (%s)", path.c_str(),
                        validationError.c_str());
        return E_FAIL;
    }

    meshData.vertices.clear();
    meshData.indices.clear();
    meshData.submeshes.clear();

    uint32_t vertexBase = 0;
    for (const auto& src : result.meshes)
    {
        if (src.vertices.empty() || src.indices.empty())
        {
            continue;
        }

        meshData.submeshes.push_back(static_cast<uint32_t>(meshData.indices.size()));
        const size_t vertexCount = src.vertices.size() / 3;
        const size_t normalCount = src.normals.size() / 3;
        const size_t uvCount = src.uvs.size() / 2;

        for (size_t i = 0; i < vertexCount; ++i)
        {
            MeshAssetData::Vertex vertex{};
            vertex.position.x = src.vertices[i * 3 + 0];
            vertex.position.y = src.vertices[i * 3 + 1];
            vertex.position.z = src.vertices[i * 3 + 2];
            if (i < normalCount)
            {
                vertex.normal.x = src.normals[i * 3 + 0];
                vertex.normal.y = src.normals[i * 3 + 1];
                vertex.normal.z = src.normals[i * 3 + 2];
            }
            if (i < uvCount)
            {
                vertex.texCoord0.x = src.uvs[i * 2 + 0];
                vertex.texCoord0.y = src.uvs[i * 2 + 1];
            }
            vertex.tangent.x = 1.0f;
            vertex.tangent.y = 0.0f;
            vertex.tangent.z = 0.0f;
            vertex.color.x = 1.0f;
            vertex.color.y = 1.0f;
            vertex.color.z = 1.0f;
            vertex.color.w = 1.0f;
            meshData.vertices.push_back(vertex);
        }

        for (uint32_t index : src.indices)
        {
            meshData.indices.push_back(vertexBase + index);
        }
        vertexBase += static_cast<uint32_t>(vertexCount);
    }

    if (meshData.vertices.empty() || meshData.indices.empty())
    {
        return E_FAIL;
    }
    return S_OK;
}

HRESULT AssetPipeline::LoadGLTF(const std::string& path, MeshAssetData& meshData)
{
#if SPARK_HAS_CGLTF
    cgltf_options options = {};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, path.c_str(), &data);
    if (result != cgltf_result_success)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "glTF parse failed: %s (error %d)", path.c_str(),
                        static_cast<int>(result));
        return E_FAIL;
    }

    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "glTF buffer load failed: %s", path.c_str());
        cgltf_free(data);
        return E_FAIL;
    }

    meshData.vertices.clear();
    meshData.indices.clear();
    meshData.submeshes.clear();

    XMFLOAT3 bboxMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 bboxMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
    {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
        {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles)
                continue;

            meshData.submeshes.push_back(static_cast<uint32_t>(meshData.indices.size()));
            uint32_t vertexOffset = static_cast<uint32_t>(meshData.vertices.size());

            // Find position/normal/texcoord accessors
            const cgltf_accessor* posAccessor = nullptr;
            const cgltf_accessor* normAccessor = nullptr;
            const cgltf_accessor* texAccessor = nullptr;

            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
            {
                if (prim.attributes[ai].type == cgltf_attribute_type_position)
                    posAccessor = prim.attributes[ai].data;
                else if (prim.attributes[ai].type == cgltf_attribute_type_normal)
                    normAccessor = prim.attributes[ai].data;
                else if (prim.attributes[ai].type == cgltf_attribute_type_texcoord && prim.attributes[ai].index == 0)
                    texAccessor = prim.attributes[ai].data;
            }

            if (!posAccessor)
                continue;

            // Read vertices
            cgltf_size vertCount = posAccessor->count;
            std::vector<float> positions(vertCount * 3);
            cgltf_accessor_unpack_floats(posAccessor, positions.data(), vertCount * 3);

            std::vector<float> normals;
            if (normAccessor)
            {
                normals.resize(vertCount * 3);
                cgltf_accessor_unpack_floats(normAccessor, normals.data(), vertCount * 3);
            }

            std::vector<float> texcoords;
            if (texAccessor)
            {
                texcoords.resize(vertCount * 2);
                cgltf_accessor_unpack_floats(texAccessor, texcoords.data(), vertCount * 2);
            }

            for (cgltf_size vi = 0; vi < vertCount; ++vi)
            {
                MeshAssetData::Vertex vertex{};
                vertex.position = {positions[vi * 3 + 0], positions[vi * 3 + 1], positions[vi * 3 + 2]};

                bboxMin.x = std::min(bboxMin.x, vertex.position.x);
                bboxMin.y = std::min(bboxMin.y, vertex.position.y);
                bboxMin.z = std::min(bboxMin.z, vertex.position.z);
                bboxMax.x = std::max(bboxMax.x, vertex.position.x);
                bboxMax.y = std::max(bboxMax.y, vertex.position.y);
                bboxMax.z = std::max(bboxMax.z, vertex.position.z);

                if (!normals.empty())
                    vertex.normal = {normals[vi * 3 + 0], normals[vi * 3 + 1], normals[vi * 3 + 2]};
                if (!texcoords.empty())
                    vertex.texCoord0 = {texcoords[vi * 2 + 0], texcoords[vi * 2 + 1]};
                vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};

                meshData.vertices.push_back(vertex);
            }

            // Read indices
            if (prim.indices)
            {
                for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
                {
                    cgltf_uint idx = 0;
                    cgltf_accessor_read_uint(prim.indices, ii, &idx, 1);
                    meshData.indices.push_back(vertexOffset + idx);
                }
            }
            else
            {
                // No index buffer — generate sequential indices
                for (uint32_t vi = 0; vi < static_cast<uint32_t>(vertCount); ++vi)
                    meshData.indices.push_back(vertexOffset + vi);
            }
        }
    }

    meshData.boundingBoxMin = bboxMin;
    meshData.boundingBoxMax = bboxMax;
    meshData.boundingSphereCenter = {(bboxMin.x + bboxMax.x) * 0.5f, (bboxMin.y + bboxMax.y) * 0.5f,
                                     (bboxMin.z + bboxMax.z) * 0.5f};
    float dx = bboxMax.x - bboxMin.x;
    float dy = bboxMax.y - bboxMin.y;
    float dz = bboxMax.z - bboxMin.z;
    meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Loaded glTF: %s (%zu verts, %zu tris, %zu submeshes)", path.c_str(),
                   meshData.vertices.size(), meshData.indices.size() / 3, meshData.submeshes.size());

    cgltf_free(data);
    return S_OK;
#else
    (void)path;
    (void)meshData;
    return E_NOTIMPL;
#endif
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// RENDERING HELPERS (cross-platform with platform guards)
// ============================================================================

void AssetPipeline::BindMesh(const std::string& meshPath)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    auto it = m_assets.find(meshPath);
    if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
    {
        return;
    }

    auto* meshAsset = dynamic_cast<MeshAsset*>(it->second.get());
    if (!meshAsset)
    {
        return;
    }

    ID3D11Buffer* vertexBuffer = meshAsset->GetVertexBuffer();
    ID3D11Buffer* indexBuffer = meshAsset->GetIndexBuffer();
    if (!vertexBuffer || !indexBuffer)
    {
        return;
    }

    constexpr UINT stride = sizeof(MeshAssetData::Vertex);
    constexpr UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#else
    (void)meshPath;
#endif
}

void AssetPipeline::BindMaterial(const std::string& materialPath)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    auto it = m_assets.find(materialPath);
    if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
    {
        return;
    }

    auto* textureAsset = dynamic_cast<TextureAsset*>(it->second.get());
    if (!textureAsset)
    {
        return;
    }

    ID3D11ShaderResourceView* srv = textureAsset->GetSRV();
    if (srv)
    {
        m_context->PSSetShaderResources(0, 1, &srv);
    }
#else
    (void)materialPath;
#endif
}

void AssetPipeline::DrawBoundMesh()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    for (const auto& [path, asset] : m_assets)
    {
        if (!asset || asset->GetType() != AssetType::Mesh || !asset->IsLoaded())
        {
            continue;
        }

        auto* meshAsset = dynamic_cast<MeshAsset*>(asset.get());
        if (meshAsset && meshAsset->GetIndexCount() > 0)
        {
            m_context->DrawIndexed(meshAsset->GetIndexCount(), 0, 0);
            return;
        }
    }
#endif
}
