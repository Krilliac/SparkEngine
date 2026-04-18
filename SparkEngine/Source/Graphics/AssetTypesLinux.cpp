/**
 * @file AssetTypesLinux.cpp
 * @brief Linux asset type implementations (MeshAsset, TextureAsset, AudioAsset, AssetCache)
 *
 * Uses tinyobjloader, cgltf, stb_image, and FBXImporter for mesh/texture loading
 * without D3D11 dependencies. The Windows counterpart lives in AssetTypesWindows.cpp.
 * Split from AssetTypes.cpp for platform isolation.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "FBXImporter.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#include "Utils/LogMacros.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cfloat>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include <tiny_obj_loader.h>

#if SPARK_HAS_CGLTF
#include <cgltf.h>
#endif

#if SPARK_HAS_STB_IMAGE
#include <stb_image.h>
#endif

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
#if SPARK_HAS_CGLTF
        else if (ext == ".gltf" || ext == ".glb")
        {
            cgltf_options options = {};
            cgltf_data* data = nullptr;
            if (cgltf_parse_file(&options, m_path.c_str(), &data) == cgltf_result_success)
            {
                cgltf_load_buffers(&options, data, m_path.c_str());
                m_meshData.vertices.clear();
                m_meshData.indices.clear();

                for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
                {
                    const cgltf_mesh& mesh = data->meshes[mi];
                    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
                    {
                        const cgltf_primitive& prim = mesh.primitives[pi];
                        if (prim.type != cgltf_primitive_type_triangles)
                            continue;

                        uint32_t vertexOffset = static_cast<uint32_t>(m_meshData.vertices.size());
                        const cgltf_accessor* posAccessor = nullptr;
                        const cgltf_accessor* normAccessor = nullptr;
                        const cgltf_accessor* texAccessor = nullptr;

                        for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
                        {
                            if (prim.attributes[ai].type == cgltf_attribute_type_position)
                                posAccessor = prim.attributes[ai].data;
                            else if (prim.attributes[ai].type == cgltf_attribute_type_normal)
                                normAccessor = prim.attributes[ai].data;
                            else if (prim.attributes[ai].type == cgltf_attribute_type_texcoord)
                                texAccessor = prim.attributes[ai].data;
                        }

                        if (!posAccessor)
                            continue;

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
                            vertex.position = {positions[vi * 3], positions[vi * 3 + 1], positions[vi * 3 + 2]};
                            if (!normals.empty())
                                vertex.normal = {normals[vi * 3], normals[vi * 3 + 1], normals[vi * 3 + 2]};
                            if (!texcoords.empty())
                                vertex.texCoord0 = {texcoords[vi * 2], texcoords[vi * 2 + 1]};
                            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                            m_meshData.vertices.push_back(vertex);
                        }

                        if (prim.indices)
                        {
                            for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
                            {
                                cgltf_uint idx = 0;
                                cgltf_accessor_read_uint(prim.indices, ii, &idx, 1);
                                m_meshData.indices.push_back(vertexOffset + idx);
                            }
                        }
                        else
                        {
                            for (uint32_t vi = 0; vi < static_cast<uint32_t>(vertCount); ++vi)
                                m_meshData.indices.push_back(vertexOffset + vi);
                        }
                    }
                }
                cgltf_free(data);
                GenerateTangents(m_meshData);
                ComputeBounds(m_meshData);
            }
        }
#endif // SPARK_HAS_CGLTF
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

HRESULT TextureAsset::Load(ID3D11Device* /*device*/)
{
    m_metadata.filePath = m_path;
    m_metadata.name = std::filesystem::path(m_path).stem().string();
    m_metadata.type = AssetType::Texture;
    m_metadata.state = StreamingState::Loaded;
    if (std::filesystem::exists(m_path))
    {
        m_metadata.fileSize = std::filesystem::file_size(m_path);

#if SPARK_HAS_STB_IMAGE
        // Decode the image into a CPU-side RGBA8 buffer, then hand that to
        // the RHI bridge as a textured surface. stb_image converts any
        // supported format (PNG, JPG, TGA, BMP, HDR...) to 4-channel 8-bit.
        // Linux / macOS get the full texture pipeline this way without
        // needing WIC (Windows) or a platform-specific image library.
        int iw = 0;
        int ih = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(m_path.c_str(), &iw, &ih, &channels, STBI_rgb_alpha);
        if (pixels && iw > 0 && ih > 0)
        {
            m_width = static_cast<uint32_t>(iw);
            m_height = static_cast<uint32_t>(ih);

            auto& rhi = Spark::Graphics::Detail::GetRHI();
            if (rhi.initialized)
            {
                // CreateTexture2D takes RGBA8 pixel data directly. NullRHI
                // stubs the upload but returns a valid handle so the bind
                // path in ModelLoading.cpp still runs on headless builds.
                auto tex = rhi.bridge.CreateTexture2D(m_width, m_height, Spark::RHI::PixelFormat::R8G8B8A8_UNORM,
                                                      Spark::RHI::RHITextureUsage::ShaderResource, pixels);
                if (tex)
                {
                    m_rhiTexture = std::move(tex);
                }
                else
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "TextureAsset::Load: RHI texture creation failed for '%s'", m_path.c_str());
                }
            }
            // If the bridge isn't up yet (pre-graphics init, tests, etc.)
            // the CPU dimensions above still get reported via GetWidth /
            // GetHeight; a late RHI upload can re-read `m_path` later.

            stbi_image_free(pixels);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "TextureAsset::Load: stbi_load failed for '%s' (%s)",
                           m_path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        }
