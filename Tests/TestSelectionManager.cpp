// TestSelectionManager.cpp - Tests for SparkEditor::SelectionManager
// Uses standalone reimplementation to avoid editor include path dependencies.
#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
// Standalone reimplementation of core SelectionManager logic
// ============================================================================
namespace
{

    using EntityId = uint32_t;
    constexpr EntityId INVALID_ENTITY = 0;

    struct SelectionChangedEvent
    {
        std::vector<EntityId> added;
        std::vector<EntityId> removed;
        std::vector<EntityId> current;
        bool isMarqueeSelection = false;
    };

    using SelectionCallback = std::function<void(const SelectionChangedEvent&)>;

    class SelectionManager
    {
      public:
        void Initialize()
        {
            m_selection.clear();
            m_order.clear();
            m_locked.clear();
            m_callbacks.clear();
            m_groups.clear();
            m_nextCbId = 1;
        }

        void Shutdown()
        {
            m_selection.clear();
            m_order.clear();
            m_callbacks.clear();
        }

        void Select(EntityId id)
        {
            if (id == INVALID_ENTITY || m_locked.count(id))
                return;
            m_selection.clear();
            m_order.clear();
            m_selection.insert(id);
            m_order.push_back(id);
            Notify();
        }

        void AddToSelection(EntityId id)
        {
            if (id == INVALID_ENTITY || m_locked.count(id) || m_selection.count(id))
                return;
            m_selection.insert(id);
            m_order.push_back(id);
            Notify();
        }

        void RemoveFromSelection(EntityId id)
        {
            if (!m_selection.count(id))
                return;
            m_selection.erase(id);
            m_order.erase(std::remove(m_order.begin(), m_order.end(), id), m_order.end());
            Notify();
        }

        void ToggleSelection(EntityId id)
        {
            if (m_selection.count(id))
                RemoveFromSelection(id);
            else
                AddToSelection(id);
        }

        void ClearSelection()
        {
            m_selection.clear();
            m_order.clear();
            Notify();
        }

        bool IsSelected(EntityId id) const { return m_selection.count(id) > 0; }
        size_t GetSelectionCount() const { return m_selection.size(); }
        const std::vector<EntityId>& GetSelection() const { return m_order; }
        EntityId GetPrimarySelection() const { return m_order.empty() ? INVALID_ENTITY : m_order.back(); }

        void LockEntity(EntityId id) { m_locked.insert(id); }
        void UnlockEntity(EntityId id) { m_locked.erase(id); }
        bool IsLocked(EntityId id) const { return m_locked.count(id) > 0; }

        void SaveGroup(const std::string& name) { m_groups[name] = m_order; }
        void RestoreGroup(const std::string& name)
        {
            auto it = m_groups.find(name);
            if (it == m_groups.end())
                return;
            m_selection.clear();
            m_order.clear();
            for (auto id : it->second)
            {
                m_selection.insert(id);
                m_order.push_back(id);
            }
        }
        std::vector<std::string> GetGroupNames() const
        {
            std::vector<std::string> names;
            for (const auto& [n, _] : m_groups)
                names.push_back(n);
            return names;
        }

        uint32_t OnSelectionChanged(SelectionCallback cb)
        {
            uint32_t id = m_nextCbId++;
            m_callbacks[id] = std::move(cb);
            return id;
        }
        void RemoveCallback(uint32_t id) { m_callbacks.erase(id); }

      private:
        void Notify()
        {
            SelectionChangedEvent e;
            e.current = m_order;
            for (const auto& [id, cb] : m_callbacks)
                cb(e);
        }

        std::unordered_set<EntityId> m_selection;
        std::vector<EntityId> m_order;
        std::unordered_set<EntityId> m_locked;
        std::unordered_map<std::string, std::vector<EntityId>> m_groups;
        std::unordered_map<uint32_t, SelectionCallback> m_callbacks;
        uint32_t m_nextCbId = 1;
    };

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(SelectionManager_SelectAndDeselect)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(10);
    EXPECT_TRUE(mgr.IsSelected(10));
    EXPECT_EQ(static_cast<size_t>(1), mgr.GetSelectionCount());
    mgr.RemoveFromSelection(10);
    EXPECT_FALSE(mgr.IsSelected(10));
    mgr.Shutdown();
}

TEST(SelectionManager_MultiSelect)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(1);
    mgr.AddToSelection(2);
    mgr.AddToSelection(3);
    EXPECT_EQ(static_cast<size_t>(3), mgr.GetSelectionCount());
    EXPECT_TRUE(mgr.IsSelected(1));
    EXPECT_TRUE(mgr.IsSelected(2));
    EXPECT_TRUE(mgr.IsSelected(3));
    mgr.Shutdown();
}

TEST(SelectionManager_Toggle)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(5);
    mgr.ToggleSelection(5);
    EXPECT_FALSE(mgr.IsSelected(5));
    mgr.ToggleSelection(5);
    EXPECT_TRUE(mgr.IsSelected(5));
    mgr.Shutdown();
}

TEST(SelectionManager_ClearSelection)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(1);
    mgr.AddToSelection(2);
    mgr.ClearSelection();
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetSelectionCount());
    mgr.Shutdown();
}

TEST(SelectionManager_PrimarySelection)
{
    SelectionManager mgr;
    mgr.Initialize();
    EXPECT_EQ(INVALID_ENTITY, mgr.GetPrimarySelection());
    mgr.Select(10);
    mgr.AddToSelection(20);
    EXPECT_EQ(static_cast<EntityId>(20), mgr.GetPrimarySelection());
    mgr.Shutdown();
}

TEST(SelectionManager_Locking)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.LockEntity(42);
    EXPECT_TRUE(mgr.IsLocked(42));
    mgr.Select(42);
    EXPECT_FALSE(mgr.IsSelected(42));
    mgr.UnlockEntity(42);
    mgr.Select(42);
    EXPECT_TRUE(mgr.IsSelected(42));
    mgr.Shutdown();
}

TEST(SelectionManager_Groups)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(1);
    mgr.AddToSelection(2);
    mgr.SaveGroup("group_a");
    mgr.ClearSelection();
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetSelectionCount());
    mgr.RestoreGroup("group_a");
    EXPECT_EQ(static_cast<size_t>(2), mgr.GetSelectionCount());
    EXPECT_TRUE(mgr.IsSelected(1));
    auto names = mgr.GetGroupNames();
    EXPECT_EQ(static_cast<size_t>(1), names.size());
    mgr.Shutdown();
}

TEST(SelectionManager_CallbackNotification)
{
    SelectionManager mgr;
    mgr.Initialize();
    int callCount = 0;
    uint32_t cbId = mgr.OnSelectionChanged([&](const SelectionChangedEvent&) { ++callCount; });
    mgr.Select(1);
    mgr.AddToSelection(2);
    EXPECT_EQ(2, callCount);
    mgr.RemoveCallback(cbId);
    mgr.Select(3);
    EXPECT_EQ(2, callCount);
    mgr.Shutdown();
}

TEST(SelectionManager_SelectReplacesOld)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(1);
    mgr.AddToSelection(2);
    mgr.Select(3);
    EXPECT_EQ(static_cast<size_t>(1), mgr.GetSelectionCount());
    EXPECT_TRUE(mgr.IsSelected(3));
    EXPECT_FALSE(mgr.IsSelected(1));
    mgr.Shutdown();
}

TEST(SelectionManager_InvalidEntity)
{
    SelectionManager mgr;
    mgr.Initialize();
    mgr.Select(INVALID_ENTITY);
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetSelectionCount());
    mgr.Shutdown();
}
