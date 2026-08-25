/**
 * @file TestGameModuleRTS.cpp
 * @brief Tests for SparkGameRTS module: units, buildings, and resources
 *
 * All tests are gated behind SPARK_TEST_HAS_IMGUI because the RTS .cpp
 * files depend on ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include <limits>

#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.h"

using namespace RTS;

// =============================================================================
// RTSUnitSystem
// =============================================================================

TEST(RTS_Unit_Initialize)
{
    RTSUnitSystem units;
    bool ok = units.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(RTS_Unit_SpawnReturnsValidId)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    uint32_t id = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 0.0f, 0.0f);
    EXPECT_TRUE(id > 0);
}

TEST(RTS_Unit_GetUnitVerifyTypeAndFaction)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    uint32_t id = units.SpawnUnit(RTSUnitType::Tank, RTSFaction::Sentinel, 5.0f, 10.0f);
    const UnitData* unit = units.GetUnit(id);
    EXPECT_TRUE(unit != nullptr);
    EXPECT_TRUE(unit->type == RTSUnitType::Tank);
    EXPECT_TRUE(unit->faction == RTSFaction::Sentinel);
}

TEST(RTS_Unit_CountIncrementsOnSpawn)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    size_t before = units.GetUnitCount();
    units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 0.0f, 0.0f);
    EXPECT_EQ(units.GetUnitCount(), before + 1);
    units.SpawnUnit(RTSUnitType::Scout, RTSFaction::Human, 1.0f, 1.0f);
    EXPECT_EQ(units.GetUnitCount(), before + 2);
}

TEST(RTS_Unit_GetUnitsByFaction)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 0.0f, 0.0f);
    units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Swarm, 1.0f, 1.0f);
    units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 2.0f, 2.0f);

    auto humanUnits = units.GetUnitsByFaction(RTSFaction::Human);
    auto swarmUnits = units.GetUnitsByFaction(RTSFaction::Swarm);
    EXPECT_EQ(humanUnits.size(), 2u);
    EXPECT_EQ(swarmUnits.size(), 1u);
}

TEST(RTS_Unit_KillRemovesFromCount)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    uint32_t id = units.SpawnUnit(RTSUnitType::Flyer, RTSFaction::Sentinel, 0.0f, 0.0f);
    size_t countAfterSpawn = units.GetUnitCount();
    units.KillUnit(id);
    EXPECT_EQ(units.GetUnitCount(), countAfterSpawn - 1);
    EXPECT_TRUE(units.GetUnit(id) == nullptr);
}

TEST(RTS_Unit_SetStateAndTarget)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    uint32_t id = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 0.0f, 0.0f);
    units.SetUnitState(id, RTSUnitState::Attacking);
    units.SetUnitTarget(id, 99);
    const UnitData* unit = units.GetUnit(id);
    EXPECT_TRUE(unit->state == RTSUnitState::Attacking);
    EXPECT_EQ(unit->targetId, 99u);
}

TEST(RTS_Unit_ListStringNonEmpty)
{
    RTSUnitSystem units;
    units.Initialize(nullptr);
    units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Swarm, 0.0f, 0.0f);
    std::string list = units.GetUnitListString();
    EXPECT_TRUE(!list.empty());
}

// =============================================================================
// RTSCommandSystem
// =============================================================================

TEST(RTS_Command_SelectAllOfTypeUsesLiveUnitRoster)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t firstMarine = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 0.0f, 0.0f);
    units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 1.0f, 0.0f);
    const uint32_t secondMarine = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 2.0f, 0.0f);
    units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Swarm, 3.0f, 0.0f);

    RTSCommandSystem commands;
    EXPECT_TRUE(commands.Initialize(nullptr, &units));
    commands.SelectAllOfType(RTSUnitType::Marine, RTSFaction::Human);

    EXPECT_EQ(commands.GetSelectionCount(), static_cast<size_t>(2));
    EXPECT_EQ(commands.GetSelection()[0], firstMarine);
    EXPECT_EQ(commands.GetSelection()[1], secondMarine);
}

TEST(RTS_Command_MoveAdvancesAndCompletesDeterministically)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t worker = units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 0.0f, 0.0f);

    RTSCommandSystem commands;
    EXPECT_TRUE(commands.Initialize(nullptr, &units));
    units.SetUnitTarget(worker, 99);
    commands.IssueCommand(worker, {RTSCommandType::Move, 3.0f, 4.0f, 0});
    commands.Update(1.0f);

    const UnitData* unit = units.GetUnit(worker);
    EXPECT_TRUE(unit != nullptr);
    EXPECT_NEAR(unit->posX, 1.8f, 0.001f);
    EXPECT_NEAR(unit->posY, 2.4f, 0.001f);
    EXPECT_TRUE(unit->state == RTSUnitState::Moving);
    EXPECT_EQ(unit->targetId, static_cast<uint32_t>(0));
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(1));

    commands.Update(1.0f);
    EXPECT_NEAR(unit->posX, 3.0f, 0.001f);
    EXPECT_NEAR(unit->posY, 4.0f, 0.001f);
    EXPECT_TRUE(unit->state == RTSUnitState::Idle);
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(0));
}

TEST(RTS_Command_QueueAdvancesAndStopClearsImmediately)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t scout = units.SpawnUnit(RTSUnitType::Scout, RTSFaction::Human, 0.0f, 0.0f);

    RTSCommandSystem commands;
    EXPECT_TRUE(commands.Initialize(nullptr, &units));
    commands.IssueCommand(scout, {RTSCommandType::Move, 1.0f, 0.0f, 0});
    commands.QueueCommand(scout, {RTSCommandType::Hold, 0.0f, 0.0f, 0});
    commands.Update(1.0f);
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(1));

    commands.Update(0.0f);
    EXPECT_TRUE(units.GetUnit(scout)->state == RTSUnitState::Holding);
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(0));

    commands.IssueCommand(scout, {RTSCommandType::Move, 10.0f, 0.0f, 0});
    commands.QueueCommand(scout, {RTSCommandType::Stop, 0.0f, 0.0f, 0});
    commands.IssueCommand(scout, {RTSCommandType::Stop, 0.0f, 0.0f, 0});
    commands.Update(0.0f);
    EXPECT_TRUE(units.GetUnit(scout)->state == RTSUnitState::Idle);
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(0));
}

TEST(RTS_Command_StateOnlyCommandsYieldToQueuedWork)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t worker = units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 0.0f, 0.0f);

    RTSCommandSystem commands;
    EXPECT_TRUE(commands.Initialize(nullptr, &units));

    const auto expectStateThenStop =
        [&](const UnitCommand& command, RTSUnitState expectedState, uint32_t expectedTarget)
    {
        units.SetUnitTarget(worker, 999);
        commands.IssueCommand(worker, command);
        commands.QueueCommand(worker, {RTSCommandType::Stop, 0.0f, 0.0f, 0});

        commands.Update(0.1f);
        EXPECT_TRUE(units.GetUnit(worker)->state == expectedState);
        EXPECT_EQ(units.GetUnit(worker)->targetId, expectedTarget);
        EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(1));

        commands.Update(0.1f);
        EXPECT_TRUE(units.GetUnit(worker)->state == RTSUnitState::Idle);
        EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(0));
    };

    expectStateThenStop({RTSCommandType::Attack, 0.0f, 0.0f, 77}, RTSUnitState::Attacking, 77);
    expectStateThenStop({RTSCommandType::Gather, 0.0f, 0.0f, 88}, RTSUnitState::Gathering, 88);
    expectStateThenStop({RTSCommandType::Build, 4.0f, 5.0f, 99}, RTSUnitState::Building, 99);
    expectStateThenStop({RTSCommandType::Patrol, 6.0f, 7.0f, 0}, RTSUnitState::Patrolling, 0);
}

TEST(RTS_Command_PrunesDestroyedUnitsAndRejectsNonFiniteTargets)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t marine = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 0.0f, 0.0f);

    RTSCommandSystem commands;
    EXPECT_TRUE(commands.Initialize(nullptr, &units));
    commands.Select(marine);
    commands.IssueCommand(marine, {RTSCommandType::Move, std::numeric_limits<float>::quiet_NaN(), 2.0f, 0});
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(0));

    commands.IssueCommand(marine, {RTSCommandType::Move, 5.0f, 0.0f, 0});
    units.KillUnit(marine);
    commands.Update(0.1f);
    EXPECT_EQ(commands.GetSelectionCount(), static_cast<size_t>(0));
    EXPECT_EQ(commands.GetPendingCommandCount(), static_cast<size_t>(0));
}

// =============================================================================
// RTSBuildingSystem
// =============================================================================

TEST(RTS_Building_Initialize)
{
    RTSBuildingSystem buildings;
    bool ok = buildings.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(RTS_Building_PlaceReturnsValidId)
{
    RTSBuildingSystem buildings;
    buildings.Initialize(nullptr);
    uint32_t id = buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 0.0f, 0.0f);
    EXPECT_TRUE(id > 0);
}

TEST(RTS_Building_GetBuildingVerifyTypeAndFaction)
{
    RTSBuildingSystem buildings;
    buildings.Initialize(nullptr);
    uint32_t id = buildings.PlaceBuilding(RTSBuildingType::Factory, RTSFaction::Sentinel, 10.0f, 20.0f);
    const BuildingData* b = buildings.GetBuilding(id);
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(b->type == RTSBuildingType::Factory);
    EXPECT_TRUE(b->faction == RTSFaction::Sentinel);
}

TEST(RTS_Building_CountAndCountByFaction)
{
    RTSBuildingSystem buildings;
    buildings.Initialize(nullptr);
    buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 0.0f, 0.0f);
    buildings.PlaceBuilding(RTSBuildingType::Turret, RTSFaction::Human, 5.0f, 5.0f);
    buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Swarm, 10.0f, 10.0f);
    EXPECT_EQ(buildings.GetBuildingCount(), 3u);
    EXPECT_EQ(buildings.GetBuildingCountByFaction(RTSFaction::Human), 2u);
    EXPECT_EQ(buildings.GetBuildingCountByFaction(RTSFaction::Swarm), 1u);
}

TEST(RTS_Building_DestroyDecrementsCount)
{
    RTSBuildingSystem buildings;
    buildings.Initialize(nullptr);
    uint32_t id = buildings.PlaceBuilding(RTSBuildingType::Refinery, RTSFaction::Sentinel, 0.0f, 0.0f);
    size_t countAfterPlace = buildings.GetBuildingCount();
    buildings.DestroyBuilding(id);
    EXPECT_EQ(buildings.GetBuildingCount(), countAfterPlace - 1);
}

TEST(RTS_Building_SupplyProvidedNonNegative)
{
    RTSBuildingSystem buildings;
    buildings.Initialize(nullptr);
    buildings.PlaceBuilding(RTSBuildingType::SupplyDepot, RTSFaction::Human, 0.0f, 0.0f);
    int supply = buildings.GetSupplyProvided(RTSFaction::Human);
    EXPECT_TRUE(supply >= 0);
}

TEST(RTS_Building_ListStringNonEmpty)
{
    RTSBuildingSystem buildings;
    buildings.Initialize(nullptr);
    buildings.PlaceBuilding(RTSBuildingType::CommandCenter, RTSFaction::Human, 0.0f, 0.0f);
    std::string list = buildings.GetBuildingListString();
    EXPECT_TRUE(!list.empty());
}

// =============================================================================
// RTSResourceSystem
// =============================================================================

TEST(RTS_Resource_Initialize)
{
    RTSResourceSystem resources;
    bool ok = resources.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(RTS_Resource_InitializePlayerAndGetResources)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    resources.InitializePlayer(RTSFaction::Human);
    const PlayerResources* pr = resources.GetPlayerResources(RTSFaction::Human);
    EXPECT_TRUE(pr != nullptr);
    EXPECT_TRUE(pr->minerals >= 0);
}

TEST(RTS_Resource_AddAndGetRoundTrip)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    resources.InitializePlayer(RTSFaction::Sentinel);
    const PlayerResources* before = resources.GetPlayerResources(RTSFaction::Sentinel);
    int mineralsBefore = before->minerals;
    resources.AddResources(RTSFaction::Sentinel, 200, 50);
    const PlayerResources* after = resources.GetPlayerResources(RTSFaction::Sentinel);
    EXPECT_EQ(after->minerals, mineralsBefore + 200);
    EXPECT_EQ(after->gas, 50);
}

TEST(RTS_Resource_CanAffordTrueAndFalse)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    resources.InitializePlayer(RTSFaction::Human);
    // Starting minerals = 400, gas = 0
    EXPECT_TRUE(resources.CanAfford(RTSFaction::Human, 100, 0));
    EXPECT_FALSE(resources.CanAfford(RTSFaction::Human, 100, 50)); // no gas
    EXPECT_FALSE(resources.CanAfford(RTSFaction::Human, 9999, 0));
}

TEST(RTS_Resource_SpendReducesBalance)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    resources.InitializePlayer(RTSFaction::Swarm);
    resources.AddResources(RTSFaction::Swarm, 0, 100); // add gas on top of starting minerals
    bool spent = resources.SpendResources(RTSFaction::Swarm, 50, 30);
    EXPECT_TRUE(spent);
    const PlayerResources* pr = resources.GetPlayerResources(RTSFaction::Swarm);
    EXPECT_EQ(pr->minerals, 350); // 400 - 50
    EXPECT_EQ(pr->gas, 70);       // 100 - 30
}

TEST(RTS_Resource_SupplyTracking)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    resources.InitializePlayer(RTSFaction::Human);
    const PlayerResources* pr = resources.GetPlayerResources(RTSFaction::Human);
    int initialMax = pr->maxSupply;

    resources.AddSupply(RTSFaction::Human, 8);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->maxSupply, initialMax + 8);

    resources.UseSupply(RTSFaction::Human, 3);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->currentSupply, 3);

    resources.FreeSupply(RTSFaction::Human, 2);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->currentSupply, 1);
}

TEST(RTS_Resource_CreateNodeReturnsValidId)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    uint32_t id = resources.CreateNode(RTSResourceType::Minerals, 10.0f, 20.0f, 1500);
    EXPECT_TRUE(id > 0);
}

TEST(RTS_Resource_NodeCountIncrements)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    size_t before = resources.GetNodeCount();
    resources.CreateNode(RTSResourceType::Minerals, 0.0f, 0.0f, 1000);
    resources.CreateNode(RTSResourceType::Gas, 5.0f, 5.0f, 800);
    EXPECT_EQ(resources.GetNodeCount(), before + 2);
}

TEST(RTS_Resource_ListStringNonEmpty)
{
    RTSResourceSystem resources;
    resources.Initialize(nullptr);
    resources.InitializePlayer(RTSFaction::Human);
    resources.CreateNode(RTSResourceType::Minerals, 0.0f, 0.0f, 500);
    std::string list = resources.GetResourceListString();
    EXPECT_TRUE(!list.empty());
}

#endif // SPARK_TEST_HAS_IMGUI
