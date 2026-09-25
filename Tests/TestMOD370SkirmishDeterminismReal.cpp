/**
 * @file TestMOD370SkirmishDeterminismReal.cpp
 * @brief MOD-370: the RTS skirmish tick is deterministic and reaches a win state
 *
 * Every test drives the real SparkGameRTS sources (units, buildings, economy,
 * commands, fog, match, and the fixed-step skirmish simulation). Determinism is
 * proven by hashing the full simulation state after every tick and comparing
 * runs that differ only in container insertion order or frame pacing.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSPersistence.h"
#include "../GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.h"
#include "../GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.h"
#include "../GameModules/SparkGameRTS/Source/Navigation/RTSGridPathfinder.h"
#include "../GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.h"
#include "../GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.h"
#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace RTS;

namespace
{
    struct EconomyWorld
    {
        RTSUnitSystem units;
        RTSResourceSystem resources;
        RTSBuildingSystem buildings;

        EconomyWorld()
        {
            units.Initialize(nullptr);
            resources.Initialize(nullptr, &units);
            buildings.Initialize(nullptr, &units, &resources);
        }
    };

    /// Faction of every unit, ordered by unit id: this is what diverges when
    /// production order follows hash-container iteration order.
    std::vector<RTSFaction> FactionByUnitId(const RTSUnitSystem& units)
    {
        std::vector<uint32_t> ids;
        for (int faction = 0; faction < static_cast<int>(RTSFaction::Count); ++faction)
        {
            for (uint32_t id : units.GetUnitsByFaction(static_cast<RTSFaction>(faction)))
                ids.push_back(id);
        }
        std::ranges::sort(ids);

        std::vector<RTSFaction> factions;
        for (uint32_t id : ids)
            factions.push_back(units.GetUnit(id)->faction);
        return factions;
    }

    /// The full production stack the module wires together, driven only through RTSSkirmishSimulation.
    struct Skirmish
    {
        RTSUnitSystem units;
        RTSBuildingSystem buildings;
        RTSResourceSystem resources;
        RTSCommandSystem commands;
        RTSFogOfWarSystem fog;
        RTSMatchSystem match;
        RTSSkirmishSimulation simulation;

        Skirmish()
        {
            simulation.Initialize(nullptr, {&units, &buildings, &resources, &commands, &fog, &match});
            simulation.StartDefaultSkirmish();
        }
    };

    constexpr uint32_t TICKS_PER_SECOND = 32;
    constexpr uint64_t COUNTER_ATTACK_TICK = TICKS_PER_SECOND * 45;
    constexpr uint64_t MAX_SKIRMISH_TICKS = TICKS_PER_SECOND * 60 * 15;

    /**
     * The local Human player's input stream. Fixed orders at fixed ticks (train, hold the base while the Swarm AI
     * attacks, then counter-attack), then once per second the idle army is sent to the oldest remaining Swarm
     * structure. Orders only read simulation state, so a deterministic tick
     * yields an identical stream.
     */
    void ApplyHumanInput(Skirmish& game)
    {
        const uint64_t tick = game.simulation.GetTick();
        const auto humanBarracks = [&]() -> uint32_t
        {
            for (uint32_t id : game.buildings.GetBuildingsByFaction(RTSFaction::Human))
            {
                if (game.buildings.GetBuilding(id)->type == RTSBuildingType::Barracks)
                    return id;
            }
            return 0;
        };

        if (tick == 0 || tick == TICKS_PER_SECOND * 20 || tick == TICKS_PER_SECOND * 40)
            game.buildings.StartProduction(humanBarracks(), RTSUnitType::Marine);
        if (tick == COUNTER_ATTACK_TICK)
        {
            game.commands.DeselectAll();
            for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Human))
            {
                if (game.units.GetUnit(id)->type != RTSUnitType::Worker)
                    game.commands.AddToSelection(id);
            }
            game.commands.IssueCommandToSelection({RTSCommandType::Attack, 60.0f, 60.0f, 0});
        }
        if (tick > COUNTER_ATTACK_TICK && tick % TICKS_PER_SECOND == 0)
        {
            const std::vector<uint32_t> targets = game.buildings.GetBuildingsByFaction(RTSFaction::Swarm);
            const std::vector<uint32_t> survivors = game.units.GetUnitsByFaction(RTSFaction::Swarm);
            float x = 0.0f;
            float y = 0.0f;
            if (!targets.empty())
            {
                x = game.buildings.GetBuilding(targets.front())->posX;
                y = game.buildings.GetBuilding(targets.front())->posY;
            }
            else if (!survivors.empty())
            {
                x = game.units.GetUnit(survivors.front())->posX;
                y = game.units.GetUnit(survivors.front())->posY;
            }
            else
            {
                return;
            }
            for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Human))
            {
                const UnitData* unit = game.units.GetUnit(id);
                if (unit->type != RTSUnitType::Worker && unit->state == RTSUnitState::Idle &&
                    !game.commands.GetCurrentCommand(id))
                {
                    game.commands.IssueCommand(id, {RTSCommandType::Attack, x, y, 0});
                }
            }
        }
    }

    /// Re-insert the same world state into every container in a different order.
    void ShuffleContainerInsertionOrder(Skirmish& game)
    {
        RTSPersistenceSnapshot snapshot = RTSPersistence::Capture(game.units, game.buildings, game.resources);
        std::ranges::reverse(snapshot.units);
        std::ranges::rotate(snapshot.units, snapshot.units.begin() + 3);
        std::ranges::reverse(snapshot.buildings);
        std::ranges::reverse(snapshot.players);
        std::ranges::reverse(snapshot.resourceNodes);
        EXPECT_TRUE(game.units.RestoreState(snapshot.units));
        EXPECT_TRUE(game.buildings.RestoreState(snapshot.buildings));
        EXPECT_TRUE(game.resources.RestoreState(snapshot.players, snapshot.resourceNodes));
    }

    /// Per-tick state hashes (index 0 = initial state) until the match ends or maxTicks elapse.
    std::vector<uint64_t> RunScriptedSkirmish(bool shuffleInsertionOrder, uint64_t maxTicks = MAX_SKIRMISH_TICKS)
    {
        auto game = std::make_unique<Skirmish>();
        if (shuffleInsertionOrder)
            ShuffleContainerInsertionOrder(*game);

        std::vector<uint64_t> hashes{game->simulation.ComputeStateHash()};
        while (game->simulation.GetTick() < maxTicks && game->match.GetMatchState() == RTSMatchState::Playing)
        {
            ApplyHumanInput(*game);
            game->simulation.Step();
            hashes.push_back(game->simulation.ComputeStateHash());
        }
        return hashes;
    }

    size_t FirstDivergence(const std::vector<uint64_t>& lhs, const std::vector<uint64_t>& rhs)
    {
        const size_t count = std::min(lhs.size(), rhs.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (lhs[i] != rhs[i])
                return i;
        }
        return lhs.size() == rhs.size() ? static_cast<size_t>(-1) : count;
    }
} // namespace

