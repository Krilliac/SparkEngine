/**
 * @file TestMOD370RTSSaveReal.cpp
 * @brief MOD-370: a mid-skirmish RTS save resumes bit-identically.
 *
 * Every test drives the real SparkGameRTS sources: units, buildings, economy, commands, fog of war, match, the
 * fixed-step RTSSkirmishSimulation, the RTSPersistence v2 codec, and the console-facing
 * RTSEngineSystems::SaveMatch/LoadMatch path on the real SaveSystem. A save taken at tick N and loaded into
 * freshly built systems must reproduce the uninterrupted run's ComputeStateHash for every following tick, which
 * covers every persisted field (records, id counters, harvest timer, command queues, selection, match lifecycle,
 * fog grids, and the tick that fixes the AI decision phase).
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSEngineSystems.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSPersistence.h"
#include "../GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.h"
#include "../GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.h"
#include "../GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.h"
#include "../GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.h"
#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace RTS;

namespace
{
    constexpr uint64_t TICKS_PER_SECOND = 32;
    constexpr uint64_t WAYPOINT_TICK = TICKS_PER_SECOND * 10;
    constexpr uint64_t COUNTER_ATTACK_TICK = TICKS_PER_SECOND * 45;
    constexpr uint64_t MAX_SKIRMISH_TICKS = TICKS_PER_SECOND * 60 * 15;
    constexpr uint64_t COMPARE_TICKS = 2000;

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
            simulation.Initialize(nullptr, Systems());
            simulation.StartDefaultSkirmish();
        }

        RTSSkirmishSystems Systems() { return {&units, &buildings, &resources, &commands, &fog, &match}; }
        bool Playing() const { return match.GetMatchState() == RTSMatchState::Playing; }
    };

    void SelectHumanArmy(Skirmish& game)
    {
        game.commands.DeselectAll();
        for (uint32_t id : game.units.GetUnitsByFaction(RTSFaction::Human))
        {
            if (game.units.GetUnit(id)->type != RTSUnitType::Worker)
                game.commands.AddToSelection(id);
        }
    }

    /**
     * The local Human player's input stream: train marines, walk the army through shift-queued waypoints and hold
     * (multi-order queues plus a live selection), then counter-attack and keep sending idle units at the oldest
     * Swarm structure. It reads only simulation state, so identical state yields an identical stream.
     */
    void ApplyScriptedInput(Skirmish& game)
    {
        const uint64_t tick = game.simulation.GetTick();
        if (tick == 0 || tick == TICKS_PER_SECOND * 20 || tick == TICKS_PER_SECOND * 40)
        {
            for (uint32_t id : game.buildings.GetBuildingsByFaction(RTSFaction::Human))
            {
                if (game.buildings.GetBuilding(id)->type == RTSBuildingType::Barracks)
                    game.buildings.StartProduction(id, RTSUnitType::Marine);
            }
        }
        if (tick == WAYPOINT_TICK)
        {
            SelectHumanArmy(game);
            game.commands.IssueCommandToSelection({RTSCommandType::Move, 30.0f, 33.0f, 0});
            for (uint32_t id : game.commands.GetSelection())
            {
                game.commands.QueueCommand(id, {RTSCommandType::Move, 27.0f, 36.0f, 0});
                game.commands.QueueCommand(id, {RTSCommandType::Hold, 0.0f, 0.0f, 0});
            }
        }
        if (tick == COUNTER_ATTACK_TICK)
        {
            SelectHumanArmy(game);
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

    /// One scripted tick: input for this tick, then exactly one fixed step.
    void PlayTick(Skirmish& game)
    {
        ApplyScriptedInput(game);
        game.simulation.Step();
    }

    /// Hash after every tick (index = tick) of an uninterrupted scripted skirmish.
    std::vector<uint64_t> RunReference()
    {
        auto game = std::make_unique<Skirmish>();
        std::vector<uint64_t> hashes{game->simulation.ComputeStateHash()};
        while (game->simulation.GetTick() < MAX_SKIRMISH_TICKS && game->Playing())
        {
            PlayTick(*game);
            hashes.push_back(game->simulation.ComputeStateHash());
        }
        return hashes;
    }

    /// Play the scripted skirmish up to (not including) @p tick and return the encoded full-state save.
    std::string SaveAtTick(Skirmish& game, uint64_t tick)
    {
        while (game.simulation.GetTick() < tick && game.Playing())
            PlayTick(game);
        std::string error;
        const std::string encoded =
            RTSPersistence::Serialize(RTSPersistence::Capture(game.Systems(), game.simulation), error);
        EXPECT_EQ(error, std::string());
        return encoded;
    }

    /// A freshly built skirmish that has drifted away from the saved state before the load replaces it.
    std::unique_ptr<Skirmish> FreshDivergedSkirmish()
    {
        auto game = std::make_unique<Skirmish>();
        for (int i = 0; i < 77; ++i)
            game->simulation.Step();
        game->commands.Select(game->units.GetUnitsByFaction(RTSFaction::Swarm).front());
        return game;
    }

    /// Engine context exposing only the real SaveSystem and a World, as the module sees them.
    class RTSSaveContext final : public Spark::IEngineContext
    {
      public:
        RTSSaveContext(Spark::SaveSystem* saveSystem, ::World* world) : m_saveSystem(saveSystem), m_world(world) {}

        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        ::World* GetWorld() override { return m_world; }
        const ::World* GetWorld() const override { return m_world; }
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

      private:
        Spark::SaveSystem* m_saveSystem = nullptr;
        ::World* m_world = nullptr;
    };

    /**
     * The module's save bridge on the real SaveSystem singleton, writing into an isolated directory that survives
     * rebinding to another skirmish (a restarted module). RTSEngineSystems::Initialize points the singleton at
     * "Saves/RTS", so every Bind re-applies the temporary directory; the original directory is restored on
     * destruction.
     */
    class RTSSaveBridge
    {
      public:
        explicit RTSSaveBridge(const char* name)
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / (std::string("spark_mod370_") + name)),
              m_context(&m_saveSystem, &m_world)
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            m_world.AddComponent<Transform>(m_world.CreateEntity("mod370-rts-map"));
        }

        ~RTSSaveBridge()
        {
            m_engine.Shutdown();
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        RTSSaveBridge(const RTSSaveBridge&) = delete;
        RTSSaveBridge& operator=(const RTSSaveBridge&) = delete;

        /// Bind the module bridge to @p game exactly as SparkGameRTSModule::OnLoad does.
        bool Bind(Skirmish& game)
        {
            m_engine.Shutdown();
            const bool bound = m_engine.Initialize(&m_context, game.Systems(), &game.simulation);
            m_saveSystem.SetFileCache(nullptr);
            return m_saveSystem.Initialize(m_directory.string()) && bound;
        }

        RTSEngineSystems& Engine() { return m_engine; }
        ::World& World() { return m_world; }

        /// Write a slot whose only custom state is @p key = @p payload, as an older or damaged build would.
        bool WriteRawSlot(const std::string& slot, const std::string& key, const std::string& payload)
        {
            Spark::SaveMetadata meta;
            meta.saveName = slot;
            meta.sceneName = "RTSMatch";
            return m_saveSystem.Save(slot, m_world, meta, std::unordered_map<std::string, std::string>{{key, payload}});
        }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        ::World m_world;
        RTSSaveContext m_context;
        RTSEngineSystems m_engine;
    };
} // namespace

