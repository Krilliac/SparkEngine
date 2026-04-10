/**
 * @file TestEditorLayoutManager.cpp
 * @brief Tests for editor layout management
 *
 * The first half of this file contains a standalone reimplementation
 * used by legacy tests (kept for historical coverage). The second half
 * tests the real `SparkEditor::EditorLayoutManager` class from
 * `SparkEditor/Source/Core/EditorLayoutManager.h`, which is now linked
 * into SparkTests and performs disk-backed JSON round-trips.
 */

#include "TestFramework.h"

#include "Core/EditorLayoutManager.h"

#include <filesystem>
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

// ============================================================================
// Real SparkEditor::EditorLayoutManager tests
// ============================================================================

namespace
{
    // Per-test layout directory so parallel / repeat runs don't collide.
    std::string MakeLayoutDir(const char* tag)
    {
        auto dir = std::filesystem::temp_directory_path() / ("spark_layout_test_" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }

    SparkEditor::PanelConfig MakePanel(const std::string& name, float x, float y, float w, float h, bool visible = true)
    {
        SparkEditor::PanelConfig p;
        p.name = name;
        p.displayName = name;
        p.posX = x;
        p.posY = y;
        p.sizeX = w;
        p.sizeY = h;
        p.isVisible = visible;
        p.dockPosition = SparkEditor::LayoutDockPosition::Center;
        return p;
    }
} // namespace

TEST(EditorLayoutMgr_InitCreatesDirectory)
{
    const std::string dir = MakeLayoutDir("init");
    std::filesystem::remove_all(dir); // ensure absent

    SparkEditor::EditorLayoutManager mgr;
    EXPECT_TRUE(mgr.Initialize(dir));
    EXPECT_TRUE(mgr.IsInitialized());
    EXPECT_TRUE(std::filesystem::exists(dir));
    mgr.Shutdown();
    EXPECT_FALSE(mgr.IsInitialized());
}

TEST(EditorLayoutMgr_RegisterPanelTracksDefaults)
{
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(MakeLayoutDir("register"));

    mgr.RegisterPanel(MakePanel("Hierarchy", 0, 0, 250, 600));
    mgr.RegisterPanel(MakePanel("Inspector", 800, 0, 300, 600));

    EXPECT_EQ(mgr.GetRegisteredPanelCount(), static_cast<uint32_t>(2));

    const auto* hier = mgr.GetPanelConfig("Hierarchy");
    EXPECT_NE(hier, nullptr);
    EXPECT_NEAR(hier->sizeX, 250.0f, 1e-3f);

    EXPECT_EQ(mgr.GetPanelConfig("Missing"), nullptr);

    mgr.Shutdown();
}

TEST(EditorLayoutMgr_ResetToDefaultRestoresFirstSeen)
{
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(MakeLayoutDir("reset"));

    mgr.RegisterPanel(MakePanel("Hierarchy", 0, 0, 250, 600));
    mgr.SetPanelPosition("Hierarchy", 111.0f, 222.0f);
    mgr.SetPanelSize("Hierarchy", 555.0f, 666.0f);
    mgr.SetPanelVisible("Hierarchy", false);

    mgr.ResetToDefault();

    const auto* hier = mgr.GetPanelConfig("Hierarchy");
    EXPECT_NE(hier, nullptr);
    EXPECT_NEAR(hier->posX, 0.0f, 1e-3f);
    EXPECT_NEAR(hier->posY, 0.0f, 1e-3f);
    EXPECT_NEAR(hier->sizeX, 250.0f, 1e-3f);
    EXPECT_NEAR(hier->sizeY, 600.0f, 1e-3f);
    EXPECT_TRUE(hier->isVisible);

    mgr.Shutdown();
}

TEST(EditorLayoutMgr_SaveLoadRoundtripWritesAndReadsJSON)
{
    const std::string dir = MakeLayoutDir("roundtrip");

    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(dir);
    mgr.RegisterPanel(MakePanel("Hierarchy", 10, 20, 300, 400));
    mgr.RegisterPanel(MakePanel("Inspector", 800, 0, 250, 600));

    // Mutate state, then save.
    mgr.SetPanelPosition("Hierarchy", 99, 88);
    mgr.SetPanelSize("Inspector", 777, 555);
    mgr.SetPanelVisible("Inspector", false);

    EXPECT_TRUE(mgr.SaveCurrentLayout("TestLayout", "dual screen setup"));

    // File should exist.
    const std::string filePath = (std::filesystem::path(dir) / "TestLayout.json").string();
    EXPECT_TRUE(std::filesystem::exists(filePath));
    EXPECT_EQ(mgr.GetCurrentLayoutName(), std::string("TestLayout"));

    // Mutate further, then load — values should revert to saved snapshot.
    mgr.SetPanelPosition("Hierarchy", -1.0f, -1.0f);
    mgr.SetPanelSize("Inspector", 1.0f, 1.0f);
    mgr.SetPanelVisible("Inspector", true);

    EXPECT_TRUE(mgr.LoadLayout("TestLayout"));

    const auto* hier = mgr.GetPanelConfig("Hierarchy");
    EXPECT_NEAR(hier->posX, 99.0f, 1e-3f);
    EXPECT_NEAR(hier->posY, 88.0f, 1e-3f);

    const auto* insp = mgr.GetPanelConfig("Inspector");
    EXPECT_NEAR(insp->sizeX, 777.0f, 1e-3f);
    EXPECT_NEAR(insp->sizeY, 555.0f, 1e-3f);
    EXPECT_FALSE(insp->isVisible);

    mgr.Shutdown();
}

TEST(EditorLayoutMgr_LoadUnknownLayoutReturnsFalse)
{
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(MakeLayoutDir("unknown"));
    EXPECT_FALSE(mgr.LoadLayout("DoesNotExist"));
    mgr.Shutdown();
}

TEST(EditorLayoutMgr_DeleteLayoutRemovesFile)
{
    const std::string dir = MakeLayoutDir("delete");
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(dir);
    mgr.RegisterPanel(MakePanel("Hierarchy", 0, 0, 250, 600));

    EXPECT_TRUE(mgr.SaveCurrentLayout("ToDelete"));
    const std::string filePath = (std::filesystem::path(dir) / "ToDelete.json").string();
    EXPECT_TRUE(std::filesystem::exists(filePath));

    EXPECT_TRUE(mgr.DeleteLayout("ToDelete"));
    EXPECT_FALSE(std::filesystem::exists(filePath));

    // Deleting a non-existent layout returns false.
    EXPECT_FALSE(mgr.DeleteLayout("ToDelete"));

    mgr.Shutdown();
}

TEST(EditorLayoutMgr_GetSavedLayoutsListsFiles)
{
    const std::string dir = MakeLayoutDir("list");
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(dir);
    mgr.RegisterPanel(MakePanel("Hierarchy", 0, 0, 250, 600));

    mgr.SaveCurrentLayout("Alpha", "first");
    mgr.SaveCurrentLayout("Beta", "second");
    mgr.SaveCurrentLayout("Gamma", "");

    auto layouts = mgr.GetSavedLayouts();
    EXPECT_EQ(layouts.size(), static_cast<size_t>(3));
    // Sorted alphabetically.
    EXPECT_EQ(layouts[0].name, std::string("Alpha"));
    EXPECT_EQ(layouts[1].name, std::string("Beta"));
    EXPECT_EQ(layouts[2].name, std::string("Gamma"));
    EXPECT_EQ(layouts[0].description, std::string("first"));
    EXPECT_EQ(layouts[1].description, std::string("second"));

    mgr.Shutdown();
}

TEST(EditorLayoutMgr_SaveEmptyNameFails)
{
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(MakeLayoutDir("emptyname"));
    EXPECT_FALSE(mgr.SaveCurrentLayout("", "desc"));
    mgr.Shutdown();
}

TEST(EditorLayoutMgr_LoadIgnoresUnregisteredPanels)
{
    // Save a layout with Hierarchy+Inspector, then load it into a
    // different manager that only registered Hierarchy. Inspector should
    // be silently ignored.
    const std::string dir = MakeLayoutDir("unregistered");

    {
        SparkEditor::EditorLayoutManager writer;
        writer.Initialize(dir);
        writer.RegisterPanel(MakePanel("Hierarchy", 0, 0, 300, 600));
        writer.RegisterPanel(MakePanel("Inspector", 800, 0, 300, 600));
        writer.SaveCurrentLayout("Mix");
        writer.Shutdown();
    }

    SparkEditor::EditorLayoutManager reader;
    reader.Initialize(dir);
    reader.RegisterPanel(MakePanel("Hierarchy", 0, 0, 100, 100));

    EXPECT_TRUE(reader.LoadLayout("Mix"));

    // Hierarchy picked up from file.
    const auto* hier = reader.GetPanelConfig("Hierarchy");
    EXPECT_NEAR(hier->sizeX, 300.0f, 1e-3f);
    // Inspector was not registered in reader, so it should not be added.
    EXPECT_EQ(reader.GetPanelConfig("Inspector"), nullptr);

    reader.Shutdown();
}

TEST(EditorLayoutMgr_ConsoleStatusContainsName)
{
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(MakeLayoutDir("status"));
    mgr.RegisterPanel(MakePanel("Hierarchy", 0, 0, 100, 100));
    const std::string s = mgr.Console_GetStatus();
    EXPECT_TRUE(s.find("EditorLayoutManager") != std::string::npos);
    EXPECT_TRUE(s.find("panels=1") != std::string::npos);
    mgr.Shutdown();
}
