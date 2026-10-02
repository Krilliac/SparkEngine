/**
 * @file TestMOD340PlatformerProgressReal.cpp
 * @brief MOD-340: platformer progress persists through the real SaveSystem and bad saves change nothing.
 *
 * Every test drives the real SparkGamePlatformer sources: the level, checkpoint, collectible, hazard and player
 * systems, PlatformerLevelFlow, the PlatformerProgress codec, and the console-facing
 * PlatformerEngineSystems::SaveProgress/LoadProgress path on the real SaveSystem singleton writing into a temporary
 * directory. A "restart" is a second set of freshly built systems bound to a new PlatformerEngineSystems.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGamePlatformer/Source/Checkpoint/PlatformerCheckpointSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Collectible/PlatformerCollectibleSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Core/PlatformerEngineSystems.h"
#include "../GameModules/SparkGamePlatformer/Source/Core/PlatformerLevelFlow.h"
#include "../GameModules/SparkGamePlatformer/Source/Core/PlatformerProgress.h"
#include "../GameModules/SparkGamePlatformer/Source/Hazard/PlatformerHazardSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Level/PlatformerLevelSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Player/PlatformerPlayerController.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Platformer;

namespace
{
    /// The gameplay systems SparkGamePlatformerModule owns, started on level 0 the way OnLoad starts them.
    struct PlatformerGame
    {
        PlatformerLevelSystem level;
        PlatformerCheckpointSystem checkpoints;
        PlatformerCollectibleSystem collectibles;
        PlatformerHazardSystem hazards;
        PlatformerPlayerController player;
        PlatformerLevelFlow flow{level, player, collectibles, hazards, checkpoints};

        PlatformerGame()
        {
            level.Initialize(nullptr);
            checkpoints.Initialize(nullptr);
            player.Initialize(nullptr, &checkpoints, &level);
            collectibles.Initialize(nullptr);
            hazards.Initialize(nullptr);
            flow.StartLevel(0);
        }

        ~PlatformerGame()
        {
            hazards.Shutdown();
            collectibles.Shutdown();
            player.Shutdown();
            checkpoints.Shutdown();
            level.Shutdown();
        }

        PlatformerGame(const PlatformerGame&) = delete;
        PlatformerGame& operator=(const PlatformerGame&) = delete;

        PlatformerProgressSystems Systems() { return {&level, &collectibles, &checkpoints, &player}; }
        std::string Encoded() { return PlatformerProgress::Serialize(PlatformerProgress::Capture(Systems())); }
    };

    /**
     * Earn progress through the systems' own gameplay entry points: pick up coins, a star, a key and ability orbs,
     * raise two checkpoints, lose a life, and finish level 0 in 75 s (two stars, which unlocks level 1).
     */
    void EarnProgress(PlatformerGame& game)
    {
        const float pickupSpots[][2] = {{5.0f, 1.0f},  {7.0f, 1.0f},  {20.0f, 1.0f},
                                        {30.0f, 8.0f}, {40.0f, 1.0f}, {48.0f, 5.0f}};
        for (const auto& spot : pickupSpots)
        {
            for (const CollectedPickup& pickup : game.collectibles.CheckCollection(spot[0], spot[1], 0.0f, false))
            {
                if (pickup.type == CollectibleType::AbilityOrb)
                    game.player.UnlockAbility(pickup.abilityType);
            }
        }

        game.checkpoints.CheckActivation(25.0f, 5.0f, 0.0f);
        game.checkpoints.CheckActivation(55.0f, 13.0f, 0.0f);
        for (int frame = 0; frame < 120; ++frame)
            game.player.Update(1.0f / 60.0f); // Let the respawn invincibility frames expire
        EXPECT_TRUE(game.player.TakeDamage(1));
        game.level.CompleteLevel(75.0f, 1);
    }

    /// Engine context exposing only the real SaveSystem and a World, as the module sees them.
    class PlatformerSaveContext final : public Spark::IEngineContext
    {
      public:
        PlatformerSaveContext(Spark::SaveSystem* saveSystem, ::World* world) : m_saveSystem(saveSystem), m_world(world)
        {
        }

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
     * rebinding to another set of systems (a restarted module). PlatformerEngineSystems::Initialize points the
     * singleton at "Saves/Platformer", so every Bind re-applies the temporary directory; the original directory is
     * restored on destruction.
     */
    class PlatformerSaveBridge
    {
      public:
        explicit PlatformerSaveBridge(const char* name)
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / (std::string("spark_mod340_") + name)),
              m_context(&m_saveSystem, &m_world)
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            m_world.AddComponent<Transform>(m_world.CreateEntity("mod340-platformer-level"));
        }

        ~PlatformerSaveBridge()
        {
            m_engine.reset();
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        PlatformerSaveBridge(const PlatformerSaveBridge&) = delete;
        PlatformerSaveBridge& operator=(const PlatformerSaveBridge&) = delete;

        /// Bind a brand-new PlatformerEngineSystems to @p game exactly as SparkGamePlatformerModule::OnLoad does.
        bool Bind(PlatformerGame& game)
        {
            if (m_engine)
                m_engine->Shutdown();
            m_engine = std::make_unique<PlatformerEngineSystems>();
            const bool bound = m_engine->Initialize(&m_context, game.Systems());
            m_saveSystem.SetFileCache(nullptr);
            return m_saveSystem.Initialize(m_directory.string()) && bound;
        }

        PlatformerEngineSystems& Engine() { return *m_engine; }
        ::World& World() { return m_world; }

        /// Write a slot whose only custom state is @p key = @p payload, as an older or damaged build would.
        bool WriteRawSlot(const std::string& slot, const std::string& key, const std::string& payload)
        {
            Spark::SaveMetadata meta;
            meta.saveName = slot;
            meta.sceneName = "platformer";
            return m_saveSystem.Save(slot, m_world, meta, std::unordered_map<std::string, std::string>{{key, payload}});
        }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        ::World m_world;
        PlatformerSaveContext m_context;
        std::unique_ptr<PlatformerEngineSystems> m_engine;
    };

    /// Replace the first occurrence of @p from in @p text with @p to.
    std::string Replaced(std::string text, const std::string& from, const std::string& to)
    {
        const size_t at = text.find(from);
        EXPECT_TRUE(at != std::string::npos);
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return text;
    }
} // namespace

