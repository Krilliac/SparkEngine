/**
 * @file UILayoutExtensions.h
 * @brief Declarative UI layout containers, input widgets, and data binding
 */

#pragma once

#include "UISystem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Spark::UI
{

    enum class FlexAlign : uint8_t
    {
        Start,
        Center,
        End,
        Stretch,
        SpaceBetween,
        SpaceAround
    };

    struct Padding
    {
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        float left = 0.0f;
    };

    class UIFlexContainer : public UIWidget
    {
      public:
        explicit UIFlexContainer(const std::string& name);

        void SetDirection(LayoutDirection direction);
        [[nodiscard]] LayoutDirection GetDirection() const;

        void SetAlignment(FlexAlign align);
        [[nodiscard]] FlexAlign GetAlignment() const;

        void SetSpacing(float spacing);
        void SetPadding(float top, float right, float bottom, float left);
        void AddChild(UIWidget* child);
        void CalculateLayout();

      private:
        void LayoutAxis(float startX, float startY, float availW, float availH, bool horizontal);

      private:
        LayoutDirection m_direction = LayoutDirection::Horizontal;
        FlexAlign m_alignment = FlexAlign::Start;
        float m_spacing = 0.0f;
        Padding m_padding{};
        std::vector<UIWidget*> m_children;
    };

    class UIGridLayout : public UIWidget
    {
      public:
        explicit UIGridLayout(const std::string& name);

        void SetColumns(uint32_t cols);
        void SetRows(uint32_t rows);
        void SetCellSpacing(float spacing);
        void SetCell(uint32_t row, uint32_t col, UIWidget* widget);
        void CalculateLayout();

      private:
        uint32_t m_columns = 1;
        uint32_t m_rows = 1;
        float m_cellSpacing = 4.0f;
        std::vector<UIWidget*> m_cells;
    };

    class UIScrollView : public UIWidget
    {
      public:
        explicit UIScrollView(const std::string& name);

        void SetContentSize(float w, float h);
        void SetViewportSize(float w, float h);
        void SetScrollOffset(float x, float y);

        [[nodiscard]] std::pair<float, float> GetScrollOffset() const;
        [[nodiscard]] std::pair<float, float> GetMaxScroll() const;

        void ScrollBy(float dx, float dy);
        [[nodiscard]] bool IsScrollable() const;

        struct Rect
        {
            float x;
            float y;
            float w;
            float h;
        };

        [[nodiscard]] Rect GetVisibleRect() const;

      private:
        [[nodiscard]] float GetMaxScrollX() const;
        [[nodiscard]] float GetMaxScrollY() const;

      private:
        float m_contentW = 0.0f;
        float m_contentH = 0.0f;
        float m_scrollX = 0.0f;
        float m_scrollY = 0.0f;
    };

    class UITextInput : public UIWidget
    {
      public:
        explicit UITextInput(const std::string& name);

        void SetText(const std::string& text);
        [[nodiscard]] const std::string& GetText() const;

        void SetPlaceholder(const std::string& placeholder);
        void SetMaxLength(uint32_t maxLen);
        void SetReadOnly(bool readOnly);

        void OnTextChanged(std::function<void(const std::string&)> callback);

        void SetCursorPosition(uint32_t pos);
        [[nodiscard]] uint32_t GetCursorPosition() const;

        void SelectAll();
        [[nodiscard]] bool IsFocused() const;
        void SetFocused(bool focused);

      private:
        std::string m_text;
        std::string m_placeholder;
        uint32_t m_maxLength = 0;
        uint32_t m_cursorPos = 0;
        uint32_t m_selectionStart = 0;
        uint32_t m_selectionEnd = 0;
        bool m_readOnly = false;
        bool m_focused = false;
        std::function<void(const std::string&)> m_onTextChanged;
    };

    class UISlider : public UIWidget
    {
      public:
        explicit UISlider(const std::string& name);

        void SetRange(float minVal, float maxVal);
        void SetValue(float value);

        [[nodiscard]] float GetValue() const;
        void SetStep(float step);

        void OnValueChanged(std::function<void(float)> callback);
        void SetOrientation(LayoutDirection orientation);

      private:
        float m_min = 0.0f;
        float m_max = 1.0f;
        float m_value = 0.0f;
        float m_step = 0.0f;
        LayoutDirection m_orientation = LayoutDirection::Horizontal;
        std::function<void(float)> m_onValueChanged;
    };

    class UIDropdown : public UIWidget
    {
      public:
        explicit UIDropdown(const std::string& name);

        void AddOption(const std::string& label, const std::string& value);
        void RemoveOption(const std::string& value);

        void SetSelectedIndex(uint32_t index);
        [[nodiscard]] uint32_t GetSelectedIndex() const;
        [[nodiscard]] std::string GetSelectedValue() const;

        void OnSelectionChanged(std::function<void(uint32_t, const std::string&)> callback);
        void SetPlaceholder(const std::string& placeholder);

        [[nodiscard]] bool IsOpen() const;
        void Toggle();

      private:
        struct Option
        {
            std::string label;
            std::string value;
        };

        std::vector<Option> m_options;
        uint32_t m_selectedIndex = 0;
        bool m_open = false;
        std::string m_placeholder;
        std::function<void(uint32_t, const std::string&)> m_onSelectionChanged;
    };

    template <typename T> class UIDataBinding
    {
      public:
        UIDataBinding() = default;

        // Intentionally inline: this is a templated leaf type used across TU boundaries.
        void Bind(T* source) { m_source = source; }
        void SetFormatter(std::function<std::string(const T&)> formatter) { m_formatter = std::move(formatter); }

        void Update()
        {
            if (m_source)
            {
                m_cachedValue = *m_source;
                if (m_formatter)
                {
                    m_formattedText = m_formatter(m_cachedValue);
                }
            }
        }

        [[nodiscard]] const T& GetBoundValue() const { return m_cachedValue; }
        [[nodiscard]] const std::string& GetFormattedText() const { return m_formattedText; }

      private:
        T* m_source = nullptr;
        T m_cachedValue{};
        std::function<std::string(const T&)> m_formatter;
        std::string m_formattedText;
    };

    class UILayoutLoader
    {
      public:
        static bool LoadFromJSON(std::string_view json, UIPanel* parent);

      private:
        static size_t FindMatchingBrace(std::string_view json, size_t pos);
        static std::string ExtractString(std::string_view block, std::string_view key);
        static float ExtractFloat(std::string_view block, std::string_view key, float fallback);
        static void ParseWidgetBlock(std::string_view block, UIPanel* parent);
    };

} // namespace Spark::UI