// Two barracks finish a marine on the same tick. The spawned unit ids must not
// depend on the order the buildings were inserted into the building container.
TEST(RTSSkirmish_ProductionOrderIgnoresContainerInsertionOrder)
{
    EconomyWorld source;
    source.resources.InitializePlayer(RTSFaction::Human);
    source.resources.InitializePlayer(RTSFaction::Swarm);
    source.resources.AddSupply(RTSFaction::Human, 10);
    source.resources.AddSupply(RTSFaction::Swarm, 10);
    for (int i = 0; i < 6; ++i)
    {
        const float offset = static_cast<float>(i) * 10.0f;
        ASSERT_NE(source.buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, offset, 0.0f), 0u);
        ASSERT_NE(source.buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Swarm, offset, 50.0f), 0u);
    }
    source.buildings.Update(120.0f);
    for (int faction : {static_cast<int>(RTSFaction::Human), static_cast<int>(RTSFaction::Swarm)})
    {
        for (uint32_t id : source.buildings.GetBuildingsByFaction(static_cast<RTSFaction>(faction)))
            ASSERT_TRUE(source.buildings.StartProduction(id, RTSUnitType::Marine));
    }

    const RTSPersistenceSnapshot snapshot = RTSPersistence::Capture(source.units, source.buildings, source.resources);
    ASSERT_EQ(snapshot.buildings.size(), static_cast<size_t>(12));

    std::vector<std::vector<RTSFaction>> outcomes;
    std::vector<BuildingData> order = snapshot.buildings;
    for (int permutation = 0; permutation < 4; ++permutation)
    {
        // Forward, reverse, and two interleavings of the same buildings.
        if (permutation == 1)
            std::ranges::reverse(order);
        else if (permutation > 1)
            std::ranges::rotate(order, order.begin() + permutation * 3);

        EconomyWorld world;
        ASSERT_TRUE(world.units.RestoreState(snapshot.units));
        ASSERT_TRUE(world.resources.RestoreState(snapshot.players, snapshot.resourceNodes));
        ASSERT_TRUE(world.buildings.RestoreState(order));
        world.buildings.Update(30.0f);
        ASSERT_EQ(world.units.GetUnitCount(), static_cast<size_t>(12));
        outcomes.push_back(FactionByUnitId(world.units));
    }

    for (size_t i = 1; i < outcomes.size(); ++i)
        EXPECT_TRUE(outcomes[i] == outcomes[0]);
}

