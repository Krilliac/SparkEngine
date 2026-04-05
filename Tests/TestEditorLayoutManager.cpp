/**
 * @file TestEditorLayoutManager.cpp
 * @brief Tests for editor layout management
 *
 * Standalone reimplementation for CI testing without engine dependencies.
 */

#include "TestFramework.h"

#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation
// ============================================================================

namespace LayoutTest
{

    struct PanelState
    {
        std::string name;
        bool visible = true;
        float x = 0.0f;
        float y = 0.0f;
        float width = 200.0f;
        float height = 300.0f;

        bool operator==(const PanelState& other) const
        {
            return name == other.name && visible == other.visible && x == other.x && y == other.y &&
                   width == other.width && height == other.height;
        }
    };

    struct LayoutSnapshot
    {
        std::vector<PanelState> panels;

        bool operator==(const LayoutSnapshot& other) const { return panels == other.panels; }
    };

    class LayoutManager
    {
      public:
        void RegisterPanel(const std::string& name, bool defaultVisible, float x, float y, float w, float h)
        {
            PanelState panel{name, defaultVisible, x, y, w, h};
            m_panels[name] = panel;
            m_defaults[name] = panel;
        }

        void SetPanelVisible(const std::string& name, bool visible)
        {
            auto it = m_panels.find(name);
            if (it != m_panels.end())
            {
                it->second.visible = visible;
            }
        }

        bool IsPanelVisible(const std::string& name) const
        {
            auto it = m_panels.find(name);
            if (it == m_panels.end())
            {
                return false;
            }
            return it->second.visible;
        }

        void SetPanelPosition(const std::string& name, float x, float y)
        {
            auto it = m_panels.find(name);
            if (it != m_panels.end())
            {
                it->second.x = x;
                it->second.y = y;
            }
        }

        void SetPanelSize(const std::string& name, float w, float h)
        {
            auto it = m_panels.find(name);
            if (it != m_panels.end())
            {
                it->second.width = w;
                it->second.height = h;
            }
        }

        const PanelState* GetPanel(const std::string& name) const
        {
            auto it = m_panels.find(name);
            if (it == m_panels.end())
            {
                return nullptr;
            }
            return &it->second;
        }

        LayoutSnapshot SaveSnapshot() const
        {
            LayoutSnapshot snap;
            for (const auto& [name, panel] : m_panels)
            {
                snap.panels.push_back(panel);
            }
            return snap;
        }

        void RestoreSnapshot(const LayoutSnapshot& snap)
        {
            for (const auto& panel : snap.panels)
            {
                m_panels[panel.name] = panel;
            }
        }

        void ResetToDefaults() { m_panels = m_defaults; }

      private:
        std::unordered_map<std::string, PanelState> m_panels;
        std::unordered_map<std::string, PanelState> m_defaults;
    };

} // namespace LayoutTest

// ============================================================================
// Tests
// ============================================================================

TEST(Layout_SaveAndRestore)
{
    using namespace LayoutTest;

    LayoutManager mgr;
    mgr.RegisterPanel("Hierarchy", true, 0.0f, 0.0f, 250.0f, 600.0f);
    mgr.RegisterPanel("Inspector", true, 800.0f, 0.0f, 300.0f, 600.0f);
    mgr.RegisterPanel("Console", true, 0.0f, 600.0f, 1100.0f, 200.0f);

    // Save original layout
    auto saved = mgr.SaveSnapshot();

    // Modify layout
    mgr.SetPanelPosition("Hierarchy", 50.0f, 50.0f);
    mgr.SetPanelSize("Inspector", 400.0f, 700.0f);
    mgr.SetPanelVisible("Console", false);

    // Verify modifications took effect
    auto* hierarchy = mgr.GetPanel("Hierarchy");
    EXPECT_NEAR(hierarchy->x, 50.0f, 0.001f);

    auto* inspector = mgr.GetPanel("Inspector");
    EXPECT_NEAR(inspector->width, 400.0f, 0.001f);

    EXPECT_FALSE(mgr.IsPanelVisible("Console"));

    // Restore original
    mgr.RestoreSnapshot(saved);

    hierarchy = mgr.GetPanel("Hierarchy");
    EXPECT_NEAR(hierarchy->x, 0.0f, 0.001f);
    EXPECT_NEAR(hierarchy->y, 0.0f, 0.001f);

    inspector = mgr.GetPanel("Inspector");
    EXPECT_NEAR(inspector->width, 300.0f, 0.001f);

    EXPECT_TRUE(mgr.IsPanelVisible("Console"));
}

TEST(Layout_PanelVisibility)
{
    using namespace LayoutTest;

    LayoutManager mgr;
    mgr.RegisterPanel("SceneView", true, 0.0f, 0.0f, 800.0f, 600.0f);
    mgr.RegisterPanel("AssetBrowser", false, 0.0f, 600.0f, 800.0f, 200.0f);

    // Check initial state
    EXPECT_TRUE(mgr.IsPanelVisible("SceneView"));
    EXPECT_FALSE(mgr.IsPanelVisible("AssetBrowser"));

    // Toggle visibility
    mgr.SetPanelVisible("SceneView", false);
    mgr.SetPanelVisible("AssetBrowser", true);

    EXPECT_FALSE(mgr.IsPanelVisible("SceneView"));
    EXPECT_TRUE(mgr.IsPanelVisible("AssetBrowser"));

    // Toggle back
    mgr.SetPanelVisible("SceneView", true);
    EXPECT_TRUE(mgr.IsPanelVisible("SceneView"));
}

TEST(Layout_DefaultReset)
{
    using namespace LayoutTest;

    LayoutManager mgr;
    mgr.RegisterPanel("Hierarchy", true, 0.0f, 0.0f, 250.0f, 600.0f);
    mgr.RegisterPanel("Inspector", true, 800.0f, 0.0f, 300.0f, 600.0f);

    // Modify everything
    mgr.SetPanelPosition("Hierarchy", 100.0f, 100.0f);
    mgr.SetPanelSize("Hierarchy", 400.0f, 400.0f);
    mgr.SetPanelVisible("Inspector", false);
    mgr.SetPanelPosition("Inspector", 500.0f, 500.0f);

    // Reset to defaults
    mgr.ResetToDefaults();

    auto* hierarchy = mgr.GetPanel("Hierarchy");
    EXPECT_NEAR(hierarchy->x, 0.0f, 0.001f);
    EXPECT_NEAR(hierarchy->y, 0.0f, 0.001f);
    EXPECT_NEAR(hierarchy->width, 250.0f, 0.001f);
    EXPECT_NEAR(hierarchy->height, 600.0f, 0.001f);
    EXPECT_TRUE(hierarchy->visible);

    auto* inspector = mgr.GetPanel("Inspector");
    EXPECT_NEAR(inspector->x, 800.0f, 0.001f);
    EXPECT_NEAR(inspector->y, 0.0f, 0.001f);
    EXPECT_NEAR(inspector->width, 300.0f, 0.001f);
    EXPECT_NEAR(inspector->height, 600.0f, 0.001f);
    EXPECT_TRUE(inspector->visible);
}
