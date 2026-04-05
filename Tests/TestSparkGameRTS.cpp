// TestSparkGameRTS.cpp - Tests for RTS game module mechanics
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TestRTS
{

    struct ResourceNode
    {
        std::string type = "Gold";
        int amountRemaining = 1000;
        float gatherRatePerWorker = 10.0f; // per second
    };

    struct ResourceStorage
    {
        int gold = 0;
        int wood = 0;
        int food = 0;

        void Add(const std::string& type, int amount)
        {
            if (type == "Gold")
            {
                gold += amount;
            }
            else if (type == "Wood")
            {
                wood += amount;
            }
            else if (type == "Food")
            {
                food += amount;
            }
        }
    };

    static void SimulateGathering(ResourceNode& node, ResourceStorage& storage, int workerCount, float seconds)
    {
        int gathered = static_cast<int>(node.gatherRatePerWorker * static_cast<float>(workerCount) * seconds);
        gathered = std::min(gathered, node.amountRemaining);
        node.amountRemaining -= gathered;
        storage.Add(node.type, gathered);
    }

    enum class Faction
    {
        Red,
        Blue,
        Neutral
    };

    struct Unit
    {
        std::string name;
        Faction faction = Faction::Red;
        int supplyCost = 1;
        float posX = 0.0f;
        float posY = 0.0f;
        float distanceTraveled = 0.0f;
    };

    struct BuildQueueEntry
    {
        std::string unitName;
        float buildTime = 10.0f;
        float elapsed = 0.0f;
        int supplyCost = 1;

        bool IsComplete() const { return elapsed >= buildTime; }
    };

    struct Building
    {
        std::string name;
        Faction faction = Faction::Red;
        float constructionTime = 15.0f;
        float constructionElapsed = 0.0f;
        bool constructed = false;
        std::vector<BuildQueueEntry> productionQueue;

        bool IsConstructed() const { return constructed; }

        void AdvanceConstruction(float dt)
        {
            if (constructed)
            {
                return;
            }
            constructionElapsed += dt;
            if (constructionElapsed >= constructionTime)
            {
                constructed = true;
            }
        }

        void QueueUnit(const std::string& unitName, float buildTime, int supplyCost)
        {
            productionQueue.push_back({unitName, buildTime, 0.0f, supplyCost});
        }

        // Returns name of produced unit if front of queue completes, empty otherwise
        std::string AdvanceProduction(float dt)
        {
            if (!constructed || productionQueue.empty())
            {
                return "";
            }

            auto& front = productionQueue.front();
            front.elapsed += dt;
            if (front.IsComplete())
            {
                std::string produced = front.unitName;
                productionQueue.erase(productionQueue.begin());
                return produced;
            }
            return "";
        }
    };

    struct Army
    {
        std::vector<Unit> units;
        int supplyCap = 20;

        int CurrentSupply() const
        {
            int total = 0;
            for (const auto& u : units)
            {
                total += u.supplyCost;
            }
            return total;
        }

        bool CanSpawn(int supplyCost) const { return CurrentSupply() + supplyCost <= supplyCap; }

        bool Spawn(const std::string& name, Faction faction, int supplyCost)
        {
            if (!CanSpawn(supplyCost))
            {
                return false;
            }
            units.push_back({name, faction, supplyCost, 0.0f, 0.0f, 0.0f});
            return true;
        }

        int CountByFaction(Faction faction) const
        {
            int count = 0;
            for (const auto& u : units)
            {
                if (u.faction == faction)
                {
                    count++;
                }
            }
            return count;
        }
    };

} // namespace TestRTS

TEST(RTS_ResourceAccumulation)
{
    TestRTS::ResourceNode goldMine;
    goldMine.type = "Gold";
    goldMine.amountRemaining = 500;
    goldMine.gatherRatePerWorker = 10.0f;

    TestRTS::ResourceStorage storage;

    // 3 workers gathering for 5 seconds: 10 * 3 * 5 = 150 gold
    TestRTS::SimulateGathering(goldMine, storage, 3, 5.0f);
    EXPECT_EQ(storage.gold, 150);
    EXPECT_EQ(goldMine.amountRemaining, 350);

    // Gather more than remaining: capped at what's left
    TestRTS::SimulateGathering(goldMine, storage, 5, 100.0f);
    EXPECT_EQ(storage.gold, 500);
    EXPECT_EQ(goldMine.amountRemaining, 0);
}

