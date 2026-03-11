/**
 * @file EditorIntegration.h
 * @brief Cross-links editor architecture components with the editor lifecycle
 *
 * Integrates EditorPluginManager (R7.1) and EditorConsoleBridge (R7.3)
 * with the editor's startup/shutdown sequence, providing a single point
 * of initialization for all editor architecture extensions.
 *
 * @see EditorPluginManager.h, EditorConsoleBridge.h, EditorApplication.h
 */

#pragma once

#include "EditorPluginManager.h"
#include "IEditorPlugin.h"
#include "../Utils/EditorConsoleBridge.h"

#include <memory>
#include <string>
#include <sstream>
#include <vector>
#include <filesystem>

namespace SparkEditor
{

    /**
     * @brief Configuration for editor architecture extensions.
     */
    struct EditorExtensionConfig
    {
        /// Directory to scan for editor plugins
        std::string pluginDirectory = "EditorPlugins";

        /// Whether to auto-load plugins on startup
        bool autoLoadPlugins = true;

        /// Whether to initialize the console bridge
        bool enableConsoleBridge = true;

        /// Plugin file extension (.dll on Windows, .so on Linux)
#ifdef _WIN32
        std::string pluginExtension = ".dll";
#else
        std::string pluginExtension = ".so";
#endif
    };

    /**
     * @brief Manages the lifecycle of all editor architecture extensions.
     *
     * Coordinates EditorPluginManager and EditorConsoleBridge initialization
     * and shutdown, providing a single integration point for the editor
     * application.
     */
    class EditorExtensions
    {
      public:
        EditorExtensions() = default;
        ~EditorExtensions() { Shutdown(); }

        // Non-copyable
        EditorExtensions(const EditorExtensions&) = delete;
        EditorExtensions& operator=(const EditorExtensions&) = delete;

        /**
         * @brief Initialize all editor extensions.
         *
         * 1. Connects EditorConsoleBridge to the engine's console
         * 2. Initializes EditorPluginManager
         * 3. Auto-loads plugins from the configured directory
         *
         * @param config  Extension configuration.
         * @return true on success.
         */
        bool Initialize(const EditorExtensionConfig& config = {})
        {
            if (m_initialized)
                return true;

            m_config = config;

            // Initialize console bridge (R7.3)
            if (config.enableConsoleBridge)
            {
                auto& bridge = EditorConsoleBridge::GetInstance();
                if (bridge.Initialize())
                {
                    m_consoleBridgeActive = true;
                    bridge.LogInfo("Editor extensions initializing...");
                }
            }

            // Initialize plugin manager (R7.1)
            m_pluginManager = std::make_unique<EditorPluginManager>();

            // Auto-load plugins if configured
            if (config.autoLoadPlugins)
            {
                LoadPluginsFromDirectory(config.pluginDirectory);
            }

            m_initialized = true;

            if (m_consoleBridgeActive)
            {
                auto& bridge = EditorConsoleBridge::GetInstance();
                bridge.LogSuccess("Editor extensions initialized successfully");
                bridge.RegisterEditorCommand(
                    "extensions", [this](const std::vector<std::string>& /*args*/) -> std::string
                    { return Console_GetStatus(); }, "Show editor extensions status");
                bridge.RegisterEditorCommand(
                    "plugins", [this](const std::vector<std::string>& /*args*/) -> std::string
                    { return Console_ListPlugins(); }, "List loaded editor plugins");
            }

            return true;
        }

        /**
         * @brief Shut down all editor extensions in reverse order.
         */
        void Shutdown()
        {
            if (!m_initialized)
                return;

            // Unload all plugins
            if (m_pluginManager)
            {
                m_pluginManager->UnloadAll();
                m_pluginManager.reset();
            }

            // Disconnect console bridge
            if (m_consoleBridgeActive)
            {
                EditorConsoleBridge::GetInstance().Shutdown();
                m_consoleBridgeActive = false;
            }

            m_initialized = false;
        }

        /**
         * @brief Update all active plugins (called each frame).
         */
        void Update(float deltaTime)
        {
            if (!m_initialized || !m_pluginManager)
                return;

            m_pluginManager->UpdateAll(deltaTime);
        }

        /**
         * @brief Load plugins from a directory.
         */
        void LoadPluginsFromDirectory(const std::string& directory)
        {
            if (!m_pluginManager)
                return;

            try
            {
                if (!std::filesystem::exists(directory))
                {
                    if (m_consoleBridgeActive)
                    {
                        EditorConsoleBridge::GetInstance().LogWarning("Plugin directory not found: " + directory);
                    }
                    return;
                }

                for (const auto& entry : std::filesystem::directory_iterator(directory))
                {
                    if (entry.is_regular_file() && entry.path().extension() == m_config.pluginExtension)
                    {
                        std::string pluginPath = entry.path().string();
                        if (m_pluginManager->LoadPlugin(pluginPath))
                        {
                            m_loadedPluginPaths.push_back(pluginPath);
                            if (m_consoleBridgeActive)
                            {
                                EditorConsoleBridge::GetInstance().LogSuccess("Loaded plugin: " +
                                                                              entry.path().filename().string());
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                if (m_consoleBridgeActive)
                {
                    EditorConsoleBridge::GetInstance().LogError("Error scanning plugin directory: " +
                                                                std::string(e.what()));
                }
            }
        }

        // -- Accessors --

        EditorPluginManager* GetPluginManager() { return m_pluginManager.get(); }
        bool IsConsoleBridgeActive() const { return m_consoleBridgeActive; }
        bool IsInitialized() const { return m_initialized; }
        size_t GetLoadedPluginCount() const { return m_loadedPluginPaths.size(); }

        /**
         * @brief Console integration: extensions status.
         */
        std::string Console_GetStatus() const
        {
            std::stringstream ss;
            ss << "=== Editor Extensions ===\n";
            ss << "  Initialized:       " << (m_initialized ? "YES" : "NO") << "\n";
            ss << "  Console Bridge:    " << (m_consoleBridgeActive ? "ACTIVE" : "INACTIVE") << "\n";
            ss << "  Plugin Manager:    " << (m_pluginManager ? "ACTIVE" : "INACTIVE") << "\n";
            ss << "  Loaded Plugins:    " << m_loadedPluginPaths.size() << "\n";
            ss << "  Plugin Directory:  " << m_config.pluginDirectory << "\n";
            ss << "  Auto-Load:         " << (m_config.autoLoadPlugins ? "ON" : "OFF") << "\n";
            return ss.str();
        }

        /**
         * @brief Console integration: list loaded plugins.
         */
        std::string Console_ListPlugins() const
        {
            std::stringstream ss;
            ss << "=== Loaded Editor Plugins ===\n";
            if (m_loadedPluginPaths.empty())
            {
                ss << "  (no plugins loaded)\n";
            }
            for (const auto& path : m_loadedPluginPaths)
            {
                ss << "  " << path << "\n";
            }
            return ss.str();
        }

      private:
        std::unique_ptr<EditorPluginManager> m_pluginManager;
        EditorExtensionConfig m_config;
        std::vector<std::string> m_loadedPluginPaths;
        bool m_consoleBridgeActive = false;
        bool m_initialized = false;
    };

} // namespace SparkEditor
