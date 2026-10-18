/**
 * @file UISystem.cpp
 * @brief Implementation of the runtime UI canvas and system (the widgets are in UIWidgets.cpp)
 */

#include "UISystem.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <sstream>

namespace Spark::UI
{

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
            SPARK_GUARDED_UPDATE("UI:PanelUpdate", "UI", { panel->Update(deltaTime); });
        }
    }

    void UICanvas::Render() const
    {
        for (const auto& panel : m_panels)
        {
            SPARK_GUARDED_UPDATE("UI:PanelRender", "UI", { panel->Render(); });
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

        // Phase R: activate the per-widget composition stack. Clamp
        // non-positive dimensions to 1 so the compositor has a valid
        // reference size even when the caller passed garbage.
        const uint32_t compW = screenWidth > 0 ? static_cast<uint32_t>(screenWidth) : 1u;
        const uint32_t compH = screenHeight > 0 ? static_cast<uint32_t>(screenHeight) : 1u;
        m_compositor.Initialize(compW, compH);
    }

    void UISystem::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_GUARDED_UPDATE("UI:Canvas", "UI", {
            if (m_visible)
            {
                m_canvas.Update(deltaTime);
            }
        });
    }

    void UISystem::Render()
    {
        // Phase R: advance the compositor frame counter so pool
        // entries not used for 10+ frames are reclaimed. Runs every
        // frame regardless of visibility — the reclaim path should
        // drain the pool even when the UI is hidden.
        m_compositor.BeginFrame();

        SPARK_GUARDED_UPDATE("UI:CanvasRender", "UI", {
            if (m_visible)
            {
                m_canvas.Render();
            }
        });
    }

    void UISystem::OnResize(int width, int height)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "UISystem resizing to %dx%d", width, height);
        m_canvas.Resize(width, height);

        // Phase R: re-initialise the compositor with the new
        // dimensions. Initialize() clears the pool so the metadata
        // slots re-align with the new screen size. Any in-flight
        // composition stack entries are also dropped — callers that
        // push composite levels across a resize are responsible for
        // recreating them.
        const uint32_t compW = width > 0 ? static_cast<uint32_t>(width) : 1u;
        const uint32_t compH = height > 0 ? static_cast<uint32_t>(height) : 1u;
        m_compositor.Initialize(compW, compH);
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
