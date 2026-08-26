#include "TestFramework.h"

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/WorldServer.h"

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace Spark::Net;

static_assert(std::is_same_v<decltype(std::declval<const WorldServer&>().GetAreaInfoSnapshot(AreaID{})),
                             std::optional<AreaRegistration>>);
static_assert(std::is_same_v<decltype(std::declval<const WorldServer&>().GetPlayerSessionSnapshot(ClientID{})),
                             std::optional<PlayerSession>>);
static_assert(!std::is_pointer_v<decltype(std::declval<const WorldServer&>().GetAreaInfoSnapshot(AreaID{}))>);
static_assert(!std::is_pointer_v<decltype(std::declval<const WorldServer&>().GetPlayerSessionSnapshot(ClientID{}))>);

namespace
{
    AreaServerConfig MakeAreaConfig(const std::string& name)
    {
        AreaServerConfig config;
        config.areaName = name;
        config.maxClients = 32;
        return config;
    }
} // namespace

TEST(WorldServerSnapshot_AreaValueOutlivesMapEntry)
{
    WorldServer server;
    const AreaID areaId = server.RegisterAreaServer(MakeAreaConfig("snapshot-area"));

    const auto snapshot = server.GetAreaInfoSnapshot(areaId);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->areaId, areaId);
    EXPECT_EQ(snapshot->areaName, std::string("snapshot-area"));

    server.UnregisterAreaServer(areaId);

    EXPECT_FALSE(server.GetAreaInfoSnapshot(areaId).has_value());
    EXPECT_EQ(snapshot->areaId, areaId);
    EXPECT_EQ(snapshot->areaName, std::string("snapshot-area"));
    EXPECT_EQ(server.GetStats().activeAreas, 0u);
}

TEST(WorldServerSnapshot_PlayerValueDoesNotTrackMutation)
{
    WorldServer server;
    const AreaID firstArea = server.RegisterAreaServer(MakeAreaConfig("first"));
    const AreaID secondArea = server.RegisterAreaServer(MakeAreaConfig("second"));
    const ClientID clientId = 41;

    const AreaID assignedArea = server.HandlePlayerConnect(clientId, "snapshot-player", XMFLOAT3{1.0f, 1.0f, 1.0f});
    ASSERT_TRUE(assignedArea != INVALID_AREA);
    const AreaID targetArea = assignedArea == firstArea ? secondArea : firstArea;

    const auto beforeTransfer = server.GetPlayerSessionSnapshot(clientId);
    ASSERT_TRUE(beforeTransfer.has_value());
    EXPECT_EQ(beforeTransfer->pendingArea, INVALID_AREA);
    EXPECT_FALSE(beforeTransfer->isTransferring);

    ASSERT_TRUE(server.TransferPlayer(clientId, targetArea));
    const auto duringTransfer = server.GetPlayerSessionSnapshot(clientId);
    ASSERT_TRUE(duringTransfer.has_value());
    EXPECT_EQ(duringTransfer->pendingArea, targetArea);
    EXPECT_TRUE(duringTransfer->isTransferring);

    EXPECT_EQ(beforeTransfer->pendingArea, INVALID_AREA);
    EXPECT_FALSE(beforeTransfer->isTransferring);
    EXPECT_EQ(server.GetStats().totalAreaTransfers, 1u);
}

TEST(WorldServerConcurrency_StatsAndAreaSnapshotsRemainSafe)
{
    WorldServer server;
    std::atomic<bool> writerFinished{false};
    std::atomic<bool> writerFailed{false};
    std::atomic<bool> invalidSnapshotObserved{false};
    std::atomic<AreaID> currentArea{INVALID_AREA};
    std::atomic<uint32_t> readPasses{0};

    std::thread writer(
        [&]()
        {
            for (uint32_t i = 0; i < 64; ++i)
            {
                const AreaID areaId = server.RegisterAreaServer(MakeAreaConfig("concurrent-area"));
                if (areaId == INVALID_AREA)
                {
                    writerFailed.store(true, std::memory_order_release);
                    break;
                }
                currentArea.store(areaId, std::memory_order_release);
                server.UnregisterAreaServer(areaId);
                currentArea.store(INVALID_AREA, std::memory_order_release);
            }
            writerFinished.store(true, std::memory_order_release);
        });

    std::thread reader(
        [&]()
        {
            do
            {
                const WorldServerStats stats = server.GetStats();
                if (stats.activeAreas > 1u)
                {
                    invalidSnapshotObserved.store(true, std::memory_order_release);
                }

                const AreaID areaId = currentArea.load(std::memory_order_acquire);
                if (areaId != INVALID_AREA)
                {
                    const auto area = server.GetAreaInfoSnapshot(areaId);
                    if (area.has_value() && area->areaId != areaId)
                    {
                        invalidSnapshotObserved.store(true, std::memory_order_release);
                    }
                }
                (void)server.GetAllAreas();
                readPasses.fetch_add(1, std::memory_order_relaxed);
            } while (!writerFinished.load(std::memory_order_acquire));
        });

    writer.join();
    reader.join();

    EXPECT_FALSE(writerFailed.load(std::memory_order_acquire));
    EXPECT_FALSE(invalidSnapshotObserved.load(std::memory_order_acquire));
    EXPECT_TRUE(readPasses.load(std::memory_order_relaxed) > 0u);
    EXPECT_TRUE(server.GetAllAreas().empty());
    EXPECT_EQ(server.GetStats().activeAreas, 0u);
}

#endif // ENABLE_NETWORKING