TEST(RTSSkirmish_SameCommandStreamIsBitIdenticalPerTick)
{
    const std::vector<uint64_t> first = RunScriptedSkirmish(false);
    const std::vector<uint64_t> second = RunScriptedSkirmish(false);
    EXPECT_GT(first.size(), static_cast<size_t>(TICKS_PER_SECOND * 30));
    EXPECT_EQ(FirstDivergence(first, second), static_cast<size_t>(-1));
}

TEST(RTSSkirmish_ShuffledContainerInsertionIsBitIdenticalPerTick)
{
    const std::vector<uint64_t> canonical = RunScriptedSkirmish(false);
    const std::vector<uint64_t> shuffled = RunScriptedSkirmish(true);
    EXPECT_GT(canonical.size(), static_cast<size_t>(TICKS_PER_SECOND * 30));
    EXPECT_EQ(FirstDivergence(canonical, shuffled), static_cast<size_t>(-1));
}

// Wall-clock frame pacing only decides how many fixed ticks run per frame; every tick reached must hash exactly
// like the reference run that calls Step() once per tick.
TEST(RTSSkirmish_FramePacingNeverChangesSimulatedState)
{
    constexpr uint64_t ticks = TICKS_PER_SECOND * 45;
    Skirmish reference;
    std::vector<uint64_t> expected{reference.simulation.ComputeStateHash()};
    while (reference.simulation.GetTick() < ticks)
    {
        reference.simulation.Step();
        expected.push_back(reference.simulation.ComputeStateHash());
    }

    // Exactly representable frame times: 30/60/120 Hz-like, a hitchy mix, and whole multi-tick frames.
    const std::vector<std::vector<float>> pacings = {
        {1.0f / 32.0f}, {1.0f / 64.0f}, {1.0f / 128.0f, 3.0f / 128.0f, 5.0f / 128.0f, 1.0f / 64.0f}, {3.0f / 32.0f}};
    for (const std::vector<float>& pacing : pacings)
    {
        Skirmish game;
        size_t frame = 0;
        size_t matchedTicks = 0;
        while (game.simulation.GetTick() < ticks)
        {
            const uint64_t before = game.simulation.GetTick();
            game.simulation.Advance(pacing[frame++ % pacing.size()]);
            const uint64_t after = game.simulation.GetTick();
            if (after != before)
            {
                ASSERT_TRUE(after < expected.size());
                EXPECT_EQ(game.simulation.ComputeStateHash(), expected[after]);
                ++matchedTicks;
            }
        }
        EXPECT_GT(matchedTicks, static_cast<size_t>(TICKS_PER_SECOND));
    }

    // Garbage frame times are ignored and a long hitch runs at most MAX_TICKS_PER_ADVANCE ticks.
    Skirmish hitch;
    EXPECT_EQ(hitch.simulation.Advance(-1.0f), 0u);
    EXPECT_EQ(hitch.simulation.Advance(std::numeric_limits<float>::quiet_NaN()), 0u);
    EXPECT_EQ(hitch.simulation.Advance(10.0f), RTSSkirmishSimulation::MAX_TICKS_PER_ADVANCE);
    EXPECT_EQ(hitch.simulation.Advance(1.0f / 64.0f), 0u);
}

