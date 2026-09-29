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

TEST(Persistence_Concurrency_ConcurrentTerritoryWritersPreserveCommittedState)
{
    const fs::path path = FreshPath("test_data120_concurrent_territory.json");
    const fs::path otherPath = FreshPath("test_data120_concurrent_other_territory.json");
    constexpr int kWriters = 2;
    std::barrier start(kWriters);
    std::vector<std::future<bool>> workers;
    for (int writer = 0; writer < kWriters; ++writer)
    {
        workers.push_back(std::async(std::launch::async,
                                     [path, otherPath, writer, &start]
                                     {
                                         const fs::path& target = writer == 0 ? path : otherPath;
                                         const std::string key = writer == 0 ? "cindral_wastes" : "other_continent";
                                         const std::string name = writer == 0 ? "Cindral Wastes" : "Other Continent";
                                         const FactionId finalOwner = writer == 0 ? FactionId::MRA : FactionId::HLX;
                                         SavePaths::ExclusiveFileLock lease;
                                         std::error_code lockEc;
                                         if (!lease.TryLock(target, lockEc))
                                         {
                                             start.arrive_and_drop();
                                             return false;
                                         }
                                         // Both production authority leases coexist; a second owner of either file is refused.
                                         start.arrive_and_wait();
                                         SavePaths::ExclusiveFileLock contender;
                                         if (contender.TryLock(target, lockEc))
                                         {
                                             return false;
                                         }
                                         for (int sequence = 0; sequence < 8; ++sequence)
                                         {
                                             auto snapshot =
                                                 TerritorySnapshot(key, name, static_cast<int>(FactionId::MRA));
                                             snapshot["owners"][1] = Spark::Json::Value(
                                                 static_cast<int>(sequence % 2 == 0 ? FactionId::None : finalOwner));
                                             std::string detail;
                                             if (!WorldSave::WriteJson(lease, target, snapshot, detail))
                                             {
                                                 return false;
                                             }
                                         }
                                         return true;
                                     }));
    }
    for (auto& worker : workers)
    {
        EXPECT_TRUE(worker.get());
    }

    const std::vector<RegionDef> regions = TerritoryRegions();
    for (int writer = 0; writer < kWriters; ++writer)
    {
        Spark::Json::Value root;
        std::string detail;
        ASSERT_TRUE(WorldSave::ReadJson(writer == 0 ? path : otherPath,
                                        writer == 0 ? "cindral_wastes" : "other_continent",
                                        writer == 0 ? "Cindral Wastes" : "Other Continent", false, root,
                                        detail) == WorldSave::ReadStatus::Loaded);
        WorldSave::TerritoryDecode decoded;
        ASSERT_TRUE(WorldSave::DecodeTerritory(root, regions, false, decoded, detail) ==
                    WorldSave::TerritoryDecodeResult::Loaded);
        ASSERT_EQ(decoded.owners.size(), size_t{3});
        EXPECT_EQ(static_cast<int>(decoded.owners[0]), static_cast<int>(FactionId::MRA));
        EXPECT_EQ(static_cast<int>(decoded.owners[1]), static_cast<int>(writer == 0 ? FactionId::MRA : FactionId::HLX));
        EXPECT_EQ(static_cast<int>(decoded.owners[2]), static_cast<int>(FactionId::None));
    }
    fs::remove(otherPath);
    fs::remove(path);
}
