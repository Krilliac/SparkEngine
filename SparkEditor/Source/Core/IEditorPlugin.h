/**
 * @file IEditorPlugin.h
 * @brief Abstract interface for editor plugins (R7.1)
 * @author Spark Engine Team
 * @date 2025
 *
 * Defines the in-process C++ contract for editor plugins compiled into the
 * editor. Runtime shared libraries use Spark/PluginABI.h instead so C++
 * vtables, exceptions, and allocations never cross a DLL boundary.
 */

#pragma once

#include <cstdint>
#include <string>

namespace SparkEditor
{

    class EditorApplication;

    /**
     * @brief Abstract interface for editor plugins
     *
     * All editor plugins must implement this interface. Plugins are managed
     * by EditorPluginManager, which handles their lifecycle (init, update,
     * render, shutdown) and provides hooks for editor events.
     *
     * Built-in plugins can be registered via RegisterPlugin<T>(). External
     * plugins implement the stable C ABI declared by Spark/PluginABI.h.
     */
    class IEditorPlugin
    {
      public:
        virtual ~IEditorPlugin() = default;

        /**
         * @brief Get the human-readable plugin name
         * @return Null-terminated plugin name string
         */
        virtual const char* GetName() const = 0;

        /**
         * @brief Get the plugin version string (e.g. "1.0.0")
         * @return Null-terminated version string
         */
        virtual const char* GetVersion() const = 0;

        /**
         * @brief Initialize the plugin
         * @param app Pointer to the editor application instance
         * @return true if initialization succeeded, false otherwise
         */
        virtual bool Initialize(EditorApplication* app) = 0;

        /**
         * @brief Shutdown the plugin and release resources
         */
        virtual void Shutdown() = 0;

        /**
         * @brief Per-frame update tick
         * @param deltaTime Time elapsed since last frame in seconds
         */
        virtual void Update(float deltaTime) = 0;

        /**
         * @brief Render plugin UI via ImGui
         *
         * Called once per frame during the ImGui render pass.
         */
        virtual void OnGUI() = 0;

        // ----- Optional hooks (default no-op) -----

        /**
         * @brief Called when a scene is loaded
         * @param scenePath Path to the loaded scene file
         */
        virtual void OnSceneLoad(const std::string& scenePath) { (void)scenePath; }

        /**
         * @brief Called when a scene is saved
         * @param scenePath Path to the saved scene file
         */
        virtual void OnSceneSave(const std::string& scenePath) { (void)scenePath; }

        /**
         * @brief Called when the user selects an entity
         * @param entityID ID of the selected entity
         */
        virtual void OnEntitySelected(uint32_t entityID) { (void)entityID; }

        /**
         * @brief Called during menu bar rendering to add custom items
         */
        virtual void OnMenuBar() {}
    };

} // namespace SparkEditor
