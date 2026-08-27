// TestInstanceManager.cpp - Tests for dungeon/instance management system
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestInstance
{
    using InstanceID = uint32_t;
    using EncounterID = uint32_t;
    using EntityID = uint32_t;
    using Clock = std::chrono::steady_clock;
    enum class EncounterState
    {
        NotStarted,
        InProgress,
        Done,
        Failed
    };

    struct EncounterDefinition
    {
        EncounterID id = 0;
        std::string name;
        int requiredPlayers = 1;
        float enrageTimer = 0.0f;
        bool optional = false;
    };

    struct InstanceTemplate
    {
        uint32_t templateId = 0;
        std::string name;
        int maxPlayers = 5;
        float resetTimerSeconds = 3600.0f;
        std::vector<EncounterDefinition> encounters;
    };

    struct InstanceData
    {
        InstanceID id = 0;
        uint32_t templateId = 0;
        std::set<EntityID> players;
        std::unordered_map<EncounterID, EncounterState> encounterStates;
        Clock::time_point createdAt;
        bool completed = false;
    };

    struct LockoutEntry
    {
        uint32_t templateId = 0;
        Clock::time_point expiresAt;
    };

    class InstanceManager
    {
      public:
        bool RegisterTemplate(const InstanceTemplate& t)
        {
            if (m_templates.contains(t.templateId))
                return false;
            m_templates[t.templateId] = t;
            return true;
        }
        const InstanceTemplate* GetTemplate(uint32_t id) const
        {
            auto it = m_templates.find(id);
            return it != m_templates.end() ? &it->second : nullptr;
        }
        InstanceID CreateInstance(uint32_t tid)
        {
            auto it = m_templates.find(tid);
            if (it == m_templates.end())
                return 0;
            InstanceID id = m_nextId++;
            InstanceData d{id, tid, {}, {}, Clock::now(), false};
            for (const auto& e : it->second.encounters)
                d.encounterStates[e.id] = EncounterState::NotStarted;
            m_instances[id] = std::move(d);
            return id;
        }
        bool DestroyInstance(InstanceID id)
        {
            auto it = m_instances.find(id);
            if (it == m_instances.end())
                return false;
            for (EntityID p : it->second.players)
                m_playerMap.erase(p);
            m_instances.erase(it);
            return true;
        }
        const InstanceData* GetInstance(InstanceID id) const
        {
            auto it = m_instances.find(id);
            return it != m_instances.end() ? &it->second : nullptr;
        }
        bool AddPlayer(InstanceID iid, EntityID pid)
        {
            auto it = m_instances.find(iid);
            if (it == m_instances.end())
                return false;
            const auto* t = GetTemplate(it->second.templateId);
            if (!t || static_cast<int>(it->second.players.size()) >= t->maxPlayers)
                return false;
            if (m_playerMap.contains(pid))
                return false;
            it->second.players.insert(pid);
            m_playerMap[pid] = iid;
            return true;
        }
        bool RemovePlayer(InstanceID iid, EntityID pid)
        {
            auto it = m_instances.find(iid);
            if (it == m_instances.end() || it->second.players.erase(pid) == 0)
                return false;
            m_playerMap.erase(pid);
            return true;
        }
        std::optional<InstanceID> GetPlayerInstance(EntityID pid) const
        {
            auto it = m_playerMap.find(pid);
            return it != m_playerMap.end() ? std::optional(it->second) : std::nullopt;
        }
        bool StartEncounter(InstanceID iid, EncounterID eid)
        {
            return Transition(iid, eid, EncounterState::NotStarted, EncounterState::InProgress);
        }
        bool CompleteEncounter(InstanceID iid, EncounterID eid)
        {
            if (!Transition(iid, eid, EncounterState::InProgress, EncounterState::Done))
                return false;
            if (AllEncountersDone(iid))
                m_instances[iid].completed = true;
            return true;
        }
        bool FailEncounter(InstanceID iid, EncounterID eid)
        {
            return Transition(iid, eid, EncounterState::InProgress, EncounterState::Failed);
        }
        bool ResetEncounter(InstanceID iid, EncounterID eid)
        {
            auto it = m_instances.find(iid);
            if (it == m_instances.end())
                return false;
            auto ei = it->second.encounterStates.find(eid);
            if (ei == it->second.encounterStates.end())
                return false;
            ei->second = EncounterState::NotStarted;
            it->second.completed = false;
            return true;
        }
        bool AllEncountersDone(InstanceID iid) const
        {
            const auto* d = GetInstance(iid);
            const auto* t = d ? GetTemplate(d->templateId) : nullptr;
            if (!t)
                return false;
            for (const auto& e : t->encounters)
            {
                if (e.optional)
                    continue;
                auto it = d->encounterStates.find(e.id);
                if (it == d->encounterStates.end() || it->second != EncounterState::Done)
                    return false;
            }
            return true;
        }
        int GetCompletedEncounterCount(InstanceID iid) const
        {
            const auto* d = GetInstance(iid);
            if (!d)
                return 0;
            int c = 0;
            for (const auto& [id, s] : d->encounterStates)
                if (s == EncounterState::Done)
                    ++c;
            return c;
        }
        bool HasLockout(EntityID pid, uint32_t tid) const
        {
            auto it = m_lockouts.find(pid);
            if (it == m_lockouts.end())
                return false;
            auto now = Clock::now();
            for (const auto& e : it->second)
                if (e.templateId == tid && e.expiresAt > now)
                    return true;
            return false;
        }
        void AddLockout(EntityID pid, uint32_t tid, float seconds)
        {
            auto exp =
                Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(seconds));
            m_lockouts[pid].push_back({tid, exp});
        }
        void ClearExpiredLockouts(EntityID pid)
        {
            auto it = m_lockouts.find(pid);
            if (it == m_lockouts.end())
                return;
            auto now = Clock::now();
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [now](const LockoutEntry& e) { return e.expiresAt <= now; }),
                    v.end());
            if (v.empty())
                m_lockouts.erase(it);
        }
        size_t GetInstanceCount() const { return m_instances.size(); }

      private:
        bool Transition(InstanceID iid, EncounterID eid, EncounterState from, EncounterState to)
        {
            auto it = m_instances.find(iid);
            if (it == m_instances.end())
                return false;
            auto ei = it->second.encounterStates.find(eid);
            if (ei == it->second.encounterStates.end() || ei->second != from)
                return false;
            ei->second = to;
            return true;
        }
        std::unordered_map<uint32_t, InstanceTemplate> m_templates;
        std::unordered_map<InstanceID, InstanceData> m_instances;
        std::unordered_map<EntityID, InstanceID> m_playerMap;
        std::unordered_map<EntityID, std::vector<LockoutEntry>> m_lockouts;
        InstanceID m_nextId = 1;
    };

    inline InstanceTemplate MakeTestTemplate(uint32_t id = 1, int maxPlayers = 5)
    {
        return {id,
                "Test Dungeon",
                maxPlayers,
                3600.0f,
                {{101, "Boss A", 1, 300.0f, false},
                 {102, "Boss B", 2, 600.0f, false},
                 {103, "Bonus Boss", 1, 120.0f, true}}};
    }
} // namespace TestInstance

