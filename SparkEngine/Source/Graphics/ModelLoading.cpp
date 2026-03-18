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

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// ============================================================================
// FILE-TO-ASSET CREATION HELPERS (Windows)
// ============================================================================

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    auto meshAsset = std::make_shared<MeshAsset>(path);
    HRESULT hr = meshAsset->Load(m_device);

    if (FAILED(hr))
    {
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
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <tiny_obj_loader.h>

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

    fprintf(stderr, "[AssetPipeline] Loaded OBJ: %s (%zu verts, %zu tris)\n", path.c_str(), meshData.vertices.size(),
            meshData.indices.size() / 3);
    return S_OK;
}

HRESULT AssetPipeline::LoadFBX(const std::string& path, MeshAssetData& /*meshData*/)
{
    // FBX loading not implemented on Linux - requires proprietary SDK
    (void)path;
    return E_NOTIMPL;
}

HRESULT AssetPipeline::LoadGLTF(const std::string& path, MeshAssetData& /*meshData*/)
{
    // glTF loading not implemented on Linux - requires external library
    (void)path;
    return E_NOTIMPL;
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
