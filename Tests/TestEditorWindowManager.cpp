// TestEditorWindowManager.cpp - Tests for EditorWindowManager
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestWinMgr
{

    struct PanelWindowState
    {
        std::string panelName;
        bool isOpen = true;
        bool isFloating = false;
        float posX = 0, posY = 0, width = 400, height = 300;
        int32_t monitorIndex = -1;
    };

    struct WindowLayout
    {
        std::string name;
        std::vector<PanelWindowState> panels;
        std::string dockLayoutINI;
    };

    class EditorWindowManager
    {
      public:
        void Initialize()
        {
            m_currentLayout.name = "Default";
            m_initialized = true;
        }

        void Shutdown()
        {
            if (m_autoSave && m_initialized)
                SaveLayout("__autosave__");
            m_initialized = false;
        }

        void SaveLayout(const std::string& name)
        {
            WindowLayout layout = m_currentLayout;
            layout.name = name;
            for (auto& saved : m_savedLayouts)
            {
                if (saved.name == name)
                {
                    saved = layout;
                    return;
                }
            }
            m_savedLayouts.push_back(std::move(layout));
        }

        bool LoadLayout(const std::string& name)
        {
            for (const auto& layout : m_savedLayouts)
            {
                if (layout.name == name)
                {
                    m_currentLayout = layout;
                    return true;
                }
            }
            return false;
        }

        std::vector<std::string> GetSavedLayouts() const
        {
            std::vector<std::string> names;
            for (const auto& l : m_savedLayouts)
                names.push_back(l.name);
            return names;
        }

        bool DeleteLayout(const std::string& name)
        {
            for (auto it = m_savedLayouts.begin(); it != m_savedLayouts.end(); ++it)
            {
                if (it->name == name)
                {
                    m_savedLayouts.erase(it);
                    return true;
                }
            }
            return false;
        }

        void FloatPanel(const std::string& panelName) { m_floating[panelName] = true; }
        void DockPanel(const std::string& panelName) { m_floating[panelName] = false; }

        bool IsPanelFloating(const std::string& panelName) const
        {
            auto it = m_floating.find(panelName);
            return it != m_floating.end() && it->second;
        }

        void SetAutoSaveEnabled(bool enabled) { m_autoSave = enabled; }
        bool IsAutoSaveEnabled() const { return m_autoSave; }

        const WindowLayout& GetCurrentLayout() const { return m_currentLayout; }

        std::string Console_GetStatus() const
        {
            return "EditorWindowManager: layout='" + m_currentLayout.name +
                   "' savedLayouts=" + std::to_string(m_savedLayouts.size());
        }

      private:
        std::vector<WindowLayout> m_savedLayouts;
        WindowLayout m_currentLayout;
        bool m_autoSave = true;
        bool m_initialized = false;
        std::unordered_map<std::string, bool> m_floating;
    };

} // namespace TestWinMgr

TEST(EditorWinMgr_SaveLoadRoundtrip)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    mgr.SaveLayout("TestLayout");
    bool loaded = mgr.LoadLayout("TestLayout");
    EXPECT_TRUE(loaded);
    EXPECT_EQ(mgr.GetCurrentLayout().name, std::string("TestLayout"));
}

TEST(EditorWinMgr_LoadNonexistentReturnsFalse)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    bool loaded = mgr.LoadLayout("DoesNotExist");
    EXPECT_FALSE(loaded);
}

TEST(EditorWinMgr_FloatDockToggle)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    EXPECT_FALSE(mgr.IsPanelFloating("SceneView"));

    mgr.FloatPanel("SceneView");
    EXPECT_TRUE(mgr.IsPanelFloating("SceneView"));

    mgr.DockPanel("SceneView");
    EXPECT_FALSE(mgr.IsPanelFloating("SceneView"));
}

TEST(EditorWinMgr_DeleteLayout)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    mgr.SaveLayout("ToDelete");
    EXPECT_EQ(mgr.GetSavedLayouts().size(), size_t(1));

    bool deleted = mgr.DeleteLayout("ToDelete");
    EXPECT_TRUE(deleted);
    EXPECT_EQ(mgr.GetSavedLayouts().size(), size_t(0));

    bool deletedAgain = mgr.DeleteLayout("ToDelete");
    EXPECT_FALSE(deletedAgain);
}

TEST(EditorWinMgr_AutoSaveFlag)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    EXPECT_TRUE(mgr.IsAutoSaveEnabled());
    mgr.SetAutoSaveEnabled(false);
    EXPECT_FALSE(mgr.IsAutoSaveEnabled());
}

TEST(EditorWinMgr_MultipleLayouts)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    mgr.SaveLayout("Layout1");
    mgr.SaveLayout("Layout2");
    mgr.SaveLayout("Layout3");

    auto names = mgr.GetSavedLayouts();
    EXPECT_EQ(names.size(), size_t(3));
    EXPECT_EQ(names[0], std::string("Layout1"));
    EXPECT_EQ(names[2], std::string("Layout3"));
}

TEST(EditorWinMgr_ConsoleStatus)
{
    using namespace TestWinMgr;
    EditorWindowManager mgr;
    mgr.Initialize();

    std::string status = mgr.Console_GetStatus();
    EXPECT_TRUE(status.find("EditorWindowManager") != std::string::npos);
    EXPECT_TRUE(status.find("Default") != std::string::npos);
}
