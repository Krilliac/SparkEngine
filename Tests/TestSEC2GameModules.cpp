/**
 * @file TestSEC2GameModules.cpp
 * @brief SEC2 game-module security fixes: bounded console inputs, fog-of-war work bounds,
 *        owner-scoped network handlers across module hot reload, and server-attributed chat.
 *
 * Each SEC2GM_ test pins one fix from the SEC2-game-modules lane:
 * - ProgressionSystem::AwardXP saturates instead of reaching float-to-int or signed-overflow UB.
 * - WaveComposition bounds every wave to MAX_ENEMIES_PER_WAVE, heavies included.
 * - NetworkManager handler ownership: a hot-reload replacement keeps its handlers when the outgoing
 *   image tears down, and everything an image owns is removed before it is unmapped.
 * - MMO chat relays only routable channels and never forwards a client-chosen sender name.
 * - RTS fog of war clips vision to the grid and restored saves reject absurd vision ranges.
 *
 * The CTest registration SEC2GameModules pins the family size, which depends on the
 * ImGui (module sources) and networking features compiled into SparkTests.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameFPS/Source/Game/ProgressionSystem.h"
#include "../GameModules/SparkGameFPS/Source/Game/WaveComposition.h"

#include <climits>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// FPS progression (xp console command)
// ---------------------------------------------------------------------------

TEST(SEC2GM_ProgressionAwardSaturatesInsteadOfOverflowing)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    int observedBase = -1;
    int observedModified = -1;
    progression.GetCallbacks().onXPAwarded = [&](int base, const std::string&, int modified)
    {
        observedBase = base;
        observedModified = modified;
    };

    // float(INT_MAX) rounds up to 2^31; the old cast back to int was undefined and produced INT_MIN,
    // leaving a negative XP total that was then persisted.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(observedBase, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(observedModified, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetLevel(), progression.GetMaxLevel());

    // Awards stop at max level, so repetition cannot push the total toward INT_MAX either.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);

    // Ordinary gameplay awards are unchanged.
    Spark::ProgressionSystem ordinary;
    ordinary.Initialize();
    ordinary.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");
    EXPECT_EQ(ordinary.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
}

TEST(SEC2GM_ProgressionIgnoresNonPositiveAwards)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    progression.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");

    progression.AwardXP(0, "console");
    progression.AwardXP(-500, "console");
    progression.AwardXP(INT_MIN, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
    EXPECT_EQ(progression.GetLevel(), 1);
}

// ---------------------------------------------------------------------------
// FPS wave composition (wave_skip / wave_difficulty console commands)
// ---------------------------------------------------------------------------

TEST(SEC2GM_WaveCompositionNeverExceedsCap)
{
    using namespace Spark::WaveComposition;
    const float scales[] = {
        1.0f, 3.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -5.0f, 1000.0f};
    std::vector<int> waves = {600000, INT_MAX, INT_MAX - 1, INT_MIN, -1, 0};
    for (int wave = 1; wave <= MAX_WAVE_NUMBER; ++wave)
        waves.push_back(wave);

    for (const int wave : waves)
    {
        for (const float scale : scales)
        {
            const Spark::WaveDefinition definition = Compose(wave, scale);
            const int total = definition.TotalEnemies();
            EXPECT_GT(total, 0);
            EXPECT_LE(total, MAX_ENEMIES_PER_WAVE);
            EXPECT_GE(definition.waveNumber, 1);
            EXPECT_LE(definition.waveNumber, MAX_WAVE_NUMBER);
            if (definition.isBossWave)
                EXPECT_GE(definition.heavyCount, 1);
            EXPECT_TRUE(std::isfinite(definition.healthMultiplier));
            EXPECT_TRUE(std::isfinite(definition.damageMultiplier));
            EXPECT_TRUE(std::isfinite(definition.speedMultiplier));
        }
    }
}

TEST(SEC2GM_WaveCompositionClampsInputs)
{
    using namespace Spark::WaveComposition;

    // wave_skip 600000 used to produce a boss wave of 120,001 unscaled heavies.
    const Spark::WaveDefinition huge = Compose(600000, 1.0f);
    EXPECT_EQ(huge.waveNumber, MAX_WAVE_NUMBER);
    EXPECT_LE(huge.heavyCount, MAX_ENEMIES_PER_WAVE);

    // Ordinary early waves are unchanged by the cap.
    const Spark::WaveDefinition first = Compose(1, 1.0f);
    EXPECT_EQ(first.gruntCount, 3);
    EXPECT_EQ(first.TotalEnemies(), 3);
    const Spark::WaveDefinition boss = Compose(5, 1.0f);
    EXPECT_TRUE(boss.isBossWave);
    EXPECT_EQ(boss.heavyCount, 2);

    EXPECT_EQ(ClampWaveNumber(INT_MIN), 1);
    EXPECT_EQ(ClampWaveNumber(INT_MAX), MAX_WAVE_NUMBER);
    EXPECT_TRUE(IsValidDifficultyScale(1.0f));
    EXPECT_TRUE(IsValidDifficultyScale(3.0f));
    EXPECT_FALSE(IsValidDifficultyScale(0.5f));
    EXPECT_FALSE(IsValidDifficultyScale(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(IsValidDifficultyScale(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(SanitizeDifficultyScale(std::numeric_limits<float>::quiet_NaN()), 1.0f);
    EXPECT_EQ(SanitizeDifficultyScale(50.0f), MAX_DIFFICULTY_SCALE);
}

// ---------------------------------------------------------------------------
// NetworkManager handler ownership across module hot reload
// ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/NetworkManager.h"

namespace
{
    using SEC2NetworkManager = Spark::Net::NetworkManager;

    Spark::Net::MessageType SEC2TestMessageType(uint16_t offset)
    {
        return static_cast<Spark::Net::MessageType>(
            static_cast<uint16_t>(static_cast<uint16_t>(Spark::Net::MessageType::UserDefined) + 200u + offset));
    }

    /// A handler that keeps @p token alive exactly as long as NetworkManager keeps the handler.
    SEC2NetworkManager::MessageHandler SEC2TokenHandler(std::shared_ptr<int> token)
    {
        return [held = std::move(token)](const Spark::Net::NetworkMessage&) { (void)held; };
    }
} // namespace

TEST(SEC2GM_NetworkHotReloadKeepsReplacementHandlers)
{
    auto& network = SEC2NetworkManager::GetInstance();
    const Spark::Net::MessageType type = SEC2TestMessageType(1);
    const std::string outgoingOwner = "SEC2GM_Outgoing#1";
    const std::string replacementOwner = "SEC2GM_Replacement#2";

    auto outgoingToken = std::make_shared<int>(1);
    const std::weak_ptr<int> outgoingAlive = outgoingToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner);
        network.RegisterHandler(type, SEC2TokenHandler(std::move(outgoingToken)));
    }
    EXPECT_FALSE(outgoingAlive.expired());

    // ModuleManager::ReloadModule initializes the replacement first; its handler takes over the slot,
    // destroying the outgoing callback while the outgoing image is still mapped.
    auto replacementToken = std::make_shared<int>(2);
    const std::weak_ptr<int> replacementAlive = replacementToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, replacementOwner);
        network.RegisterHandler(type, SEC2TokenHandler(std::move(replacementToken)));
    }
    EXPECT_TRUE(outgoingAlive.expired());
    EXPECT_FALSE(replacementAlive.expired());

    // Then the outgoing image tears down. The old MMO code overwrote the slot with an empty lambda compiled
    // into the outgoing image; none of these writes may touch the replacement's handler.
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner, true);
        network.RegisterHandler(type, [](const Spark::Net::NetworkMessage&) {});
        network.RegisterSensitiveHandler(type, [](const Spark::Net::NetworkMessage&) {});
        network.UnregisterHandler(type);
        network.ClearHandlers();
    }
    EXPECT_FALSE(replacementAlive.expired());
    EXPECT_EQ(network.UnregisterHandlersByOwner(outgoingOwner), static_cast<size_t>(0));
    EXPECT_FALSE(replacementAlive.expired());

    // Unloading the replacement removes what it owns.
    EXPECT_EQ(network.UnregisterHandlersByOwner(replacementOwner), static_cast<size_t>(1));
    EXPECT_TRUE(replacementAlive.expired());
}

TEST(SEC2GM_NetworkUnloadRemovesEveryOwnedCallback)
{
    auto& network = SEC2NetworkManager::GetInstance();
    const Spark::Net::MessageType hostType = SEC2TestMessageType(2);
    const Spark::Net::MessageType moduleType = SEC2TestMessageType(3);
    const Spark::Net::MessageType teardownType = SEC2TestMessageType(4);
    const std::string moduleOwner = "SEC2GM_Module#3";

    auto hostToken = std::make_shared<int>(1);
    const std::weak_ptr<int> hostAlive = hostToken;
    network.RegisterHandler(hostType, SEC2TokenHandler(std::move(hostToken)));

    auto moduleToken = std::make_shared<int>(2);
    const std::weak_ptr<int> moduleAlive = moduleToken;
    auto timeoutToken = std::make_shared<int>(3);
    const std::weak_ptr<int> timeoutAlive = timeoutToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, moduleOwner);
        network.RegisterHandler(moduleType, SEC2TokenHandler(std::move(moduleToken)));
        network.SetTimeoutHandler([held = std::move(timeoutToken)](Spark::Net::ClientID) { (void)held; });
    }

    // A teardown that installs its own placeholder (the old pattern) still produces an owned slot.
    auto teardownToken = std::make_shared<int>(4);
    const std::weak_ptr<int> teardownAlive = teardownToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, moduleOwner, true);
        network.RegisterHandler(teardownType, SEC2TokenHandler(std::move(teardownToken)));
    }

    // Before the image is unmapped every callback it owns is destroyed; host handlers survive.
    EXPECT_EQ(network.UnregisterHandlersByOwner(moduleOwner), static_cast<size_t>(3));
    EXPECT_TRUE(moduleAlive.expired());
    EXPECT_TRUE(timeoutAlive.expired());
    EXPECT_TRUE(teardownAlive.expired());
    EXPECT_FALSE(hostAlive.expired());

    // Host code outside any scope keeps unrestricted removal.
    network.UnregisterHandler(hostType);
    EXPECT_TRUE(hostAlive.expired());
}

#endif // ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Module sources compiled only with ImGui (RTS fog of war, MMO chat)
// ---------------------------------------------------------------------------

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSPersistence.h"
#include "../GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.h"
#include "../GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.h"
#include "../GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.h"
#include "../GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.h"
#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"

TEST(SEC2GM_RTSFogVisionIsClippedToTheGrid)
{
    RTS::RTSFogOfWarSystem fog;
    ASSERT_TRUE(fog.Initialize(nullptr, 128, 128));

    // A restored visionRange of 1e9 used to mean ~4e18 loop iterations (and UB in the int arithmetic);
    // clipped to the grid it is at most 128 * 128 cells and reveals the whole map.
    fog.UpdateVision(RTS::RTSFaction::Human, 64.0f, 64.0f, 1.0e9f);
    EXPECT_EQ(fog.GetExploredPercent(RTS::RTSFaction::Human), 100.0f);
    fog.HideArea(RTS::RTSFaction::Human, 64.0f, 64.0f, std::numeric_limits<float>::max());
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Human, 10.0f, 10.0f));

    // Non-finite ranges and positions far off the grid reveal nothing and are defined behaviour.
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, std::numeric_limits<float>::quiet_NaN());
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, std::numeric_limits<float>::infinity());
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, -3.0f);
    fog.UpdateVision(RTS::RTSFaction::Sentinel, std::numeric_limits<float>::quiet_NaN(), 64.0f, 5.0f);
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 1.0e30f, -1.0e30f, 5.0f);
    EXPECT_EQ(fog.GetExploredPercent(RTS::RTSFaction::Sentinel), 0.0f);

    // An ordinary unit still reveals exactly its disc.
    fog.UpdateVision(RTS::RTSFaction::Swarm, 10.0f, 10.0f, 8.0f);
    EXPECT_TRUE(fog.IsVisible(RTS::RTSFaction::Swarm, 18.0f, 10.0f));
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Swarm, 19.0f, 10.0f));
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Swarm, 17.0f, 17.0f));

    // Grids larger than the persisted bound are refused up front.
    RTS::RTSFogOfWarSystem oversized;
    EXPECT_FALSE(oversized.Initialize(nullptr, RTS::RTSFogOfWarSystem::MAX_MAP_DIMENSION + 1, 16));
}

TEST(SEC2GM_RTSRestoreRejectsUnboundedVisionRange)
{
    RTS::UnitData unit;
    unit.unitId = 1;
    unit.visionRange = 10000.0f;
    RTS::RTSUnitSystem units;
    EXPECT_FALSE(units.RestoreState({unit}, 2));
    unit.visionRange = RTS::RTSUnitSystem::MAX_VISION_RANGE;
    EXPECT_TRUE(units.RestoreState({unit}, 2));

    // The save decoder's validator applies the same bound to a captured snapshot.
    RTS::RTSUnitSystem liveUnits;
    RTS::RTSBuildingSystem buildings;
    RTS::RTSResourceSystem resources;
    RTS::RTSCommandSystem commands;
    RTS::RTSFogOfWarSystem fog;
    RTS::RTSMatchSystem match;
    RTS::RTSSkirmishSimulation simulation;
    const RTS::RTSSkirmishSystems systems{&liveUnits, &buildings, &resources, &commands, &fog, &match};
    simulation.Initialize(nullptr, systems);
    simulation.StartDefaultSkirmish();

    RTS::RTSPersistenceSnapshot snapshot = RTS::RTSPersistence::Capture(systems, simulation);
    ASSERT_FALSE(snapshot.units.empty());
    std::string error;
    EXPECT_TRUE(RTS::RTSPersistence::Validate(snapshot, error));
    snapshot.units.front().visionRange = 10000.0f;
    EXPECT_FALSE(RTS::RTSPersistence::Validate(snapshot, error));
    EXPECT_FALSE(error.empty());
}

#ifdef ENABLE_NETWORKING

#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "Spark/IEngineContext.h"

namespace
{
    /// Context exposing only the engine NetworkManager, as the MMO module sees it.
    class SEC2NetworkContext final : public Spark::IEngineContext
    {
      public:
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
        Spark::Net::NetworkManager* GetNetwork() override { return &Spark::Net::NetworkManager::GetInstance(); }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
    };
} // namespace

TEST(SEC2GM_MMOChatHotReloadTeardownKeepsReplacementHandler)
{
    auto& network = Spark::Net::NetworkManager::GetInstance();
    SEC2NetworkContext context;
    const std::string outgoingOwner = "SEC2GM_MMOOutgoing#4";
    const std::string replacementOwner = "SEC2GM_MMOReplacement#5";

    MMO::MMOChatSystem outgoing;
    {
        Spark::Net::NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner);
        ASSERT_TRUE(outgoing.Initialize(&context));
    }
    MMO::MMOChatSystem replacement;
    {
        Spark::Net::NetworkManager::ScopedRegistrationOwner scope(network, replacementOwner);
        ASSERT_TRUE(replacement.Initialize(&context));
    }

    // Reload order: the replacement is live before the outgoing module's OnUnload runs.
    {
        Spark::Net::NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner, true);
        outgoing.Shutdown();
    }

    // The outgoing image owns nothing any more, and the chat slot still belongs to the replacement.
    EXPECT_EQ(network.UnregisterHandlersByOwner(outgoingOwner), static_cast<size_t>(0));
    EXPECT_EQ(network.UnregisterHandlersByOwner(replacementOwner), static_cast<size_t>(1));
    replacement.Shutdown();
}

#endif // ENABLE_NETWORKING
#endif // SPARK_TEST_HAS_IMGUI
