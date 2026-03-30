#pragma once

/**
 * @file ResourceVersionTracker.h
 * @brief Tracks version numbers for loaded resources to detect staleness
 *
 * Each resource is identified by its path string and assigned a monotonically
 * increasing version number starting at 1.  When a resource is hot-reloaded,
 * its version is incremented so that systems caching the old version can
 * detect the change via IsStale().
 */

#include "Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Spark
{

    /// @brief Singleton that tracks per-resource version numbers for hot-reload
    class ResourceVersionTracker
    {
      public:
        /// @brief Get the singleton instance
        static ResourceVersionTracker& GetInstance();

        /// @brief Register a resource and return its initial version (1)
        /// @param path Resource path (used as unique key)
        /// @return Initial version number (1), or current version if already registered
        uint32_t RegisterResource(const std::string& path);

        /// @brief Get the current version of a resource
        /// @param path Resource path
        /// @return Current version, or 0 if the resource is not tracked
        uint32_t GetVersion(const std::string& path) const;

        /// @brief Increment a resource's version (call on hot-reload)
        /// @param path Resource path
        /// @return New version number, or 0 if the resource is not tracked
        uint32_t IncrementVersion(const std::string& path);

        /// @brief Check whether a cached version is out of date
        /// @param path Resource path
        /// @param cachedVersion The version the caller last loaded
        /// @return true if the resource has been updated since cachedVersion
        bool IsStale(const std::string& path, uint32_t cachedVersion) const;

        /// @brief Stop tracking a resource
        void UnregisterResource(const std::string& path);

        /// @brief Number of currently tracked resources
        uint32_t GetTrackedResourceCount() const;

        /// @brief Console command: return a human-readable status string
        std::string Console_GetStatus() const;

        /// @brief Clear all tracked resources
        void Shutdown();

      private:
        ResourceVersionTracker() = default;
        ~ResourceVersionTracker() = default;

        ResourceVersionTracker(const ResourceVersionTracker&) = delete;
        ResourceVersionTracker& operator=(const ResourceVersionTracker&) = delete;

        std::unordered_map<std::string, uint32_t> m_versions;
    };

} // namespace Spark