// command -> move -> combat -> fog -> AI -> win/lose, all through the production systems.
TEST(RTSSkirmish_ScriptedSkirmishReachesVictoryThroughProductionSystems)
{
    Skirmish game;
    const uint32_t firstSwarmCommandCenter = game.buildings.GetBuildingsByFaction(RTSFaction::Swarm).front();
    const float swarmBaseX = game.buildings.GetBuilding(firstSwarmCommandCenter)->posX;
    const float swarmBaseY = game.buildings.GetBuilding(firstSwarmCommandCenter)->posY;
    EXPECT_FALSE(game.fog.IsExplored(RTSFaction::Human, swarmBaseX, swarmBaseY));

    uint32_t highestSwarmUnitId = 0;
    for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Swarm))
        highestSwarmUnitId = std::max(highestSwarmUnitId, id);
    const uint32_t initialHighestSwarmUnitId = highestSwarmUnitId;
    bool swarmLaunchedAttackWave = false;
    bool humanTookDamage = false;

    while (game.simulation.GetTick() < MAX_SKIRMISH_TICKS && game.match.GetMatchState() == RTSMatchState::Playing)
    {
        ApplyHumanInput(game);
        game.simulation.Step();
        for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Swarm))
        {
            const UnitData* unit = game.units.GetUnit(id);
            highestSwarmUnitId = std::max(highestSwarmUnitId, id);
            if (const UnitCommand* command = game.commands.GetCurrentCommand(id))
                swarmLaunchedAttackWave = swarmLaunchedAttackWave || (command->type == RTSCommandType::Attack &&
                                                                      unit->type != RTSUnitType::Worker);
        }
        for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Human))
            humanTookDamage = humanTookDamage || game.units.GetUnit(id)->health < game.units.GetUnit(id)->maxHealth;
    }

    EXPECT_TRUE(game.match.GetMatchState() == RTSMatchState::Victory);
    EXPECT_TRUE(game.match.GetWinner() == RTSFaction::Human);
    EXPECT_TRUE(game.match.IsPlayerEliminated(1));
    EXPECT_FALSE(game.match.IsPlayerEliminated(0));
    EXPECT_EQ(game.units.GetUnitCountByFaction(RTSFaction::Swarm), static_cast<size_t>(0));
    EXPECT_EQ(game.buildings.GetBuildingCountByFaction(RTSFaction::Swarm), static_cast<size_t>(0));
    EXPECT_TRUE(game.fog.IsExplored(RTSFaction::Human, swarmBaseX, swarmBaseY)); // fog rebuilt from live units
    EXPECT_GT(highestSwarmUnitId, initialHighestSwarmUnitId);                    // AI trained reinforcements
    EXPECT_TRUE(swarmLaunchedAttackWave);                                        // AI launched an attack wave
    EXPECT_TRUE(humanTookDamage);                                                // combat went both ways

    // Once the match is decided the tick freezes.
    const uint64_t finalTick = game.simulation.GetTick();
    const uint64_t finalHash = game.simulation.ComputeStateHash();
    game.simulation.Step();
    EXPECT_EQ(game.simulation.GetTick(), finalTick);
    EXPECT_EQ(game.simulation.ComputeStateHash(), finalHash);
}

TEST(RTSSkirmish_MatchReportsDefeatWhenAIWins)
{
    RTSMatchSystem match;
    EXPECT_TRUE(match.Initialize(nullptr));
    match.SetupMatch(2);
    match.SetPlayerFaction(0, RTSFaction::Human);
    match.SetPlayerFaction(1, RTSFaction::Swarm);
    match.SetPlayerIsAI(1, true);
    EXPECT_TRUE(match.StartMatch());
    match.MarkPlayerEliminated(0);
    match.Update(RTSSkirmishSimulation::TICK_SECONDS);
    EXPECT_TRUE(match.GetMatchState() == RTSMatchState::Defeat);
    EXPECT_TRUE(match.GetWinner() == RTSFaction::Swarm);
}

