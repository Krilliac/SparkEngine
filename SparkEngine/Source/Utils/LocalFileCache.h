/**
 * @file LocalFileCache.h
 * @brief In-memory file cache with LRU eviction for local file I/O
 * @author Spark Engine Team
 * @date 2026
 *
 * Caches file contents in memory to avoid redundant disk reads.
 * Supports read, write, create, and delete operations with configurable
 * size limits and LRU eviction.
 *
 * Thread-safe: all public methods are guarded by a mutex.
 *
 * ## Usage
 * @code
 *   Spark::LocalFileCache cache(32 * 1024 * 1024); // 32 MB limit
 *
 *   // Read (caches on first access)
 *   auto text = cache.ReadText("config.ini");
 *   auto bin  = cache.ReadBinary("model.fbx");
 *
 *   // Write (updates disk + cache)
 *   cache.WriteText("output.txt", "Hello");
 *
 *   // Create (fails if file exists)
 *   cache.Create("new_file.txt", "initial content");
 *
 *   // Delete (removes from disk + cache)
 *   cache.Delete("output.txt");
 *
 *   // Register with EngineContext
 *   context->RegisterSystem<Spark::LocalFileCache>(&cache);
 * @endcode
 */

#pragma once

#include "ContainerUtils.h"
#include "FileUtils.h"
#include "Result.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Cache performance metrics
     */
    struct CacheMetrics
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        uint64_t totalBytesServed = 0;
    };

    /**
     * @brief In-memory file cache with LRU eviction
     *
     * Caches file contents keyed by normalized path. When the cache exceeds
     * its size limit, least-recently-used entries are evicted.
     */
    class LocalFileCache
    {
      public:
        /// Default max cache size: 64 MB
        static constexpr size_t DEFAULT_MAX_SIZE = size_t{64} * 1024 * 1024;

        explicit LocalFileCache(size_t maxSizeBytes = DEFAULT_MAX_SIZE) : m_maxSize(maxSizeBytes) {}

        ~LocalFileCache() = default;

        // Non-copyable, non-movable (mutex member)
        LocalFileCache(const LocalFileCache&) = delete;
        LocalFileCache& operator=(const LocalFileCache&) = delete;
        LocalFileCache(LocalFileCache&&) = delete;
        LocalFileCache& operator=(LocalFileCache&&) = delete;

        // =====================================================================
        // Read operations
        // =====================================================================

        /**
         * @brief Read a text file, returning cached content on subsequent calls
         * @param path File path (will be normalized for cache key)
         * @return File contents as string, or error if file not found
         */
        Result<std::string> ReadText(const std::string& path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::string key = NormalizeKey(path);

            auto it = m_entries.find(key);
            if (it != m_entries.end())
            {
                TouchEntry(key);
                m_metrics.hits++;
                const auto& data = it->second.data;
                m_metrics.totalBytesServed += data.size();
                return Ok(std::string(data.begin(), data.end()));
            }

            m_metrics.misses++;
            auto content = FileUtils::ReadTextFile(path);
            if (!content.has_value())
            {
                return Err("Failed to read file: " + path);
            }

            std::vector<uint8_t> bytes(content->begin(), content->end());
            m_metrics.totalBytesServed += bytes.size();
            std::string result = *content;
            InsertOrUpdate(key, std::move(bytes));
            return Ok(std::move(result));
        }

        /**
         * @brief Read a binary file, returning cached content on subsequent calls
         * @param path File path (will be normalized for cache key)
         * @return File contents as byte vector, or error if file not found
         */
        Result<std::vector<uint8_t>> ReadBinary(const std::string& path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::string key = NormalizeKey(path);

            auto it = m_entries.find(key);
            if (it != m_entries.end())
            {
                TouchEntry(key);
                m_metrics.hits++;
                m_metrics.totalBytesServed += it->second.data.size();
                return Ok(it->second.data);
            }

            m_metrics.misses++;
            auto content = FileUtils::ReadBinaryFile(path);
            if (!content.has_value())
            {
                return Err("Failed to read file: " + path);
            }

            m_metrics.totalBytesServed += content->size();
            std::vector<uint8_t> result = *content;
            InsertOrUpdate(key, std::move(*content));
            return Ok(std::move(result));
        }

        // =====================================================================
        // Write operations
        // =====================================================================

        /**
         * @brief Write text content to disk and update the cache
         * @param path File path
         * @param content Text content to write
         * @return Success or error
         */
        Result<void> WriteText(const std::string& path, const std::string& content)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!FileUtils::WriteTextFile(path, content))
            {
                return Err("Failed to write file: " + path);
            }

            const std::string key = NormalizeKey(path);
            std::vector<uint8_t> bytes(content.begin(), content.end());
            InsertOrUpdate(key, std::move(bytes));
            return Ok();
        }

        /**
         * @brief Write binary data to disk and update the cache
         * @param path File path
         * @param data Binary data to write
         * @return Success or error
         */
        Result<void> WriteBinary(const std::string& path, const std::vector<uint8_t>& data)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!FileUtils::WriteBinaryFile(path, data))
            {
                return Err("Failed to write file: " + path);
            }

            const std::string key = NormalizeKey(path);
            std::vector<uint8_t> copy(data);
            InsertOrUpdate(key, std::move(copy));
            return Ok();
        }

        // =====================================================================
        // Create / Delete
        // =====================================================================

        /**
         * @brief Create a new file with optional initial content
         * @param path File path to create
         * @param content Initial text content (default empty)
         * @return Success or error (fails if file already exists)
         */
        Result<void> Create(const std::string& path, const std::string& content = "")
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (FileUtils::FileExists(path))
            {
                return Err("File already exists: " + path);
            }

            // Ensure parent directory exists
            const std::string dir = FileUtils::GetDirectory(path);
            if (!dir.empty())
            {
                FileUtils::CreateDirectories(dir);
            }

            if (!FileUtils::WriteTextFile(path, content))
            {
                return Err("Failed to create file: " + path);
            }

            const std::string key = NormalizeKey(path);
            std::vector<uint8_t> bytes(content.begin(), content.end());
            InsertOrUpdate(key, std::move(bytes));
            return Ok();
        }

        /**
         * @brief Delete a file from disk and evict it from cache
         * @param path File path to delete
         * @return Success or error
         */
        Result<void> Delete(const std::string& path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (!FileUtils::FileExists(path))
            {
                return Err("File not found: " + path);
            }

            std::error_code ec;
            fs::remove(path, ec);
            if (ec)
            {
                return Err("Failed to delete file: " + path + " (" + ec.message() + ")");
            }

            const std::string key = NormalizeKey(path);
            RemoveEntry(key);
            return Ok();
        }

        // =====================================================================
        // Cache management
        // =====================================================================

        /**
         * @brief Evict a single entry from the cache (file remains on disk)
         */
        void Invalidate(const std::string& path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::string key = NormalizeKey(path);
            RemoveEntry(key);
        }

        /**
         * @brief Evict all entries from the cache
         */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.clear();
            m_accessOrder.clear();
            m_accessIterators.clear();
            m_currentSize = 0;
        }

        /**
         * @brief Check if a path is currently cached
         */
        bool Contains(const std::string& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::string key = NormalizeKey(path);
            return Spark::ContainerUtils::Contains(m_entries, key);
        }

        /**
         * @brief Return a snapshot of cache metrics
         */
        CacheMetrics GetMetrics() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_metrics;
        }

        /**
         * @brief Set the maximum cache size in bytes; evicts if needed
         */
        void SetMaxSize(size_t bytes)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_maxSize = bytes;
            EvictLRU();
        }

        /**
         * @brief Current total bytes stored in cache
         */
        size_t GetCurrentSize() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_currentSize;
        }

        /**
         * @brief Number of entries currently in cache
         */
        size_t GetEntryCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_entries.size();
        }

      private:
        struct CacheEntry
        {
            std::vector<uint8_t> data;
            size_t size = 0;
        };

        /// Normalize a file path to a consistent cache key
        static std::string NormalizeKey(const std::string& path)
        {
#if SPARK_HAS_FILESYSTEM
            return FileUtils::NormalizePath(path);
#else
            return path;
#endif
        }

        /// Move an entry to the front (most recently used) of the LRU list
        void TouchEntry(const std::string& key)
        {
            auto it = m_accessIterators.find(key);
            if (it != m_accessIterators.end())
            {
                m_accessOrder.erase(it->second);
                m_accessOrder.push_front(key);
                it->second = m_accessOrder.begin();
            }
        }

        /// Insert or update a cache entry, evicting LRU entries if needed
        void InsertOrUpdate(const std::string& key, std::vector<uint8_t> data)
        {
            const size_t dataSize = data.size();

            // If entry already exists, remove its old size
            auto existing = m_entries.find(key);
            if (existing != m_entries.end())
            {
                m_currentSize -= existing->second.size;
                // Update data
                existing->second.data = std::move(data);
                existing->second.size = dataSize;
                m_currentSize += dataSize;
                TouchEntry(key);
            }
            else
            {
                // New entry
                CacheEntry entry;
                entry.data = std::move(data);
                entry.size = dataSize;

                m_entries[key] = std::move(entry);
                m_accessOrder.push_front(key);
                m_accessIterators[key] = m_accessOrder.begin();
                m_currentSize += dataSize;
            }

            EvictLRU();
        }


        /// Remove an entry from cache entirely
        void RemoveEntry(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it != m_entries.end())
            {
                m_currentSize -= it->second.size;
                m_entries.erase(it);

                auto accessIt = m_accessIterators.find(key);
                if (accessIt != m_accessIterators.end())
                {
                    m_accessOrder.erase(accessIt->second);
                    m_accessIterators.erase(accessIt);
                }
            }
        }

        /// Evict least-recently-used entries until cache is within size limit
        void EvictLRU()
        {
            while (m_currentSize > m_maxSize && !m_accessOrder.empty())
            {
                const std::string& lruKey = m_accessOrder.back();

                auto it = m_entries.find(lruKey);
                if (it != m_entries.end())
                {
                    m_currentSize -= it->second.size;
                    m_entries.erase(it);
                }

                m_accessIterators.erase(lruKey);
                m_accessOrder.pop_back();
                m_metrics.evictions++;
            }
        }

        // Cache storage
        std::unordered_map<std::string, CacheEntry> m_entries;
        std::list<std::string> m_accessOrder; // front = MRU, back = LRU
        std::unordered_map<std::string, std::list<std::string>::iterator> m_accessIterators;

        // Limits and tracking
        size_t m_currentSize = 0;
        size_t m_maxSize = DEFAULT_MAX_SIZE;
        CacheMetrics m_metrics;

        // Thread safety
        mutable std::mutex m_mutex;
    };

} // namespace Spark
