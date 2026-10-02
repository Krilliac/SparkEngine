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
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
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

TEST(EditorLayoutMgr_MalformedLayoutLeavesPanelsUntouchedAndReportsError)
{
    const std::string dir = MakeLayoutDir("malformed");
    {
        SparkEditor::EditorLayoutManager writer;
        writer.Initialize(dir);
        writer.RegisterPanel(MakePanel("Hierarchy", 0, 0, 999, 999));
        writer.RegisterPanel(MakePanel("Inspector", 800, 0, 999, 999));
        ASSERT_TRUE(writer.SaveCurrentLayout("Cut"));
    }
    // Cut the file inside the second panel, as an interrupted external copy would.
    const std::filesystem::path file = std::filesystem::path(dir) / "Cut.json";
    std::string text;
    {
        std::ifstream input(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const size_t second = text.find("\"name\": \"Inspector\"");
    ASSERT_TRUE(second != std::string::npos);
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << text.substr(0, second + 10);
    }

    SparkEditor::EditorLayoutManager reader;
    reader.Initialize(dir);
    reader.RegisterPanel(MakePanel("Hierarchy", 0, 0, 300, 600));
    reader.RegisterPanel(MakePanel("Inspector", 800, 0, 250, 600));

    // The first panel parsed fine, but nothing may be applied from a damaged file.
    EXPECT_FALSE(reader.LoadLayout("Cut"));
    EXPECT_NEAR(reader.GetPanelConfig("Hierarchy")->sizeX, 300.0f, 1e-3f);
    EXPECT_NEAR(reader.GetPanelConfig("Inspector")->sizeX, 250.0f, 1e-3f);
    EXPECT_EQ(reader.GetCurrentLayoutName(), std::string("Default"));
    EXPECT_STR_CONTAINS(reader.GetLastError(), "Cut.json");
    EXPECT_STR_CONTAINS(reader.GetLastError(), "malformed or truncated panel 2; no panel was changed");

    // A file cut right after its panels array is rejected too.
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << text.substr(0, text.rfind(']') + 1);
    }
    EXPECT_FALSE(reader.LoadLayout("Cut"));
    EXPECT_NEAR(reader.GetPanelConfig("Hierarchy")->sizeX, 300.0f, 1e-3f);
    EXPECT_STR_CONTAINS(reader.GetLastError(), "is truncated or has content after its panels array");

    // The intact file still loads and clears the error.
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << text;
    }
    EXPECT_TRUE(reader.LoadLayout("Cut"));
    EXPECT_NEAR(reader.GetPanelConfig("Hierarchy")->sizeX, 999.0f, 1e-3f);
    EXPECT_TRUE(reader.GetLastError().empty());
}

TEST(EditorLayoutMgr_FailedSaveKeepsPreviousLayoutFile)
{
    const std::string dir = MakeLayoutDir("failedsave");
    SparkEditor::EditorLayoutManager mgr;
    mgr.Initialize(dir);
    mgr.RegisterPanel(MakePanel("Hierarchy", 0, 0, 300, 600));
    ASSERT_TRUE(mgr.SaveCurrentLayout("Keep", "first"));

    const std::filesystem::path file = std::filesystem::path(dir) / "Keep.json";
    std::string before;
    {
        std::ifstream input(file, std::ios::binary);
        before.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // Occupy the staging name with a non-empty directory so the staged write fails.
    const std::filesystem::path staging = std::filesystem::path(dir) / "Keep.json.tmp";
    std::filesystem::create_directories(staging);
    {
        std::ofstream occupant(staging / "occupant");
        occupant << "x";
    }

    mgr.SetPanelSize("Hierarchy", 1.0f, 1.0f);
    EXPECT_FALSE(mgr.SaveCurrentLayout("Keep", "second"));
    EXPECT_STR_CONTAINS(mgr.GetLastError(), "Keep.json");
    EXPECT_STR_CONTAINS(mgr.GetLastError(), "The previous file is unchanged");

    std::string after;
    {
        std::ifstream input(file, std::ios::binary);
        after.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    EXPECT_TRUE(after == before);

    // The staging leftover is not listed as a layout, and the kept file still loads.
    const auto layouts = mgr.GetSavedLayouts();
    ASSERT_EQ(layouts.size(), static_cast<size_t>(1));
    EXPECT_EQ(layouts[0].name, std::string("Keep"));
    EXPECT_TRUE(mgr.LoadLayout("Keep"));
    EXPECT_NEAR(mgr.GetPanelConfig("Hierarchy")->sizeX, 300.0f, 1e-3f);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(EditorLayoutMgr_ValuesNamedLikeKeysDoNotShadowKeys)
{
    // A layout named "version" with the description "panels", and a panel named "sizeX", write those
    // words as string values ahead of the real keys. The reader must match keys, not values.
    const std::string dir = MakeLayoutDir("keynames");
    {
        SparkEditor::EditorLayoutManager writer;
        writer.Initialize(dir);
        writer.RegisterPanel(MakePanel("Hierarchy", 0, 0, 111, 600));
        writer.RegisterPanel(MakePanel("sizeX", 800, 0, 321, 600));
        ASSERT_TRUE(writer.SaveCurrentLayout("version", "panels"));
    }

    SparkEditor::EditorLayoutManager reader;
    reader.Initialize(dir);
    reader.RegisterPanel(MakePanel("Hierarchy", 0, 0, 300, 600));
    reader.RegisterPanel(MakePanel("sizeX", 800, 0, 250, 600));
    EXPECT_TRUE(reader.LoadLayout("version"));
    EXPECT_TRUE(reader.GetLastError().empty());
    EXPECT_NEAR(reader.GetPanelConfig("Hierarchy")->sizeX, 111.0f, 1e-3f);
    EXPECT_NEAR(reader.GetPanelConfig("sizeX")->sizeX, 321.0f, 1e-3f);

    const auto layouts = reader.GetSavedLayouts();
    ASSERT_EQ(layouts.size(), static_cast<size_t>(1));
    EXPECT_EQ(layouts[0].description, std::string("panels"));

    // The same file declaring a newer format still fails closed, without applying any panel.
    const std::filesystem::path file = std::filesystem::path(dir) / "version.json";
    std::string text;
    {
        std::ifstream input(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const std::string declared = "\"version\": 1,";
    const size_t at = text.find(declared);
    ASSERT_TRUE(at != std::string::npos);
    text.replace(at, declared.size(), "\"version\": 2,");
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << text;
    }
    reader.SetPanelSize("Hierarchy", 5.0f, 5.0f);
    EXPECT_FALSE(reader.LoadLayout("version"));
    EXPECT_STR_CONTAINS(reader.GetLastError(), "is layout format version 2");
    EXPECT_NEAR(reader.GetPanelConfig("Hierarchy")->sizeX, 5.0f, 1e-3f);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
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