// Save at several ticks (mid waypoint queue, off the AI decision phase, mid battle), load each into freshly built
// systems, and require the uninterrupted run's hash at the save tick and for every one of the next 2000 ticks.
TEST(RTSSave_MidSkirmishLoadMatchesUninterruptedPerTickHashes)
{
    const std::vector<uint64_t> reference = RunReference();
    ASSERT_TRUE(reference.size() > static_cast<size_t>(COUNTER_ATTACK_TICK + COMPARE_TICKS));

    const std::vector<uint64_t> saveTicks = {WAYPOINT_TICK + 5, COUNTER_ATTACK_TICK - 13, COUNTER_ATTACK_TICK + 211};
    for (uint64_t saveTick : saveTicks)
    {
        auto source = std::make_unique<Skirmish>();
        const std::string encoded = SaveAtTick(*source, saveTick);
        ASSERT_FALSE(encoded.empty());
        ASSERT_EQ(source->simulation.GetTick(), saveTick);
        ASSERT_EQ(source->simulation.ComputeStateHash(), reference[saveTick]);
        if (saveTick == WAYPOINT_TICK + 5)
        {
            // The save must carry shift-queued orders and a live selection, not just unit records.
            EXPECT_GT(source->commands.GetSelectionCount(), static_cast<size_t>(0));
            EXPECT_GT(source->commands.GetPendingCommandCount(), source->commands.GetSelectionCount());
        }
        EXPECT_NE(saveTick % RTSSkirmishSimulation::AI_DECISION_TICKS, static_cast<uint64_t>(0));

        RTSPersistenceSnapshot decoded;
        std::string error;
        ASSERT_TRUE(RTSPersistence::Deserialize(encoded, decoded, error));
        EXPECT_EQ(RTSPersistence::Serialize(decoded), encoded);

        auto loaded = FreshDivergedSkirmish();
        ASSERT_NE(loaded->simulation.ComputeStateHash(), reference[saveTick]);
        ASSERT_TRUE(RTSPersistence::Apply(decoded, loaded->Systems(), loaded->simulation, error));
        EXPECT_EQ(error, std::string());
        EXPECT_EQ(loaded->simulation.GetTick(), saveTick);
        EXPECT_EQ(loaded->simulation.ComputeStateHash(), reference[saveTick]);

        size_t compared = 0;
        size_t firstMismatch = 0;
        const uint64_t lastTick = std::min<uint64_t>(saveTick + COMPARE_TICKS, reference.size() - 1);
        while (loaded->simulation.GetTick() < lastTick)
        {
            PlayTick(*loaded);
            const uint64_t tick = loaded->simulation.GetTick();
            ASSERT_TRUE(tick < reference.size());
            if (loaded->simulation.ComputeStateHash() != reference[tick] && firstMismatch == 0)
                firstMismatch = static_cast<size_t>(tick);
            ++compared;
        }
        EXPECT_EQ(firstMismatch, static_cast<size_t>(0));
        EXPECT_EQ(compared, static_cast<size_t>(lastTick - saveTick));

        // Negative control: resuming from only what the v1 format carried (records, no orders, selection, or
        // explored fog) plays out differently, so the per-tick comparison above is sensitive to the added state.
        if (saveTick == WAYPOINT_TICK + 5)
        {
            RTSPersistenceSnapshot recordsOnly = decoded;
            recordsOnly.commandQueues.clear();
            recordsOnly.selection.clear();
            for (FogGrid& grid : recordsOnly.fog)
                std::ranges::fill(grid.cells, RTSVisibility::Unexplored);
            auto partial = FreshDivergedSkirmish();
            ASSERT_TRUE(RTSPersistence::Apply(recordsOnly, partial->Systems(), partial->simulation, error));
            while (partial->simulation.GetTick() < lastTick)
                PlayTick(*partial);
            EXPECT_NE(partial->simulation.ComputeStateHash(), reference[lastTick]);
        }
    }
}

