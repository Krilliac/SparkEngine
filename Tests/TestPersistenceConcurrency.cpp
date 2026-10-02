/**
 * @file TestPersistenceConcurrency.cpp
 * @brief DATA-120 concurrent economy and territory writes against real stores.
 *
 * The workers use separate TFDatabase instances and a real territory JSON
 * file. Each iteration reloads the committed value while holding the
 * production store's authority lock, applies one delta, and commits it. The
 * final totals therefore prove that concurrent read-modify-write operations
 * are serialized without lost state.
 */
#include "TestFramework.h"

#include "Data/TFDataTables.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFSavePaths.h"
#include "Persistence/TFWorldSave.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace Terrafront;

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshPath(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove(path.string() + ".lock");
        fs::remove(path.string() + ".authority.cindral_wastes.lock");
        fs::remove(path.string() + ".tmp");
        return path;
    }

    uint64_t SeedCharacter(TFDatabase& db)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.CreateAccount("concurrent-economy", "salt", "hash", account))
        {
            return 0;
        }
        if (!db.CreateCharacter(account.id, "Concurrent Wallet", FactionId::MRA, character))
        {
            return 0;
        }
        return character.id;
    }

    bool AddFlux(TFDatabase& db, uint64_t charId)
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            TFCharacterRecord row;
            if (!db.AcquireCharacter(charId, row))
            {
                if (db.LastStatus() != TFDatabaseStatus::Locked)
                {
                    return false;
                }
                std::this_thread::yield();
                continue;
            }

            if (db.SaveCharacterProgress(charId, row.xp + 1, row.rank, row.flux + 1, row.lastPlayedMs + 1))
            {
                return true;
            }
            if (db.LastStatus() != TFDatabaseStatus::Conflict && db.LastStatus() != TFDatabaseStatus::Locked)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return false;
    }

    std::vector<RegionDef> TerritoryRegions()
    {
        std::vector<RegionDef> regions(3);
        regions[0].tier = "skyanchor";
        regions[0].homeFaction = FactionId::MRA;
        regions[1].tier = "outpost";
        regions[2].tier = "fort";
        return regions;
    }

    Spark::Json::Value TerritorySnapshot(std::string_view key, std::string_view name, int owner)
    {
        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["version"] = Spark::Json::Value(static_cast<int>(WorldSave::kTerritorySchemaVersion));
        root["continentKey"] = Spark::Json::Value(std::string(key));
        root["continent"] = Spark::Json::Value(std::string(name));
        root["regionCount"] = Spark::Json::Value(3);
        Spark::Json::Value owners = Spark::Json::Value::MakeArray();
        owners.PushBack(Spark::Json::Value(owner));
        owners.PushBack(Spark::Json::Value(static_cast<int>(FactionId::None)));
        owners.PushBack(Spark::Json::Value(static_cast<int>(FactionId::None)));
        root["owners"] = owners;
        Spark::Json::Value dominion = Spark::Json::Value::MakeObject();
        dominion["active"] = Spark::Json::Value(false);
        dominion["faction"] = Spark::Json::Value(static_cast<int>(FactionId::None));
        dominion["remainingSec"] = Spark::Json::Value(0.0);
        root["dominion"] = dominion;
        return root;
    }

} // namespace

TEST(Persistence_Concurrency_ConcurrentEconomyWritersRetainEveryDelta)
{
    const fs::path path = FreshPath("test_data120_concurrent_economy.db");
    TFDatabase seed;
    ASSERT_TRUE(seed.Open(path));
    const uint64_t charId = SeedCharacter(seed);
    ASSERT_TRUE(charId != 0);
    TFCharacterRecord baseline;
    ASSERT_TRUE(seed.FindCharacter(charId, baseline));
    ASSERT_TRUE(seed.Close());

    constexpr int kWriters = 4;
    constexpr int kRounds = 12;
    std::barrier start(kWriters);
    std::vector<std::future<bool>> workers;
    for (int writer = 0; writer < kWriters; ++writer)
    {
        workers.push_back(std::async(std::launch::async,
                                     [path, charId, &start]
                                     {
                                         TFDatabase db;
                                         if (!db.Open(path))
                                         {
                                             start.arrive_and_drop();
                                             return false;
                                         }
                                         start.arrive_and_wait();
                                         for (int round = 0; round < kRounds; ++round)
                                         {
                                             if (!AddFlux(db, charId))
                                             {
                                                 db.Close();
                                                 return false;
                                             }
                                         }
                                         return db.Close();
                                     }));
    }

    for (auto& worker : workers)
    {
        EXPECT_TRUE(worker.get());
    }

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    TFCharacterRecord row;
    ASSERT_TRUE(observer.FindCharacter(charId, row));
    EXPECT_EQ(row.flux, uint32_t{kWriters * kRounds});
    EXPECT_EQ(row.xp, uint32_t{kWriters * kRounds});
    EXPECT_EQ(row.lastPlayedMs, baseline.lastPlayedMs + int64_t{kWriters * kRounds});
    EXPECT_TRUE(observer.Close());
    fs::remove(path);
}

