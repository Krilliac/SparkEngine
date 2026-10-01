/**
 * @file TestENG200ScriptAudioAnimationReal.cpp
 * @brief ENG-200: script playSound()/playAnimation() drive the real audio and animation systems.
 *
 * Both bindings used to only write a debug log line. These tests run real
 * AngelScript through the production AngelScriptEngine bound to a World and
 * then tick the production ECS systems:
 *
 * - playSound() queues a ScriptAudioCues cue on the entity (positioned where
 *   the entity was when the script asked); Spark::ECS::AudioUpdateSystem hands
 *   it to the real AudioEngine and empties the queue. With an XAudio2 device
 *   the cue must come back as a live voice.
 * - playAnimation() switches the entity's AnimationController to the clip and
 *   Spark::ECS::AnimationUpdateSystem advances it; re-requesting the playing
 *   clip every frame does not restart it.
 *
 * Every test fails against the log-only bindings.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Audio/AudioEngine.h"
#include "Engine/ECS/Systems/ECSystems.h"
#include "Engine/Scripting/AngelScriptEngine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    /// A real, initialized AngelScriptEngine bound to a fresh World.
    struct ScriptMediaFixture
    {
        AngelScriptEngine engine;
        World world;
        bool ready = false;

        ScriptMediaFixture()
        {
            ready = engine.Initialize();
            AngelScriptEngine::BindWorld(&world);
        }

        ~ScriptMediaFixture()
        {
            AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
        }

        ScriptMediaFixture(const ScriptMediaFixture&) = delete;
        ScriptMediaFixture& operator=(const ScriptMediaFixture&) = delete;

        /// Compile @p source and attach @p className to @p entity; prints the diagnostic on failure.
        bool Attach(const char* source, const char* moduleName, EntityID entity, const char* className)
        {
            if (!engine.CompileScriptFromString(source, moduleName) ||
                !engine.AttachScript(entity, className, moduleName))
            {
                std::printf("  script diagnostic: %s\n", engine.GetLastError().c_str());
                return false;
            }
            return true;
        }
    };

    // Plays the pickup cue once, then moves itself away (as the shipped Collectible does).
    const char* const kPickupScript = "class Pickup\n"
                                      "{\n"
                                      "    bool collected = false;\n"
                                      "    void Update(float dt)\n"
                                      "    {\n"
                                      "        if (collected) return;\n"
                                      "        collected = true;\n"
                                      "        EntityID self = getEntityByName(\"Pickup\");\n"
                                      "        playSound(self, \"coin_pickup\");\n"
                                      "        setPosition(self, Vector3(0.0f, -100.0f, 0.0f));\n"
                                      "    }\n"
                                      "}\n";

    // Requests the walk loop every frame, like the shipped EnemyPatrol.
    const char* const kWalkerScript = "class Walker\n"
                                      "{\n"
                                      "    void Update(float dt)\n"
                                      "    {\n"
                                      "        playAnimation(getEntityByName(\"Walker\"), \"walk\");\n"
                                      "    }\n"
                                      "}\n";

#ifdef _WIN32
    /// Write 50 ms of 16-bit mono PCM silence as a WAV under the temp directory; "" on failure.
    std::wstring WriteSilentWav(const wchar_t* stem)
    {
        constexpr uint32_t kSampleRate = 44100;
        constexpr uint32_t kDataSize = (kSampleRate / 20) * 2;
        std::vector<unsigned char> wav;
        const auto push = [&wav](uint32_t value, int bytes)
        {
            for (int i = 0; i < bytes; ++i)
                wav.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
        };
        const auto tag = [&wav](const char* fourCC) { wav.insert(wav.end(), fourCC, fourCC + 4); };
        tag("RIFF");
        push(36u + kDataSize, 4);
        tag("WAVE");
        tag("fmt ");
        push(16u, 4);
        push(1u, 2); // PCM
        push(1u, 2); // mono
        push(kSampleRate, 4);
        push(kSampleRate * 2u, 4);
        push(2u, 2);
        push(16u, 2);
        tag("data");
        push(kDataSize, 4);
        wav.insert(wav.end(), kDataSize, static_cast<unsigned char>(0));

        std::error_code error;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(error) / "SparkENG200ScriptMedia";
        std::filesystem::create_directories(dir, error);
        if (error)
            return std::wstring();
        const std::filesystem::path file = dir / (std::wstring(stem) + L".wav");
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        return out ? file.wstring() : std::wstring();
    }
#endif // _WIN32
} // namespace

TEST(ScriptBindings_ENG200_MediaPlaySoundQueuesCueOnEntity)
{
    ScriptMediaFixture fx;
    ASSERT_TRUE(fx.ready);

    const EntityID pickup = fx.world.CreateEntity("Pickup");
    fx.world.AddComponent<Transform>(pickup).position = {4.0f, 1.0f, -2.0f};
    ASSERT_TRUE(fx.Attach(kPickupScript, "ENG200MediaPickup", pickup, "Pickup"));

    fx.engine.CallUpdate(pickup, 0.016f);
    fx.engine.CallUpdate(pickup, 0.016f); // collected: no second request
    EXPECT_FALSE(fx.engine.IsScriptFaulted(pickup));

    const auto* cues = fx.world.GetRegistry().try_get<ScriptAudioCues>(pickup);
    ASSERT_TRUE(cues != nullptr);
    EXPECT_EQ(cues->requested, 1u);
    ASSERT_EQ(cues->pending.size(), static_cast<size_t>(1));
    EXPECT_EQ(cues->pending[0].soundName, std::string("coin_pickup"));

    // The cue keeps the position the pickup had when the script asked, not where it moved afterwards.
    EXPECT_TRUE(cues->pending[0].positional);
    EXPECT_NEAR(cues->pending[0].position.x, 4.0f, 1e-6f);
    EXPECT_NEAR(cues->pending[0].position.y, 1.0f, 1e-6f);
    EXPECT_NEAR(fx.world.GetComponent<Transform>(pickup)->position.y, -100.0f, 1e-6f);

    // An entity without a Transform (the GameManager) gets a non-positional cue.
    const EntityID manager = fx.world.CreateEntity("Manager");
    ASPlaySound(manager, "victory_fanfare");
    const auto* managerCues = fx.world.GetRegistry().try_get<ScriptAudioCues>(manager);
    ASSERT_TRUE(managerCues != nullptr);
    ASSERT_EQ(managerCues->pending.size(), static_cast<size_t>(1));
    EXPECT_FALSE(managerCues->pending[0].positional);
}

TEST(ScriptBindings_ENG200_MediaPlaySoundRejectsBadInputAndBoundsQueue)
{
    ScriptMediaFixture fx;
    ASSERT_TRUE(fx.ready);
    auto& registry = fx.world.GetRegistry();

    // Invalid targets and names never create the queue component.
    const EntityID target = fx.world.CreateEntity("Target");
    const EntityID doomed = fx.world.CreateEntity("Doomed");
    fx.world.DestroyEntity(doomed);
    ASPlaySound(entt::null, "coin_pickup");
    ASPlaySound(doomed, "coin_pickup");
    ASPlaySound(target, "");
    ASPlaySound(target, std::string(200, 'a'));
    ASPlaySound(target, std::string("bad\nname"));
    EXPECT_TRUE(registry.try_get<ScriptAudioCues>(target) == nullptr);

    // Without a bound World the call is ignored too.
    AngelScriptEngine::BindWorld(nullptr);
    ASPlaySound(target, "coin_pickup");
    AngelScriptEngine::BindWorld(&fx.world);
    EXPECT_TRUE(registry.try_get<ScriptAudioCues>(target) == nullptr);

    // The longest accepted name is 128 characters.
    ASPlaySound(target, std::string(128, 'b'));
    ASSERT_TRUE(registry.try_get<ScriptAudioCues>(target) != nullptr);

    // The queue holds kMaxPending cues; the rest are counted, not stored.
    for (int i = 0; i < 19; ++i)
        ASPlaySound(target, "step");
    const auto& cues = registry.get<ScriptAudioCues>(target);
    EXPECT_EQ(cues.pending.size(), ScriptAudioCues::kMaxPending);
    EXPECT_EQ(cues.requested, 20u);
    EXPECT_EQ(cues.dropped, 4u);
}

TEST(ScriptBindings_ENG200_MediaAudioSystemDrainsCues)
{
    ScriptMediaFixture fx;
    ASSERT_TRUE(fx.ready);

    // Never initialized: no sound is registered, so every cue is refused. This path needs no device.
    AudioEngine silentAudio;
    Spark::ECS::AudioUpdateSystem silentSystem(&silentAudio);

    const EntityID pickup = fx.world.CreateEntity("Pickup");
    fx.world.AddComponent<Transform>(pickup).position = {1.0f, 0.0f, 0.0f};
    ASSERT_TRUE(fx.Attach(kPickupScript, "ENG200MediaDrain", pickup, "Pickup"));
    fx.engine.CallUpdate(pickup, 0.016f);
    ASPlaySound(pickup, "enemy_attack");

    silentSystem.Update(fx.world, 0.016f);
    const auto& cues = fx.world.GetRegistry().get<ScriptAudioCues>(pickup);
    EXPECT_TRUE(cues.pending.empty());
    EXPECT_EQ(cues.requested, 2u);
    EXPECT_EQ(cues.dropped, 2u);
    EXPECT_EQ(cues.played, 0u);

    // A drained queue is not replayed on the next tick.
    silentSystem.Update(fx.world, 0.016f);
    EXPECT_EQ(cues.dropped, 2u);
}

// The live-voice half needs a real audio backend (XAudio2, Windows only). It is a
// separate test so the device-free cue-drain checks above report as passed on
// every lane instead of being folded into this test's skip.
TEST(ScriptBindings_ENG200_MediaAudioCueStartsLiveVoice)
{
    ScriptMediaFixture fx;
    ASSERT_TRUE(fx.ready);

#ifdef _WIN32
    // With an XAudio2 device the same cue path must start a live voice at the cue position.
    AudioEngine audio;
    const bool requireAudioDevice = []
    {
        const char* value = std::getenv("SPARK_REQUIRE_AUDIO_DEVICE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const HRESULT audioResult = audio.Initialize(2);
    if (FAILED(audioResult))
    {
        if (requireAudioDevice)
        {
            EXPECT_TRUE(false);
            std::printf("Audio device required by SPARK_REQUIRE_AUDIO_DEVICE=1; Initialize failed: 0x%08lX\n",
                        static_cast<unsigned long>(audioResult));
            return;
        }

        std::printf("SKIP: XAudio2 device unavailable (set SPARK_REQUIRE_AUDIO_DEVICE=1 to make this a failure)\n");
        SKIP_TEST("no XAudio2 output device");
    }
    else
    {
        const std::wstring wav = WriteSilentWav(L"eng200_coin_pickup");
        ASSERT_TRUE(!wav.empty());
        ASSERT_TRUE(SUCCEEDED(audio.LoadSound("coin_pickup", wav)));
        Spark::ECS::AudioUpdateSystem system(&audio);

        const EntityID speaker = fx.world.CreateEntity("Speaker");
        fx.world.AddComponent<Transform>(speaker).position = {3.0f, 0.0f, 0.0f};
        auto& authored = fx.world.AddComponent<AudioSourceComponent>(speaker);
        authored.minDistance = 2.0f;
        authored.maxDistance = 30.0f;
        ASPlaySound(speaker, "coin_pickup");

        EXPECT_EQ(audio.GetActiveSourceCount(), static_cast<size_t>(0));
        system.Update(fx.world, 0.016f);
        const auto& played = fx.world.GetRegistry().get<ScriptAudioCues>(speaker);
        EXPECT_TRUE(played.pending.empty());
        EXPECT_EQ(played.played, 1u);
        EXPECT_EQ(played.dropped, 0u);
        // The only voice playing is the script cue (the authored component has no sound of its own).
        EXPECT_EQ(audio.GetActiveSourceCount(), static_cast<size_t>(1));

        audio.StopAllSounds();
        audio.Shutdown();
    }
#endif // _WIN32
#ifndef _WIN32
    const char* requireDevice = std::getenv("SPARK_REQUIRE_AUDIO_DEVICE");
    if (requireDevice != nullptr && std::strcmp(requireDevice, "1") == 0)
    {
        std::printf("Audio device required by SPARK_REQUIRE_AUDIO_DEVICE=1, but this lane has no XAudio2 backend\n");
        EXPECT_TRUE(false);
    }
    else
    {
        std::printf("SKIP: live audio voice assertion requires the Windows XAudio2 backend\n");
        SKIP_TEST("live audio voice assertion requires Windows XAudio2");
    }
#endif // !_WIN32
}

TEST(ScriptBindings_ENG200_MediaPlayAnimationDrivesController)
{
    ScriptMediaFixture fx;
    ASSERT_TRUE(fx.ready);

    const EntityID walker = fx.world.CreateEntity("Walker");
    fx.world.AddComponent<Transform>(walker);
    auto& controller = fx.world.AddComponent<AnimationController>(walker);
    controller.availableAnimations = {"idle", "walk", "attack_swing"};
    controller.currentAnimation = "idle";
    controller.currentTime = 0.7f;
    controller.normalizedTime = 0.35f;
    controller.duration = 2.0f;
    controller.playing = false;
    ASSERT_TRUE(fx.Attach(kWalkerScript, "ENG200MediaWalker", walker, "Walker"));

    fx.engine.CallUpdate(walker, 0.016f);
    EXPECT_FALSE(fx.engine.IsScriptFaulted(walker));
    EXPECT_EQ(controller.currentAnimation, std::string("walk"));
    EXPECT_TRUE(controller.playing);
    EXPECT_TRUE(controller.currentTime == 0.0f);
    EXPECT_TRUE(controller.normalizedTime == 0.0f);

    // The production animation system advances the clip the script selected.
    Spark::ECS::AnimationUpdateSystem animation;
    animation.Update(fx.world, 0.5f);
    EXPECT_NEAR(controller.currentTime, 0.5f, 1e-6f);
    EXPECT_NEAR(controller.normalizedTime, 0.25f, 1e-6f);

    // A different clip restarts playback.
    ASPlayAnimation(walker, "attack_swing");
    EXPECT_EQ(controller.currentAnimation, std::string("attack_swing"));
    EXPECT_TRUE(controller.currentTime == 0.0f);
}

TEST(ScriptBindings_ENG200_MediaPlayAnimationIsIdempotentAndValidated)
{
    ScriptMediaFixture fx;
    ASSERT_TRUE(fx.ready);

    const EntityID walker = fx.world.CreateEntity("Walker");
    fx.world.AddComponent<Transform>(walker);
    auto& controller = fx.world.AddComponent<AnimationController>(walker);
    controller.availableAnimations = {"walk"};
    ASSERT_TRUE(fx.Attach(kWalkerScript, "ENG200MediaIdempotent", walker, "Walker"));

    // The per-frame request selects the loop once and then leaves it running.
    Spark::ECS::AnimationUpdateSystem animation;
    fx.engine.CallUpdate(walker, 0.016f);
    animation.Update(fx.world, 0.25f);
    fx.engine.CallUpdate(walker, 0.016f);
    animation.Update(fx.world, 0.25f);
    fx.engine.CallUpdate(walker, 0.016f);
    EXPECT_EQ(controller.currentAnimation, std::string("walk"));
    EXPECT_NEAR(controller.currentTime, 0.5f, 1e-6f);

    // A clip the controller does not list, and invalid names, change nothing.
    ASPlayAnimation(walker, "attack_swing");
    ASPlayAnimation(walker, "");
    ASPlayAnimation(walker, std::string("walk\t"));
    EXPECT_EQ(controller.currentAnimation, std::string("walk"));
    EXPECT_NEAR(controller.currentTime, 0.5f, 1e-6f);

    // An entity without a controller is not given one.
    const EntityID statue = fx.world.CreateEntity("Statue");
    ASPlayAnimation(statue, "walk");
    ASPlayAnimation(entt::null, "walk");
    EXPECT_TRUE(fx.world.GetRegistry().try_get<AnimationController>(statue) == nullptr);
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
