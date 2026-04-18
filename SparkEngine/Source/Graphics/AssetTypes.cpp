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
 * Methods that are pure C++ with no platform-specific APIs live here
 * instead so they compile exactly once per translation unit and don't
 * need to be duplicated across the platform files.
 */

#include "AssetPipeline.h"

#include "RHI/RHIResources.h"

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
MeshAsset::MeshAsset(const std::string& path) : Asset(path, AssetType::Mesh) {}

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
