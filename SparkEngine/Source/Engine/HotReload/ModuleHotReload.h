/**
 * @file ModuleHotReload.h
 * @brief Runtime hot-reload system for game module DLLs
 * @author Spark Engine Team
 * @date 2026
 *
 * Monitors game module DLL files for changes and performs live unload/reload
 * during Play-in-Editor sessions. Uses file timestamp polling to detect
 * recompiled modules, then invokes the IModule OnUnload/OnLoad lifecycle
 * to swap the new code in without restarting the engine.
 *
 * ## Usage
 * @code
 *   auto& hotReload = Spark::HotReload::ModuleHotReload::GetInstance();
 *   hotReload.Initialize();
 *   hotReload.SetWatchDirectory("GameModules/bin");
 *   hotReload.RegisterModule("SparkGameFPS", handle, createFunc, destroyFunc);
 *
 *   // In main loop:
 *   hotReload.Update(deltaTime);  // polls timestamps, reloads if changed
 *
 *   // Query status:
 *   auto status = hotReload.Console_GetStatus();
 * @endcode
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LogMacros.h"

namespace Spark::HotReload
{

    /** @brief Result of a module reload attempt */
    enum class ReloadResult
    {
        Success,       ///< Module reloaded successfully
        FileNotFound,  ///< DLL file not found at expected path
        CopyFailed,    ///< Failed to copy DLL for shadow loading
        LoadFailed,    ///< Failed to load the new DLL
        InitFailed,    ///< Module loaded but Initialize() failed
        NotModified,   ///< File has not changed since last load
        CooldownActive ///< Reload requested too soon after previous reload
    };

    /** @brief Information about a single watched module */
    struct WatchedModule
    {
        std::string name;                                       ///< Module name (e.g. "SparkGameFPS")
        std::filesystem::path dllPath;                          ///< Path to the original DLL
        std::filesystem::path shadowPath;                       ///< Path to the shadow copy used for loading
        std::filesystem::file_time_type lastWriteTime{};        ///< Last known modification time
        uint32_t reloadCount = 0;                               ///< Number of successful reloads
        bool enabled = true;                                    ///< Whether watching is enabled for this module
        std::chrono::steady_clock::time_point lastReloadTime{}; ///< Time of last reload attempt
    };

    /** @brief Record of a reload event for history/logging */
    struct ReloadEvent
    {
        std::string moduleName;                            ///< Module that was reloaded
        ReloadResult result = ReloadResult::NotModified;   ///< Outcome of the reload
        std::chrono::steady_clock::time_point timestamp{}; ///< When the reload occurred
        double durationMs = 0.0;                           ///< Time taken for the reload operation
    };

    /**
     * @brief Callback signature for module lifecycle during hot-reload
     *
     * The hot-reload system calls these to perform the actual DLL unload/load.
     * The engine's ModuleManager provides these callbacks when registering modules.
     */
    using UnloadCallback = std::function<void(const std::string& moduleName)>;
    using ReloadCallback = std::function<bool(const std::string& moduleName, const std::filesystem::path& dllPath)>;

    /**
     * @brief Runtime hot-reload system for game module DLLs
     *
     * Polls watched DLL files for timestamp changes each frame. When a change is
     * detected (and cooldown has elapsed), the module is unloaded via IModule::OnUnload(),
     * the DLL is shadow-copied, reloaded, and IModule::OnLoad() is called.
     *
     * Thread safety: Update() must be called from the main thread only.
     */
    class ModuleHotReload
    {
      public:
        /** @brief Get the singleton instance */
        static ModuleHotReload& GetInstance()
        {
            static ModuleHotReload instance;
            return instance;
        }

        /**
         * @brief Initialize the hot-reload system
         * @param pollIntervalSeconds How often to check file timestamps (default: 1.0s)
         */
        void Initialize(float pollIntervalSeconds = 1.0f)
        {
            m_pollInterval = pollIntervalSeconds;
            m_timeSinceLastPoll = 0.0f;
            m_reloadCooldownSeconds = 2.0f;
            m_enabled = true;
            m_initialized = true;
            m_modules.clear();
            m_history.clear();
            SPARK_LOG_INFO(Spark::LogCategory::Core, "ModuleHotReload initialized (poll: %.1fs)", pollIntervalSeconds);
        }

        /** @brief Shut down and clear all watched modules */
        void Shutdown()
        {
            m_modules.clear();
            m_history.clear();
            m_initialized = false;
            m_enabled = false;
        }

        /**
         * @brief Set the base directory to watch for DLL changes
         * @param directory Path to the directory containing game module DLLs
         */
        void SetWatchDirectory(const std::string& directory) { m_watchDirectory = directory; }

        /**
         * @brief Register a module for hot-reload watching
         * @param name Module name (must match the DLL filename stem)
         * @param dllPath Full path to the module DLL
         * @return true if successfully registered
         */
        bool RegisterModule(const std::string& name, const std::filesystem::path& dllPath)
        {
            if (name.empty() || !m_initialized)
                return false;

            WatchedModule mod;
            mod.name = name;
            mod.dllPath = dllPath;
            mod.shadowPath = dllPath.parent_path() / (name + "_hotreload" + dllPath.extension().string());

            std::error_code ec;
            if (std::filesystem::exists(dllPath, ec))
            {
                mod.lastWriteTime = std::filesystem::last_write_time(dllPath, ec);
            }

            m_modules[name] = std::move(mod);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "HotReload: Registered module '%s'", name.c_str());
            return true;
        }

        /**
         * @brief Unregister a module from hot-reload watching
         * @param name Module name to stop watching
         */
        void UnregisterModule(const std::string& name) { m_modules.erase(name); }

        /**
         * @brief Set callbacks for the actual unload/reload operations
         * @param onUnload Called before unloading a module
         * @param onReload Called to load the new module DLL; returns true on success
         */
        void SetCallbacks(UnloadCallback onUnload, ReloadCallback onReload)
        {
            m_onUnload = std::move(onUnload);
            m_onReload = std::move(onReload);
        }

        /**
         * @brief Per-frame update — polls timestamps and triggers reloads
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime)
        {
            if (!m_initialized || !m_enabled)
                return;

            m_timeSinceLastPoll += deltaTime;
            if (m_timeSinceLastPoll < m_pollInterval)
                return;
            m_timeSinceLastPoll = 0.0f;

            auto now = std::chrono::steady_clock::now();

            for (auto& [name, mod] : m_modules)
            {
                if (!mod.enabled)
                    continue;

                // Check cooldown
                auto elapsed = std::chrono::duration<float>(now - mod.lastReloadTime).count();
                if (elapsed < m_reloadCooldownSeconds)
                    continue;

                // Check if file has been modified
                std::error_code ec;
                if (!std::filesystem::exists(mod.dllPath, ec))
                    continue;

                auto currentWriteTime = std::filesystem::last_write_time(mod.dllPath, ec);
                if (ec || currentWriteTime == mod.lastWriteTime)
                    continue;

                // File changed — attempt reload
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Module '%s' changed on disk, reloading...", name.c_str());
                auto result = PerformReload(mod);
                mod.lastReloadTime = now;
                mod.lastWriteTime = currentWriteTime;

                ReloadEvent event;
                event.moduleName = name;
                event.result = result;
                event.timestamp = now;
                m_history.push_back(event);

                // Keep history bounded
                if (m_history.size() > 100)
                    m_history.erase(m_history.begin());
            }
        }

        /**
         * @brief Force an immediate reload of a specific module
         * @param name Module name to reload
         * @return Result of the reload attempt
         */
        ReloadResult ForceReload(const std::string& name)
        {
            auto it = m_modules.find(name);
            if (it == m_modules.end())
                return ReloadResult::FileNotFound;

            auto result = PerformReload(it->second);

            ReloadEvent event;
            event.moduleName = name;
            event.result = result;
            event.timestamp = std::chrono::steady_clock::now();
            m_history.push_back(event);

            return result;
        }

        /** @brief Enable or disable the entire hot-reload system */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /** @brief Check if hot-reload is enabled */
        bool IsEnabled() const { return m_enabled; }

        /** @brief Set the poll interval in seconds */
        void SetPollInterval(float seconds) { m_pollInterval = std::max(0.1f, seconds); }

        /** @brief Set the cooldown between reloads for the same module */
        void SetReloadCooldown(float seconds) { m_reloadCooldownSeconds = std::max(0.5f, seconds); }

        /** @brief Get the number of watched modules */
        size_t GetWatchedModuleCount() const { return m_modules.size(); }

        /** @brief Get total successful reloads across all modules */
        uint32_t GetTotalReloadCount() const
        {
            uint32_t total = 0;
            for (const auto& [name, mod] : m_modules)
                total += mod.reloadCount;
            return total;
        }

        /** @brief Get the reload history */
        const std::vector<ReloadEvent>& GetHistory() const { return m_history; }

        /** @brief Get info about a specific watched module */
        const WatchedModule* GetModule(const std::string& name) const
        {
            auto it = m_modules.find(name);
            return (it != m_modules.end()) ? &it->second : nullptr;
        }

        /** @brief Get all watched module names */
        std::vector<std::string> GetWatchedModuleNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_modules.size());
            for (const auto& [name, mod] : m_modules)
                names.push_back(name);
            return names;
        }

        /** @brief Get a console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[HotReload] Not initialized";

            std::string status = "[HotReload] ";
            status += m_enabled ? "ENABLED" : "DISABLED";
            status += " | Watching: " + std::to_string(m_modules.size()) + " modules";
            status += " | Total reloads: " + std::to_string(GetTotalReloadCount());
            status += " | Poll interval: " + std::to_string(m_pollInterval) + "s";
            if (!m_watchDirectory.empty())
                status += " | Dir: " + m_watchDirectory;
            return status;
        }

      private:
        ModuleHotReload() = default;

        /**
         * @brief Perform the actual reload sequence for a module
         * @param mod The module to reload
         * @return Result of the reload attempt
         */
        ReloadResult PerformReload(WatchedModule& mod)
        {
            // Step 1: Shadow-copy the DLL so the original can be overwritten by the compiler
            std::error_code ec;
            std::filesystem::copy_file(mod.dllPath, mod.shadowPath, std::filesystem::copy_options::overwrite_existing,
                                       ec);
            if (ec)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "HotReload: Failed to copy DLL for module '%s'",
                               mod.name.c_str());
                return ReloadResult::CopyFailed;
            }

            // Step 2: Unload the current module
            if (m_onUnload)
                m_onUnload(mod.name);

            // Step 3: Load the new module from the shadow copy
            if (m_onReload)
            {
                if (!m_onReload(mod.name, mod.shadowPath))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "HotReload: Failed to load module '%s'",
                                    mod.name.c_str());
                    return ReloadResult::LoadFailed;
                }
            }

            mod.reloadCount++;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "HotReload: Successfully reloaded module '%s' (count: %u)",
                           mod.name.c_str(), mod.reloadCount);
            return ReloadResult::Success;
        }

        bool m_initialized = false;
        bool m_enabled = false;
        float m_pollInterval = 1.0f;
        float m_timeSinceLastPoll = 0.0f;
        float m_reloadCooldownSeconds = 2.0f;
        std::string m_watchDirectory;

        std::unordered_map<std::string, WatchedModule> m_modules;
        std::vector<ReloadEvent> m_history;

        UnloadCallback m_onUnload;
        ReloadCallback m_onReload;
    };

} // namespace Spark::HotReload
