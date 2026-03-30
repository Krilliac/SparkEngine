/**
 * @file ModuleHotReload.cpp
 * @brief DLL/SO hot-reload file watcher implementation
 */

#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <sstream>
#include <thread>

namespace Spark
{

    ModuleHotReloadManager::~ModuleHotReloadManager()
    {
        Stop();
    }

    void ModuleHotReloadManager::Initialize(ModuleManager* moduleManager, IEngineContext* context)
    {
        if (!moduleManager || !context)
        {
            const char* missing = !moduleManager ? "moduleManager" : "context";
            SPARK_LOG_WARN(Spark::LogCategory::Core, "ModuleHotReloadManager::Initialize called with null %s", missing);
            SimpleConsole::GetInstance().LogWarning("ModuleHotReloadManager::Initialize called with null " +
                                                    std::string(missing));
        }
        m_moduleManager = moduleManager;
        m_context = context;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "ModuleHotReloadManager initialized");
        SimpleConsole::GetInstance().LogInfo("ModuleHotReloadManager initialized");
    }

    void ModuleHotReloadManager::WatchModule(const std::string& moduleName, const std::string& dllPath)
    {
        std::lock_guard lock(m_mutex);

        WatchedModule watched;
        watched.moduleName = moduleName;
        watched.dllPath = dllPath;
        SnapshotFile(watched);

        m_watchedModules[moduleName] = std::move(watched);

        auto& console = SimpleConsole::GetInstance();
        console.LogInfo("Hot-reload watching: " + moduleName + " (" + dllPath + ")");
    }

    void ModuleHotReloadManager::WatchAllLoadedModules()
    {
        if (!m_moduleManager)
            return;

        auto modules = m_moduleManager->GetModulePathsAndNames();
        for (const auto& [name, path] : modules)
        {
            WatchModule(name, path);
        }
    }

    void ModuleHotReloadManager::Start()
    {
        m_running = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Module hot-reload watcher started (debounce=%ums)", m_debounceMs);
        SimpleConsole::GetInstance().LogInfo("Module hot-reload started");
    }

    void ModuleHotReloadManager::Stop()
    {
        if (m_running)
        {
            m_running = false;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Module hot-reload watcher stopped (total reloads=%d)",
                           m_reloadCount);
            SimpleConsole::GetInstance().LogInfo("Module hot-reload stopped");
        }
    }

    int ModuleHotReloadManager::PollChanges()
    {
        if (!m_running || !m_enabled || !m_moduleManager)
            return 0;

        auto now = std::chrono::steady_clock::now();
        int reloadedCount = 0;

        std::lock_guard lock(m_mutex);

        // 1. Check each watched module for file changes
        for (auto& [name, watched] : m_watchedModules)
        {
            if (!HasFileChanged(watched))
                continue;

            // Avoid adding duplicate pending reloads
            bool alreadyPending = false;
            for (const auto& pending : m_pendingReloads)
            {
                if (pending.moduleName == name)
                {
                    alreadyPending = true;
                    break;
                }
            }

            if (!alreadyPending)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Hot-reload: Change detected in module '%s'", name.c_str());
                SimpleConsole::GetInstance().LogInfo("Change detected in module: " + name);
                m_pendingReloads.push_back({name, now});
            }
        }

        // 2. Process pending reloads that have passed the debounce threshold
        auto debounceThreshold = std::chrono::milliseconds(m_debounceMs);
        std::vector<PendingReload> remaining;

        for (const auto& pending : m_pendingReloads)
        {
            if (now - pending.detectedAt < debounceThreshold)
            {
                remaining.push_back(pending);
                continue;
            }

            // Debounce window passed — perform reload (with one retry if DLL is locked)
            auto& console = SimpleConsole::GetInstance();
            console.LogInfo("Reloading module: " + pending.moduleName);

            bool success = m_moduleManager->ReloadModule(pending.moduleName, m_context);

            if (!success)
            {
                // Retry once after a short delay — the file may still be locked by the compiler
                console.LogWarning("Module reload failed, retrying in 200ms: " + pending.moduleName);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                success = m_moduleManager->ReloadModule(pending.moduleName, m_context);
            }

            if (success)
            {
                console.LogSuccess("Module hot-reloaded: " + pending.moduleName);
                ++m_reloadCount;
                ++reloadedCount;
            }
            else
            {
                console.LogError("Module hot-reload failed after retry: " + pending.moduleName);
            }

            // Re-snapshot after reload attempt
            auto it = m_watchedModules.find(pending.moduleName);
            if (it != m_watchedModules.end())
            {
                SnapshotFile(it->second);
            }

            if (m_reloadCallback)
            {
                m_reloadCallback(pending.moduleName, success);
            }
        }

        m_pendingReloads = std::move(remaining);
        return reloadedCount;
    }

    bool ModuleHotReloadManager::ForceReload(const std::string& moduleName)
    {
        if (!m_moduleManager || !m_context)
            return false;

        auto& console = SimpleConsole::GetInstance();
        console.LogInfo("Force-reloading module: " + moduleName);

        bool success = m_moduleManager->ReloadModule(moduleName, m_context);

        if (success)
        {
            console.LogSuccess("Module force-reloaded: " + moduleName);
            ++m_reloadCount;
        }
        else
        {
            console.LogError("Module force-reload failed: " + moduleName);
        }

        // Re-snapshot
        std::lock_guard lock(m_mutex);
        auto it = m_watchedModules.find(moduleName);
        if (it != m_watchedModules.end())
        {
            SnapshotFile(it->second);
        }

        if (m_reloadCallback)
        {
            m_reloadCallback(moduleName, success);
        }

        return success;
    }

    void ModuleHotReloadManager::SetReloadCallback(ModuleReloadCallback callback)
    {
        m_reloadCallback = std::move(callback);
    }

    std::string ModuleHotReloadManager::GetStatus() const
    {
        std::lock_guard lock(m_mutex);
        std::stringstream ss;

        ss << "=== Module Hot-Reload Status ===\n"
           << "  Enabled:  " << (m_enabled ? "YES" : "NO") << "\n"
           << "  Running:  " << (m_running ? "YES" : "NO") << "\n"
           << "  Debounce: " << m_debounceMs << "ms\n"
           << "  Reloads:  " << m_reloadCount << "\n"
           << "  Watched modules (" << m_watchedModules.size() << "):\n";

        for (const auto& [name, watched] : m_watchedModules)
        {
            ss << "    " << name << " -> " << watched.dllPath << (watched.valid ? " [OK]" : " [NOT FOUND]") << "\n";
        }

        if (!m_pendingReloads.empty())
        {
            ss << "  Pending reloads: " << m_pendingReloads.size() << "\n";
        }

        return ss.str();
    }

    void ModuleHotReloadManager::SnapshotFile(WatchedModule& watched)
    {
        std::error_code ec;
        auto path = std::filesystem::path(watched.dllPath);

        if (!std::filesystem::exists(path, ec))
        {
            watched.valid = false;
            watched.lastSize = 0;
            return;
        }

        watched.lastModified = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            watched.valid = false;
            return;
        }

        watched.lastSize = static_cast<size_t>(std::filesystem::file_size(path, ec));
        if (ec)
        {
            watched.valid = false;
            return;
        }

        watched.valid = true;
    }

    bool ModuleHotReloadManager::HasFileChanged(const WatchedModule& watched) const
    {
        if (!watched.valid)
            return false;

        std::error_code ec;
        auto path = std::filesystem::path(watched.dllPath);

        if (!std::filesystem::exists(path, ec))
            return false;

        auto currentTime = std::filesystem::last_write_time(path, ec);
        if (ec)
            return false;

        auto currentSize = static_cast<size_t>(std::filesystem::file_size(path, ec));
        if (ec)
            return false;

        return (currentTime != watched.lastModified) || (currentSize != watched.lastSize);
    }

} // namespace Spark
