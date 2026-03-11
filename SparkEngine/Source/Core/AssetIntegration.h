/**
 * @file AssetIntegration.h
 * @brief Cross-links AssetHandle with the engine's resource systems
 *
 * Provides an AssetRegistry that maps AssetHandles to loaded resources,
 * enabling O(1) asset lookups by hash instead of string comparisons.
 *
 * ## Migration from string-based assets
 * ```
 *   // Old: string-based lookup (O(N) hash + string compare)
 *   auto* mesh = assetPipeline.GetMesh("models/character.obj");
 *
 *   // New: handle-based lookup (O(1) hash lookup)
 *   auto handle = AssetHandle::FromPath("models/character.obj");
 *   auto* mesh = registry.Get<Mesh>(handle);
 * ```
 *
 * @see AssetHandle.h
 */

#pragma once

#include "AssetHandle.h"

#include <any>
#include <memory>
#include <string>
#include <sstream>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace Spark
{

    /**
     * @brief Metadata about a registered asset.
     */
    struct AssetMetadata
    {
        AssetHandle handle;
        std::string path;       ///< Original file path
        std::string typeName;   ///< Human-readable type name
        std::type_index typeId; ///< C++ type for safe casting
        size_t sizeBytes = 0;   ///< Estimated memory usage
        bool loaded = false;    ///< Whether the asset data is currently loaded

        AssetMetadata() : typeId(typeid(void)) {}
    };

    /**
     * @brief Central registry mapping AssetHandles to loaded resources.
     *
     * Provides O(1) asset lookup by handle, type-safe resource retrieval,
     * and memory tracking. Thread-safe for concurrent read access.
     *
     * @code
     *   AssetRegistry registry;
     *
     *   // Register and store an asset
     *   auto handle = AssetHandle::FromPath("textures/brick.dds");
     *   registry.Register<Texture>(handle, "textures/brick.dds", texturePtr, textureSize);
     *
     *   // Retrieve by handle
     *   Texture* tex = registry.Get<Texture>(handle);
     *
     *   // Retrieve by path (convenience)
     *   Texture* tex2 = registry.GetByPath<Texture>("textures/brick.dds");
     * @endcode
     */
    class AssetRegistry
    {
      public:
        AssetRegistry() = default;
        ~AssetRegistry() = default;

        // Non-copyable
        AssetRegistry(const AssetRegistry&) = delete;
        AssetRegistry& operator=(const AssetRegistry&) = delete;

        /**
         * @brief Register a loaded asset with the registry.
         *
         * @tparam T       Asset type (Mesh, Texture, Material, etc.)
         * @param handle   Asset handle (from AssetHandle::FromPath).
         * @param path     Original file path.
         * @param resource Non-owning pointer to the loaded resource.
         * @param sizeBytes Estimated memory usage in bytes.
         */
        template <typename T>
        void Register(AssetHandle handle, const std::string& path, T* resource, size_t sizeBytes = 0)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            AssetEntry entry;
            entry.metadata.handle = handle;
            entry.metadata.path = path;
            entry.metadata.typeName = typeid(T).name();
            entry.metadata.typeId = std::type_index(typeid(T));
            entry.metadata.sizeBytes = sizeBytes;
            entry.metadata.loaded = (resource != nullptr);
            entry.resource = static_cast<void*>(resource);

            m_assets[handle] = std::move(entry);
            m_pathToHandle[path] = handle;
        }

        /**
         * @brief Retrieve a loaded asset by handle.
         *
         * @tparam T  Expected asset type.
         * @param handle  Asset handle to look up.
         * @return Pointer to the asset, or nullptr if not found or type mismatch.
         */
        template <typename T> T* Get(AssetHandle handle) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_assets.find(handle);
            if (it == m_assets.end())
                return nullptr;

            if (it->second.metadata.typeId != std::type_index(typeid(T)))
                return nullptr;

            return static_cast<T*>(it->second.resource);
        }

        /**
         * @brief Retrieve a loaded asset by file path (convenience).
         */
        template <typename T> T* GetByPath(const std::string& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto handleIt = m_pathToHandle.find(path);
            if (handleIt == m_pathToHandle.end())
                return nullptr;

            // Unlock and re-lock via Get (avoid deadlock)
            AssetHandle handle = handleIt->second;

            auto it = m_assets.find(handle);
            if (it == m_assets.end())
                return nullptr;

            if (it->second.metadata.typeId != std::type_index(typeid(T)))
                return nullptr;

            return static_cast<T*>(it->second.resource);
        }

        /**
         * @brief Check if an asset is registered.
         */
        bool Contains(AssetHandle handle) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_assets.find(handle) != m_assets.end();
        }

        /**
         * @brief Remove an asset from the registry.
         */
        void Unregister(AssetHandle handle)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_assets.find(handle);
            if (it != m_assets.end())
            {
                m_pathToHandle.erase(it->second.metadata.path);
                m_assets.erase(it);
            }
        }

        /**
         * @brief Get the number of registered assets.
         */
        size_t GetAssetCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_assets.size();
        }

        /**
         * @brief Get the total estimated memory usage of all registered assets.
         */
        size_t GetTotalMemoryBytes() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t total = 0;
            for (const auto& [handle, entry] : m_assets)
            {
                total += entry.metadata.sizeBytes;
            }
            return total;
        }

        /**
         * @brief Get metadata for a specific asset.
         */
        const AssetMetadata* GetMetadata(AssetHandle handle) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_assets.find(handle);
            return it != m_assets.end() ? &it->second.metadata : nullptr;
        }

        /**
         * @brief Console integration: list all registered assets.
         */
        std::string Console_ListAssets() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::stringstream ss;
            ss << "=== Asset Registry ===\n";
            ss << "Total: " << m_assets.size() << " assets, " << (GetTotalMemoryBytesUnsafe() / 1024) << " KB\n\n";

            for (const auto& [handle, entry] : m_assets)
            {
                ss << "  [" << handle.hash << "] " << entry.metadata.path << "\n"
                   << "    Type: " << entry.metadata.typeName << " | Size: " << entry.metadata.sizeBytes << " bytes"
                   << " | Loaded: " << (entry.metadata.loaded ? "YES" : "NO") << "\n";
            }

            if (m_assets.empty())
            {
                ss << "  (no assets registered)\n";
            }
            return ss.str();
        }

      private:
        struct AssetEntry
        {
            AssetMetadata metadata;
            void* resource = nullptr;
        };

        size_t GetTotalMemoryBytesUnsafe() const
        {
            size_t total = 0;
            for (const auto& [handle, entry] : m_assets)
            {
                total += entry.metadata.sizeBytes;
            }
            return total;
        }

        mutable std::mutex m_mutex;
        std::unordered_map<AssetHandle, AssetEntry> m_assets;
        std::unordered_map<std::string, AssetHandle> m_pathToHandle;
    };

} // namespace Spark
