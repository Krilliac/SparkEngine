/**
 * @file AssetHandle.h
 * @brief Lightweight hashed asset identifier for zero-allocation asset references
 *
 * Replaces string-based asset paths in per-frame structures like MeshDrawCommand
 * to eliminate heap allocations during rendering (R3.4 — Architecture Analysis).
 *
 * ## Usage
 * @code
 *   AssetHandle mesh = AssetHandle::FromPath("Assets/Meshes/Soldier.fbx");
 *   AssetHandle mat  = AssetHandle::FromPath("Assets/Materials/Metal.mat");
 *
 *   // Compare handles (fast integer comparison)
 *   if (mesh == otherMesh) { ... }
 *
 *   // Use in hash maps
 *   std::unordered_map<AssetHandle, MeshData*> cache;
 * @endcode
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/**
 * @brief Lightweight hashed asset reference — 8 bytes, no heap allocation.
 *
 * AssetHandle stores a 64-bit FNV-1a hash of the asset path string. This
 * allows per-frame draw commands to reference assets without allocating
 * strings on every frame. The AssetPipeline resolves handles back to
 * loaded resources via a hash-keyed lookup table.
 *
 * @note Hash collisions are theoretically possible but statistically
 *       negligible for FNV-1a over typical asset path lengths.
 */
struct AssetHandle
{
    uint64_t hash = 0;

    constexpr AssetHandle() = default;
    constexpr explicit AssetHandle(uint64_t h) : hash(h) {}

    /**
     * @brief Create a handle from an asset path string.
     * @param path  Asset path (e.g. "Assets/Meshes/Soldier.fbx").
     * @return      AssetHandle with FNV-1a hash of the path.
     */
    static AssetHandle FromPath(std::string_view path) { return AssetHandle(FNV1a(path)); }

    /**
     * @brief Create a handle from an std::string asset path.
     * @param path  Asset path string.
     * @return      AssetHandle with FNV-1a hash of the path.
     */
    static AssetHandle FromString(const std::string& path) { return AssetHandle(FNV1a(std::string_view(path))); }

    constexpr bool IsValid() const { return hash != 0; }
    constexpr bool operator==(const AssetHandle& other) const { return hash == other.hash; }
    constexpr bool operator!=(const AssetHandle& other) const { return hash != other.hash; }
    constexpr bool operator<(const AssetHandle& other) const { return hash < other.hash; }

  private:
    static constexpr uint64_t FNV1a(std::string_view str)
    {
        constexpr uint64_t kFNVOffsetBasis = 14695981039346656037ULL;
        constexpr uint64_t kFNVPrime = 1099511628211ULL;
        uint64_t h = kFNVOffsetBasis;
        for (char c : str)
        {
            h ^= static_cast<uint64_t>(c);
            h *= kFNVPrime;
        }
        return h;
    }
};

namespace std
{
    template <> struct hash<AssetHandle>
    {
        size_t operator()(const AssetHandle& handle) const noexcept { return static_cast<size_t>(handle.hash); }
    };
} // namespace std
