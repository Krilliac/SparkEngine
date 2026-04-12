/**
 * @file UIFactory.h
 * @brief Config-driven UI creation with typed data binding
 *
 * Inspired by Halley's UIFactory and UIDataBind pattern. Builds widget trees
 * from configuration data with bidirectional data binding for reactive game UI.
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::UI
{

    /// @brief Bidirectional data binding for UI widgets
    class UIDataBinding
    {
      public:
        virtual ~UIDataBinding() = default;

        /// @brief Push data value to widget display
        virtual void PushToWidget() = 0;

        /// @brief Pull widget value back to data
        virtual void PullFromWidget() = 0;

        /// @brief Get string representation of current value
        virtual std::string GetDisplayValue() const = 0;
    };

    /// @brief Templated bidirectional data binding — replaces per-type classes
    template <typename T> class UITypedBinding : public UIDataBinding
    {
      public:
        explicit UITypedBinding(T* target, std::function<void(const T&)> onChange = nullptr)
            : m_target(target), m_onChange(std::move(onChange))
        {
            if (m_target)
                m_widgetValue = *m_target;
        }

        void PushToWidget() override
        {
            if (m_target)
                m_widgetValue = *m_target;
        }

        void PullFromWidget() override
        {
            if (m_target && *m_target != m_widgetValue)
            {
                *m_target = m_widgetValue;
                if (m_onChange)
                    m_onChange(m_widgetValue);
            }
        }

        std::string GetDisplayValue() const override
        {
            if constexpr (std::is_same_v<T, bool>)
                return m_widgetValue ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>)
                return m_widgetValue;
            else
                return std::to_string(m_widgetValue);
        }

        const T& GetValue() const { return m_widgetValue; }

        void SetValue(const T& value)
        {
            m_widgetValue = value;
            PullFromWidget();
        }

      private:
        T* m_target;
        T m_widgetValue{};
        std::function<void(const T&)> m_onChange;
    };

    // Type aliases for backward compatibility
    using UIFloatBinding = UITypedBinding<float>;
    using UIStringBinding = UITypedBinding<std::string>;
    using UIBoolBinding = UITypedBinding<bool>;
    using UIIntBinding = UITypedBinding<int32_t>;

    /// @brief Simple config node for UI widget definition
    struct UIWidgetConfig
    {
        std::string type;                                        ///< Widget type (panel, label, button, etc.)
        std::string id;                                          ///< Unique widget identifier
        std::unordered_map<std::string, std::string> properties; ///< Key-value properties
        std::vector<UIWidgetConfig> children;                    ///< Child widgets
        std::string bindingKey;                                  ///< Data binding key
    };

    /// @brief Widget factory function type
    using WidgetFactoryFn =
        std::function<UIWidgetConfig(const std::unordered_map<std::string, std::string>& properties)>;

    /// @brief Creates UI widget trees from configuration data
    class UIFactory
    {
      public:
        static UIFactory& GetInstance()
        {
            static UIFactory instance;
            return instance;
        }

        /// @brief Initialize with default widget types
        bool Initialize();

        /// @brief Register a custom widget type factory
        void RegisterWidgetType(const std::string& type, WidgetFactoryFn factory);

        /// @brief Parse a simple config format string into a widget tree
        UIWidgetConfig ParseConfig(const std::string& configText) const;

        /// @brief Register a data binding by key
        void RegisterBinding(const std::string& key, std::shared_ptr<UIDataBinding> binding);

        /// @brief Get a binding by key
        std::shared_ptr<UIDataBinding> GetBinding(const std::string& key) const;

        /// @brief Push all data values to widgets
        void UpdateAllBindings();

        /// @brief Pull all widget values back to data
        void SyncAllBindings();

        /// @brief Get counts
        uint32_t GetRegisteredTypeCount() const;
        uint32_t GetActiveBindingCount() const;

        /// @brief Console status
        std::string Console_GetStatus() const;

        /// @brief Shutdown
        void Shutdown();

      private:
        UIFactory() = default;
        ~UIFactory() = default;
        UIFactory(const UIFactory&) = delete;
        UIFactory& operator=(const UIFactory&) = delete;

        std::unordered_map<std::string, WidgetFactoryFn> m_widgetFactories;
        std::unordered_map<std::string, std::shared_ptr<UIDataBinding>> m_bindings;
    };

} // namespace Spark::UI