// The shipped module must advance gameplay only through the fixed-step simulation, never with raw frame time.
TEST(RTSSkirmish_ModuleTicksOnlyThroughFixedStepSimulation)
{
    const auto modulePath =
        std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules" / "SparkGameRTS" / "Source" / "Core" / "Main.cpp";
    std::ifstream stream(modulePath, std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string source = buffer.str();

    const size_t update = source.find("void SparkGameRTSModule::OnUpdate(float deltaTime)");
    const size_t fixedUpdate = source.find("void SparkGameRTSModule::OnFixedUpdate(");
    ASSERT_TRUE(update != std::string::npos && fixedUpdate != std::string::npos && update < fixedUpdate);
    const std::string body = source.substr(update, fixedUpdate - update);
    EXPECT_TRUE(body.find("m_simulation->Advance(deltaTime)") != std::string::npos);
    for (const char* direct : {"m_unitSystem->Update", "m_commandSystem->Update", "m_buildingSystem->Update",
                               "m_resourceSystem->Update", "m_matchSystem->Update", "m_fogOfWarSystem->Update"})
    {
        EXPECT_TRUE(body.find(direct) == std::string::npos);
    }
}

// =============================================================================
// Grid pathfinding for move and attack-move orders
// =============================================================================

namespace
{
    /// Units, buildings, and commands wired the way the skirmish wires them, without the rest of the tick.
    struct RoutingWorld
    {
        RTSUnitSystem units;
        RTSResourceSystem resources;
        RTSBuildingSystem buildings;
        RTSCommandSystem commands;
        RTSGridPathfinder obstacles; ///< Independent obstacle map the assertions check positions against

        RoutingWorld()
        {
            units.Initialize(nullptr);
            resources.Initialize(nullptr, &units);
            buildings.Initialize(nullptr, &units, &resources);
            commands.Initialize(nullptr, &units, &buildings);
        }

        void Place(float x, float y)
        {
            ASSERT_NE(buildings.PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, x, y), 0u);
            obstacles.RebuildObstacles(buildings);
        }
    };

    struct WalkResult
    {
        bool arrived = false;
        bool enteredBlockedCell = false;
        size_t longestRoute = 0;
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
    };

    /// Tick the command system until the unit's order completes, checking its cell after every tick.
    WalkResult WalkUntilIdle(RoutingWorld& world, uint32_t unitId, uint64_t maxTicks)
    {
        WalkResult result;
        for (uint64_t tick = 0; tick < maxTicks; ++tick)
        {
            world.commands.Update(RTSSkirmishSimulation::TICK_SECONDS);
            const UnitData* unit = world.units.GetUnit(unitId);
            result.enteredBlockedCell =
                result.enteredBlockedCell || world.obstacles.IsBlockedAt(unit->posX, unit->posY);
            result.minY = std::min(result.minY, unit->posY);
            result.maxY = std::max(result.maxY, unit->posY);
            if (const UnitCommand* command = world.commands.GetCurrentCommand(unitId))
            {
                result.longestRoute = std::max(result.longestRoute, command->path.size());
                continue;
            }
            result.arrived = true;
            break;
        }
        return result;
    }
} // namespace