// Territory files are single-writer: authorities for one continent race for its lease. Run the race on real
// threads twice: each round exactly one contender holds the lease, every other contender's write is refused,
// and the next owner starts from the previous owner's committed snapshot.
TEST(Persistence_Concurrency_ConcurrentTerritoryWritersPreserveCommittedState)
{
    const fs::path path = FreshPath("test_data120_concurrent_territory.json");
    const std::vector<RegionDef> regions = TerritoryRegions();
    const auto ownerOnDisk = [&]() -> int
    {
        Spark::Json::Value root;
        std::string detail;
        WorldSave::TerritoryDecode decoded;
        if (WorldSave::ReadJson(path, "cindral_wastes", "Cindral Wastes", false, root, detail) !=
                WorldSave::ReadStatus::Loaded ||
            WorldSave::DecodeTerritory(root, regions, false, decoded, detail) !=
                WorldSave::TerritoryDecodeResult::Loaded ||
            decoded.owners.size() != 3)
        {
            return -1;
        }
        return static_cast<int>(decoded.owners[1]);
    };

    constexpr FactionId kContenders[] = {FactionId::MRA, FactionId::AUC, FactionId::HLX};
    constexpr int kWriters = 3;
    int previousOwner = -1;
    for (int round = 0; round < 2; ++round)
    {
        std::barrier attempted(kWriters);
        std::barrier written(kWriters);
        std::atomic<int> winners{0};
        std::atomic<int> refusedWrites{0};
        std::atomic<int> winnerFaction{-1};
        std::atomic<int> ownerSeenByWinner{-2};
        std::vector<std::future<bool>> workers;
        for (int writer = 0; writer < kWriters; ++writer)
        {
            workers.push_back(std::async(
                std::launch::async,
                [&, writer]
                {
                    const FactionId faction = kContenders[writer];
                    SavePaths::ExclusiveFileLock lease;
                    std::error_code lockEc;
                    const bool won = lease.TryLock(path, lockEc);
                    if (won)
                    {
                        ++winners;
                    }
                    // Nobody releases or writes until every contender has tried to take the lease.
                    attempted.arrive_and_wait();
                    bool ok = true;
                    std::string detail;
                    if (won)
                    {
                        ownerSeenByWinner = ownerOnDisk();
                        for (int sequence = 0; sequence < 8; ++sequence)
                        {
                            auto snapshot =
                                TerritorySnapshot("cindral_wastes", "Cindral Wastes", static_cast<int>(FactionId::MRA));
                            snapshot["owners"][1] =
                                Spark::Json::Value(static_cast<int>(sequence % 2 == 0 ? FactionId::None : faction));
                            ok = ok && WorldSave::WriteJson(lease, path, snapshot, detail);
                        }
                        winnerFaction = static_cast<int>(faction);
                    }
                    else
                    {
                        auto snapshot =
                            TerritorySnapshot("cindral_wastes", "Cindral Wastes", static_cast<int>(FactionId::MRA));
                        snapshot["owners"][1] = Spark::Json::Value(static_cast<int>(faction));
                        if (!WorldSave::WriteJson(lease, path, snapshot, detail))
                        {
                            ++refusedWrites;
                        }
                    }
                    // The lease is held until every contender has finished writing.
                    written.arrive_and_wait();
                    return ok;
                }));
        }
        for (auto& worker : workers)
        {
            EXPECT_TRUE(worker.get());
        }

        EXPECT_EQ(winners.load(), 1);
        EXPECT_EQ(refusedWrites.load(), kWriters - 1);
        EXPECT_EQ(ownerSeenByWinner.load(), previousOwner);
        EXPECT_EQ(ownerOnDisk(), winnerFaction.load());
        previousOwner = ownerOnDisk();
    }
    fs::remove(path);
}