// =============================================================================
// Tests
// =============================================================================

using namespace TestInstance;

TEST(Instance_RegisterTemplateAndRetrieve)
{
    InstanceManager mgr;
    auto tmpl = MakeTestTemplate();
    EXPECT_TRUE(mgr.RegisterTemplate(tmpl));
    const auto* ret = mgr.GetTemplate(1);
    ASSERT_TRUE(ret != nullptr);
    EXPECT_EQ(ret->name, std::string("Test Dungeon"));
    EXPECT_EQ(ret->maxPlayers, 5);
    EXPECT_EQ(static_cast<int>(ret->encounters.size()), 3);
    EXPECT_FALSE(mgr.RegisterTemplate(tmpl)); // duplicate fails
}

TEST(Instance_CreateFromTemplate)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    EXPECT_NE(id, (InstanceID)0);
    const auto* inst = mgr.GetInstance(id);
    ASSERT_TRUE(inst != nullptr);
    EXPECT_EQ(inst->templateId, (uint32_t)1);
    EXPECT_FALSE(inst->completed);
    EXPECT_EQ(static_cast<int>(inst->encounterStates.size()), 3);
    for (const auto& [eid, state] : inst->encounterStates)
        EXPECT_EQ(static_cast<int>(state), static_cast<int>(EncounterState::NotStarted));
    EXPECT_EQ(mgr.CreateInstance(999), (InstanceID)0);
}

TEST(Instance_AddRemovePlayers)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    EXPECT_TRUE(mgr.AddPlayer(id, 10));
    EXPECT_TRUE(mgr.AddPlayer(id, 20));
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->players.size()), 2);
    EXPECT_TRUE(mgr.RemovePlayer(id, 10));
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->players.size()), 1);
    EXPECT_FALSE(mgr.GetInstance(id)->players.contains(10));
    EXPECT_FALSE(mgr.RemovePlayer(id, 99));
}

TEST(Instance_PlayerTracking)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id1 = mgr.CreateInstance(1);
    EXPECT_FALSE(mgr.GetPlayerInstance(10).has_value());
    mgr.AddPlayer(id1, 10);
    EXPECT_TRUE(mgr.GetPlayerInstance(10).has_value());
    EXPECT_EQ(mgr.GetPlayerInstance(10).value(), id1);
    auto id2 = mgr.CreateInstance(1);
    EXPECT_FALSE(mgr.AddPlayer(id2, 10)); // already in instance
    mgr.RemovePlayer(id1, 10);
    EXPECT_TRUE(mgr.AddPlayer(id2, 10)); // can join after removal
}

