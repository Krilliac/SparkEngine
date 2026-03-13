/**
 * @file UISystem.cpp
 * @brief Implementation of the runtime UI/Widget system
 */

#include "UISystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <sstream>

namespace Spark::UI
{

    // =============================================================================
    // UIWidget
    // =============================================================================

    UIWidget::UIWidget(const std::string& name) : m_name(name) {}

    void UIWidget::Update(float deltaTime)
    {
        (void)deltaTime;
    }

    void UIWidget::Render() const
    {
        // Base class does nothing — subclasses override
    }

    bool UIWidget::HandleClick(float x, float y)
    {
        (void)x;
        (void)y;
        return false;
    }

    // =============================================================================
    // UILabel
    // =============================================================================

    UILabel::UILabel(const std::string& name, const std::string& text) : UIWidget(name), m_text(text) {}

    void UILabel::Render() const
    {
        if (!m_visible)
        {
            return;
        }
        // Rendering is handled by the graphics backend (D3D11 text rendering)
        // This provides the data; the render system reads it
    }

    // =============================================================================
    // UIButton
    // =============================================================================

    UIButton::UIButton(const std::string& name, const std::string& label) : UIWidget(name), m_label(label) {}

    bool UIButton::HandleClick(float x, float y)
    {
        if (!m_visible)
        {
            return false;
        }

        bool inside = x >= m_x && x <= m_x + m_width && y >= m_y && y <= m_y + m_height;

        if (inside && m_onClick)
        {
            m_onClick();
            return true;
        }
        return false;
    }

    void UIButton::Render() const
    {
        if (!m_visible)
        {
            return;
        }
        // Draw button background and label text
        // Actual rendering delegated to graphics backend
    }

    // =============================================================================
    // UIProgressBar
    // =============================================================================

    UIProgressBar::UIProgressBar(const std::string& name) : UIWidget(name) {}

    void UIProgressBar::Render() const
    {
        if (!m_visible)
        {
            return;
        }
        // Draw background rect, then filled rect at m_value * m_width
    }

    // =============================================================================
    // UIImageWidget
    // =============================================================================

    UIImageWidget::UIImageWidget(const std::string& name, const std::string& texturePath)
        : UIWidget(name), m_texturePath(texturePath)
    {
    }

    void UIImageWidget::Render() const
    {
        if (!m_visible)
        {
            return;
        }
        // Draw textured quad with tint
    }

    // =============================================================================
    // UIPanel
    // =============================================================================

    UIPanel::UIPanel(const std::string& name) : UIWidget(name) {}

    UILabel* UIPanel::CreateLabel(const std::string& name, const std::string& text)
    {
        auto widget = std::make_unique<UILabel>(name, text);
        auto* ptr = widget.get();
        m_children.push_back(std::move(widget));
        return ptr;
    }

    UIButton* UIPanel::CreateButton(const std::string& name, const std::string& label)
    {
        auto widget = std::make_unique<UIButton>(name, label);
        auto* ptr = widget.get();
        m_children.push_back(std::move(widget));
        return ptr;
    }

    UIProgressBar* UIPanel::CreateProgressBar(const std::string& name)
    {
        auto widget = std::make_unique<UIProgressBar>(name);
        auto* ptr = widget.get();
        m_children.push_back(std::move(widget));
        return ptr;
    }

    UIImageWidget* UIPanel::CreateImage(const std::string& name, const std::string& texturePath)
    {
        auto widget = std::make_unique<UIImageWidget>(name, texturePath);
        auto* ptr = widget.get();
        m_children.push_back(std::move(widget));
        return ptr;
    }

    UIPanel* UIPanel::CreatePanel(const std::string& name)
    {
        auto widget = std::make_unique<UIPanel>(name);
        auto* ptr = widget.get();
        m_children.push_back(std::move(widget));
        return ptr;
    }

    UIWidget* UIPanel::FindWidget(const std::string& name)
    {
        for (auto& child : m_children)
        {
            if (child->GetName() == name)
            {
                return child.get();
            }
            if (auto* panel = dynamic_cast<UIPanel*>(child.get()))
            {
                if (auto* found = panel->FindWidget(name))
                {
                    return found;
                }
            }
        }
        return nullptr;
    }

    void UIPanel::RemoveWidget(const std::string& name)
    {
        m_children.erase(std::remove_if(m_children.begin(), m_children.end(),
                                        [&name](const std::unique_ptr<UIWidget>& w) { return w->GetName() == name; }),
                         m_children.end());
    }

