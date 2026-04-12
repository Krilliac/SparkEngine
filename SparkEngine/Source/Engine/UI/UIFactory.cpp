/**
 * @file UIFactory.cpp
 * @brief Config-driven UI creation with data binding implementation
 */

#include "UIFactory.h"
#include "../../Utils/Validate.h"

#include <sstream>

namespace Spark::UI
{

    // UITypedBinding<T> is fully inline in UIFactory.h — no .cpp definitions needed.

    // ============================================================================
    // UIFactory
    // ============================================================================

    bool UIFactory::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "UIFactory::Initialize");
        m_widgetFactories.clear();
        m_bindings.clear();

        // Register default widget types
        RegisterWidgetType("panel",
                           [](const std::unordered_map<std::string, std::string>& props)
                           {
                               UIWidgetConfig config;
                               config.type = "panel";
                               config.properties = props;
                               return config;
                           });

        RegisterWidgetType("label",
                           [](const std::unordered_map<std::string, std::string>& props)
                           {
                               UIWidgetConfig config;
                               config.type = "label";
                               config.properties = props;
                               return config;
                           });

        RegisterWidgetType("button",
                           [](const std::unordered_map<std::string, std::string>& props)
                           {
                               UIWidgetConfig config;
                               config.type = "button";
                               config.properties = props;
                               return config;
                           });

        RegisterWidgetType("progressBar",
                           [](const std::unordered_map<std::string, std::string>& props)
                           {
                               UIWidgetConfig config;
                               config.type = "progressBar";
                               config.properties = props;
                               return config;
                           });

        RegisterWidgetType("image",
                           [](const std::unordered_map<std::string, std::string>& props)
                           {
                               UIWidgetConfig config;
                               config.type = "image";
                               config.properties = props;
                               return config;
                           });

        return true;
    }

    void UIFactory::RegisterWidgetType(const std::string& type, WidgetFactoryFn factory)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "UIFactory::RegisterWidgetType '%s'", type.c_str());
        m_widgetFactories[type] = std::move(factory);
    }

    UIWidgetConfig UIFactory::ParseConfig(const std::string& configText) const
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "UIFactory::ParseConfig (%zu bytes)", configText.size());
        UIWidgetConfig root;
        root.type = "root";

        // Simple line-by-line parser for key-value widget configs
        std::istringstream stream(configText);
        std::string line;

        while (std::getline(stream, line))
        {
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            line = line.substr(start);

            // Extract widget type
            size_t typeEnd = line.find_first_of(" \t{");
            if (typeEnd == std::string::npos)
                continue;

            UIWidgetConfig widget;
            widget.type = line.substr(0, typeEnd);

            // Extract properties (id="value" bind="value" etc.)
            size_t pos = typeEnd;
            while (pos < line.size())
            {
                size_t eqPos = line.find('=', pos);
                if (eqPos == std::string::npos)
                    break;

                std::string key = line.substr(pos, eqPos - pos);
                // Trim key
                size_t keyStart = key.find_first_not_of(" \t");
                if (keyStart != std::string::npos)
                    key = key.substr(keyStart);
                size_t keyEnd2 = key.find_last_not_of(" \t");
                if (keyEnd2 != std::string::npos)
                    key = key.substr(0, keyEnd2 + 1);

                // Extract quoted value
                size_t quoteStart = line.find('"', eqPos + 1);
                if (quoteStart == std::string::npos)
                    break;
                size_t quoteEnd = line.find('"', quoteStart + 1);
                if (quoteEnd == std::string::npos)
                    break;

                std::string value = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);

                if (key == "id")
                    widget.id = value;
                else if (key == "bind")
                    widget.bindingKey = value;
                else
                    widget.properties[key] = value;

                pos = quoteEnd + 1;
            }

            root.children.push_back(std::move(widget));
        }

        return root;
    }

    void UIFactory::RegisterBinding(const std::string& key, std::shared_ptr<UIDataBinding> binding)
    {
        m_bindings[key] = std::move(binding);
    }

    std::shared_ptr<UIDataBinding> UIFactory::GetBinding(const std::string& key) const
    {
        auto it = m_bindings.find(key);
        return (it != m_bindings.end()) ? it->second : nullptr;
    }

    void UIFactory::UpdateAllBindings()
    {
        for (auto& [key, binding] : m_bindings)
            binding->PushToWidget();
    }

    void UIFactory::SyncAllBindings()
    {
        for (auto& [key, binding] : m_bindings)
            binding->PullFromWidget();
    }

    uint32_t UIFactory::GetRegisteredTypeCount() const
    {
        return static_cast<uint32_t>(m_widgetFactories.size());
    }

    uint32_t UIFactory::GetActiveBindingCount() const
    {
        return static_cast<uint32_t>(m_bindings.size());
    }

    std::string UIFactory::Console_GetStatus() const
    {
        std::string status = "UIFactory:\n";
        status += "  Widget types: " + std::to_string(m_widgetFactories.size()) + "\n";
        status += "  Active bindings: " + std::to_string(m_bindings.size()) + "\n";
        for (const auto& [key, binding] : m_bindings)
        {
            status += "    " + key + " = " + binding->GetDisplayValue() + "\n";
        }
        return status;
    }

    void UIFactory::Shutdown()
    {
        m_bindings.clear();
        m_widgetFactories.clear();
    }

} // namespace Spark::UI
