#pragma once

/**
 * @file PluginRegistry.h
 * @brief Macro-based plugin registration using a static linked list
 *
 * Plugins register at static-init time via the SPARK_DECLARE_PLUGIN macro.
 * Each PluginDescriptor inserts itself into a singly-linked list whose head
 * is owned by PluginRegistry.  At engine startup, PluginRegistry::InitializeAll()
 * walks the list and calls every plugin's init function — no manual wiring needed.
 */

#include "Platform.h"

#include <cstdint>
#include <string>

namespace Spark
{

    /// @brief Plugin lifecycle function types
    using PluginInitFn = bool (*)();
    using PluginUpdateFn = void (*)(float);
    using PluginShutdownFn = void (*)();

    /// @brief Describes a single plugin and links it into the global registry
    ///
    /// Constructed at static-init time; the constructor appends this descriptor
    /// to the head of PluginRegistry's linked list.
    struct PluginDescriptor
    {
        const char* name;            ///< Human-readable plugin name
        PluginInitFn initFn;         ///< Called during InitializeAll()
        PluginUpdateFn updateFn;     ///< Called during UpdateAll(dt)
        PluginShutdownFn shutdownFn; ///< Called during ShutdownAll()
        PluginDescriptor* next;      ///< Next descriptor in the linked list

        /// @brief Register this plugin into the global linked list
        PluginDescriptor(const char* pluginName, PluginInitFn init, PluginUpdateFn update, PluginShutdownFn shutdown);
    };

    /// @brief Central registry of all statically-declared plugins
    class PluginRegistry
    {
      public:
        /// @brief Get the head of the plugin linked list
        static PluginDescriptor* First();

        /// @brief Initialize all registered plugins (calls each initFn)
        static void InitializeAll();

        /// @brief Update all registered plugins (calls each updateFn)
        /// @param deltaTime Frame delta time in seconds
        static void UpdateAll(float deltaTime);

        /// @brief Shut down all registered plugins in reverse-registration order
        static void ShutdownAll();

        /// @brief Number of registered plugins
        static uint32_t GetPluginCount();

        /// @brief Console command: return a human-readable status string
        static std::string Console_GetStatus();

      private:
        static PluginDescriptor* s_head;
        friend struct PluginDescriptor;
    };

/// @brief Declare and register a plugin at static-init time
///
/// Usage:
/// @code
/// static bool MyPluginInit() { return true; }
/// static void MyPluginUpdate(float dt) { }
/// static void MyPluginShutdown() { }
/// SPARK_DECLARE_PLUGIN(MyPlugin, "My Plugin", MyPluginInit, MyPluginUpdate, MyPluginShutdown);
/// @endcode
#define SPARK_DECLARE_PLUGIN(className, pluginName, initFn, updateFn, shutdownFn)                                      \
    static ::Spark::PluginDescriptor s_plugin_##className(pluginName, initFn, updateFn, shutdownFn)

} // namespace Spark