// A wall of three barracks spans x [18, 22) and y [14, 26) straight across the order's line. Both a move and an
// attack-move walk around the wall, never through it, and still finish exactly on the ordered point.
TEST(RTSSkirmish_MoveOrderRoutesAroundBuilding)
{
    for (const bool attackMove : {false, true})
    {
        RoutingWorld world;
        for (float wallY : {16.0f, 20.0f, 24.0f})
            world.Place(20.0f, wallY);
        EXPECT_TRUE(world.obstacles.IsBlocked(18, 14));
        EXPECT_TRUE(world.obstacles.IsBlocked(21, 25));
        EXPECT_FALSE(world.obstacles.IsBlocked(17, 20));
        EXPECT_FALSE(world.obstacles.IsBlocked(22, 20));
        EXPECT_FALSE(world.obstacles.IsBlocked(20, 26));

        const float startX = 14.25f;
        const float startY = 20.25f;
        const float targetX = 26.75f;
        const float targetY = 20.5f;
        EXPECT_FALSE(world.obstacles.IsSegmentClear(startX, startY, targetX, targetY, 0.0f)); // the wall is in the way

        const uint32_t marine = world.units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, startX, startY);
        const RTSCommandType type = attackMove ? RTSCommandType::Attack : RTSCommandType::Move;
        world.commands.IssueCommand(marine, {type, targetX, targetY, 0});
        world.commands.Update(RTSSkirmishSimulation::TICK_SECONDS);
        const UnitCommand* command = world.commands.GetCurrentCommand(marine);
        ASSERT_TRUE(command != nullptr);
        EXPECT_GE(command->path.size(), static_cast<size_t>(2)); // a detour, not the straight line
        EXPECT_TRUE(world.units.GetUnit(marine)->state ==
                    (attackMove ? RTSUnitState::Attacking : RTSUnitState::Moving));

        const WalkResult walk = WalkUntilIdle(world, marine, 32 * 60);
        const UnitData* unit = world.units.GetUnit(marine);
        EXPECT_TRUE(walk.arrived);
        EXPECT_FALSE(walk.enteredBlockedCell);
        EXPECT_TRUE(walk.minY < 14.0f || walk.maxY >= 26.0f); // went around an end of the wall
        EXPECT_EQ(unit->posX, targetX);
        EXPECT_EQ(unit->posY, targetY);
        EXPECT_TRUE(unit->state == RTSUnitState::Idle);
        EXPECT_EQ(world.commands.GetPendingCommandCount(), static_cast<size_t>(0));
    }
}

// A target under a footprint ends on the nearest free cell; an enclosed unit gets as close as the grid allows.
TEST(RTSSkirmish_MoveOrderIntoStructureStopsAtNearestFreeCell)
{
    RoutingWorld world;
    for (float wallY : {16.0f, 20.0f, 24.0f})
        world.Place(20.0f, wallY);
    const uint32_t marine = world.units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 30.25f, 20.75f);
    world.commands.IssueCommand(marine, {RTSCommandType::Move, 20.0f, 20.0f, 0});
    WalkResult walk = WalkUntilIdle(world, marine, 32 * 60);
    EXPECT_TRUE(walk.arrived);
    EXPECT_FALSE(walk.enteredBlockedCell);
    EXPECT_EQ(world.units.GetUnit(marine)->posX, 22.5f); // centre of cell (22, 20), the nearest free cell
    EXPECT_EQ(world.units.GetUnit(marine)->posY, 20.5f);

    // Eight barracks enclose the 4x4 pocket [48, 52) x [48, 52); the target outside cannot be reached.
    RoutingWorld pocket;
    for (float y : {46.0f, 50.0f, 54.0f})
    {
        for (float x : {46.0f, 50.0f, 54.0f})
        {
            if (x != 50.0f || y != 50.0f)
                pocket.Place(x, y);
        }
    }
    const uint32_t trapped = pocket.units.SpawnUnit(RTSUnitType::Marine, RTSFaction::Human, 48.5f, 48.5f);
    pocket.commands.IssueCommand(trapped, {RTSCommandType::Move, 80.0f, 80.0f, 0});
    walk = WalkUntilIdle(pocket, trapped, 32 * 60);
    EXPECT_TRUE(walk.arrived);
    EXPECT_FALSE(walk.enteredBlockedCell);
    EXPECT_EQ(pocket.units.GetUnit(trapped)->posX, 51.5f); // pocket cell nearest the target
    EXPECT_EQ(pocket.units.GetUnit(trapped)->posY, 51.5f);

    // A planned route is replaced when a structure is built across it after planning.
    RoutingWorld late;
    const uint32_t scout = late.units.SpawnUnit(RTSUnitType::Scout, RTSFaction::Human, 10.5f, 40.5f);
    late.commands.IssueCommand(scout, {RTSCommandType::Move, 40.5f, 40.5f, 0});
    late.commands.Update(RTSSkirmishSimulation::TICK_SECONDS);
    ASSERT_TRUE(late.commands.GetCurrentCommand(scout) != nullptr);
    EXPECT_EQ(late.commands.GetCurrentCommand(scout)->path.size(), static_cast<size_t>(1)); // open ground: direct
    late.Place(25.0f, 40.0f);
    walk = WalkUntilIdle(late, scout, 32 * 60);
    EXPECT_TRUE(walk.arrived);
    EXPECT_FALSE(walk.enteredBlockedCell);
    EXPECT_GE(walk.longestRoute, static_cast<size_t>(2));
    EXPECT_EQ(late.units.GetUnit(scout)->posX, 40.5f);
    EXPECT_EQ(late.units.GetUnit(scout)->posY, 40.5f);

    // The search itself is a pure function of the obstacle map and endpoints.
    const std::vector<RTSWaypoint> first = world.obstacles.FindPath(14.25f, 20.25f, 26.75f, 20.5f);
    EXPECT_TRUE(first == world.obstacles.FindPath(14.25f, 20.25f, 26.75f, 20.5f));
    EXPECT_GE(first.size(), static_cast<size_t>(2));
}