TEST(RTS_BuildQueue)
{
    TestRTS::Building barracks;
    barracks.name = "Barracks";
    barracks.constructed = true; // Already built

    barracks.QueueUnit("Soldier", 5.0f, 1);
    EXPECT_EQ(static_cast<int>(barracks.productionQueue.size()), 1);

    // Advance 3 seconds: not done yet
    std::string produced = barracks.AdvanceProduction(3.0f);
    EXPECT_TRUE(produced.empty());
    EXPECT_EQ(static_cast<int>(barracks.productionQueue.size()), 1);

    // Advance 2 more seconds: unit produced
    produced = barracks.AdvanceProduction(2.0f);
    EXPECT_EQ(produced, std::string("Soldier"));
    EXPECT_EQ(static_cast<int>(barracks.productionQueue.size()), 0);
}

TEST(RTS_UnitSelection)
{
    TestRTS::Army army;
    army.supplyCap = 50;

    army.Spawn("Soldier", TestRTS::Faction::Red, 1);
    army.Spawn("Soldier", TestRTS::Faction::Red, 1);
    army.Spawn("Archer", TestRTS::Faction::Red, 1);
    army.Spawn("Scout", TestRTS::Faction::Blue, 1);
    army.Spawn("Scout", TestRTS::Faction::Blue, 1);

    EXPECT_EQ(army.CountByFaction(TestRTS::Faction::Red), 3);
    EXPECT_EQ(army.CountByFaction(TestRTS::Faction::Blue), 2);
    EXPECT_EQ(army.CountByFaction(TestRTS::Faction::Neutral), 0);
    EXPECT_EQ(static_cast<int>(army.units.size()), 5);
}

TEST(RTS_SupplyLimit)
{
    TestRTS::Army army;
    army.supplyCap = 5;

    // Spawn 4 units at cost 1 each
    EXPECT_TRUE(army.Spawn("Soldier", TestRTS::Faction::Red, 1));
    EXPECT_TRUE(army.Spawn("Soldier", TestRTS::Faction::Red, 1));
    EXPECT_TRUE(army.Spawn("Soldier", TestRTS::Faction::Red, 1));
    EXPECT_TRUE(army.Spawn("Soldier", TestRTS::Faction::Red, 1));
    EXPECT_EQ(army.CurrentSupply(), 4);

    // Spawn a unit costing 2: would exceed cap (4 + 2 = 6 > 5)
    EXPECT_FALSE(army.Spawn("Knight", TestRTS::Faction::Red, 2));
    EXPECT_EQ(army.CurrentSupply(), 4);

    // Spawn a unit costing 1: exactly at cap
    EXPECT_TRUE(army.Spawn("Archer", TestRTS::Faction::Red, 1));
    EXPECT_EQ(army.CurrentSupply(), 5);

    // No more room
    EXPECT_FALSE(army.Spawn("Scout", TestRTS::Faction::Red, 1));
}

TEST(RTS_BuildingConstruction)
{
    TestRTS::Building tower;
    tower.name = "Watch Tower";
    tower.constructionTime = 10.0f;

    EXPECT_FALSE(tower.IsConstructed());

    // Advance 6 seconds: not done
    tower.AdvanceConstruction(6.0f);
    EXPECT_FALSE(tower.IsConstructed());
    EXPECT_NEAR(tower.constructionElapsed, 6.0f, 0.001f);

    // Advance 4 more seconds: complete
    tower.AdvanceConstruction(4.0f);
    EXPECT_TRUE(tower.IsConstructed());

    // Further advances do nothing
    tower.AdvanceConstruction(5.0f);
    EXPECT_TRUE(tower.IsConstructed());
}
