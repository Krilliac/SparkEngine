/**
 * @file PluginRegistry.cpp
 * @brief Implementation of the static plugin registry
 */

#include "PluginRegistry.h"

#include <format>
#include <vector>

namespace Spark
{

    // ---------------------------------------------------------------------------
    // PluginDescriptor — self-registering constructor
    // ---------------------------------------------------------------------------

    PluginDescriptor* PluginRegistry::s_head = nullptr;

    PluginDescriptor::PluginDescriptor(const char* pluginName, PluginInitFn init, PluginUpdateFn update,
                                       PluginShutdownFn shutdown)
        : name(pluginName), initFn(init), updateFn(update), shutdownFn(shutdown), next(PluginRegistry::s_head)
    {
        PluginRegistry::s_head = this;
    }

    // ---------------------------------------------------------------------------
    // PluginRegistry
    // ---------------------------------------------------------------------------

    PluginDescriptor* PluginRegistry::First()
    {
        return s_head;
    }

    void PluginRegistry::InitializeAll()
    {
        // Walk in registration order is reversed (LIFO), so collect into a
        // vector first to initialize in declaration order.
        std::vector<PluginDescriptor*> plugins;
        for (auto* desc = s_head; desc != nullptr; desc = desc->next)
        {
            plugins.push_back(desc);
        }

        // Initialize in reverse so the first-declared plugin initializes first
        for (auto it = plugins.rbegin(); it != plugins.rend(); ++it)
        {
            if ((*it)->initFn)
            {
                (*it)->initFn();
            }
        }
    }

    void PluginRegistry::UpdateAll(float deltaTime)
    {
        for (auto* desc = s_head; desc != nullptr; desc = desc->next)
        {
            if (desc->updateFn)
            {
                desc->updateFn(deltaTime);
            }
        }
    }

    void PluginRegistry::ShutdownAll()
    {
        // Shutdown in registration order (LIFO = reverse of init order)
        for (auto* desc = s_head; desc != nullptr; desc = desc->next)
        {
            if (desc->shutdownFn)
            {
                desc->shutdownFn();
            }
        }
    }

    uint32_t PluginRegistry::GetPluginCount()
    {
        uint32_t count = 0;
        for (auto* desc = s_head; desc != nullptr; desc = desc->next)
        {
            ++count;
        }
        return count;
    }

    std::string PluginRegistry::Console_GetStatus()
    {
        uint32_t count = GetPluginCount();
        std::string result = std::format("PluginRegistry: {} plugin(s) registered\n", count);

        uint32_t index = 0;
        for (auto* desc = s_head; desc != nullptr; desc = desc->next)
        {
            result += std::format("  [{}] {}\n", index, desc->name ? desc->name : "(unnamed)");
            ++index;
        }

        return result;
    }

} // namespace Spark
