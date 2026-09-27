/**
 * @file TestMOD390VisualScriptGameplayReal.cpp
 * @brief MOD-390: the shipped SparkGameVisualScript scripts play headless to the win objective.
 *
 * Plays the shipped Generated .as scripts from the module's source tree through
 * the shared headless harness (VisualScriptGameplayHarness.h): the module's real
 * DemoWorld on a real World, the production AngelScriptEngine and a real
 * InputManager in the injected EngineContext. The test only plays the role of
 * the human at the keyboard and ticks every demo script as
 * SparkGameVisualScriptModule::OnUpdate does.
 *
 * All gameplay (movement, pickup, scoring, enemy damage, healing, the win) is
 * decided by the scripts. It is observed only through script-visible state:
 * Transform and HealthComponent values the scripts write, the messages the
 * scripts print(), and the sound cues and animation clips they request through
 * playSound()/playAnimation() on the components DemoWorld spawns them with.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "VisualScriptGameplayHarness.h"
#include "Engine/ECS/Systems/ECSystems.h"

#include <string>
#include <vector>

namespace
{
    using VisualScriptGameplayHarness::GameplayFixture;
    using VisualScriptGameplayHarness::kCoinRoute;
    using VisualScriptGameplayHarness::kFrameDelta;
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

    constexpr int kFrameBudget = 60 * 60; // one minute of game time
    int frame = 0;
    for (int coinIndex : kCoinRoute)
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

TEST(VisualScriptGameplay_ScriptCuesReachAudioQueueAndAnimationControllers)
{
    // playSound() queues ScriptAudioCues for AudioUpdateSystem and playAnimation() switches the
    // AnimationController DemoWorld spawns on coins and enemies. Nothing drains the cue queues here,
    // so every cue a script requested during the run is still pending when the route ends.
    GameplayFixture fx;
    ASSERT_TRUE(fx.ready);
    Spark::ECS::AnimationUpdateSystem animation;
    auto& registry = fx.world.GetRegistry();

    std::vector<EntityID> coins;
    for (int i = 0; i < 5; ++i)
    {
        coins.push_back(fx.Find("VS_Coin_" + std::to_string(i)));
        ASSERT_TRUE(coins.back() != entt::null);
        const auto* controller = registry.try_get<AnimationController>(coins.back());
        ASSERT_TRUE(controller != nullptr);
        EXPECT_EQ(controller->currentAnimation, std::string("idle"));
        EXPECT_TRUE(registry.try_get<ScriptAudioCues>(coins.back()) == nullptr);
    }
    const EntityID manager = fx.Find("VS_GameManager");
    ASSERT_TRUE(manager != entt::null);

    // VS_Enemy_2 patrols x=35, beyond detection range of the whole coin route, so it only ever walks.
    const EntityID patroller = fx.Find("VS_Enemy_2");
    ASSERT_TRUE(patroller != entt::null);
    const auto* patrol = registry.try_get<AnimationController>(patroller);
    ASSERT_TRUE(patrol != nullptr);
    EXPECT_EQ(patrol->currentAnimation, std::string("idle"));

    constexpr int kFrameBudget = 60 * 60;
    int frame = 0;
    float previousPatrolTime = -1.0f;
    const auto tickWithAnimation = [&]()
    {
        fx.Tick();
        animation.Update(fx.world, kFrameDelta);
        ++frame;
        // The per-frame playAnimation("walk") must not restart the clip the system is advancing.
        EXPECT_EQ(patrol->currentAnimation, std::string("walk"));
        EXPECT_GT(patrol->currentTime, previousPatrolTime);
        previousPatrolTime = patrol->currentTime;
    };

    for (int coinIndex : kCoinRoute)
    {
        const EntityID coin = coins[static_cast<size_t>(coinIndex)];
        const auto spawn = fx.Position(coin);
        while (fx.Position(coin).y > -50.0f && frame < kFrameBudget)
        {
            fx.SteerTowards(spawn.x, spawn.z);
            tickWithAnimation();
        }
        ASSERT_TRUE(fx.Position(coin).y == -100.0f);

        // The pickup cue is queued once, positioned where the coin was collected: at its spawn X/Z
        // (the coin only bobs vertically), not at the hidden y = -100 it moves itself to afterwards.
        const auto* cues = registry.try_get<ScriptAudioCues>(coin);
        ASSERT_TRUE(cues != nullptr);
        EXPECT_EQ(cues->requested, 1u);
        ASSERT_EQ(cues->pending.size(), static_cast<size_t>(1));
        EXPECT_EQ(cues->pending[0].soundName, std::string("coin_pickup"));
        EXPECT_TRUE(cues->pending[0].positional);
        EXPECT_NEAR(cues->pending[0].position.x, spawn.x, 1e-4f);
        EXPECT_NEAR(cues->pending[0].position.z, spawn.z, 1e-4f);
        EXPECT_GT(cues->pending[0].position.y, -50.0f);
        EXPECT_EQ(registry.get<AnimationController>(coin).currentAnimation, std::string("collect_burst"));
        EXPECT_TRUE(registry.get<AnimationController>(coin).playing);
    }
    fx.ReleaseAllKeys();
    tickWithAnimation(); // GameManager evaluates the win on its next Update()
    ASSERT_TRUE(frame < kFrameBudget);
    EXPECT_EQ(fx.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(1));

    // The win fanfare is queued once on the (Transform-less, so non-positional) GameManager.
    const auto* fanfare = registry.try_get<ScriptAudioCues>(manager);
    ASSERT_TRUE(fanfare != nullptr);
    EXPECT_EQ(fanfare->requested, 1u);
    ASSERT_EQ(fanfare->pending.size(), static_cast<size_t>(1));
    EXPECT_EQ(fanfare->pending[0].soundName, std::string("victory_fanfare"));
    EXPECT_FALSE(fanfare->pending[0].positional);

    // The patrol clip advanced by the system's delta every frame of the run.
    EXPECT_NEAR(patrol->currentTime, static_cast<float>(frame) * kFrameDelta, 0.01f);
    EXPECT_FALSE(fx.AnyScriptFaulted());
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