namespace
{
    /// Human army ordered through its own barracks (footprint [29, 33) x [17, 21)) before the first tick.
    void IssueDetourOrders(Skirmish& game)
    {
        for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Human))
        {
            if (game.units.GetUnit(id)->type == RTSUnitType::Worker)
                continue;
            game.commands.IssueCommand(id, {RTSCommandType::Move, 35.5f, 18.5f, 0});
            game.commands.QueueCommand(id, {RTSCommandType::Attack, 60.0f, 60.0f, 0});
        }
    }

    bool AnyUnitInsideStructure(const Skirmish& game)
    {
        RTSGridPathfinder obstacles;
        obstacles.RebuildObstacles(game.buildings);
        for (int faction = 0; faction < static_cast<int>(RTSFaction::Count); ++faction)
        {
            for (uint32_t id : game.units.GetUnitsByFaction(static_cast<RTSFaction>(faction)))
            {
                const UnitData* unit = game.units.GetUnit(id);
                if (obstacles.IsBlockedAt(unit->posX, unit->posY))
                    return true;
            }
        }
        return false;
    }

    size_t LongestRoute(const Skirmish& game)
    {
        size_t longest = 0;
        for (const auto& [id, queue] : game.commands.GetCommandQueues())
        {
            for (const UnitCommand& command : queue)
                longest = std::max(longest, command.path.size());
        }
        return longest;
    }

    struct RoutedRun
    {
        std::vector<uint64_t> hashes; ///< index 0 = state before the first tick
        size_t longestRoute = 0;
        bool unitInsideStructure = false;
    };

    RoutedRun RunRoutedSkirmish(bool shuffleInsertionOrder, uint64_t ticks)
    {
        auto game = std::make_unique<Skirmish>();
        if (shuffleInsertionOrder)
            ShuffleContainerInsertionOrder(*game);
        IssueDetourOrders(*game);

        RoutedRun run;
        run.hashes.push_back(game->simulation.ComputeStateHash());
        while (game->simulation.GetTick() < ticks && game->match.GetMatchState() == RTSMatchState::Playing)
        {
            ApplyHumanInput(*game);
            game->simulation.Step();
            run.hashes.push_back(game->simulation.ComputeStateHash());
            run.longestRoute = std::max(run.longestRoute, LongestRoute(*game));
            run.unitInsideStructure = run.unitInsideStructure || AnyUnitInsideStructure(*game);
        }
        return run;
    }
} // namespace