// Earn progress, save through the module bridge, then load into freshly built systems bound to a new bridge
// and compare every persisted field.
TEST(PlatformerCompletion_ProgressSurvivesSaveSystemRestart)
{
    PlatformerSaveBridge bridge("restart");

    auto source = std::make_unique<PlatformerGame>();
    EarnProgress(*source);
    const PlatformerProgressSnapshot saved = PlatformerProgress::Capture(source->Systems());

    // The progress under test is not the fresh-game default.
    ASSERT_TRUE(saved.levels.at(0).completed);
    ASSERT_EQ(saved.levels.at(0).starsEarned, 2);
    ASSERT_TRUE(saved.levels.at(1).unlocked);
    ASSERT_TRUE(saved.collection.collectedIds.size() >= 6u);
    ASSERT_TRUE(saved.collection.coins >= 2);
    ASSERT_EQ(saved.collection.stars, 1);
    ASSERT_EQ(saved.collection.keys, 1);
    ASSERT_EQ(saved.checkpoints.activatedIds.size(), 2u);
    ASSERT_EQ(saved.checkpoints.lastActivatedId, 2u);
    ASSERT_EQ(saved.player.lives, PlatformerPlayerController::DEFAULT_LIVES - 1);
    ASSERT_TRUE(saved.player.abilities.doubleJump);
    ASSERT_TRUE(saved.player.abilities.dash);

    ASSERT_TRUE(bridge.Bind(*source));
    ASSERT_TRUE(bridge.Engine().SaveProgress("progress_slot"));
    source.reset();

    // Restart: new systems and a new engine bridge. Before loading, level 1 is still locked.
    auto restored = std::make_unique<PlatformerGame>();
    ASSERT_TRUE(bridge.Bind(*restored));
    EXPECT_FALSE(restored->level.LoadLevel(1));
    ASSERT_TRUE(restored->level.LoadLevel(0));
    ASSERT_TRUE(bridge.Engine().LoadProgress("progress_slot"));

    const PlatformerProgressSnapshot loaded = PlatformerProgress::Capture(restored->Systems());
    ASSERT_EQ(loaded.levels.size(), saved.levels.size());
    for (size_t i = 0; i < saved.levels.size(); ++i)
    {
        EXPECT_EQ(loaded.levels[i].completed, saved.levels[i].completed);
        EXPECT_EQ(loaded.levels[i].unlocked, saved.levels[i].unlocked);
        EXPECT_EQ(loaded.levels[i].starsEarned, saved.levels[i].starsEarned);
        EXPECT_EQ(loaded.levels[i].bestTime, saved.levels[i].bestTime);
        EXPECT_EQ(loaded.levels[i].bestDeaths, saved.levels[i].bestDeaths);
        EXPECT_EQ(loaded.levels[i].secretFound, saved.levels[i].secretFound);
    }
    EXPECT_TRUE(loaded.collection.collectedIds == saved.collection.collectedIds);
    EXPECT_EQ(loaded.collection.coins, saved.collection.coins);
    EXPECT_EQ(loaded.collection.gems, saved.collection.gems);
    EXPECT_EQ(loaded.collection.stars, saved.collection.stars);
    EXPECT_EQ(loaded.collection.keys, saved.collection.keys);
    EXPECT_TRUE(loaded.checkpoints.activatedIds == saved.checkpoints.activatedIds);
    EXPECT_EQ(loaded.checkpoints.lastActivatedId, saved.checkpoints.lastActivatedId);
    EXPECT_EQ(loaded.player.lives, saved.player.lives);
    EXPECT_EQ(loaded.player.abilities.doubleJump, saved.player.abilities.doubleJump);
    EXPECT_EQ(loaded.player.abilities.wallJump, saved.player.abilities.wallJump);
    EXPECT_EQ(loaded.player.abilities.dash, saved.player.abilities.dash);
    EXPECT_EQ(loaded.player.abilities.groundPound, saved.player.abilities.groundPound);
    EXPECT_EQ(loaded.player.abilities.climb, saved.player.abilities.climb);

    // The restored state drives gameplay: the second checkpoint is the respawn point, the collected items stay
    // collected, and the unlock earned by the saved stars lets level 1 load.
    const PlayerPosition respawn = restored->checkpoints.GetLastCheckpointPosition();
    EXPECT_NEAR(respawn.x, 55.0f, 1e-4f);
    EXPECT_NEAR(respawn.y, 13.0f, 1e-4f);
    EXPECT_EQ(restored->collectibles.GetCurrentLevelStats().collectedStars, 1);
    EXPECT_TRUE(restored->collectibles.CheckCollection(30.0f, 8.0f, 0.0f, false).empty());
    EXPECT_TRUE(restored->level.LoadLevel(1));
}

