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

    /// @brief Float data binding
    class UIFloatBinding : public UIDataBinding
    {
      public:
        explicit UIFloatBinding(float* target, std::function<void(float)> onChange = nullptr);

        void PushToWidget() override;
        void PullFromWidget() override;
        std::string GetDisplayValue() const override;

        float GetValue() const { return *m_target; }
        void SetValue(float value);

      private:
        float* m_target;
        float m_widgetValue = 0.0f;
        std::function<void(float)> m_onChange;
    };

    /// @brief String data binding
    class UIStringBinding : public UIDataBinding
    {
      public:
        explicit UIStringBinding(std::string* target, std::function<void(const std::string&)> onChange = nullptr);

        void PushToWidget() override;
        void PullFromWidget() override;
        std::string GetDisplayValue() const override;

      private:
        std::string* m_target;
        std::string m_widgetValue;
        std::function<void(const std::string&)> m_onChange;
    };

    /// @brief Bool data binding
    class UIBoolBinding : public UIDataBinding
    {
      public:
        explicit UIBoolBinding(bool* target, std::function<void(bool)> onChange = nullptr);

        void PushToWidget() override;
        void PullFromWidget() override;
        std::string GetDisplayValue() const override;

      private:
        bool* m_target;
        bool m_widgetValue = false;
        std::function<void(bool)> m_onChange;
    };

    /// @brief Int data binding
    class UIIntBinding : public UIDataBinding
    {
      public:
        explicit UIIntBinding(int32_t* target, std::function<void(int32_t)> onChange = nullptr);

        void PushToWidget() override;
        void PullFromWidget() override;
        std::string GetDisplayValue() const override;

      private:
        int32_t* m_target;
        int32_t m_widgetValue = 0;
        std::function<void(int32_t)> m_onChange;
    };

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
        [[deprecated("Use EngineContext::Get()->GetSystem<UIFactory>() instead")]]
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
