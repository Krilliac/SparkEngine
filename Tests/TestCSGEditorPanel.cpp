// TestCSGEditorPanel.cpp - Tests for the CSGEditorPanel public API
//
// These exercise the non-ImGui bookkeeping layer that CSGEditorPanel
// exposes (CreateBrush, RemoveBrushById, RebuildMesh, auto-rebuild toggle,
// selection cursor, brush tracking). ImGui rendering is covered by
// live editor testing and is compiled out of the test binary via the
// SPARK_CSG_PANEL_HAS_IMGUI guard in the header.
//
// We deliberately avoid instantiating the real CSGEditorPanel class
// here because it inherits from EditorPanel whose .cpp requires ImGui.
// Instead we validate the exact same algorithm via a standalone helper
// that mirrors the panel's member state, and we verify that the real
// underlying Spark::LevelDesign::CSGSystem operations succeed.

#include "TestFramework.h"

#include "Engine/LevelDesign/CSGSystem.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Spark::LevelDesign;

namespace
{

    // Mirrors CSGEditorPanel's bookkeeping state without the ImGui /
    // EditorPanel base class dependency. Every method here delegates to
    // the same CSGSystem singleton the real panel uses, so a bug in the
    // underlying system will surface here.
    class CSGPanelHelper
    {
      public:
        uint32_t CreateBrush(BrushShape shape, const CSGVec3& size, CSGOperation op = CSGOperation::Additive)
        {
            auto& csg = CSGSystem::GetInstance();
            const uint32_t id = csg.CreateBrush(shape, size);
            if (id == 0)
                return 0;
            csg.SetBrushOperation(id, op);
            m_brushIds.push_back(id);
            m_selectedBrush = id;
            if (m_autoRebuild)
                RebuildMesh();
            return id;
        }

        bool RemoveBrushById(uint32_t brushId)
        {
            auto it = std::find(m_brushIds.begin(), m_brushIds.end(), brushId);
            if (it == m_brushIds.end())
                return false;
            CSGSystem::GetInstance().RemoveBrush(brushId);
            m_brushIds.erase(it);
            if (m_selectedBrush == brushId)
                m_selectedBrush = m_brushIds.empty() ? 0 : m_brushIds.back();
            if (m_autoRebuild)
                RebuildMesh();
            return true;
        }

        void RebuildMesh() { m_lastMesh = CSGSystem::GetInstance().BuildAll(); }

        void SetSelectedBrushId(uint32_t brushId)
        {
            if (brushId == 0)
            {
                m_selectedBrush = 0;
                return;
            }
            if (std::find(m_brushIds.begin(), m_brushIds.end(), brushId) != m_brushIds.end())
                m_selectedBrush = brushId;
        }

        uint32_t GetSelectedBrush() const { return m_selectedBrush; }
        uint32_t GetBrushCount() const { return static_cast<uint32_t>(m_brushIds.size()); }
        const std::vector<uint32_t>& GetBrushes() const { return m_brushIds; }
        const CSGMesh& GetLastMesh() const { return m_lastMesh; }
        void SetAutoRebuildEnabled(bool enabled) { m_autoRebuild = enabled; }
        bool IsAutoRebuildEnabled() const { return m_autoRebuild; }

        void Reset()
        {
            // Clean up any brushes we created so other tests see a clean system.
            auto& csg = CSGSystem::GetInstance();
            for (uint32_t id : m_brushIds)
                csg.RemoveBrush(id);
            m_brushIds.clear();
            m_selectedBrush = 0;
            m_lastMesh = {};
            m_autoRebuild = true;
        }

      private:
        std::vector<uint32_t> m_brushIds;
        uint32_t m_selectedBrush = 0;
        bool m_autoRebuild = true;
        CSGMesh m_lastMesh;
    };

} // namespace

// ============================================================================
// Brush lifecycle
// ============================================================================

TEST(CSGEditorPanel_CreateBrushTracksIdAndSelects)
{
    CSGPanelHelper panel;
    const uint32_t id = panel.CreateBrush(BrushShape::Box, {2.0f, 2.0f, 2.0f});
    EXPECT_GT(id, static_cast<uint32_t>(0));
    EXPECT_EQ(panel.GetBrushCount(), static_cast<uint32_t>(1));
    EXPECT_EQ(panel.GetSelectedBrush(), id);
    EXPECT_EQ(panel.GetBrushes()[0], id);
    panel.Reset();
}

TEST(CSGEditorPanel_CreateBrushWithSubtractiveOperation)
{
    CSGPanelHelper panel;
    const uint32_t id = panel.CreateBrush(BrushShape::Box, {3.0f, 3.0f, 3.0f}, CSGOperation::Subtractive);
    EXPECT_GT(id, static_cast<uint32_t>(0));

    const auto* brush = CSGSystem::GetInstance().GetBrush(id);
    EXPECT_NE(brush, nullptr);
    if (brush)
    {
        EXPECT_EQ(static_cast<int>(brush->operation), static_cast<int>(CSGOperation::Subtractive));
    }
    panel.Reset();
}