// Through the console-facing SaveMatch/LoadMatch on the real SaveSystem: a late save loaded into a fresh module
// finishes the match with the same winner, on the same tick, with the same final state hash.
TEST(RTSSave_LoadedSkirmishStillReachesVictory)
{
    const std::vector<uint64_t> reference = RunReference();
    const uint64_t victoryTick = reference.size() - 1;
    ASSERT_TRUE(victoryTick > COUNTER_ATTACK_TICK);
    ASSERT_TRUE(victoryTick < MAX_SKIRMISH_TICKS);
    const uint64_t saveTick = victoryTick > 600 ? victoryTick - 600 : COUNTER_ATTACK_TICK + 1;

    RTSSaveBridge bridge("victory");
    auto source = std::make_unique<Skirmish>();
    ASSERT_TRUE(bridge.Bind(*source));
    while (source->simulation.GetTick() < saveTick)
        PlayTick(*source);
    ASSERT_EQ(source->simulation.ComputeStateHash(), reference[saveTick]);
    ASSERT_TRUE(bridge.Engine().SaveMatch("mod370_victory"));
    source.reset();

    // A restarted module: every system rebuilt (and drifted), then the slot is loaded back in.
    auto loaded = FreshDivergedSkirmish();
    ASSERT_TRUE(bridge.Bind(*loaded));
    ASSERT_TRUE(bridge.Engine().LoadMatch("mod370_victory"));
    EXPECT_EQ(loaded->simulation.GetTick(), saveTick);
    EXPECT_EQ(loaded->simulation.ComputeStateHash(), reference[saveTick]);

    while (loaded->Playing() && loaded->simulation.GetTick() < MAX_SKIRMISH_TICKS)
        PlayTick(*loaded);
    EXPECT_TRUE(loaded->match.GetMatchState() == RTSMatchState::Victory);
    EXPECT_TRUE(loaded->match.GetWinner() == RTSFaction::Human);
    EXPECT_TRUE(loaded->match.IsPlayerEliminated(1));
    EXPECT_EQ(loaded->simulation.GetTick(), victoryTick);
    EXPECT_EQ(loaded->simulation.ComputeStateHash(), reference[victoryTick]);
}

