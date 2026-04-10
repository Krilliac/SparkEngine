/**
 * @file TestConnectionScopeWiring.cpp
 * @brief Verifies that NetworkManager and ConnectionScopeFilter are wired together
 *
 * Prior sessions added ConnectionScopeFilter (per-connection interest management)
 * and TestConnectionScopeFilter exercises the filter in isolation, but the filter
 * was never called from NetworkManager's replication path. These tests verify the
 * new wiring:
 *   - NetworkManager::SetClientScope forwards into ConnectionScopeFilter
 *   - NetworkManager::ClearClientScope removes the scope
 *   - ReplicatedEntity exposes the areaId/teamMask/visibilityMask fields the
 *     replication loop consults when deciding whether to send updates
 */

#include "TestFramework.h"

// Platform stubs for test builds (no Windows headers)
#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "../SparkEngine/Source/Engine/Networking/ConnectionScopeFilter.h"
#include "../SparkEngine/Source/Engine/Networking/NetworkManager.h"

using namespace Spark::Net;

// Helper — reset both singletons between tests so state does not leak
static void ResetState()
{
    NetworkManager::GetInstance().Shutdown();
    ConnectionScopeFilter::GetInstance().Shutdown();
}

// ============================================================================
// 1. SetClientScope forwards into the ConnectionScopeFilter singleton
// ============================================================================

TEST(ConnectionScopeWiring_SetClientScopeRegistersFilter)
{
    ResetState();
    auto& nm = NetworkManager::GetInstance();
    auto& filter = ConnectionScopeFilter::GetInstance();

    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(0));

    XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    nm.SetClientScope(42u, origin, 50.0f);

    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(1));

    // An entity inside the sphere should be in-scope; outside should not.
    EXPECT_TRUE(filter.IsEntityInScope(42u, {10.0f, 0.0f, 0.0f}, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu));
    EXPECT_FALSE(filter.IsEntityInScope(42u, {200.0f, 0.0f, 0.0f}, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu));

    ResetState();
}

// ============================================================================
// 2. ClearClientScope removes the scope
// ============================================================================

TEST(ConnectionScopeWiring_ClearClientScopeRemovesFilter)
{
    ResetState();
    auto& nm = NetworkManager::GetInstance();
    auto& filter = ConnectionScopeFilter::GetInstance();

    nm.SetClientScope(7u, {0.0f, 0.0f, 0.0f}, 100.0f);
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(1));

    nm.ClearClientScope(7u);
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(0));

    // With no scope set, every entity is in-scope (default accept).
    EXPECT_TRUE(filter.IsEntityInScope(7u, {10000.0f, 10000.0f, 10000.0f}, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu));

    ResetState();
}

// ============================================================================
// 3. Rewriting a scope replaces the previous one (no accumulation)
// ============================================================================

TEST(ConnectionScopeWiring_SetClientScopeReplacesPrevious)
{
    ResetState();
    auto& nm = NetworkManager::GetInstance();
    auto& filter = ConnectionScopeFilter::GetInstance();

    nm.SetClientScope(1u, {0.0f, 0.0f, 0.0f}, 10.0f);
    EXPECT_FALSE(filter.IsEntityInScope(1u, {50.0f, 0.0f, 0.0f}, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu));

    // Grow the radius — the previously out-of-scope entity should now be in-scope.
    nm.SetClientScope(1u, {0.0f, 0.0f, 0.0f}, 100.0f);
    EXPECT_TRUE(filter.IsEntityInScope(1u, {50.0f, 0.0f, 0.0f}, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu));

    // There should still be exactly one scope entry — replacement, not accumulation.
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(1));

    ResetState();
}

// ============================================================================
// 4. Team mask filter forwarding
// ============================================================================

TEST(ConnectionScopeWiring_SetClientScopeAppliesTeamMask)
{
    ResetState();
    auto& nm = NetworkManager::GetInstance();
    auto& filter = ConnectionScopeFilter::GetInstance();

    // Client on team 0x1 only, large radius so distance does not interfere.
    nm.SetClientScope(5u, {0.0f, 0.0f, 0.0f}, 10000.0f, /*areaId*/ 0u,
                      /*teamMask*/ 0x1u, /*visibilityMask*/ 0xFFFFFFFFu);

    // Entity on team 0x1 — visible.
    EXPECT_TRUE(filter.IsEntityInScope(5u, {1.0f, 0.0f, 0.0f}, 0u, 0x1u, 0xFFFFFFFFu));

    // Entity on team 0x2 — filtered.
    EXPECT_FALSE(filter.IsEntityInScope(5u, {1.0f, 0.0f, 0.0f}, 0u, 0x2u, 0xFFFFFFFFu));

    ResetState();
}

// ============================================================================
// 5. ReplicatedEntity exposes the scope metadata fields
//    — guards against accidental removal
// ============================================================================

TEST(ConnectionScopeWiring_ReplicatedEntityHasScopeFields)
{
    ReplicatedEntity entity;
    // Defaults should be "accept all" so existing code that does not set these
    // continues to replicate everywhere.
    EXPECT_EQ(entity.areaId, 0u);
    EXPECT_EQ(entity.teamMask, 0xFFFFFFFFu);
    EXPECT_EQ(entity.visibilityMask, 0xFFFFFFFFu);

    // Assignment should work — the replication loop writes these from game code.
    entity.areaId = 3u;
    entity.teamMask = 0x01u;
    entity.visibilityMask = 0xF0u;
    EXPECT_EQ(entity.areaId, 3u);
    EXPECT_EQ(entity.teamMask, 0x01u);
    EXPECT_EQ(entity.visibilityMask, 0xF0u);
}

// ============================================================================
// 6. Clearing a non-existent scope is a no-op (idempotent)
// ============================================================================

TEST(ConnectionScopeWiring_ClearNonexistentScopeIsSafe)
{
    ResetState();
    auto& nm = NetworkManager::GetInstance();
    auto& filter = ConnectionScopeFilter::GetInstance();

    // No scopes set yet.
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(0));
    nm.ClearClientScope(999u);
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(0));

    ResetState();
}
