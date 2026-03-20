// TestConnectionScope.cpp - Tests for per-connection object scoping
// Validates that entities enter/exit scope based on spatial proximity.

#include "TestFramework.h"

// Standalone reimplementation for testing (no NetworkManager dependency)
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TestScope
{
    using ClientID = uint32_t;

    struct ScopeChanges
    {
        std::vector<uint32_t> entered;
        std::vector<uint32_t> exited;
        std::vector<uint32_t> inScope;
    };

    struct ScopedEntity
    {
        uint32_t networkID = 0;
        float x = 0, y = 0, z = 0;
        float priorityBias = 1.0f;
        bool alwaysRelevant = false;
    };

    struct ClientViewpoint
    {
        float x = 0, y = 0, z = 0;
    };

    class ConnectionScope
    {
      public:
        void SetScopeRadius(float r) { m_radius = r; }
        float GetScopeRadius() const { return m_radius; }

        void AddClient(ClientID id)
        {
            m_vps[id] = {};
            m_scopes[id] = {};
        }

        void RemoveClient(ClientID id)
        {
            m_vps.erase(id);
            m_scopes.erase(id);
        }

        void SetClientViewpoint(ClientID id, float x, float y, float z) { m_vps[id] = {x, y, z}; }

        void AddEntity(uint32_t id, float x = 0, float y = 0, float z = 0, bool always = false)
        {
            m_entities[id] = {id, x, y, z, 1.0f, always};
        }

        void RemoveEntity(uint32_t id)
        {
            m_entities.erase(id);
            for (auto& [cid, scope] : m_scopes)
                scope.erase(id);
        }

        void UpdateEntityPosition(uint32_t id, float x, float y, float z)
        {
            auto it = m_entities.find(id);
            if (it != m_entities.end())
            {
                it->second.x = x;
                it->second.y = y;
                it->second.z = z;
            }
        }

        void SetEntityPriorityBias(uint32_t id, float bias)
        {
            auto it = m_entities.find(id);
            if (it != m_entities.end())
                it->second.priorityBias = bias;
        }

        ScopeChanges EvaluateScope(ClientID clientId)
        {
            ScopeChanges changes;
            auto vpIt = m_vps.find(clientId);
            auto scopeIt = m_scopes.find(clientId);
            if (vpIt == m_vps.end() || scopeIt == m_scopes.end())
                return changes;

            const auto& vp = vpIt->second;
            auto& prev = scopeIt->second;
            std::unordered_set<uint32_t> next;
            float rSq = m_radius * m_radius;

            for (const auto& [eid, e] : m_entities)
            {
                bool vis = e.alwaysRelevant;
                if (!vis)
                {
                    float dx = e.x - vp.x, dy = e.y - vp.y, dz = e.z - vp.z;
                    float distSq = dx * dx + dy * dy + dz * dz;
                    vis = (distSq <= rSq * e.priorityBias * e.priorityBias);
                }
                if (vis)
                    next.insert(eid);
            }

            for (uint32_t id : next)
            {
                if (prev.find(id) == prev.end())
                    changes.entered.push_back(id);
                changes.inScope.push_back(id);
            }
            for (uint32_t id : prev)
            {
                if (next.find(id) == next.end())
                    changes.exited.push_back(id);
            }

            prev = std::move(next);
            return changes;
        }

        bool IsInScope(ClientID cid, uint32_t eid) const
        {
            auto it = m_scopes.find(cid);
            if (it == m_scopes.end())
                return false;
            return it->second.count(eid) > 0;
        }

        size_t GetScopeCount(ClientID cid) const
        {
            auto it = m_scopes.find(cid);
            return it != m_scopes.end() ? it->second.size() : 0;
        }

      private:
        float m_radius = 500.0f;
        std::unordered_map<uint32_t, ScopedEntity> m_entities;
        std::unordered_map<ClientID, ClientViewpoint> m_vps;
        std::unordered_map<ClientID, std::unordered_set<uint32_t>> m_scopes;
    };
} // namespace TestScope