// The retired v1 format, every truncation of a v2 save, trailing data, and a snapshot the systems cannot hold are
// all rejected, and none of them changes the decoder output or the running match.
TEST(RTSSave_RejectsV1AndTruncatedWithoutMutation)
{
    auto game = std::make_unique<Skirmish>();
    const std::string encoded = SaveAtTick(*game, COUNTER_ATTACK_TICK + 211);
    ASSERT_FALSE(encoded.empty());
    const uint64_t liveHash = game->simulation.ComputeStateHash();
    const uint64_t liveTick = game->simulation.GetTick();

    RTSPersistenceSnapshot sentinel;
    sentinel.tick = 4242;
    sentinel.units.push_back(UnitData{});
    sentinel.units.back().unitId = 42;
    std::string error;

    const std::string v1 = "SPARK_RTS_STATE_V1\nUNITS 0\nBUILDINGS 0\nPLAYERS 0\nNODES 0\nEND\n";
    EXPECT_FALSE(RTSPersistence::Deserialize(v1, sentinel, error));
    EXPECT_TRUE(error.find("version 1") != std::string::npos);

    // Every strict prefix that drops more than the final newline is truncated and must fail.
    const size_t complete = encoded.find_last_not_of('\n') + 1;
    size_t acceptedPrefixes = 0;
    for (size_t length = 0; length < complete; ++length)
    {
        error.clear();
        if (RTSPersistence::Deserialize(std::string_view(encoded).substr(0, length), sentinel, error) || error.empty())
            ++acceptedPrefixes;
    }
    EXPECT_EQ(acceptedPrefixes, static_cast<size_t>(0));
    EXPECT_FALSE(RTSPersistence::Deserialize(encoded + "U 1\n", sentinel, error));
    EXPECT_FALSE(
        RTSPersistence::Deserialize("SPARK_RTS_STATE_V3\n" + encoded.substr(encoded.find('\n') + 1), sentinel, error));
    EXPECT_EQ(sentinel.tick, static_cast<uint64_t>(4242));
    ASSERT_EQ(sentinel.units.size(), static_cast<size_t>(1));
    EXPECT_EQ(sentinel.units.front().unitId, static_cast<uint32_t>(42));

    // A validated-looking snapshot missing a faction's fog grid is refused before any system changes.
    RTSPersistenceSnapshot missingFog;
    ASSERT_TRUE(RTSPersistence::Deserialize(encoded, missingFog, error));
    missingFog.fog.pop_back();
    EXPECT_FALSE(RTSPersistence::Apply(missingFog, game->Systems(), game->simulation, error));
    EXPECT_EQ(game->simulation.ComputeStateHash(), liveHash);

    // Through LoadMatch: a v1 slot, a truncated v2 slot, and a missing slot leave the running match untouched.
    RTSSaveBridge bridge("reject");
    ASSERT_TRUE(bridge.Bind(*game));
    ASSERT_TRUE(bridge.WriteRawSlot("legacy", std::string(RTSPersistence::LegacyStateKeyV1), v1));
    ASSERT_TRUE(
        bridge.WriteRawSlot("truncated", std::string(RTSPersistence::StateKey), encoded.substr(0, encoded.size() / 2)));
    // The rejected RTS state is decoded before the world restore commits, so the ECS world keeps an entity
    // created after the slots were written.
    bridge.World().CreateEntity("mod370-after-save");
    const size_t liveEntities = bridge.World().GetEntityCount();
    EXPECT_FALSE(bridge.Engine().LoadMatch("legacy"));
    EXPECT_FALSE(bridge.Engine().LoadMatch("truncated"));
    EXPECT_FALSE(bridge.Engine().LoadMatch("missing"));
    EXPECT_EQ(bridge.World().GetEntityCount(), liveEntities);
    EXPECT_EQ(game->simulation.GetTick(), liveTick);
    EXPECT_EQ(game->simulation.ComputeStateHash(), liveHash);
}

#endif // SPARK_TEST_HAS_IMGUI
