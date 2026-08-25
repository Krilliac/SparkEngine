/**
 * @file TestGameModuleRTS.cpp
 * @brief Tests for SparkGameRTS module: units, buildings, and resources
 *
 * All tests are gated behind SPARK_TEST_HAS_IMGUI because the RTS .cpp
 * files depend on ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSPersistence.h"
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

TEST(RTS_Building_CompletedProductionSpawnsUnitAndConsumesEconomy)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr, &units));
    resources.InitializePlayer(RTSFaction::Human);
    RTSBuildingSystem buildings;
    EXPECT_TRUE(buildings.Initialize(nullptr, &units, &resources));

    const uint32_t barracks = buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 10.0f, 12.0f);
    buildings.Update(100.0f);
    const size_t unitsBefore = units.GetUnitCount();
    const PlayerResources before = *resources.GetPlayerResources(RTSFaction::Human);

    EXPECT_TRUE(buildings.StartProduction(barracks, RTSUnitType::Marine));
    const PlayerResources queued = *resources.GetPlayerResources(RTSFaction::Human);
    EXPECT_TRUE(queued.minerals < before.minerals);
    EXPECT_EQ(queued.currentSupply, before.currentSupply + 1);

    buildings.Update(100.0f);
    EXPECT_EQ(units.GetUnitCount(), unitsBefore + 1);
    const auto marines = units.GetUnitsByFaction(RTSFaction::Human);
    EXPECT_EQ(marines.size(), static_cast<size_t>(1));
    const UnitData* marine = units.GetUnit(marines.front());
    EXPECT_TRUE(marine != nullptr);
    EXPECT_TRUE(marine->type == RTSUnitType::Marine);
    EXPECT_NEAR(marine->posX, 12.0f, 0.001f);
    EXPECT_NEAR(marine->posY, 14.0f, 0.001f);
}

TEST(RTS_Building_LargeUpdateCarriesTimeAcrossProductionQueue)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr, &units));
    resources.InitializePlayer(RTSFaction::Human);
    RTSBuildingSystem buildings;
    EXPECT_TRUE(buildings.Initialize(nullptr, &units, &resources));

    const uint32_t barracks = buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 0.0f, 0.0f);
    buildings.Update(100.0f);
    EXPECT_TRUE(buildings.StartProduction(barracks, RTSUnitType::Marine));
    EXPECT_TRUE(buildings.StartProduction(barracks, RTSUnitType::Marine));

    buildings.Update(36.0f);
    EXPECT_EQ(units.GetUnitsByFaction(RTSFaction::Human).size(), static_cast<size_t>(2));
    EXPECT_TRUE(buildings.GetBuilding(barracks)->productionQueue.empty());
}

TEST(RTS_Building_ProductionRejectsMissingRuntimeAndUnavailableSupply)
{
    RTSBuildingSystem disconnectedBuildings;
    EXPECT_TRUE(disconnectedBuildings.Initialize(nullptr));
    const uint32_t disconnectedBarracks =
        disconnectedBuildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 0.0f, 0.0f);
    disconnectedBuildings.Update(100.0f);
    EXPECT_FALSE(disconnectedBuildings.StartProduction(disconnectedBarracks, RTSUnitType::Marine));

    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr, &units));
    resources.InitializePlayer(RTSFaction::Human);
    resources.UseSupply(RTSFaction::Human, 10);
    RTSBuildingSystem buildings;
    EXPECT_TRUE(buildings.Initialize(nullptr, &units, &resources));
    const uint32_t barracks = buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 0.0f, 0.0f);
    buildings.Update(100.0f);
    EXPECT_FALSE(buildings.StartProduction(barracks, RTSUnitType::Marine));
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

TEST(RTS_Resource_RejectsInvalidNodes)
{
    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr));
    EXPECT_EQ(resources.CreateNode(RTSResourceType::Supply, 0.0f, 0.0f, 100), static_cast<uint32_t>(0));
    EXPECT_EQ(resources.CreateNode(RTSResourceType::Minerals, 0.0f, 0.0f, 0), static_cast<uint32_t>(0));
    EXPECT_EQ(resources.CreateNode(RTSResourceType::Gas, std::numeric_limits<float>::infinity(), 0.0f, 100),
              static_cast<uint32_t>(0));
    EXPECT_EQ(resources.GetNodeCount(), static_cast<size_t>(0));
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

TEST(RTS_Resource_HarvestCreditsOwningWorkerFaction)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t humanWorker = units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 0.0f, 0.0f);
    const uint32_t swarmWorker = units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Swarm, 10.0f, 10.0f);
    const uint32_t nonWorker = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 2.0f, 2.0f);

    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr, &units));
    resources.InitializePlayer(RTSFaction::Human);
    resources.InitializePlayer(RTSFaction::Swarm);
    const uint32_t humanNode = resources.CreateNode(RTSResourceType::Minerals, 0.0f, 0.0f, 100);
    const uint32_t swarmNode = resources.CreateNode(RTSResourceType::Minerals, 10.0f, 10.0f, 100);
    EXPECT_FALSE(resources.AssignWorker(humanNode, nonWorker));
    EXPECT_TRUE(resources.AssignWorker(humanNode, humanWorker));
    EXPECT_TRUE(resources.AssignWorker(swarmNode, swarmWorker));

    const int humanBefore = resources.GetPlayerResources(RTSFaction::Human)->minerals;
    const int swarmBefore = resources.GetPlayerResources(RTSFaction::Swarm)->minerals;
    resources.Update(2.0f);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->minerals, humanBefore + 8);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Swarm)->minerals, swarmBefore + 8);
    EXPECT_EQ(resources.GetNodes().at(humanNode).remaining, 92);
    EXPECT_EQ(resources.GetNodes().at(swarmNode).remaining, 92);
}

TEST(RTS_Resource_DeadWorkersArePrunedBeforeHarvest)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    const uint32_t worker = units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 0.0f, 0.0f);
    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr, &units));
    resources.InitializePlayer(RTSFaction::Human);
    const uint32_t node = resources.CreateNode(RTSResourceType::Minerals, 0.0f, 0.0f, 100);
    EXPECT_TRUE(resources.AssignWorker(node, worker));
    units.KillUnit(worker);

    const int before = resources.GetPlayerResources(RTSFaction::Human)->minerals;
    resources.Update(2.0f);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->minerals, before);
    EXPECT_TRUE(resources.GetNodes().at(node).assignedWorkers.empty());
}

// =============================================================================
// RTS persistence
// =============================================================================

TEST(RTS_ShutdownOrder_KeepsResourcesAliveForQueuedBuildings)
{
    const auto modulePath =
        std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules" / "SparkGameRTS" / "Source" / "Core" / "Main.cpp";
    std::ifstream stream(modulePath, std::ios::binary);
    EXPECT_TRUE(stream.is_open());
    if (!stream)
        return;

    std::ostringstream source;
    source << stream.rdbuf();
    const size_t unload = source.str().find("void SparkGameRTSModule::OnUnload()");
    const size_t building = source.str().find("if (m_buildingSystem)", unload);
    const size_t resources = source.str().find("if (m_resourceSystem)", unload);
    EXPECT_TRUE(unload != std::string::npos);
    EXPECT_TRUE(building != std::string::npos);
    EXPECT_TRUE(resources != std::string::npos);
    EXPECT_TRUE(building < resources);
}

TEST(RTS_Persistence_ValidatesPortableSlotNames)
{
    EXPECT_TRUE(RTSPersistence::IsValidSlotName("campaign-01_alpha"));
    EXPECT_TRUE(RTSPersistence::IsValidSlotName(std::string(64, 'a')));
    EXPECT_FALSE(RTSPersistence::IsValidSlotName(""));
    EXPECT_FALSE(RTSPersistence::IsValidSlotName(std::string(65, 'a')));
    EXPECT_FALSE(RTSPersistence::IsValidSlotName("../escape"));
    EXPECT_FALSE(RTSPersistence::IsValidSlotName("nested/slot"));
    EXPECT_FALSE(RTSPersistence::IsValidSlotName("slot name"));
}

TEST(RTS_Persistence_RoundTripsAndAtomicallyRestoresGameplayState)
{
    RTSUnitSystem units;
    EXPECT_TRUE(units.Initialize(nullptr));
    RTSResourceSystem resources;
    EXPECT_TRUE(resources.Initialize(nullptr, &units));
    resources.InitializePlayer(RTSFaction::Human);
    RTSBuildingSystem buildings;
    EXPECT_TRUE(buildings.Initialize(nullptr, &units, &resources));
    RTSCommandSystem commands;
    EXPECT_TRUE(commands.Initialize(nullptr, &units));

    const uint32_t worker = units.SpawnUnit(RTSUnitType::Worker, RTSFaction::Human, 3.25f, -7.5f);
    const uint32_t marine = units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 8.0f, 4.0f);
    const float savedWorkerHealth = units.GetUnit(worker)->maxHealth * 0.5f;
    units.GetUnitMutable(worker)->health = savedWorkerHealth;
    units.SetUnitTarget(marine, worker);
    const uint32_t minerals = resources.CreateNode(RTSResourceType::Minerals, 4.0f, -6.0f, 777);
    EXPECT_TRUE(resources.AssignWorker(minerals, worker));
    resources.AddResources(RTSFaction::Human, 125, 40);

    const uint32_t barracks = buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 12.0f, 15.0f);
    buildings.Update(100.0f);
    EXPECT_TRUE(buildings.StartProduction(barracks, RTSUnitType::Marine));

    const RTSPersistenceSnapshot captured = RTSPersistence::Capture(units, buildings, resources);
    std::string error;
    const std::string encoded = RTSPersistence::Serialize(captured, error);
    EXPECT_EQ(error, std::string());
    ASSERT_FALSE(encoded.empty());

    RTSPersistenceSnapshot decoded;
    ASSERT_TRUE(RTSPersistence::Deserialize(encoded, decoded, error));
    EXPECT_EQ(RTSPersistence::Serialize(decoded), encoded);

    units.GetUnitMutable(worker)->health = 1.0f;
    units.SpawnUnit(RTSUnitType::Tank, RTSFaction::Human, 99.0f, 99.0f);
    resources.AddResources(RTSFaction::Human, 999, 999);
    commands.Select(worker);

    EXPECT_TRUE(RTSPersistence::Apply(decoded, units, buildings, resources, &commands, error));
    EXPECT_EQ(units.GetUnitCount(), static_cast<size_t>(2));
    EXPECT_NEAR(units.GetUnit(worker)->health, savedWorkerHealth, 0.001f);
    EXPECT_EQ(units.GetUnit(marine)->targetId, worker);
    EXPECT_EQ(resources.GetNodes().at(minerals).remaining, 777);
    EXPECT_EQ(resources.GetNodes().at(minerals).assignedWorkers.size(), static_cast<size_t>(1));
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->minerals, captured.players.front().second.minerals);
    EXPECT_EQ(resources.GetPlayerResources(RTSFaction::Human)->gas, captured.players.front().second.gas);
    EXPECT_EQ(buildings.GetBuilding(barracks)->productionQueue.size(), static_cast<size_t>(1));
    EXPECT_EQ(commands.GetSelectionCount(), static_cast<size_t>(0));
}

TEST(RTS_Persistence_RejectsCorruptionWithoutReplacingOutput)
{
    RTSPersistenceSnapshot output;
    UnitData sentinel;
    sentinel.unitId = 42;
    output.units.push_back(sentinel);
    std::string error;

    EXPECT_FALSE(RTSPersistence::Deserialize("SPARK_RTS_STATE_V1\nUNITS 1\n", output, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(output.units.size(), static_cast<size_t>(1));
    EXPECT_EQ(output.units.front().unitId, static_cast<uint32_t>(42));
}

#endif // SPARK_TEST_HAS_IMGUI
