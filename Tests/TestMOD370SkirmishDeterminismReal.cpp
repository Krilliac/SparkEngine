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

#endif // SPARK_TEST_HAS_IMGUI