    void UIPanel::Update(float deltaTime)
    {
        if (!m_visible)
        {
            return;
        }

        // Auto-layout children if layout direction is set
        if (m_layout != LayoutDirection::None)
        {
            float offset = m_padding;
            for (auto& child : m_children)
            {
                if (!child->IsVisible())
                {
                    continue;
                }
                if (m_layout == LayoutDirection::Vertical)
                {
                    child->SetPosition(m_padding, offset);
                    offset += child->GetHeight() + m_spacing;
                }
                else
                {
                    child->SetPosition(offset, m_padding);
                    offset += child->GetWidth() + m_spacing;
                }
            }
        }

        for (auto& child : m_children)
        {
            child->Update(deltaTime);
        }
    }

    void UIPanel::Render() const
    {
        if (!m_visible)
        {
            return;
        }

        // Draw panel background
        // Then render all children
        for (const auto& child : m_children)
        {
            child->Render();
        }
    }

    bool UIPanel::HandleClick(float x, float y)
    {
        if (!m_visible)
        {
            return false;
        }

        // Check children in reverse order (top-most first)
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
        {
            if ((*it)->HandleClick(x, y))
            {
                return true;
            }
        }
        return false;
    }

    // =============================================================================
    // UICanvas
    // =============================================================================

    UICanvas::UICanvas() = default;

    void UICanvas::Initialize(int width, int height)
    {
        m_width = width;
        m_height = height;
    }

    UIPanel* UICanvas::CreatePanel(const std::string& name)
    {
        auto panel = std::make_unique<UIPanel>(name);
        auto* ptr = panel.get();
        m_panels.push_back(std::move(panel));
        return ptr;
    }

    UIWidget* UICanvas::FindWidget(const std::string& name)
    {
        for (auto& panel : m_panels)
        {
            if (panel->GetName() == name)
            {
                return panel.get();
            }
            if (auto* found = panel->FindWidget(name))
            {
                return found;
            }
        }
        return nullptr;
    }

    void UICanvas::RemovePanel(const std::string& name)
    {
        m_panels.erase(std::remove_if(m_panels.begin(), m_panels.end(),
                                      [&name](const std::unique_ptr<UIPanel>& p) { return p->GetName() == name; }),
                       m_panels.end());
    }

    void UICanvas::Update(float deltaTime)
    {
        for (auto& panel : m_panels)
        {
            panel->Update(deltaTime);
        }
    }

    void UICanvas::Render() const
    {
        for (const auto& panel : m_panels)
        {
            panel->Render();
        }
    }

    bool UICanvas::HandleClick(float x, float y)
    {
        for (auto it = m_panels.rbegin(); it != m_panels.rend(); ++it)
        {
            if ((*it)->HandleClick(x, y))
            {
                return true;
            }
        }
        return false;
    }

    void UICanvas::Resize(int width, int height)
    {
        m_width = width;
        m_height = height;
    }

    // =============================================================================
    // UISystem
    // =============================================================================

    UISystem::UISystem() = default;

    void UISystem::Initialize(int screenWidth, int screenHeight)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "UISystem initializing with resolution %dx%d", screenWidth,
                       screenHeight);
        SPARK_WARN_IF(Spark::LogCategory::Core, screenWidth <= 0 || screenHeight <= 0,
                      "UISystem initialized with non-positive screen dimensions");
        m_canvas.Initialize(screenWidth, screenHeight);
    }

    void UISystem::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        if (m_visible)
        {
            m_canvas.Update(deltaTime);
        }
    }

    void UISystem::Render()
    {
        if (m_visible)
        {
            m_canvas.Render();
        }
    }

    void UISystem::OnResize(int width, int height)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "UISystem resizing to %dx%d", width, height);
        m_canvas.Resize(width, height);
    }

    bool UISystem::HandleClick(float x, float y)
    {
        if (!m_visible)
        {
            return false;
        }
        return m_canvas.HandleClick(x, y);
    }

    std::string UISystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== UI System ===\n";
        oss << "Visible: " << (m_visible ? "YES" : "NO") << "\n";
        oss << "Canvas: " << m_canvas.GetWidth() << "x" << m_canvas.GetHeight() << "\n";
        return oss.str();
    }

} // namespace Spark::UI