// Routes are part of the hashed state: reruns, shuffled container insertion, and any frame pacing must still agree
// on every tick while units detour around structures, and a mid-route save carries the route exactly.
TEST(RTSSkirmish_PathfindingPreservesPerTickDeterminism)
{
    constexpr uint64_t ticks = TICKS_PER_SECOND * 60;
    const RoutedRun canonical = RunRoutedSkirmish(false, ticks);
    const RoutedRun rerun = RunRoutedSkirmish(false, ticks);
    const RoutedRun shuffled = RunRoutedSkirmish(true, ticks);
    EXPECT_GE(canonical.longestRoute, static_cast<size_t>(2)); // orders really routed around structures
    EXPECT_FALSE(canonical.unitInsideStructure);
    EXPECT_GT(canonical.hashes.size(), static_cast<size_t>(TICKS_PER_SECOND * 50));
    EXPECT_EQ(FirstDivergence(canonical.hashes, rerun.hashes), static_cast<size_t>(-1));
    EXPECT_EQ(FirstDivergence(canonical.hashes, shuffled.hashes), static_cast<size_t>(-1));

    // Frame pacing: the same pre-issued detour orders, advanced by wall-clock time instead of single steps.
    constexpr uint64_t pacedTicks = TICKS_PER_SECOND * 20;
    Skirmish reference;
    IssueDetourOrders(reference);
    std::vector<uint64_t> expected{reference.simulation.ComputeStateHash()};
    bool savedMidRoute = false;
    // One Advance may run several ticks past pacedTicks, so the reference covers that overshoot too.
    while (reference.simulation.GetTick() < pacedTicks + RTSSkirmishSimulation::MAX_TICKS_PER_ADVANCE)
    {
        reference.simulation.Step();
        expected.push_back(reference.simulation.ComputeStateHash());

        // Save while a detour is in progress, load into a fresh skirmish, and require the identical state.
        if (!savedMidRoute && LongestRoute(reference) >= 2)
        {
            savedMidRoute = true;
            const RTSSkirmishSystems systems{&reference.units,    &reference.buildings, &reference.resources,
                                             &reference.commands, &reference.fog,       &reference.match};
            std::string error;
            const std::string encoded =
                RTSPersistence::Serialize(RTSPersistence::Capture(systems, reference.simulation), error);
            ASSERT_FALSE(encoded.empty());
            RTSPersistenceSnapshot decoded;
            ASSERT_TRUE(RTSPersistence::Deserialize(encoded, decoded, error));
            const auto& liveQueues = reference.commands.GetCommandQueues();
            ASSERT_EQ(decoded.commandQueues.size(), liveQueues.size());
            for (const auto& [unitId, queue] : liveQueues)
            {
                ASSERT_TRUE(decoded.commandQueues.contains(unitId));
                ASSERT_EQ(decoded.commandQueues.at(unitId).size(), queue.size());
                for (size_t i = 0; i < queue.size(); ++i)
                    EXPECT_TRUE(decoded.commandQueues.at(unitId)[i].path == queue[i].path);
            }

            Skirmish loaded;
            ASSERT_TRUE(RTSPersistence::Apply(
                decoded,
                {&loaded.units, &loaded.buildings, &loaded.resources, &loaded.commands, &loaded.fog, &loaded.match},
                loaded.simulation, error));
            EXPECT_EQ(loaded.simulation.ComputeStateHash(), reference.simulation.ComputeStateHash());
        }
    }
    EXPECT_TRUE(savedMidRoute);

    const std::vector<std::vector<float>> pacings = {
        {1.0f / 64.0f}, {1.0f / 128.0f, 3.0f / 128.0f, 5.0f / 128.0f, 1.0f / 64.0f}, {3.0f / 32.0f}};
    for (const std::vector<float>& pacing : pacings)
    {
        Skirmish game;
        IssueDetourOrders(game);
        size_t frame = 0;
        while (game.simulation.GetTick() < pacedTicks)
        {
            const uint64_t before = game.simulation.GetTick();
            game.simulation.Advance(pacing[frame++ % pacing.size()]);
            const uint64_t after = game.simulation.GetTick();
            if (after != before)
            {
                ASSERT_TRUE(after < expected.size());
                EXPECT_EQ(game.simulation.ComputeStateHash(), expected[after]);
            }
        }
    }
}

#endif // SPARK_TEST_HAS_IMGUI
