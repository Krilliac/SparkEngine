/**
 * @file TestMOD390VisualScriptGameplayReal.cpp
 * @brief MOD-390: the shipped SparkGameVisualScript scripts play headless to the win objective.
 *
 * Builds the module's real demo world (VisualScriptDemoWorld.cpp, the code
 * OnLoad and vs_restart run) from the shipped Generated .as scripts against a
 * real World, the production AngelScriptEngine and a real InputManager
 * installed in an injected EngineContext, exactly where the script getKey /
 * getKeyDown bindings read it. The test only plays the role of the human at
 * the keyboard: each frame it presses W/A/S/D towards a target, then ticks
 * every demo script through CallUpdate at the module's sanitized frame delta,
 * in spawn order, as SparkGameVisualScriptModule::OnUpdate does.
 *
 * All gameplay (movement, pickup, scoring, enemy damage, healing, the win) is
 * decided by the scripts. It is observed only through script-visible state:
 * Transform and HealthComponent values the scripts write, and the messages
 * the scripts print().
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoRuntime.h"
#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoWorld.h"
#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Input/InputManager.h"
#include "ScopedLoggerBaseline.h"
#include "Utils/Logger.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Spark::VisualScriptDemo::DemoWorld;

    constexpr float kFrameDelta = 1.0f / 60.0f;

    /// Horizontal distance at which the scripted "player" stops pressing keys towards a target.
    constexpr float kArrivalTolerance = 0.2f;

    /// Script print() output is logged as "[Script] <message>"; this is the exact prefix.
    constexpr std::string_view kScriptPrintPrefix = "[Script] ";

    /// Restores the process-wide injected EngineContext on every exit path.
    struct ScopedInjectedContext
    {
        explicit ScopedInjectedContext(EngineContext* context) { EngineContext::SetInjected(context); }
        ~ScopedInjectedContext() { EngineContext::SetInjected(nullptr); }
        ScopedInjectedContext(const ScopedInjectedContext&) = delete;
        ScopedInjectedContext& operator=(const ScopedInjectedContext&) = delete;
    };

    /// The shipped demo, running on a real World / AngelScriptEngine / InputManager.
    struct GameplayFixture
    {
        ScopedLoggerBaseline loggerBaseline; // restored last, after the capture sink is gone
        std::vector<std::string> scriptOutput;
        World world;
        InputManager input;
        EngineContext context;
        std::optional<ScopedInjectedContext> injected;
        AngelScriptEngine engine;
        std::optional<DemoWorld> demo; // destroyed first (declared last)
        bool ready = false;

        GameplayFixture()
        {
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
                [this](const Spark::LogMessage& message)
                {
                    if (message.message.starts_with(kScriptPrintPrefix))
                        scriptOutput.push_back(message.message.substr(kScriptPrintPrefix.size()));
                }));

            context.SetWorld(&world);
            context.SetInput(&input);
            injected.emplace(&context);

            const std::array<fs::path, 1> searchPaths = {fs::path(SPARK_TEST_SOURCE_DIR) /
                                                         "GameModules/SparkGameVisualScript/Assets/Scripts/Generated"};
            demo.emplace(world, engine);
            ready = engine.Initialize() && demo->LoadScripts(searchPaths) && demo->Spawn();
        }

        ~GameplayFixture()
        {
            demo.reset();
            if (AngelScriptEngine::GetBoundWorld() == &world)
                AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
            injected.reset();
            Spark::Logger::Get().ClearSinks(); // drop the sink capturing `this` before it dangles
        }

        EntityID Find(const std::string& name)
        {
            for (auto entity : world.GetEntitiesWith<NameComponent>())
            {
                const auto* component = world.GetComponent<NameComponent>(entity);
                if (component && component->name == name)
                    return entity;
            }
            return entt::null;
        }

        DirectX::XMFLOAT3 Position(EntityID entity) { return world.GetComponent<Transform>(entity)->position; }
        float Health(EntityID entity) { return world.GetComponent<HealthComponent>(entity)->health; }

        /// Hold or release one key through the same message path the platform hosts use.
        void SetKey(int virtualKey, bool down) { input.HandleMessage(down ? WM_KEYDOWN : WM_KEYUP, virtualKey, 0); }

        /// Press the WASD keys that move the player towards (x, z); release all of them once there.
        void SteerTowards(float x, float z)
        {
            const auto player = Position(Find("VS_Player"));
            const float dx = x - player.x;
            const float dz = z - player.z;
            SetKey('D', dx > kArrivalTolerance);
            SetKey('A', dx < -kArrivalTolerance);
            SetKey('W', dz > kArrivalTolerance);
            SetKey('S', dz < -kArrivalTolerance);
        }

        void ReleaseAllKeys() { SteerTowards(Position(Find("VS_Player")).x, Position(Find("VS_Player")).z); }

        /// One host frame: input edge update, then every demo script's Update() in spawn order.
        void Tick()
        {
#ifndef _WIN32
            // The Windows Update() requires the HWND from Initialize() and aborts without one. getKey reads
            // IsKeyDown, which the injected WM_KEYDOWN/WM_KEYUP messages drive on every platform, so the
            // Windows frame skips the edge update; none of these scenarios uses the edge-triggered getKeyDown.
            input.Update();
#endif
            const float scriptDelta = Spark::VisualScriptDemo::SanitizeDeltaTime(kFrameDelta);
            for (EntityID entity : demo->GetEntities())
            {
                const auto* script = world.GetComponent<Script>(entity);
                if (script && script->enabled)
                    engine.CallUpdate(entity, scriptDelta);
            }
        }

        size_t CountOutput(std::string_view text) const
        {
            size_t count = 0;
            for (const auto& line : scriptOutput)
            {
                if (line == text)
                    ++count;
            }
            return count;
        }

        bool AnyScriptFaulted()
        {
            for (EntityID entity : demo->GetEntities())
            {
                if (engine.IsScriptFaulted(entity))
                    return true;
            }
            return false;
        }
    };
} // namespace

TEST(VisualScriptGameplay_ScriptedPlayerCollectsAllPickupsAndWins)
{
    GameplayFixture fx;
    ASSERT_TRUE(fx.ready);

    const EntityID player = fx.Find("VS_Player");
    const EntityID manager = fx.Find("VS_GameManager");
    ASSERT_TRUE(player != entt::null);
    ASSERT_TRUE(manager != entt::null);
    EXPECT_EQ(fx.Health(manager), 0.0f); // GameManager.Start() resets the score it keeps in its health

    // The player visits every coin's spawn X/Z; the coin scripts decide the pickup.
    std::vector<EntityID> coins;
    for (int i = 0; i < 5; ++i)
    {
        coins.push_back(fx.Find("VS_Coin_" + std::to_string(i)));
        ASSERT_TRUE(coins.back() != entt::null);
    }
    const std::array<int, 5> route = {2, 1, 0, 3, 4};

    constexpr int kFrameBudget = 60 * 60; // one minute of game time
    int frame = 0;
    for (int coinIndex : route)
    {
        const EntityID coin = coins[static_cast<size_t>(coinIndex)];
        const float targetX = fx.Position(coin).x;
        const float targetZ = fx.Position(coin).z;
        while (fx.Position(coin).y > -50.0f && frame < kFrameBudget)
        {
            fx.SteerTowards(targetX, targetZ);
            fx.Tick();
            ++frame;
        }
        EXPECT_TRUE(fx.Position(coin).y == -100.0f); // Collectible.Collect() hid it
    }
    fx.ReleaseAllKeys();
    fx.Tick(); // GameManager evaluates the win on its next Update()
    ASSERT_TRUE(frame < kFrameBudget);

    // Script-visible outcome: five pickups, 500 points, one win, no loss.
    EXPECT_EQ(fx.CountOutput("Collected! +100 points"), static_cast<size_t>(5));
    EXPECT_EQ(fx.Health(manager), 500.0f);
    EXPECT_EQ(fx.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(1));
    EXPECT_EQ(fx.CountOutput("Score: 500"), static_cast<size_t>(1));
    EXPECT_EQ(fx.CountOutput("*** GAME OVER ***"), static_cast<size_t>(0));
    EXPECT_GT(fx.Health(player), 0.0f);
    EXPECT_FALSE(fx.AnyScriptFaulted());

    // The objective is terminal: further play neither re-awards nor re-announces it.
    for (int i = 0; i < 120; ++i)
        fx.Tick();
    EXPECT_EQ(fx.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(1));
    EXPECT_EQ(fx.Health(manager), 500.0f);
}

TEST(VisualScriptGameplay_NoInputMeansNoMovementAndNoWin)
{
    // Guards the win test above: without key presses the player script must
    // stay put, so the pickups really are the result of scripted input.
    GameplayFixture fx;
    ASSERT_TRUE(fx.ready);

    const EntityID player = fx.Find("VS_Player");
    const EntityID manager = fx.Find("VS_GameManager");
    const auto start = fx.Position(player);
    for (int i = 0; i < 300; ++i)
        fx.Tick();

    const auto end = fx.Position(player);
    EXPECT_EQ(end.x, start.x);
    EXPECT_EQ(end.z, start.z);
    EXPECT_EQ(fx.Health(manager), 0.0f);
    EXPECT_EQ(fx.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(0));
}

TEST(VisualScriptGameplay_EnemyContactDamagesAndHealthPickupHeals)
{
    GameplayFixture fx;
    ASSERT_TRUE(fx.ready);

    const EntityID player = fx.Find("VS_Player");
    const EntityID enemy = fx.Find("VS_Enemy_0");
    const EntityID healthPack = fx.Find("VS_HealthPack");
    ASSERT_TRUE(player != entt::null);
    ASSERT_TRUE(enemy != entt::null);
    ASSERT_TRUE(healthPack != entt::null);
    EXPECT_EQ(fx.Health(player), 100.0f);
    const float packSpawnHeight = fx.Position(healthPack).y; // before any Update() bobs it

    // Walk into VS_Enemy_0's detection range and stand still. The EnemyPatrol
    // script chases, closes to attack range and strikes every 1.5 s for 10.
    int frame = 0;
    int hits = 0;
    constexpr int kWantedHits = 6;
    constexpr int kFrameBudget = 60 * 60;
    while (hits < kWantedHits && frame < kFrameBudget)
    {
        fx.SteerTowards(8.0f, 5.0f);
        const float before = fx.Health(player);
        fx.Tick();
        ++frame;
        const float after = fx.Health(player);
        if (after < before)
        {
            // The attack (attackDamage 10) lands in the same frame as the
            // PlayerController's own 2 HP/s regen, so the net drop is 10 minus
            // at most one frame of regen.
            EXPECT_NEAR(before - after, 10.0f, 2.0f * kFrameDelta + 1e-3f);
            ++hits;
        }
    }
    ASSERT_EQ(hits, kWantedHits);
    EXPECT_EQ(fx.CountOutput("Enemy attacks player for 10 damage!"), static_cast<size_t>(kWantedHits));
    const auto enemyPosition = fx.Position(enemy);
    const auto playerPosition = fx.Position(player);
    const float enemyDx = enemyPosition.x - playerPosition.x;
    const float enemyDz = enemyPosition.z - playerPosition.z;
    EXPECT_LE(enemyDx * enemyDx + enemyDz * enemyDz, 4.0f); // it closed to its 2 m attack range
    EXPECT_LT(fx.Health(player), 65.0f);                    // 6 hits outpace 2 HP/s regen

    // Run to the health pack. The HealthPickup script heals 30 once, hides
    // itself and starts its respawn countdown.
    const auto packSpawn = fx.Position(healthPack);
    float healedBy = 0.0f;
    while (fx.Position(healthPack).y > -50.0f && frame < kFrameBudget)
    {
        fx.SteerTowards(packSpawn.x, packSpawn.z);
        const float before = fx.Health(player);
        fx.Tick();
        ++frame;
        healedBy = fx.Health(player) - before;
    }
    ASSERT_TRUE(fx.Position(healthPack).y == -100.0f);
    EXPECT_NEAR(healedBy, 30.0f, 0.1f); // heal plus at most one frame of 2 HP/s regen
    EXPECT_EQ(fx.CountOutput("Player healed for 30 HP!"), static_cast<size_t>(1));
    EXPECT_GT(fx.Health(player), 0.0f);

    // The pickup stays spent for its 10 s respawnTime (VS_Enemy_0 may keep
    // chasing and striking meanwhile, but nothing can heal the player), then
    // the script restores it to its spawn height in the frame it announces.
    fx.ReleaseAllKeys();
    const int pickupFrame = frame;
    while (fx.CountOutput("Health pickup respawned") == 0 && frame < kFrameBudget)
    {
        fx.Tick();
        ++frame;
        if (fx.CountOutput("Health pickup respawned") == 0)
        {
            EXPECT_TRUE(fx.Position(healthPack).y == -100.0f);
        }
    }
    ASSERT_EQ(fx.CountOutput("Health pickup respawned"), static_cast<size_t>(1));
    const float respawnSeconds = static_cast<float>(frame - pickupFrame) * kFrameDelta;
    EXPECT_NEAR(respawnSeconds, 10.0f, 2.0f * kFrameDelta);
    EXPECT_EQ(fx.CountOutput("Player healed for 30 HP!"), static_cast<size_t>(1));
    EXPECT_NEAR(fx.Position(healthPack).y, packSpawnHeight, 1e-4f);
    EXPECT_FALSE(fx.AnyScriptFaulted());
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
