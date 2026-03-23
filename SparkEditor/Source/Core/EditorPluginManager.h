/**
 * @file EditorPluginManager.h
 * @brief Manages editor plugin lifecycle and registration (R7.1)
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides registration and management of editor plugins, supporting
 * both built-in (template-registered) and DLL-loaded plugins.
 */

#pragma once

#include "EditorPanel.h"
#include <cstdint>
#include "IEditorPlugin.h"

#include <memory>
#include <string>
#include <vector>

namespace SparkEditor
{

    class EditorApplication;

    /**
     * @brief Tracks a loaded plugin and its associated resources
     */
    struct PluginEntry
    {
        std::unique_ptr<IEditorPlugin> plugin;
        bool isInitialized = false;
        bool isFromDLL = false;
        void* libraryHandle = nullptr;
        DestroyEditorPluginFn destroyFn = nullptr;
    };

    /**
     * @brief Manages editor plugin lifecycle
     *
     * Handles registration, initialization, per-frame updating, GUI rendering,
     * and shutdown of all editor plugins. Supports dynamic loading of plugin
     * DLLs at runtime.
     */
    class EditorPluginManager
    {
      public:
        EditorPluginManager() = default;
        ~EditorPluginManager();

        // Non-copyable, non-movable
        EditorPluginManager(const EditorPluginManager&) = delete;
        EditorPluginManager& operator=(const EditorPluginManager&) = delete;
        EditorPluginManager(EditorPluginManager&&) = delete;
        EditorPluginManager& operator=(EditorPluginManager&&) = delete;

        // ----- Registration -----

        /**
         * @brief Register a built-in plugin by type
         * @tparam T Plugin class deriving from IEditorPlugin
         * @return true if registration succeeded, false if a plugin with the same name exists
         */
        template <typename T> bool RegisterPlugin()
        {
            static_assert(std::is_base_of_v<IEditorPlugin, T>, "T must derive from IEditorPlugin");
            auto plugin = std::make_unique<T>();
            return RegisterPluginInstance(std::move(plugin), /*isFromDLL=*/false, /*handle=*/nullptr,
                                          /*destroyFn=*/nullptr);
        }

        /**
         * @brief Load a plugin from a shared library (DLL/.so/.dylib)
         * @param path Path to the plugin shared library
         * @return true if loading and registration succeeded
         */
        bool LoadPlugin(const std::string& path);

        /**
         * @brief Unload a plugin by name
         * @param name Name of the plugin to unload (as returned by IEditorPlugin::GetName)
         * @return true if the plugin was found and unloaded
         */
        bool UnloadPlugin(const std::string& name);

        // ----- Lookup -----

        /**
         * @brief Get a plugin by name
         * @param name Name of the plugin
         * @return Pointer to the plugin, or nullptr if not found
         */
        IEditorPlugin* GetPlugin(const std::string& name) const;

        /**
         * @brief Get the number of registered plugins
         */
        size_t GetPluginCount() const;

        /**
         * @brief Get the names of all registered plugins
         * @return Vector of plugin name strings
         */
        std::vector<std::string> GetPluginNames() const;

        // ----- Lifecycle -----

        /**
         * @brief Initialize all registered plugins
         * @param app Pointer to the editor application
         * @return true if all plugins initialized successfully
         */
        bool InitializeAll(EditorApplication* app);

        /**
         * @brief Shutdown all plugins in reverse registration order
         */
        void ShutdownAll();

        /**
         * @brief Update all initialized plugins
         * @param deltaTime Time elapsed since last frame in seconds
         */
        void UpdateAll(float deltaTime);

        /**
         * @brief Render all initialized plugins' GUI
         */
        void RenderAll();

        // ----- Event hooks -----

        /**
         * @brief Notify all plugins of a scene load event
         * @param scenePath Path to the loaded scene
         */
        void NotifySceneLoad(const std::string& scenePath);

        /**
         * @brief Notify all plugins of a scene save event
         * @param scenePath Path to the saved scene
         */
        void NotifySceneSave(const std::string& scenePath);

        /**
         * @brief Notify all plugins of an entity selection event
         * @param entityID ID of the selected entity
         */
        void NotifyEntitySelected(uint32_t entityID);

        /**
         * @brief Render menu bar items for all plugins
         */
        void RenderMenuBarItems();

        // ----- Panel registration -----

        /**
         * @brief Register an EditorPanel from a plugin
         *
         * Plugins can create custom panels and register them through the
         * manager. Registered panels are owned by the manager and will be
         * shut down when the plugin system shuts down.
         *
         * @param panel Unique pointer to the panel to register
         */
        void RegisterPanel(std::unique_ptr<EditorPanel> panel);

        /**
         * @brief Get all plugin-registered panels
         * @return Reference to the vector of registered panels
         */
        const std::vector<std::unique_ptr<EditorPanel>>& GetRegisteredPanels() const;

        // ----- Console integration -----

        /**
         * @brief Log all loaded plugins to the console
         */
        void Console_ListPlugins() const;

      private:
        /**
         * @brief Internal helper to register a plugin instance
         */
        bool RegisterPluginInstance(std::unique_ptr<IEditorPlugin> plugin, bool isFromDLL, void* libraryHandle,
                                    DestroyEditorPluginFn destroyFn);

        /**
         * @brief Find a plugin entry by name
         * @return Iterator to the entry, or m_plugins.end() if not found
         */
        std::vector<PluginEntry>::iterator FindPlugin(const std::string& name);
        std::vector<PluginEntry>::const_iterator FindPlugin(const std::string& name) const;

        /**
         * @brief Unload a DLL handle in a platform-specific way
         */
        static void UnloadLibrary(void* handle);

        std::vector<PluginEntry> m_plugins;
        std::vector<std::unique_ptr<EditorPanel>> m_registeredPanels;
    };

} // namespace SparkEditor