#endif // SPARK_HAS_STB_IMAGE
    }
    m_metadata.memorySize = GetMemoryUsage();
    m_loaded = true;
    return S_OK;
}

void TextureAsset::Unload()
{
    m_width = 0;
    m_height = 0;
    m_rhiTexture.reset();
    m_metadata.state = StreamingState::Unloaded;
    m_metadata.memorySize = 0;
    m_loaded = false;
}

size_t TextureAsset::GetMemoryUsage() const
{
    return static_cast<size_t>(m_width) * m_height * 4;
}

HRESULT AudioAsset::Load(ID3D11Device* /*device*/)
{
    m_metadata.filePath = m_path;
    m_metadata.name = std::filesystem::path(m_path).stem().string();
    m_metadata.type = AssetType::Audio;
    m_metadata.state = StreamingState::Loaded;
    if (std::filesystem::exists(m_path))
    {
        m_metadata.fileSize = std::filesystem::file_size(m_path);
        std::ifstream file(m_path, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            auto size = file.tellg();
            if (size > 0)
            {
                m_audioData.resize(static_cast<size_t>(size));
                file.seekg(0, std::ios::beg);
                file.read(reinterpret_cast<char*>(m_audioData.data()), size);
            }
            file.close();
        }
    }
    m_metadata.memorySize = GetMemoryUsage();
    m_loaded = true;
    return S_OK;
}

void AudioAsset::Unload()
{
    m_audioData.clear();
    m_sampleRate = 0;
    m_channels = 0;
    m_bitsPerSample = 0;
    m_metadata.state = StreamingState::Unloaded;
    m_metadata.memorySize = 0;
    m_loaded = false;
}

size_t AudioAsset::GetMemoryUsage() const
{
    return m_audioData.size();
}

// ============================================================================
// AssetCache (Linux)
// ============================================================================

AssetCache::AssetCache(size_t maxMemoryMB) : m_maxMemory(maxMemoryMB * 1024 * 1024), m_hits(0), m_misses(0) {}

AssetCache::~AssetCache()
{
    Clear();
}

void AssetCache::SetMaxMemory(size_t maxMemoryMB)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxMemory = maxMemoryMB * 1024 * 1024;
}

size_t AssetCache::GetCurrentMemory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = 0;
    for (const auto& pair : m_cache)
    {
        if (pair.second.asset)
        {
            total += pair.second.asset->GetMemoryUsage();
        }
    }
    return total;
}

void AssetCache::AddAsset(std::shared_ptr<Asset> asset)
{
    if (!asset)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    CacheEntry entry;
    entry.asset = asset;
    entry.lastAccessed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    entry.accessCount = 1;
    m_cache[asset->GetPath()] = entry;
}

std::shared_ptr<Asset> AssetCache::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(path);
    if (it != m_cache.end())
    {
        it->second.lastAccessed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        it->second.accessCount++;
        m_hits++;
        return it->second.asset;
    }
    m_misses++;
    return nullptr;
}

void AssetCache::RemoveAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(path);
}

void AssetCache::EvictLRU()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cache.empty())
        return;

    auto oldest = m_cache.begin();
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
    {
        if (it->second.lastAccessed < oldest->second.lastAccessed)
        {
            oldest = it;
        }
    }
    if (oldest != m_cache.end())
    {
        if (oldest->second.asset)
        {
            oldest->second.asset->Unload();
        }
        m_cache.erase(oldest);
    }
}

void AssetCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
    m_hits = 0;
    m_misses = 0;
}

float AssetCache::GetHitRatio() const
{
    uint32_t total = m_hits + m_misses;
    return total > 0 ? static_cast<float>(m_hits) / static_cast<float>(total) : 0.0f;
}

#endif // !SPARK_PLATFORM_WINDOWS
