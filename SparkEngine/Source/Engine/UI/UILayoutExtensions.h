/**
 * @file UILayoutExtensions.h
 * @brief Declarative UI layout containers, input widgets, and data binding
 * @author Spark Engine Team
 * @date 2026
 *
 * Extends the base UISystem with advanced layout primitives (flex, grid, scroll),
 * interactive input widgets (text input, slider, dropdown), and a simple data
 * binding mechanism. All classes live in `Spark::UI` and build on UIWidget/UIPanel
 * from UISystem.h.
 *
 * ## Usage
 * @code
 *   using namespace Spark::UI;
 *
 *   UIFlexContainer flex("toolbar");
 *   flex.SetDirection(LayoutDirection::Horizontal);
 *   flex.SetAlignment(FlexAlign::Center);
 *   flex.SetSpacing(8.0f);
 *   flex.AddChild(&saveButton);
 *   flex.AddChild(&loadButton);
 *   flex.CalculateLayout();
 *
 *   UISlider volumeSlider("volume");
 *   volumeSlider.SetRange(0.0f, 1.0f);
 *   volumeSlider.SetValue(0.75f);
 *   volumeSlider.OnValueChanged([](float v) { SetMasterVolume(v); });
 * @endcode
 */

#pragma once

#include "UISystem.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Spark::UI
{

    // =========================================================================
    // Enums
    // =========================================================================

    /**
     * @brief Flex alignment for children within a container.
     */
    enum class FlexAlign : uint8_t
    {
        Start,        ///< Pack children toward the start edge
        Center,       ///< Center children along the axis
        End,          ///< Pack children toward the end edge
        Stretch,      ///< Stretch children to fill the axis
        SpaceBetween, ///< Equal space between children, none at edges
        SpaceAround   ///< Equal space around each child
    };

    // =========================================================================
    // Padding helper
    // =========================================================================

    /**
     * @brief Four-sided padding values.
     */
    struct Padding
    {
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        float left = 0.0f;
    };

    // =========================================================================
    // UIFlexContainer
    // =========================================================================

    /**
     * @brief Flex-box style container that distributes children along a direction.
     */
    class UIFlexContainer : public UIWidget
    {
      public:
        explicit UIFlexContainer(const std::string& name) : UIWidget(name) {}

        /// @brief Set the primary layout direction
        void SetDirection(LayoutDirection direction) { m_direction = direction; }

        /// @brief Get the current layout direction
        [[nodiscard]] LayoutDirection GetDirection() const { return m_direction; }

        /// @brief Set cross-axis and distribution alignment
        void SetAlignment(FlexAlign align) { m_alignment = align; }

        /// @brief Get the current alignment
        [[nodiscard]] FlexAlign GetAlignment() const { return m_alignment; }

        /// @brief Set spacing between children (in pixels)
        void SetSpacing(float spacing) { m_spacing = spacing; }

        /// @brief Set padding on all four sides
        void SetPadding(float top, float right, float bottom, float left) { m_padding = {top, right, bottom, left}; }

        /// @brief Add a child widget (non-owning pointer)
        void AddChild(UIWidget* child)
        {
            if (child)
            {
                m_children.push_back(child);
            }
        }

        /// @brief Distribute children according to direction, alignment, and spacing
        void CalculateLayout()
        {
            if (m_children.empty())
            {
                return;
            }

            const float startX = m_x + m_padding.left;
            const float startY = m_y + m_padding.top;
            const float availW = m_width - m_padding.left - m_padding.right;
            const float availH = m_height - m_padding.top - m_padding.bottom;

            if (m_direction == LayoutDirection::Horizontal)
            {
                LayoutAxis(startX, startY, availW, availH, true);
            }
            else
            {
                LayoutAxis(startX, startY, availW, availH, false);
            }
        }

      private:
        void LayoutAxis(float startX, float startY, float availW, float availH, bool horizontal)
        {
            const auto count = static_cast<uint32_t>(m_children.size());
            float totalChildSize = 0.0f;

            for (const auto* child : m_children)
            {
                totalChildSize += horizontal ? child->GetWidth() : child->GetHeight();
            }

            float totalSpacing = m_spacing * static_cast<float>(count > 1 ? count - 1 : 0);
            float freeSpace = (horizontal ? availW : availH) - totalChildSize;

            float offset = 0.0f;
            float gap = m_spacing;

            switch (m_alignment)
            {
            case FlexAlign::Start:
                offset = 0.0f;
                break;
            case FlexAlign::Center:
                offset = (freeSpace - totalSpacing) * 0.5f;
                break;
            case FlexAlign::End:
                offset = freeSpace - totalSpacing;
                break;
            case FlexAlign::SpaceBetween:
                gap = (count > 1) ? freeSpace / static_cast<float>(count - 1) : 0.0f;
                offset = 0.0f;
                break;
            case FlexAlign::SpaceAround:
            {
                float pad = freeSpace / static_cast<float>(count * 2);
                gap = pad * 2.0f;
                offset = pad;
                break;
            }
            case FlexAlign::Stretch:
                offset = 0.0f;
                break;
            }

            float cursor = offset;
            for (auto* child : m_children)
            {
                if (horizontal)
                {
                    child->SetPosition(startX + cursor, startY);
                    if (m_alignment == FlexAlign::Stretch)
                    {
                        child->SetSize(child->GetWidth(), availH);
                    }
                    cursor += child->GetWidth() + gap;
                }
                else
                {
                    child->SetPosition(startX, startY + cursor);
                    if (m_alignment == FlexAlign::Stretch)
                    {
                        child->SetSize(availW, child->GetHeight());
                    }
                    cursor += child->GetHeight() + gap;
                }
            }
        }

        LayoutDirection m_direction = LayoutDirection::Horizontal;
        FlexAlign m_alignment = FlexAlign::Start;
        float m_spacing = 0.0f;
        Padding m_padding{};
        std::vector<UIWidget*> m_children;
    };

    // =========================================================================
    // UIGridLayout
    // =========================================================================

    /**
     * @brief Grid-based layout that places children into rows and columns.
     */
    class UIGridLayout : public UIWidget
    {
      public:
        explicit UIGridLayout(const std::string& name) : UIWidget(name) {}

        /// @brief Set the number of columns
        void SetColumns(uint32_t cols) { m_columns = (cols > 0) ? cols : 1; }

        /// @brief Set the number of rows
        void SetRows(uint32_t rows) { m_rows = (rows > 0) ? rows : 1; }

        /// @brief Set spacing between cells (pixels)
        void SetCellSpacing(float spacing) { m_cellSpacing = spacing; }

        /// @brief Place a widget in a specific grid cell (non-owning pointer)
        void SetCell(uint32_t row, uint32_t col, UIWidget* widget)
        {
            if (row < m_rows && col < m_columns && widget)
            {
                uint32_t index = row * m_columns + col;
                if (index >= m_cells.size())
                {
                    m_cells.resize(static_cast<size_t>(index) + 1, nullptr);
                }
                m_cells[index] = widget;
            }
        }

        /// @brief Compute child positions based on uniform cell sizes
        void CalculateLayout()
        {
            if (m_columns == 0 || m_rows == 0)
            {
                return;
            }

            float cellW = (m_width - m_cellSpacing * static_cast<float>(m_columns - 1)) / static_cast<float>(m_columns);
            float cellH = (m_height - m_cellSpacing * static_cast<float>(m_rows - 1)) / static_cast<float>(m_rows);

            for (uint32_t r = 0; r < m_rows; ++r)
            {
                for (uint32_t c = 0; c < m_columns; ++c)
                {
                    uint32_t index = r * m_columns + c;
                    if (index < m_cells.size() && m_cells[index])
                    {
                        float posX = m_x + static_cast<float>(c) * (cellW + m_cellSpacing);
                        float posY = m_y + static_cast<float>(r) * (cellH + m_cellSpacing);
                        m_cells[index]->SetPosition(posX, posY);
                        m_cells[index]->SetSize(cellW, cellH);
                    }
                }
            }
        }

      private:
        uint32_t m_columns = 1;
        uint32_t m_rows = 1;
        float m_cellSpacing = 4.0f;
        std::vector<UIWidget*> m_cells;
    };

    // =========================================================================
    // UIScrollView
    // =========================================================================

    /**
     * @brief Scrollable viewport over content larger than the visible area.
     */
    class UIScrollView : public UIWidget
    {
      public:
        explicit UIScrollView(const std::string& name) : UIWidget(name) {}

        /// @brief Set the total size of scrollable content
        void SetContentSize(float w, float h)
        {
            m_contentW = w;
            m_contentH = h;
        }

        /// @brief Set the visible viewport size
        void SetViewportSize(float w, float h)
        {
            m_width = w;
            m_height = h;
        }

        /// @brief Set absolute scroll offset
        void SetScrollOffset(float x, float y)
        {
            m_scrollX = std::clamp(x, 0.0f, GetMaxScrollX());
            m_scrollY = std::clamp(y, 0.0f, GetMaxScrollY());
        }

        /// @brief Get current scroll offset
        /// @return Pair of (offsetX, offsetY)
        [[nodiscard]] std::pair<float, float> GetScrollOffset() const { return {m_scrollX, m_scrollY}; }

        /// @brief Get maximum scroll offset
        /// @return Pair of (maxX, maxY)
        [[nodiscard]] std::pair<float, float> GetMaxScroll() const { return {GetMaxScrollX(), GetMaxScrollY()}; }

        /// @brief Scroll by a delta amount, clamping to valid range
        void ScrollBy(float dx, float dy) { SetScrollOffset(m_scrollX + dx, m_scrollY + dy); }

        /// @brief Returns true if content exceeds the viewport in either axis
        [[nodiscard]] bool IsScrollable() const { return m_contentW > m_width || m_contentH > m_height; }

        /// @brief Get the visible rectangle in content-space coordinates
        /// @return {x, y, width, height}
        struct Rect
        {
            float x, y, w, h;
        };

        [[nodiscard]] Rect GetVisibleRect() const { return {m_scrollX, m_scrollY, m_width, m_height}; }

      private:
        [[nodiscard]] float GetMaxScrollX() const { return std::max(0.0f, m_contentW - m_width); }

        [[nodiscard]] float GetMaxScrollY() const { return std::max(0.0f, m_contentH - m_height); }

        float m_contentW = 0.0f;
        float m_contentH = 0.0f;
        float m_scrollX = 0.0f;
        float m_scrollY = 0.0f;
    };

    // =========================================================================
    // UITextInput
    // =========================================================================

    /**
     * @brief Single-line text input widget with cursor and selection.
     */
    class UITextInput : public UIWidget
    {
      public:
        explicit UITextInput(const std::string& name) : UIWidget(name) {}

        /// @brief Set the current text content
        void SetText(const std::string& text)
        {
            if (m_readOnly)
            {
                return;
            }
            m_text = text;
            if (m_maxLength > 0 && m_text.size() > m_maxLength)
            {
                m_text.resize(m_maxLength);
            }
            m_cursorPos = std::min(m_cursorPos, static_cast<uint32_t>(m_text.size()));
            if (m_onTextChanged)
            {
                m_onTextChanged(m_text);
            }
        }

        /// @brief Get the current text content
        [[nodiscard]] const std::string& GetText() const { return m_text; }

        /// @brief Set placeholder text shown when input is empty
        void SetPlaceholder(const std::string& placeholder) { m_placeholder = placeholder; }

        /// @brief Set maximum character length (0 = unlimited)
        void SetMaxLength(uint32_t maxLen) { m_maxLength = maxLen; }

        /// @brief Set read-only mode
        void SetReadOnly(bool readOnly) { m_readOnly = readOnly; }

        /// @brief Register a callback for text changes
        void OnTextChanged(std::function<void(const std::string&)> callback) { m_onTextChanged = std::move(callback); }

        /// @brief Set cursor position (clamped to text length)
        void SetCursorPosition(uint32_t pos) { m_cursorPos = std::min(pos, static_cast<uint32_t>(m_text.size())); }

        /// @brief Get current cursor position
        [[nodiscard]] uint32_t GetCursorPosition() const { return m_cursorPos; }

        /// @brief Select all text
        void SelectAll()
        {
            m_selectionStart = 0;
            m_selectionEnd = static_cast<uint32_t>(m_text.size());
        }

        /// @brief Returns true if this input currently has focus
        [[nodiscard]] bool IsFocused() const { return m_focused; }

        /// @brief Set focus state
        void SetFocused(bool focused) { m_focused = focused; }

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

    // =========================================================================
    // UISlider
    // =========================================================================

    /**
     * @brief Numeric slider widget with configurable range and step.
     */
    class UISlider : public UIWidget
    {
      public:
        explicit UISlider(const std::string& name) : UIWidget(name) {}

        /// @brief Set the valid range for the slider value
        void SetRange(float minVal, float maxVal)
        {
            m_min = minVal;
            m_max = maxVal;
            SetValue(m_value); // re-clamp
        }

        /// @brief Set the current slider value (clamped to range)
        void SetValue(float value)
        {
            float clamped = std::clamp(value, m_min, m_max);
            if (m_step > 0.0f)
            {
                float steps = std::round((clamped - m_min) / m_step);
                clamped = m_min + steps * m_step;
                clamped = std::clamp(clamped, m_min, m_max);
            }
            if (clamped != m_value)
            {
                m_value = clamped;
                if (m_onValueChanged)
                {
                    m_onValueChanged(m_value);
                }
            }
        }

        /// @brief Get the current slider value
        [[nodiscard]] float GetValue() const { return m_value; }

        /// @brief Set the step increment (0 = continuous)
        void SetStep(float step) { m_step = (step >= 0.0f) ? step : 0.0f; }

        /// @brief Register a callback for value changes
        void OnValueChanged(std::function<void(float)> callback) { m_onValueChanged = std::move(callback); }

        /// @brief Set slider orientation
        void SetOrientation(LayoutDirection orientation) { m_orientation = orientation; }

      private:
        float m_min = 0.0f;
        float m_max = 1.0f;
        float m_value = 0.0f;
        float m_step = 0.0f;
        LayoutDirection m_orientation = LayoutDirection::Horizontal;
        std::function<void(float)> m_onValueChanged;
    };

    // =========================================================================
    // UIDropdown
    // =========================================================================

    /**
     * @brief Dropdown selection widget with labeled options.
     */
    class UIDropdown : public UIWidget
    {
      public:
        explicit UIDropdown(const std::string& name) : UIWidget(name) {}

        /// @brief Add a selectable option
        void AddOption(const std::string& label, const std::string& value) { m_options.push_back({label, value}); }

        /// @brief Remove an option by its value string
        void RemoveOption(const std::string& value)
        {
            auto it =
                std::remove_if(m_options.begin(), m_options.end(), [&](const Option& o) { return o.value == value; });
            if (it != m_options.end())
            {
                bool removedSelected = false;
                for (auto check = it; check != m_options.end(); ++check)
                {
                    auto idx = static_cast<uint32_t>(std::distance(m_options.begin(), check));
                    if (idx == m_selectedIndex)
                    {
                        removedSelected = true;
                    }
                }
                m_options.erase(it, m_options.end());
                if (removedSelected || m_selectedIndex >= m_options.size())
                {
                    m_selectedIndex = m_options.empty() ? 0 : 0;
                }
            }
        }

        /// @brief Set the selected option by index
        void SetSelectedIndex(uint32_t index)
        {
            if (index < m_options.size() && index != m_selectedIndex)
            {
                m_selectedIndex = index;
                if (m_onSelectionChanged)
                {
                    m_onSelectionChanged(m_selectedIndex, m_options[m_selectedIndex].value);
                }
            }
        }

        /// @brief Get the selected option index
        [[nodiscard]] uint32_t GetSelectedIndex() const { return m_selectedIndex; }

        /// @brief Get the value string of the selected option
        [[nodiscard]] std::string GetSelectedValue() const
        {
            if (m_selectedIndex < m_options.size())
            {
                return m_options[m_selectedIndex].value;
            }
            return {};
        }

        /// @brief Register a callback for selection changes
        void OnSelectionChanged(std::function<void(uint32_t, const std::string&)> callback)
        {
            m_onSelectionChanged = std::move(callback);
        }

        /// @brief Set placeholder text shown when no option is selected
        void SetPlaceholder(const std::string& placeholder) { m_placeholder = placeholder; }

        /// @brief Returns true if the dropdown list is open
        [[nodiscard]] bool IsOpen() const { return m_open; }

        /// @brief Toggle open/closed state
        void Toggle() { m_open = !m_open; }

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

    // =========================================================================
    // UIDataBinding
    // =========================================================================

    /**
     * @brief Simple one-way data binding that reads a source value and formats it.
     *
     * Binds a pointer to a data source. On Update(), reads the current value and
     * invokes the formatter to produce a string suitable for display in a widget.
     *
     * @tparam T The data type being bound
     */
    template <typename T> class UIDataBinding
    {
      public:
        UIDataBinding() = default;

        /// @brief Bind to a data source (non-owning pointer)
        void Bind(T* source) { m_source = source; }

        /// @brief Set a formatter that converts the bound value to display text
        void SetFormatter(std::function<std::string(const T&)> formatter) { m_formatter = std::move(formatter); }

        /// @brief Read the source value and cache the formatted string
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

        /// @brief Get the last-read bound value
        [[nodiscard]] const T& GetBoundValue() const { return m_cachedValue; }

        /// @brief Get the formatted text from the last Update()
        [[nodiscard]] const std::string& GetFormattedText() const { return m_formattedText; }

      private:
        T* m_source = nullptr;
        T m_cachedValue{};
        std::function<std::string(const T&)> m_formatter;
        std::string m_formattedText;
    };

    // =========================================================================
    // UILayoutLoader
    // =========================================================================

    /**
     * @brief Loads a simple JSON layout description into a UIPanel hierarchy.
     *
     * Supports a minimal JSON subset describing widget trees. Each node has
     * "type", "name", and optional properties ("x", "y", "w", "h", "text", "children").
     */
    class UILayoutLoader
    {
      public:
        /**
         * @brief Parse a JSON layout string and create widgets under a parent panel.
         * @param json JSON layout string
         * @param parent Parent panel to populate
         * @return true if parsing succeeded
         *
         * Expected JSON format:
         * @code
         * {
         *   "children": [
         *     { "type": "label", "name": "title", "text": "Hello", "x": 10, "y": 10 },
         *     { "type": "button", "name": "ok", "text": "OK", "x": 10, "y": 40 },
         *     { "type": "panel", "name": "sub", "children": [...] }
         *   ]
         * }
         * @endcode
         */
        static bool LoadFromJSON(std::string_view json, UIPanel* parent)
        {
            if (!parent || json.empty())
            {
                return false;
            }

            // Find the "children" array and process each element
            auto childrenPos = json.find("\"children\"");
            if (childrenPos == std::string_view::npos)
            {
                return false;
            }

            auto arrayStart = json.find('[', childrenPos);
            if (arrayStart == std::string_view::npos)
            {
                return false;
            }

            // Walk through each { ... } block inside the array
            size_t pos = arrayStart + 1;
            while (pos < json.size())
            {
                auto objStart = json.find('{', pos);
                if (objStart == std::string_view::npos)
                {
                    break;
                }

                auto objEnd = FindMatchingBrace(json, objStart);
                if (objEnd == std::string_view::npos)
                {
                    return false;
                }

                auto block = json.substr(objStart, objEnd - objStart + 1);
                ParseWidgetBlock(block, parent);
                pos = objEnd + 1;
            }

            return true;
        }

      private:
        /// @brief Find the closing brace matching the opening brace at pos
        static size_t FindMatchingBrace(std::string_view json, size_t pos)
        {
            int depth = 0;
            bool inString = false;
            for (size_t i = pos; i < json.size(); ++i)
            {
                char c = json[i];
                if (c == '"' && (i == 0 || json[i - 1] != '\\'))
                {
                    inString = !inString;
                }
                if (inString)
                {
                    continue;
                }
                if (c == '{')
                {
                    ++depth;
                }
                else if (c == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        return i;
                    }
                }
            }
            return std::string_view::npos;
        }

        /// @brief Extract a string value for a given key from a JSON-like block
        static std::string ExtractString(std::string_view block, std::string_view key)
        {
            std::string searchKey = "\"" + std::string(key) + "\"";
            auto keyPos = block.find(searchKey);
            if (keyPos == std::string_view::npos)
            {
                return {};
            }
            auto colonPos = block.find(':', keyPos + searchKey.size());
            if (colonPos == std::string_view::npos)
            {
                return {};
            }
            auto quoteStart = block.find('"', colonPos + 1);
            if (quoteStart == std::string_view::npos)
            {
                return {};
            }
            auto quoteEnd = block.find('"', quoteStart + 1);
            if (quoteEnd == std::string_view::npos)
            {
                return {};
            }
            return std::string(block.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
        }

        /// @brief Extract a numeric value for a given key
        static float ExtractFloat(std::string_view block, std::string_view key, float fallback)
        {
            std::string searchKey = "\"" + std::string(key) + "\"";
            auto keyPos = block.find(searchKey);
            if (keyPos == std::string_view::npos)
            {
                return fallback;
            }
            auto colonPos = block.find(':', keyPos + searchKey.size());
            if (colonPos == std::string_view::npos)
            {
                return fallback;
            }
            // Skip whitespace
            size_t numStart = colonPos + 1;
            while (numStart < block.size() && (block[numStart] == ' ' || block[numStart] == '\t'))
            {
                ++numStart;
            }
            std::string numStr;
            while (numStart < block.size() && (std::isdigit(static_cast<unsigned char>(block[numStart])) ||
                                               block[numStart] == '.' || block[numStart] == '-'))
            {
                numStr += block[numStart++];
            }
            if (numStr.empty())
            {
                return fallback;
            }
            try
            {
                return std::stof(numStr);
            }
            catch (...)
            {
                return fallback;
            }
        }

        /// @brief Parse a single widget block and add it to the parent
        static void ParseWidgetBlock(std::string_view block, UIPanel* parent)
        {
            std::string type = ExtractString(block, "type");
            std::string widgetName = ExtractString(block, "name");
            std::string text = ExtractString(block, "text");

            if (widgetName.empty())
            {
                return;
            }

            UIWidget* created = nullptr;

            if (type == "label")
            {
                created = parent->CreateLabel(widgetName, text);
            }
            else if (type == "button")
            {
                auto* btn = parent->CreateButton(widgetName, text);
                created = btn;
            }
            else if (type == "progressbar")
            {
                created = parent->CreateProgressBar(widgetName);
            }
            else if (type == "image")
            {
                std::string src = ExtractString(block, "src");
                created = parent->CreateImage(widgetName, src);
            }
            else if (type == "panel")
            {
                auto* panel = parent->CreatePanel(widgetName);
                created = panel;
                // Recursively load children
                LoadFromJSON(block, panel);
            }

            if (created)
            {
                float xPos = ExtractFloat(block, "x", created->GetX());
                float yPos = ExtractFloat(block, "y", created->GetY());
                float w = ExtractFloat(block, "w", created->GetWidth());
                float h = ExtractFloat(block, "h", created->GetHeight());
                created->SetPosition(xPos, yPos);
                created->SetSize(w, h);
            }
        }
    };

} // namespace Spark::UI
