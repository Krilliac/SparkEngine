/**
 * @file FileWatcher.h
 * @brief File and directory change notification system
 * @author Spark Engine Team
 * @date 2026
 *
 * Monitors files and directories for changes using timestamp polling.
 * Notifies registered callbacks when files are created, modified, or deleted.
 * Used for asset hot-reload, script hot-reload, and DataTable hot-reload.
 *
 * ## Usage
 * @code
 *   auto& watcher = Spark::Utils::FileWatcher::GetInstance();
 *   watcher.Initialize(1.0f);  // poll every 1 second
 *
 *   watcher.Watch("Assets/Scripts/", [](const FileChangeEvent& e) {
 *       if (e.type == FileChangeType::Modified)
 *           ReloadScript(e.path);
 *   });
 *
 *   watcher.WatchFile("config.ini", [](const FileChangeEvent& e) {
 *       ReloadConfig();
 *   });
 *
 *   // Each frame:
 *   watcher.Update(deltaTime);
 * @endcode
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../LogMacros.h"

namespace Spark::Utils
{

    /** @brief Type of file change detected */
    enum class FileChangeType
    {
        Created,  ///< New file appeared
        Modified, ///< Existing file was modified
        Deleted   ///< File was removed
    };

    /** @brief Event data for a file change notification */
    struct FileChangeEvent
    {
        std::string path;                               ///< Full path to the changed file
        std::string relativePath;                       ///< Path relative to watched directory
        FileChangeType type = FileChangeType::Modified; ///< Type of change
        uint64_t newSize = 0;                           ///< New file size (0 if deleted)
    };

    /** @brief Callback for file change events */
    using FileChangeCallback = std::function<void(const FileChangeEvent&)>;

    /** @brief Internal: tracked file state */
    struct TrackedFile
    {
        std::string path;
        std::filesystem::file_time_type lastWriteTime{};
        uint64_t size = 0;
        bool exists = false;
    };

    /** @brief A watched path entry */
    struct WatchEntry
    {
        uint32_t id = 0;
        std::string path; ///< Directory or file path
        bool isDirectory = false;
        bool recursive = true;
        std::string extensionFilter; ///< Only watch files with this extension (empty = all)
        FileChangeCallback callback;
        std::unordered_map<std::string, TrackedFile> files;
    };

    /**
     * @brief File and directory change notification system
     *
     * Polls file timestamps at a configurable interval and fires callbacks
     * when changes are detected. Supports watching individual files or
     * entire directory trees with optional extension filtering.
     *
     * Thread safety: Not thread-safe. Call Update() from main thread only.
     */
    class FileWatcher
    {
      public:
        static FileWatcher& GetInstance()
        {
            static FileWatcher instance;
            return instance;
        }

        /**
         * @brief Initialize the file watcher
         * @param pollIntervalSeconds How often to scan for changes
         */
        void Initialize(float pollIntervalSeconds = 1.0f)
        {
            m_watches.clear();
            m_nextWatchId = 1;
            m_pollInterval = pollIntervalSeconds;
            m_timeSinceLastPoll = 0.0f;
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "FileWatcher initialized (poll interval: %.1fs)",
                           pollIntervalSeconds);
        }

        /** @brief Shut down and remove all watches */
        void Shutdown()
        {
            m_watches.clear();
            m_initialized = false;
        }

        /**
         * @brief Watch a directory for changes
         * @param directoryPath Path to watch
         * @param callback Called when a file changes
         * @param extensionFilter Only watch files with this extension (e.g. ".csv"), empty for all
         * @param recursive Watch subdirectories
         * @return Watch ID for unregistering
         */
        uint32_t Watch(const std::string& directoryPath, FileChangeCallback callback,
                       const std::string& extensionFilter = "", bool recursive = true)
        {
            if (!m_initialized || directoryPath.empty())
                return 0;

            WatchEntry entry;
            entry.id = m_nextWatchId++;
            entry.path = directoryPath;
            entry.isDirectory = true;
            entry.recursive = recursive;
            entry.extensionFilter = extensionFilter;
            entry.callback = std::move(callback);

            // Initial scan
            ScanDirectory(entry);

            SPARK_LOG_INFO(Spark::LogCategory::Core, "FileWatcher: watching directory '%s'", directoryPath.c_str());
            const uint32_t watchId = entry.id;
            m_watches[watchId] = std::move(entry);
            return watchId;
        }

        /**
         * @brief Watch a single file for changes
         * @param filePath Path to the file
         * @param callback Called when the file changes
         * @return Watch ID for unregistering
         */
        uint32_t WatchFile(const std::string& filePath, FileChangeCallback callback)
        {
            if (!m_initialized || filePath.empty())
                return 0;

            WatchEntry entry;
            entry.id = m_nextWatchId++;
            entry.path = filePath;
            entry.isDirectory = false;
            entry.callback = std::move(callback);

            // Record initial state
            std::error_code ec;
            if (std::filesystem::exists(filePath, ec))
            {
                TrackedFile tf;
                tf.path = filePath;
                tf.lastWriteTime = std::filesystem::last_write_time(filePath, ec);
                tf.size = std::filesystem::file_size(filePath, ec);
                tf.exists = true;
                entry.files[filePath] = tf;
            }

            SPARK_LOG_INFO(Spark::LogCategory::Core, "FileWatcher: watching file '%s'", filePath.c_str());
            const uint32_t watchId = entry.id;
            m_watches[watchId] = std::move(entry);
            return watchId;
        }

        /** @brief Stop watching a path */
        void Unwatch(uint32_t watchId) { m_watches.erase(watchId); }

        /**
         * @brief Poll for changes — call each frame
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime)
        {
            if (!m_initialized)
                return;

            m_timeSinceLastPoll += deltaTime;
            if (m_timeSinceLastPoll < m_pollInterval)
                return;
            m_timeSinceLastPoll = 0.0f;

            for (auto& [id, entry] : m_watches)
            {
                if (entry.isDirectory)
                    PollDirectory(entry);
                else
                    PollFile(entry);
            }
        }

        /** @brief Get the number of active watches */
        size_t GetWatchCount() const { return m_watches.size(); }

        /** @brief Get total number of tracked files across all watches */
        size_t GetTrackedFileCount() const
        {
            size_t total = 0;
            for (const auto& [id, entry] : m_watches)
                total += entry.files.size();
            return total;
        }

        /** @brief Set the poll interval */
        void SetPollInterval(float seconds) { m_pollInterval = std::max(0.1f, seconds); }

        /** @brief Get console-friendly status */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[FileWatcher] Not initialized";
            return "[FileWatcher] Watches: " + std::to_string(m_watches.size()) +
                   " | Tracked files: " + std::to_string(GetTrackedFileCount()) +
                   " | Poll interval: " + std::to_string(m_pollInterval) + "s";
        }

      private:
        FileWatcher() = default;

        void ScanDirectory(WatchEntry& entry)
        {
            std::error_code ec;
            if (!std::filesystem::exists(entry.path, ec))
                return;

            auto iterate = [&](const std::filesystem::directory_entry& dirEntry)
            {
                if (!dirEntry.is_regular_file())
                    return;

                std::string filePath = dirEntry.path().string();
                if (!entry.extensionFilter.empty() && dirEntry.path().extension() != entry.extensionFilter)
                    return;

                TrackedFile tf;
                tf.path = filePath;
                tf.lastWriteTime = std::filesystem::last_write_time(dirEntry.path(), ec);
                tf.size = std::filesystem::file_size(dirEntry.path(), ec);
                tf.exists = true;
                entry.files[filePath] = tf;
            };

            if (entry.recursive)
            {
                for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(entry.path, ec))
                    iterate(dirEntry);
            }
            else
            {
                for (const auto& dirEntry : std::filesystem::directory_iterator(entry.path, ec))
                    iterate(dirEntry);
            }
        }

        void PollDirectory(WatchEntry& entry)
        {
            std::error_code ec;
            if (!std::filesystem::exists(entry.path, ec))
                return;

            // Build current state
            std::unordered_map<std::string, TrackedFile> currentFiles;
            auto iterate = [&](const std::filesystem::directory_entry& dirEntry)
            {
                if (!dirEntry.is_regular_file())
                    return;

                std::string filePath = dirEntry.path().string();
                if (!entry.extensionFilter.empty() && dirEntry.path().extension() != entry.extensionFilter)
                    return;

                TrackedFile tf;
                tf.path = filePath;
                tf.lastWriteTime = std::filesystem::last_write_time(dirEntry.path(), ec);
                tf.size = std::filesystem::file_size(dirEntry.path(), ec);
                tf.exists = true;
                currentFiles[filePath] = tf;
            };

            if (entry.recursive)
            {
                for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(
                         entry.path, std::filesystem::directory_options::skip_permission_denied, ec))
                {
                    if (ec)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Core, "FileWatcher: error iterating '%s': %s",
                                       entry.path.c_str(), ec.message().c_str());
                        ec.clear();
                        continue;
                    }
                    iterate(dirEntry);
                }
            }
            else
            {
                for (const auto& dirEntry : std::filesystem::directory_iterator(
                         entry.path, std::filesystem::directory_options::skip_permission_denied, ec))
                {
                    if (ec)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Core, "FileWatcher: error iterating '%s': %s",
                                       entry.path.c_str(), ec.message().c_str());
                        ec.clear();
                        continue;
                    }
                    iterate(dirEntry);
                }
            }

            // Check for new and modified files
            for (const auto& [path, currentTf] : currentFiles)
            {
                auto prevIt = entry.files.find(path);
                if (prevIt == entry.files.end())
                {
                    NotifyChange(entry, path, FileChangeType::Created, currentTf.size);
                }
                else if (currentTf.lastWriteTime != prevIt->second.lastWriteTime)
                {
                    NotifyChange(entry, path, FileChangeType::Modified, currentTf.size);
                }
            }

            // Check for deleted files
            for (const auto& [path, prevTf] : entry.files)
            {
                if (!currentFiles.count(path))
                    NotifyChange(entry, path, FileChangeType::Deleted, 0);
            }

            entry.files = std::move(currentFiles);
        }

        void PollFile(WatchEntry& entry)
        {
            std::error_code ec;
            bool exists = std::filesystem::exists(entry.path, ec);

            auto it = entry.files.find(entry.path);
            bool wasTracked = (it != entry.files.end() && it->second.exists);

            if (exists && !wasTracked)
            {
                TrackedFile tf;
                tf.path = entry.path;
                tf.lastWriteTime = std::filesystem::last_write_time(entry.path, ec);
                tf.size = std::filesystem::file_size(entry.path, ec);
                tf.exists = true;
                entry.files[entry.path] = tf;
                NotifyChange(entry, entry.path, FileChangeType::Created, tf.size);
            }
            else if (exists && wasTracked)
            {
                auto writeTime = std::filesystem::last_write_time(entry.path, ec);
                if (writeTime != it->second.lastWriteTime)
                {
                    it->second.lastWriteTime = writeTime;
                    it->second.size = std::filesystem::file_size(entry.path, ec);
                    NotifyChange(entry, entry.path, FileChangeType::Modified, it->second.size);
                }
            }
            else if (!exists && wasTracked)
            {
                it->second.exists = false;
                NotifyChange(entry, entry.path, FileChangeType::Deleted, 0);
            }
        }

        static const char* ChangeTypeToString(FileChangeType type)
        {
            switch (type)
            {
            case FileChangeType::Created:
                return "Created";
            case FileChangeType::Modified:
                return "Modified";
            case FileChangeType::Deleted:
                return "Deleted";
            }
            return "Unknown";
        }

        void NotifyChange(const WatchEntry& entry, const std::string& filePath, FileChangeType type, uint64_t newSize)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "FileWatcher: %s '%s'", ChangeTypeToString(type),
                           filePath.c_str());

            if (!entry.callback)
                return;

            FileChangeEvent event;
            event.path = filePath;
            event.type = type;
            event.newSize = newSize;

            // Compute relative path
            if (entry.isDirectory && filePath.size() > entry.path.size())
            {
                event.relativePath = filePath.substr(entry.path.size());
                if (!event.relativePath.empty() && (event.relativePath[0] == '/' || event.relativePath[0] == '\\'))
                    event.relativePath = event.relativePath.substr(1);
            }
            else
            {
                event.relativePath = std::filesystem::path(filePath).filename().string();
            }

            entry.callback(event);
        }

        bool m_initialized = false;
        float m_pollInterval = 1.0f;
        float m_timeSinceLastPoll = 0.0f;
        uint32_t m_nextWatchId = 1;
        std::unordered_map<uint32_t, WatchEntry> m_watches;
    };

} // namespace Spark::Utils
