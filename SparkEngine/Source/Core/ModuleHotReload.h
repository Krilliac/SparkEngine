/**
 * @file ModuleHotReload.h
 * @brief Watches module DLLs for changes and triggers automatic hot-reload
 */

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{
    class IEngineContext;
}
class ModuleManager;

namespace Spark
{

    enum class ModuleChangeType
    {
        Modified,
        Created,
        Deleted
    };

    struct ModuleChangeEvent
    {
        std::string moduleName;
        std::string dllPath;
        ModuleChangeType type;
        std::chrono::system_clock::time_point timestamp;
    };

    using ModuleReloadCallback = std::function<void(const std::string& moduleName, bool success)>;

    /**
 * @brief Watches module DLLs/SOs for changes and triggers hot-reload via ModuleManager
 *
 * Integrates with ModuleManager to provide automatic module reloading when DLLs change.
 * Uses filesystem polling (cross-platform, no platform-specific APIs needed).
 *
 * Call PollChanges() each frame from the main thread.
 */
    class ModuleHotReloadManager
    {
      public:
        ModuleHotReloadManager() = default;
        ~ModuleHotReloadManager();

        // Non-copyable
        ModuleHotReloadManager(const ModuleHotReloadManager&) = delete;
        ModuleHotReloadManager& operator=(const ModuleHotReloadManager&) = delete;

        /**
     * @brief Initialize with references to ModuleManager and EngineContext
     * Both are non-owning references that must outlive this object.
     */
        void Initialize(ModuleManager* moduleManager, IEngineContext* context);

        /**
     * @brief Add a DLL path to watch for a specific module
     * @param moduleName The module name (must match ModuleManager's loaded module name)
     * @param dllPath Path to the DLL/SO to watch
     */
        void WatchModule(const std::string& moduleName, const std::string& dllPath);

        /**
     * @brief Auto-detect all loaded modules from ModuleManager and watch their DLLs
     */
        void WatchAllLoadedModules();

        /** @brief Start watching for changes */
        void Start();

        /** @brief Stop watching */
        void Stop();

        /**
     * @brief Poll for DLL changes and trigger reloads
     * Call this each frame from the main thread.
     * @return Number of modules reloaded this frame
     */
        int PollChanges();

        /**
     * @brief Force reload a specific module
     * @return true if reload succeeded
     */
        bool ForceReload(const std::string& moduleName);

        /** @brief Set callback for reload events */
        void SetReloadCallback(ModuleReloadCallback callback);

        /** @brief Set debounce time in milliseconds (default: 500ms for DLLs) */
        void SetDebounceMs(uint32_t ms) { m_debounceMs = ms; }

        /** @brief Enable/disable hot-reload */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsEnabled() const { return m_enabled; }
        bool IsRunning() const { return m_running; }
        int GetReloadCount() const;

        /** @brief Console status string */
        std::string GetStatus() const;

      private:
        struct WatchedModule
        {
            std::string moduleName;
            std::string dllPath;
            std::filesystem::file_time_type lastModified;
            size_t lastSize = 0;
            bool valid = false;
        };

        struct PendingReload
        {
            std::string moduleName;
            std::chrono::steady_clock::time_point detectedAt;
        };

        void SnapshotFile(WatchedModule& watched);
        bool HasFileChanged(const WatchedModule& watched) const;

        ModuleManager* m_moduleManager = nullptr;
        IEngineContext* m_context = nullptr;

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, WatchedModule> m_watchedModules;
        std::vector<PendingReload> m_pendingReloads;

        ModuleReloadCallback m_reloadCallback;

        uint32_t m_debounceMs = 500;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_enabled{true};
        int m_reloadCount = 0;
    };

} // namespace Spark
