/**
 * @file ScriptHotReload.cpp
 * @brief Implementation of script hot-reload and file watching
 */

#include "ScriptHotReload.h"
#include "../../Utils/Assert.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <sstream>

namespace Spark::Scripting
{

    // ============================================================================
    // Construction / Destruction
    // ============================================================================

    ScriptHotReloadManager::ScriptHotReloadManager() = default;

    ScriptHotReloadManager::~ScriptHotReloadManager()
    {
        Stop();
    }

    // ============================================================================
    // Configuration
    // ============================================================================

    void ScriptHotReloadManager::AddWatchDirectory(const std::string& directory, bool recursive)
    {
        ASSERT_MSG(!directory.empty(), "ScriptHotReloadManager::AddWatchDirectory — directory must not be empty");
        SPARK_VALIDATE_NOT_EMPTY(LogCategory::Scripting, directory);
        m_watchDirs.push_back(directory);
        if (m_running)
        {
            ScanDirectory(directory, recursive);
        }
    }

    void ScriptHotReloadManager::SetWatchExtensions(const std::vector<std::string>& extensions)
    {
        m_extensions = extensions;
    }

    void ScriptHotReloadManager::SetRecompileCallback(RecompileCallback callback)
    {
        ASSERT_MSG(callback != nullptr, "ScriptHotReloadManager::SetRecompileCallback — callback must not be null");
        m_recompileCallback = std::move(callback);
    }

    void ScriptHotReloadManager::SetErrorCallback(ErrorCallback callback)
    {
        ASSERT_MSG(callback != nullptr, "ScriptHotReloadManager::SetErrorCallback — callback must not be null");
        m_errorCallback = std::move(callback);
    }

    void ScriptHotReloadManager::SetDebounceMs(uint32_t ms)
    {
        m_debounceMs = ms;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void ScriptHotReloadManager::Start()
    {
        SPARK_TRACE_ENTER(LogCategory::Scripting);

        if (m_running)
            return;

        m_running = true;
        m_fileStates.clear();

        // Initial scan of all watch directories
        for (const auto& dir : m_watchDirs)
        {
            ScanDirectory(dir, true);
        }
    }

    void ScriptHotReloadManager::Stop()
    {
        SPARK_TRACE_ENTER(LogCategory::Scripting);

        m_running = false;
        m_pendingChanges.clear();
    }

    // ============================================================================
    // Polling
    // ============================================================================

    int ScriptHotReloadManager::PollChanges()
    {
        SPARK_TRACE_ENTER(LogCategory::Scripting);

        if (!m_running)
            return 0;

        auto now = std::chrono::steady_clock::now();

        // Check all tracked files for modifications
        for (auto& [path, state] : m_fileStates)
        {
            try
            {
                if (!std::filesystem::exists(path))
                    continue;

                auto currentTime = std::filesystem::last_write_time(path);
                auto currentSize = std::filesystem::file_size(path);

                if (currentTime != state.lastModified || currentSize != state.lastSize)
                {
                    state.lastModified = currentTime;
                    state.lastSize = currentSize;

                    // Check if already pending
                    bool alreadyPending = false;
                    for (auto& pending : m_pendingChanges)
                    {
                        if (pending.filePath == path)
                        {
                            pending.detectedAt = now; // Reset debounce timer
                            alreadyPending = true;
                            break;
                        }
                    }

                    if (!alreadyPending)
                    {
                        m_pendingChanges.push_back({path, now});
                    }
                }
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // File may be locked by the editor — skip this poll cycle
            }
        }

        // Check for new files in watch directories
        for (const auto& dir : m_watchDirs)
        {
            ScanDirectory(dir, true);
        }

        // Process debounced changes
        int recompiled = 0;
        auto debounce = std::chrono::milliseconds(m_debounceMs);

        auto it = m_pendingChanges.begin();
        while (it != m_pendingChanges.end())
        {
            if (now - it->detectedAt >= debounce)
            {
                ProcessChange(it->filePath);
                recompiled++;
                it = m_pendingChanges.erase(it);
            }
            else
            {
                ++it;
            }
        }

        return recompiled;
    }

    int ScriptHotReloadManager::RecompileAll()
    {
        int count = 0;
        for (const auto& [path, state] : m_fileStates)
        {
            ProcessChange(path);
            count++;
        }
        return count;
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void ScriptHotReloadManager::ScanDirectory(const std::string& directory, bool recursive)
    {
        try
        {
            if (!std::filesystem::exists(directory))
                return;

            auto scanPath = [this](const std::filesystem::directory_entry& entry)
            {
                if (!entry.is_regular_file())
                    return;

                std::string path = entry.path().string();
                if (!IsWatchedExtension(path))
                    return;

                if (m_fileStates.find(path) == m_fileStates.end())
                {
                    FileState state;
                    state.path = path;
                    state.lastModified = entry.last_write_time();
                    state.lastSize = entry.file_size();
                    m_fileStates[path] = state;
                }
            };

            if (recursive)
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
                {
                    scanPath(entry);
                }
            }
            else
            {
                for (const auto& entry : std::filesystem::directory_iterator(directory))
                {
                    scanPath(entry);
                }
            }
        }
        catch (const std::filesystem::filesystem_error&)
        {
            // Directory may not exist or be inaccessible
        }
    }

    bool ScriptHotReloadManager::IsWatchedExtension(const std::string& path) const
    {
        for (const auto& ext : m_extensions)
        {
            if (path.size() >= ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void ScriptHotReloadManager::ProcessChange(const std::string& filePath)
    {
        if (!m_recompileCallback)
            return;

        RecompileResult result = m_recompileCallback(filePath);
        m_recompileCount++;

        if (!result.success)
        {
            m_errorCount++;
            m_recentErrors.push_back(result);
            if (static_cast<int>(m_recentErrors.size()) > MaxRecentErrors)
            {
                // O(n) erase on small bounded buffer (MaxRecentErrors=10) — acceptable cost
                m_recentErrors.erase(m_recentErrors.begin());
            }

            if (m_errorCallback)
            {
                m_errorCallback(result);
            }
        }
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string ScriptHotReloadManager::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "ScriptHotReload: " << (m_running ? "Running" : "Stopped") << " | Watching: " << m_fileStates.size()
            << " files" << " | Recompiles: " << m_recompileCount << " | Errors: " << m_errorCount
            << " | Pending: " << m_pendingChanges.size();

        if (!m_recentErrors.empty())
        {
            const auto& lastErr = m_recentErrors.back();
            oss << " | Last error: " << lastErr.filePath << ":" << lastErr.errorLine << " — " << lastErr.errorMessage;
        }

        return oss.str();
    }

} // namespace Spark::Scripting
