#include "UILayoutExtensions.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace Spark::UI
{

    UIFlexContainer::UIFlexContainer(const std::string& name) : UIWidget(name) {}

    void UIFlexContainer::SetDirection(LayoutDirection direction)
    {
        m_direction = direction;
    }
    LayoutDirection UIFlexContainer::GetDirection() const
    {
        return m_direction;
    }
    void UIFlexContainer::SetAlignment(FlexAlign align)
    {
        m_alignment = align;
    }
    FlexAlign UIFlexContainer::GetAlignment() const
    {
        return m_alignment;
    }
    void UIFlexContainer::SetSpacing(float spacing)
    {
        m_spacing = spacing;
    }
    void UIFlexContainer::SetPadding(float top, float right, float bottom, float left)
    {
        m_padding = {top, right, bottom, left};
    }

    void UIFlexContainer::AddChild(UIWidget* child)
    {
        if (child)
        {
            m_children.push_back(child);
        }
    }

    void UIFlexContainer::CalculateLayout()
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

    void UIFlexContainer::LayoutAxis(float startX, float startY, float availW, float availH, bool horizontal)
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

    UIGridLayout::UIGridLayout(const std::string& name) : UIWidget(name) {}
    void UIGridLayout::SetColumns(uint32_t cols)
    {
        m_columns = (cols > 0) ? cols : 1;
    }
    void UIGridLayout::SetRows(uint32_t rows)
    {
        m_rows = (rows > 0) ? rows : 1;
    }
    void UIGridLayout::SetCellSpacing(float spacing)
    {
        m_cellSpacing = spacing;
    }

    void UIGridLayout::SetCell(uint32_t row, uint32_t col, UIWidget* widget)
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

    void UIGridLayout::CalculateLayout()
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

    UIScrollView::UIScrollView(const std::string& name) : UIWidget(name) {}
    void UIScrollView::SetContentSize(float w, float h)
    {
        m_contentW = w;
        m_contentH = h;
    }
    void UIScrollView::SetViewportSize(float w, float h)
    {
        m_width = w;
        m_height = h;
    }

    void UIScrollView::SetScrollOffset(float x, float y)
    {
        m_scrollX = std::clamp(x, 0.0f, GetMaxScrollX());
        m_scrollY = std::clamp(y, 0.0f, GetMaxScrollY());
    }

    std::pair<float, float> UIScrollView::GetScrollOffset() const
    {
        return {m_scrollX, m_scrollY};
    }
    std::pair<float, float> UIScrollView::GetMaxScroll() const
    {
        return {GetMaxScrollX(), GetMaxScrollY()};
    }
    void UIScrollView::ScrollBy(float dx, float dy)
    {
        SetScrollOffset(m_scrollX + dx, m_scrollY + dy);
    }
    bool UIScrollView::IsScrollable() const
    {
        return m_contentW > m_width || m_contentH > m_height;
    }
    UIScrollView::Rect UIScrollView::GetVisibleRect() const
    {
        return {m_scrollX, m_scrollY, m_width, m_height};
    }
    float UIScrollView::GetMaxScrollX() const
    {
        return std::max(0.0f, m_contentW - m_width);
    }
    float UIScrollView::GetMaxScrollY() const
    {
        return std::max(0.0f, m_contentH - m_height);
    }

    UITextInput::UITextInput(const std::string& name) : UIWidget(name) {}

    void UITextInput::SetText(const std::string& text)
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

    const std::string& UITextInput::GetText() const
    {
        return m_text;
    }
    void UITextInput::SetPlaceholder(const std::string& placeholder)
    {
        m_placeholder = placeholder;
    }
    void UITextInput::SetMaxLength(uint32_t maxLen)
    {
        m_maxLength = maxLen;
    }
    void UITextInput::SetReadOnly(bool readOnly)
    {
        m_readOnly = readOnly;
    }
    void UITextInput::OnTextChanged(std::function<void(const std::string&)> callback)
    {
        m_onTextChanged = std::move(callback);
    }
    void UITextInput::SetCursorPosition(uint32_t pos)
    {
        m_cursorPos = std::min(pos, static_cast<uint32_t>(m_text.size()));
    }
    uint32_t UITextInput::GetCursorPosition() const
    {
        return m_cursorPos;
    }
    void UITextInput::SelectAll()
    {
        m_selectionStart = 0;
        m_selectionEnd = static_cast<uint32_t>(m_text.size());
    }
    bool UITextInput::IsFocused() const
    {
        return m_focused;
    }
    void UITextInput::SetFocused(bool focused)
    {
        m_focused = focused;
    }

    UISlider::UISlider(const std::string& name) : UIWidget(name) {}

    void UISlider::SetRange(float minVal, float maxVal)
    {
        m_min = minVal;
        m_max = maxVal;
        SetValue(m_value);
    }

    void UISlider::SetValue(float value)
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

    float UISlider::GetValue() const
    {
        return m_value;
    }
    void UISlider::SetStep(float step)
    {
        m_step = (step >= 0.0f) ? step : 0.0f;
    }
    void UISlider::OnValueChanged(std::function<void(float)> callback)
    {
        m_onValueChanged = std::move(callback);
    }
    void UISlider::SetOrientation(LayoutDirection orientation)
    {
        m_orientation = orientation;
    }

    UIDropdown::UIDropdown(const std::string& name) : UIWidget(name) {}

    void UIDropdown::AddOption(const std::string& label, const std::string& value)
    {
        m_options.push_back({label, value});
    }

    void UIDropdown::RemoveOption(const std::string& value)
    {
        // Preserve the currently selected option's value so selection can be
        // restored by identity after the erase — removing an option positioned
        // before the selected one shifts every later index down by one.
        std::string selectedValue;
        if (m_selectedIndex < m_options.size())
        {
            selectedValue = m_options[m_selectedIndex].value;
        }

        auto it = std::remove_if(m_options.begin(), m_options.end(), [&](const Option& o) { return o.value == value; });
        if (it == m_options.end())
        {
            return; // nothing matched — leave selection untouched
        }
        m_options.erase(it, m_options.end());

        // Re-locate the previously selected value; its index may have shifted.
        uint32_t newIndex = 0;
        bool stillPresent = false;
        for (uint32_t i = 0; i < m_options.size(); ++i)
        {
            if (m_options[i].value == selectedValue)
            {
                newIndex = i;
                stillPresent = true;
                break;
            }
        }

        m_selectedIndex = newIndex;

        // The effective selection only changes when the option that was
        // selected is the one that got removed. Notify listeners in that case.
        if (!stillPresent && m_onSelectionChanged && !m_options.empty())
        {
            m_onSelectionChanged(m_selectedIndex, m_options[m_selectedIndex].value);
        }
    }

    void UIDropdown::SetSelectedIndex(uint32_t index)
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

    uint32_t UIDropdown::GetSelectedIndex() const
    {
        return m_selectedIndex;
    }

    std::string UIDropdown::GetSelectedValue() const
    {
        if (m_selectedIndex < m_options.size())
        {
            return m_options[m_selectedIndex].value;
        }
        return {};
    }

    void UIDropdown::OnSelectionChanged(std::function<void(uint32_t, const std::string&)> callback)
    {
        m_onSelectionChanged = std::move(callback);
    }

    void UIDropdown::SetPlaceholder(const std::string& placeholder)
    {
        m_placeholder = placeholder;
    }
    bool UIDropdown::IsOpen() const
    {
        return m_open;
    }
    void UIDropdown::Toggle()
    {
        m_open = !m_open;
    }

    bool UILayoutLoader::LoadFromJSON(std::string_view json, UIPanel* parent)
    {
        if (!parent || json.empty())
        {
            return false;
        }

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

    size_t UILayoutLoader::FindMatchingBrace(std::string_view json, size_t pos)
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

    std::string UILayoutLoader::ExtractString(std::string_view block, std::string_view key)
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

    float UILayoutLoader::ExtractFloat(std::string_view block, std::string_view key, float fallback)
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

    void UILayoutLoader::ParseWidgetBlock(std::string_view block, UIPanel* parent)
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

} // namespace Spark::UI