TEST(Instance_StartEncounter)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    EXPECT_TRUE(mgr.StartEncounter(id, 101));
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->encounterStates.at(101)),
              static_cast<int>(EncounterState::InProgress));
    EXPECT_FALSE(mgr.StartEncounter(id, 101)); // already started
    EXPECT_FALSE(mgr.StartEncounter(id, 999)); // non-existent
}

TEST(Instance_CompleteEncounter)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    mgr.StartEncounter(id, 101);
    EXPECT_TRUE(mgr.CompleteEncounter(id, 101));
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->encounterStates.at(101)), static_cast<int>(EncounterState::Done));
    EXPECT_FALSE(mgr.CompleteEncounter(id, 102)); // not InProgress
}

TEST(Instance_FailEncounter)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    mgr.StartEncounter(id, 101);
    EXPECT_TRUE(mgr.FailEncounter(id, 101));
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->encounterStates.at(101)), static_cast<int>(EncounterState::Failed));
    EXPECT_FALSE(mgr.FailEncounter(id, 102));
}

TEST(Instance_ResetEncounter)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    mgr.StartEncounter(id, 101);
    mgr.FailEncounter(id, 101);
    EXPECT_TRUE(mgr.ResetEncounter(id, 101));
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->encounterStates.at(101)),
              static_cast<int>(EncounterState::NotStarted));
    EXPECT_TRUE(mgr.StartEncounter(id, 101)); // restartable
}

TEST(Instance_AllEncountersDone)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    EXPECT_FALSE(mgr.AllEncountersDone(id));
    mgr.StartEncounter(id, 101);
    mgr.CompleteEncounter(id, 101);
    EXPECT_FALSE(mgr.AllEncountersDone(id));
    mgr.StartEncounter(id, 102);
    mgr.CompleteEncounter(id, 102);
    EXPECT_TRUE(mgr.AllEncountersDone(id)); // optional 103 skipped
    EXPECT_TRUE(mgr.GetInstance(id)->completed);
    EXPECT_EQ(mgr.GetCompletedEncounterCount(id), 2);
}

TEST(Instance_LockoutCreationAndCheck)
{
    InstanceManager mgr;
    EXPECT_FALSE(mgr.HasLockout(10, 1));
    mgr.AddLockout(10, 1, 3600.0f);
    EXPECT_TRUE(mgr.HasLockout(10, 1));
    EXPECT_FALSE(mgr.HasLockout(10, 2)); // different template
    EXPECT_FALSE(mgr.HasLockout(20, 1)); // different player
}

TEST(Instance_LockoutExpiry)
{
    InstanceManager mgr;
    mgr.AddLockout(10, 1, 0.0f);
    mgr.ClearExpiredLockouts(10);
    EXPECT_FALSE(mgr.HasLockout(10, 1)); // expired and cleared
    mgr.AddLockout(10, 2, 3600.0f);
    mgr.ClearExpiredLockouts(10);
    EXPECT_TRUE(mgr.HasLockout(10, 2)); // survives clearing
}

TEST(Instance_MaxPlayersEnforcement)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate(1, 3));
    auto id = mgr.CreateInstance(1);
    EXPECT_TRUE(mgr.AddPlayer(id, 10));
    EXPECT_TRUE(mgr.AddPlayer(id, 20));
    EXPECT_TRUE(mgr.AddPlayer(id, 30));
    EXPECT_FALSE(mgr.AddPlayer(id, 40)); // exceeds cap
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id)->players.size()), 3);
    mgr.RemovePlayer(id, 20);
    EXPECT_TRUE(mgr.AddPlayer(id, 40)); // slot opened
}

TEST(Instance_DestroyInstanceCleanup)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id = mgr.CreateInstance(1);
    mgr.AddPlayer(id, 10);
    mgr.AddPlayer(id, 20);
    EXPECT_TRUE(mgr.DestroyInstance(id));
    EXPECT_TRUE(mgr.GetInstance(id) == nullptr);
    EXPECT_FALSE(mgr.GetPlayerInstance(10).has_value());
    EXPECT_FALSE(mgr.GetPlayerInstance(20).has_value());
    EXPECT_FALSE(mgr.DestroyInstance(id));
    EXPECT_EQ(mgr.GetInstanceCount(), (size_t)0);
}

TEST(Instance_MultipleInstancesSameTemplate)
{
    InstanceManager mgr;
    mgr.RegisterTemplate(MakeTestTemplate());
    auto id1 = mgr.CreateInstance(1);
    auto id2 = mgr.CreateInstance(1);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(mgr.GetInstanceCount(), (size_t)2);
    mgr.AddPlayer(id1, 10);
    mgr.AddPlayer(id2, 20);
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id1)->players.size()), 1);
    EXPECT_EQ(static_cast<int>(mgr.GetInstance(id2)->players.size()), 1);
    mgr.StartEncounter(id1, 101);
    mgr.CompleteEncounter(id1, 101);
    EXPECT_EQ(mgr.GetCompletedEncounterCount(id1), 1);
    EXPECT_EQ(mgr.GetCompletedEncounterCount(id2), 0);
}
