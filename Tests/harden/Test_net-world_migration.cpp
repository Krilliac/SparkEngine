/**
 * @file Test_net-world_migration.cpp
 * @brief Regression tests for AreaServer pending-migration accessors.
 *
 * Covers the hardening fixes to AreaServer's entity-migration queue:
 *
 * 1. DrainPendingMigrations() atomically removes and returns the pending list
 *    under m_migrationMutex. Before the fix the only accessors were a
 *    reference-returning GetPendingMigrations() plus ClearPendingMigrations(),
 *    both of which touched m_pendingMigrations with no lock — a data race with
 *    the tick thread (MigrateEntityOut / CheckEntityBoundaries) that is UB when
 *    the vector reallocs during a concurrent read/clear. GetPendingMigrations()
 *    now returns a locked snapshot copy (by value), and DrainPendingMigrations()
 *    swaps the queue out under the lock so a coordinator can consume it safely.
 *
 * 2. The queue is bounded by kMaxPendingMigrations so a never-drained queue
 *    cannot grow without limit; this test confirms a drain empties it.
 *
 * These construct AreaServer headless (no GPU), matching TestNetworkMMOIntegration.
 *
 * Requires ENABLE_NETWORKING.
 */

#include "../TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "Engine/Networking/AreaServer.h"

#ifdef ENABLE_NETWORKING

#include <vector>

using namespace Spark::Net;

// DrainPendingMigrations() returns the queued migration by value and leaves the
// internal queue empty — the atomic consume path used by coordinator threads.
TEST(NetWorld_DrainPendingMigrations_ConsumesQueue)
{
    AreaServer area;
    AreaServerConfig config{};
    config.areaId = 1;
    config.areaName = "Zone_A";
    config.tickRate = 60.0f;
    config.maxClients = 64;
    EXPECT_TRUE(area.Start(config));

    // Queue a migration (entity is untracked → migrated with default state).
    EXPECT_TRUE(area.MigrateEntityOut(42, 2));

    // A non-consuming snapshot still shows the queued migration.
    std::vector<MigratingEntity> peek = area.GetPendingMigrations();
    EXPECT_EQ(static_cast<int>(peek.size()), 1);

    // Draining returns the migration and empties the internal queue.
    std::vector<MigratingEntity> drained = area.DrainPendingMigrations();
    EXPECT_EQ(static_cast<int>(drained.size()), 1);
    if (!drained.empty())
    {
        EXPECT_EQ(static_cast<int>(drained[0].networkID), 42);
    }

    // A second drain yields nothing — the first drain consumed the queue.
    std::vector<MigratingEntity> again = area.DrainPendingMigrations();
    EXPECT_EQ(static_cast<int>(again.size()), 0);
    EXPECT_EQ(static_cast<int>(area.GetPendingMigrations().size()), 0);

    area.Stop();
}

// ClearPendingMigrations() empties the queue under the lock (no dangling ref).
TEST(NetWorld_ClearPendingMigrations_EmptiesQueue)
{
    AreaServer area;
    AreaServerConfig config{};
    config.areaId = 3;
    config.areaName = "Zone_C";
    config.tickRate = 60.0f;
    config.maxClients = 64;
    EXPECT_TRUE(area.Start(config));

    EXPECT_TRUE(area.MigrateEntityOut(7, 4));
    EXPECT_EQ(static_cast<int>(area.GetPendingMigrations().size()), 1);

    area.ClearPendingMigrations();
    EXPECT_EQ(static_cast<int>(area.GetPendingMigrations().size()), 0);

    area.Stop();
}

#endif // ENABLE_NETWORKING