// Malformed, out-of-range, and incompatible progress is rejected by the decoder, by validation against the live
// systems, and by LoadProgress before the SaveSystem commits the world, and nothing changes.
TEST(PlatformerCompletion_CorruptProgressIsRejectedWithoutMutation)
{
    auto game = std::make_unique<PlatformerGame>();
    EarnProgress(*game);
    const std::string encoded = game->Encoded();

    PlatformerProgressSnapshot decoded;
    std::string error;
    ASSERT_TRUE(PlatformerProgress::Deserialize(encoded, decoded, error));
    EXPECT_EQ(PlatformerProgress::Serialize(decoded), encoded);

    // Decoder: every strict prefix, trailing data, another format version, and out-of-range fields.
    PlatformerProgressSnapshot sentinel;
    sentinel.player.lives = 7;
    size_t acceptedPrefixes = 0;
    for (size_t length = 0; length + 1 < encoded.size(); ++length)
    {
        error.clear();
        if (PlatformerProgress::Deserialize(std::string_view(encoded).substr(0, length), sentinel, error) ||
            error.empty())
            ++acceptedPrefixes;
    }
    EXPECT_EQ(acceptedPrefixes, static_cast<size_t>(0));

    const std::vector<std::string> malformed = {
        encoded + "level 0 0 0 0 0 0\n",
        Replaced(encoded, "platformer-progress 1", "platformer-progress 2"),
        Replaced(encoded, "level 1 1 2 ", "level 1 1 4 "),
        Replaced(encoded, "level 1 1 2 ", "level 1 1 -1 "),
        Replaced(encoded, "level 1 1 2 42960000", "level 1 1 2 7fc00000"), // NaN best time
        Replaced(encoded, "level 1 1 2 42960000", "level 1 1 2 7f800000"), // +inf best time
        Replaced(encoded, "player 2 ", "player 0 "),
        Replaced(encoded, "player 2 ", "player 10 "),
        Replaced(encoded, "player 2 5", "player 2 32"),
        Replaced(encoded, "respawn 2", "respawn 3"),
        Replaced(encoded, "checkpoints 2 1 2", "checkpoints 2 2 1"),
        Replaced(encoded, "checkpoints 2 1 2", "checkpoints 2 1 1"),
        Replaced(encoded, "checkpoints 2 1 2", "checkpoints 3000 1 2"),
        Replaced(encoded, "counters ", "counters -1 "),
        std::string(PlatformerProgress::MAX_ENCODED_BYTES + 1, ' '),
    };
    for (const std::string& text : malformed)
    {
        error.clear();
        EXPECT_FALSE(PlatformerProgress::Deserialize(text, sentinel, error));
        EXPECT_FALSE(error.empty());
    }
    EXPECT_EQ(sentinel.player.lives, 7);
    EXPECT_TRUE(sentinel.levels.empty());

    // Validation against the live systems, through Apply: nothing changes when it fails.
    auto target = std::make_unique<PlatformerGame>();
    const std::string targetBefore = target->Encoded();
    std::vector<PlatformerProgressSnapshot> invalid(5, decoded);
    invalid[0].collection.collectedIds.push_back(9999);
    invalid[1].checkpoints.activatedIds.push_back(9999);
    invalid[2].levels.pop_back();
    invalid[3].levels.at(2).unlocked = true; // Requires more stars than the save earned
    invalid[4].levels.at(1).starsEarned = 3; // Rated but never completed
    for (const PlatformerProgressSnapshot& snapshot : invalid)
    {
        error.clear();
        EXPECT_FALSE(PlatformerProgress::Apply(snapshot, target->Systems(), error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(target->Encoded(), targetBefore);
    }

    // Through LoadProgress: a slot without the key, a truncated entry, a future version, and an unknown id leave
    // both the ECS world and the running game untouched.
    PlatformerSaveBridge bridge("reject");
    ASSERT_TRUE(bridge.Bind(*target));
    ASSERT_TRUE(bridge.WriteRawSlot("no_key", "SparkGamePlatformer.progress.v0", encoded));
    ASSERT_TRUE(bridge.WriteRawSlot("truncated", std::string(PlatformerProgress::StateKey),
                                    encoded.substr(0, encoded.size() / 2)));
    ASSERT_TRUE(bridge.WriteRawSlot("future", std::string(PlatformerProgress::StateKey),
                                    Replaced(encoded, "platformer-progress 1", "platformer-progress 2")));
    ASSERT_TRUE(bridge.WriteRawSlot("unknown_id", std::string(PlatformerProgress::StateKey),
                                    PlatformerProgress::Serialize(invalid[0])));
    bridge.World().CreateEntity("mod340-after-save");
    const size_t liveEntities = bridge.World().GetEntityCount();
    EXPECT_FALSE(bridge.Engine().LoadProgress("no_key"));
    EXPECT_FALSE(bridge.Engine().LoadProgress("truncated"));
    EXPECT_FALSE(bridge.Engine().LoadProgress("future"));
    EXPECT_FALSE(bridge.Engine().LoadProgress("unknown_id"));
    EXPECT_FALSE(bridge.Engine().LoadProgress("missing"));
    EXPECT_FALSE(bridge.Engine().LoadProgress("../escape"));
    EXPECT_EQ(bridge.World().GetEntityCount(), liveEntities);
    EXPECT_EQ(target->Encoded(), targetBefore);

    // Control: the intact entry loads through the same path.
    ASSERT_TRUE(bridge.WriteRawSlot("intact", std::string(PlatformerProgress::StateKey), encoded));
    EXPECT_TRUE(bridge.Engine().LoadProgress("intact"));
    EXPECT_EQ(target->Encoded(), encoded);
}

#endif // SPARK_TEST_HAS_IMGUI