TEST(CSGEditorPanel_DeleteBrushRemovesFromList)
{
    CSGPanelHelper panel;
    panel.SetAutoRebuildEnabled(false); // avoid rebuild churn

    const uint32_t a = panel.CreateBrush(BrushShape::Box, {1, 1, 1});
    const uint32_t b = panel.CreateBrush(BrushShape::Sphere, {1, 1, 1});
    const uint32_t c = panel.CreateBrush(BrushShape::Cylinder, {1, 1, 1});
    EXPECT_EQ(panel.GetBrushCount(), static_cast<uint32_t>(3));

    EXPECT_TRUE(panel.RemoveBrushById(b));
    EXPECT_EQ(panel.GetBrushCount(), static_cast<uint32_t>(2));

    const auto& ids = panel.GetBrushes();
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), b) == ids.end());
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), a) != ids.end());
    EXPECT_TRUE(std::find(ids.begin(), ids.end(), c) != ids.end());
    panel.Reset();
}

TEST(CSGEditorPanel_DeleteUnknownBrushReturnsFalse)
{
    CSGPanelHelper panel;
    EXPECT_FALSE(panel.RemoveBrushById(99999));
    EXPECT_FALSE(panel.RemoveBrushById(0));
    panel.Reset();
}

TEST(CSGEditorPanel_DeleteSelectedClearsOrReassignsCursor)
{
    CSGPanelHelper panel;
    panel.SetAutoRebuildEnabled(false);

    const uint32_t a = panel.CreateBrush(BrushShape::Box, {1, 1, 1});
    const uint32_t b = panel.CreateBrush(BrushShape::Sphere, {1, 1, 1});
    EXPECT_EQ(panel.GetSelectedBrush(), b); // last-created is selected

    EXPECT_TRUE(panel.RemoveBrushById(b));
    EXPECT_EQ(panel.GetSelectedBrush(), a); // cursor reassigned

    EXPECT_TRUE(panel.RemoveBrushById(a));
    EXPECT_EQ(panel.GetSelectedBrush(), static_cast<uint32_t>(0)); // cleared
    panel.Reset();
}

// ============================================================================
// Selection cursor
// ============================================================================

TEST(CSGEditorPanel_SelectBrushAcceptsKnownIdsOnly)
{
    CSGPanelHelper panel;
    panel.SetAutoRebuildEnabled(false);

    const uint32_t a = panel.CreateBrush(BrushShape::Box, {1, 1, 1});
    const uint32_t b = panel.CreateBrush(BrushShape::Sphere, {1, 1, 1});

    panel.SetSelectedBrushId(a);
    EXPECT_EQ(panel.GetSelectedBrush(), a);

    panel.SetSelectedBrushId(b);
    EXPECT_EQ(panel.GetSelectedBrush(), b);

    panel.SetSelectedBrushId(999); // unknown — ignored
    EXPECT_EQ(panel.GetSelectedBrush(), b);

    panel.SetSelectedBrushId(0); // 0 clears the cursor
    EXPECT_EQ(panel.GetSelectedBrush(), static_cast<uint32_t>(0));

    panel.Reset();
}

// ============================================================================
// Auto-rebuild
// ============================================================================

TEST(CSGEditorPanel_AutoRebuildToggle)
{
    CSGPanelHelper panel;
    EXPECT_TRUE(panel.IsAutoRebuildEnabled());

    panel.SetAutoRebuildEnabled(false);
    EXPECT_FALSE(panel.IsAutoRebuildEnabled());

    panel.SetAutoRebuildEnabled(true);
    EXPECT_TRUE(panel.IsAutoRebuildEnabled());
    panel.Reset();
}

TEST(CSGEditorPanel_RebuildMeshCapturesSnapshot)
{
    CSGPanelHelper panel;
    panel.SetAutoRebuildEnabled(false);

    panel.CreateBrush(BrushShape::Box, {2, 2, 2});
    EXPECT_EQ(panel.GetLastMesh().triangleCount, static_cast<uint32_t>(0)); // no rebuild yet

    panel.RebuildMesh();
    // Box has at least some triangles; exact count depends on CSG impl.
    EXPECT_GT(panel.GetLastMesh().triangleCount, static_cast<uint32_t>(0));
    panel.Reset();
}

TEST(CSGEditorPanel_AutoRebuildTriggersOnCreateAndDelete)
{
    CSGPanelHelper panel;
    EXPECT_TRUE(panel.IsAutoRebuildEnabled());

    panel.CreateBrush(BrushShape::Box, {2, 2, 2});
    const uint32_t triAfterCreate = panel.GetLastMesh().triangleCount;
    EXPECT_GT(triAfterCreate, static_cast<uint32_t>(0));

    panel.RemoveBrushById(panel.GetSelectedBrush());
    const uint32_t triAfterDelete = panel.GetLastMesh().triangleCount;
    EXPECT_EQ(triAfterDelete, static_cast<uint32_t>(0));

    panel.Reset();
}

// ============================================================================
// Brush variety — make sure each supported shape round-trips
// ============================================================================

TEST(CSGEditorPanel_CreateAllBrushShapes)
{
    CSGPanelHelper panel;
    panel.SetAutoRebuildEnabled(false);

    const BrushShape shapes[] = {BrushShape::Box, BrushShape::Cylinder, BrushShape::Sphere, BrushShape::Wedge,
                                 BrushShape::Cone};
    for (BrushShape s : shapes)
    {
        const uint32_t id = panel.CreateBrush(s, {2, 2, 2});
        EXPECT_GT(id, static_cast<uint32_t>(0));
    }
    EXPECT_EQ(panel.GetBrushCount(), static_cast<uint32_t>(5));
    panel.Reset();
}
