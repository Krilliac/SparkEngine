/**
 * @file AssetTypes.cpp
 * @brief Cross-platform asset type definitions.
 *
 * Platform-specific `Load()` / `Unload()` / `GetMemoryUsage()` live in
 * `AssetTypesWindows.cpp` (D3D11 GPU buffer creation, WIC texture loading)
 * and `AssetTypesLinux.cpp` (tinyobjloader, cgltf, stb_image, FBXImporter).
 * Each guards itself with `#ifdef SPARK_PLATFORM_WINDOWS` so only the
 * correct one compiles.
 *
 * Methods that are pure C++ with no platform-specific APIs (including the
 * shared glTF mesh import) live here
 * instead so they compile exactly once per translation unit and don't
 * need to be duplicated across the platform files.
 */

#include "AssetPipeline.h"

#include "GLTFSkinnedMeshLoader.h"
#include "GLTFStaticMeshLoader.h"
#include "RHI/RHIResources.h"

#include <utility>

// ---------------------------------------------------------------------------
// MeshAsset — cross-platform accessors
// ---------------------------------------------------------------------------
// `SetRHIBuffers` is a move-in of unique_ptr<IRHIBuffer> — it has zero
// platform dependencies. Putting it here keeps the platform-specific
// AssetTypes*.cpp files focused on actual format/GPU code.

// Out-of-line constructor / destructor — the `std::unique_ptr<IRHIBuffer>`
// members need `IRHIBuffer` complete at ctor-cleanup and dtor time, and this
// is the only TU that includes RHIResources.h. Keeping both definitions in a
// cross-platform TU means every consumer links against the same implementation
// (avoids ODR landmines if Windows and Linux took different paths).
MeshAsset::MeshAsset(const std::string& path) : Asset(path, AssetType::Mesh), m_meshData{} {}

MeshAsset::~MeshAsset() = default;

void MeshAsset::SetRHIBuffers(std::unique_ptr<Spark::RHI::IRHIBuffer> vb, std::unique_ptr<Spark::RHI::IRHIBuffer> ib)
{
    m_rhiVertexBuffer = std::move(vb);
    m_rhiIndexBuffer = std::move(ib);
}

// ---------------------------------------------------------------------------
// TextureAsset — cross-platform accessors
// ---------------------------------------------------------------------------
// Same rationale as MeshAsset above: out-of-line ctor/dtor so the
// `std::unique_ptr<IRHITexture>` member can use the forward-declared type.

TextureAsset::TextureAsset(const std::string& path) : Asset(path, AssetType::Texture) {}

TextureAsset::~TextureAsset() = default;

void TextureAsset::SetRHITexture(std::unique_ptr<Spark::RHI::IRHITexture> tex)
{
    m_rhiTexture = std::move(tex);
}

// ---------------------------------------------------------------------------
// glTF mesh import shared by the D3D11 and portable MeshAsset
// ---------------------------------------------------------------------------

namespace Spark::Graphics::Detail
{
    bool ImportGLTFMeshAssetData(const std::filesystem::path& path, MeshAssetData& meshData, size_t& boneCount,
                                 std::string& error)
    {
        meshData.vertices.clear();
        meshData.indices.clear();
        meshData.submeshes.clear();
        boneCount = 0;

        bool hasSkin = false;
        if (!GLTFFileHasSkin(path, hasSkin, error))
        {
            return false;
        }

        if (hasSkin)
        {
            GLTFSkinnedMeshData imported;
            if (!LoadGLTFSkinnedMesh(path, imported, error))
            {
                return false;
            }
            meshData.vertices.reserve(imported.vertices.size());
            for (const auto& source : imported.vertices)
            {
                MeshAssetData::Vertex vertex{};
                vertex.position = {source.position[0], source.position[1], source.position[2]};
                vertex.normal = {source.normal[0], source.normal[1], source.normal[2]};
                vertex.texCoord0 = {source.texCoord[0], source.texCoord[1]};
                vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
                vertex.boneIndices = {source.joints[0], source.joints[1], source.joints[2], source.joints[3]};
                vertex.boneWeights = {source.weights[0], source.weights[1], source.weights[2], source.weights[3]};
                meshData.vertices.push_back(vertex);
            }
            meshData.indices = std::move(imported.indices);
            for (const auto& primitive : imported.primitives)
            {
                meshData.submeshes.push_back(primitive.indexStart);
            }
            boneCount = imported.skeleton.bones.size();
            return true;
        }

        GLTFStaticMeshData imported;
        if (!LoadGLTFStaticMesh(path, imported, error))
        {
            return false;
        }
        meshData.vertices.reserve(imported.vertices.size());
        for (const auto& source : imported.vertices)
        {
            MeshAssetData::Vertex vertex{};
            vertex.position = {source.position[0], source.position[1], source.position[2]};
            vertex.normal = {source.normal[0], source.normal[1], source.normal[2]};
            vertex.texCoord0 = {source.texCoord[0], source.texCoord[1]};
            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
            meshData.vertices.push_back(vertex);
        }
        meshData.indices = std::move(imported.indices);
        for (const auto& primitive : imported.primitives)
        {
            meshData.submeshes.push_back(primitive.indexStart);
        }
        return true;
    }
} // namespace Spark::Graphics::Detail