using namespace TestScope;

TEST(ConnectionScope_EntityEntersScope)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 50, 0, 0);

    auto changes = scope.EvaluateScope(1);
    EXPECT_EQ(changes.entered.size(), 1u);
    EXPECT_EQ(changes.entered[0], 10u);
    EXPECT_EQ(changes.exited.size(), 0u);
    EXPECT_EQ(changes.inScope.size(), 1u);
}

TEST(ConnectionScope_EntityExitsScope)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 50, 0, 0);

    scope.EvaluateScope(1); // Entity enters scope

    // Move entity far away
    scope.UpdateEntityPosition(10, 500, 0, 0);
    auto changes = scope.EvaluateScope(1);
    EXPECT_EQ(changes.entered.size(), 0u);
    EXPECT_EQ(changes.exited.size(), 1u);
    EXPECT_EQ(changes.exited[0], 10u);
}

TEST(ConnectionScope_EntityOutOfRange)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 200, 0, 0); // Beyond radius

    auto changes = scope.EvaluateScope(1);
    EXPECT_EQ(changes.entered.size(), 0u);
    EXPECT_EQ(changes.inScope.size(), 0u);
}

TEST(ConnectionScope_AlwaysRelevant)
{
    ConnectionScope scope;
    scope.SetScopeRadius(10.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 9999, 9999, 9999, true); // Far away but alwaysRelevant

    auto changes = scope.EvaluateScope(1);
    EXPECT_EQ(changes.inScope.size(), 1u);
    EXPECT_TRUE(scope.IsInScope(1, 10));
}

TEST(ConnectionScope_PriorityBiasExtendsRange)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 150, 0, 0); // Beyond 100 radius

    // Without bias: out of scope
    auto changes1 = scope.EvaluateScope(1);
    EXPECT_EQ(changes1.inScope.size(), 0u);

    // With bias 2.0: effective radius = 200, entity at 150 → in scope
    scope.SetEntityPriorityBias(10, 2.0f);
    auto changes2 = scope.EvaluateScope(1);
    EXPECT_EQ(changes2.inScope.size(), 1u);
}

TEST(ConnectionScope_MultipleClients)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.AddClient(2);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.SetClientViewpoint(2, 500, 0, 0);
    scope.AddEntity(10, 50, 0, 0);  // Near client 1
    scope.AddEntity(20, 450, 0, 0); // Near client 2

    auto c1 = scope.EvaluateScope(1);
    auto c2 = scope.EvaluateScope(2);

    // Client 1 sees entity 10 only
    EXPECT_EQ(c1.inScope.size(), 1u);
    EXPECT_TRUE(scope.IsInScope(1, 10));
    EXPECT_FALSE(scope.IsInScope(1, 20));

    // Client 2 sees entity 20 only
    EXPECT_EQ(c2.inScope.size(), 1u);
    EXPECT_TRUE(scope.IsInScope(2, 20));
    EXPECT_FALSE(scope.IsInScope(2, 10));
}

TEST(ConnectionScope_RemoveEntityCleansUpScopes)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 50, 0, 0);

    scope.EvaluateScope(1);
    EXPECT_TRUE(scope.IsInScope(1, 10));

    scope.RemoveEntity(10);
    EXPECT_FALSE(scope.IsInScope(1, 10));
}

TEST(ConnectionScope_StableInScope)
{
    ConnectionScope scope;
    scope.SetScopeRadius(100.0f);
    scope.AddClient(1);
    scope.SetClientViewpoint(1, 0, 0, 0);
    scope.AddEntity(10, 50, 0, 0);

    auto c1 = scope.EvaluateScope(1);
    EXPECT_EQ(c1.entered.size(), 1u);

    // Second eval: entity stays in scope, no entered/exited
    auto c2 = scope.EvaluateScope(1);
    EXPECT_EQ(c2.entered.size(), 0u);
    EXPECT_EQ(c2.exited.size(), 0u);
    EXPECT_EQ(c2.inScope.size(), 1u);
}
