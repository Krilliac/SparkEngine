/**
 * @file TestAudioECSBindingReal.cpp
 * @brief AudioSourceComponent <-> AudioEngine binding through the production
 *        Spark::ECS::AudioUpdateSystem (physics-audio-input-camera-03).
 *
 * Before this wiring nothing ever wrote AudioSourceComponent::audioSourceHandle,
 * so the system skipped every entity. These tests drive the real system against
 * the real AudioEngine: the handle-hygiene cases need no audio device, and the
 * playback case skips cleanly when XAudio2 cannot be initialized.
 */

#include "TestFramework.h"

#include "Audio/AudioEngine.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/ECSystems.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    /** @brief Build a minimal 16-bit PCM WAV holding 50 ms of silence. */
    std::vector<unsigned char> MakeSilentWav(uint32_t sampleRate, uint16_t channels)
    {
        const uint16_t bitsPerSample = 16;
        const uint16_t blockAlign = static_cast<uint16_t>(channels * bitsPerSample / 8);
        const uint32_t byteRate = sampleRate * blockAlign;
        const uint32_t dataSize = (sampleRate / 20) * blockAlign;

        std::vector<unsigned char> wav;
        const auto pushU32 = [&wav](uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                wav.push_back(static_cast<unsigned char>((value >> shift) & 0xFFu));
            }
        };
        const auto pushU16 = [&wav](uint16_t value)
        {
            wav.push_back(static_cast<unsigned char>(value & 0xFFu));
            wav.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
        };
        const auto pushTag = [&wav](const char* tag)
        {
            for (int i = 0; i < 4; ++i)
            {
                wav.push_back(static_cast<unsigned char>(tag[i]));
            }
        };

        pushTag("RIFF");
        pushU32(36u + dataSize);
        pushTag("WAVE");
        pushTag("fmt ");
        pushU32(16u);
        pushU16(1u); // PCM
        pushU16(channels);
        pushU32(sampleRate);
        pushU32(byteRate);
        pushU16(blockAlign);
        pushU16(bitsPerSample);
        pushTag("data");
        pushU32(dataSize);
        wav.insert(wav.end(), dataSize, static_cast<unsigned char>(0));
        return wav;
    }

    /** @brief Write a silent WAV under the temp directory; returns "" on failure. */
    std::wstring WriteSilentWav(const std::wstring& stem)
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "SparkAudioECSBindingReal";
        if (ec)
        {
            return std::wstring();
        }
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / (stem + L".wav");

        const std::vector<unsigned char> wav = MakeSilentWav(44100, 1);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return std::wstring();
        }
        out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        out.close();
        return file.wstring();
    }
} // namespace

// ============================================================================
// Handle hygiene (no audio device required)
// ============================================================================

TEST(AudioECSBinding_StaleHandleIsDroppedBeforeUse)
{
    AudioEngine engine; // Never initialized: the source pool is empty, so no handle can be live.
    World world;
    Spark::ECS::AudioUpdateSystem system(&engine);

    AudioSource stale;
    stale.IsPlaying = true;
    stale.Generation = 3;

    const EntityID entity = world.CreateEntity("stale_audio");
    auto& transform = world.AddComponent<Transform>(entity);
    transform.position = {4.0f, 0.0f, 0.0f};
    auto& audio = world.AddComponent<AudioSourceComponent>(entity);
    audio.is3D = true;
    audio.isPlaying = true;
    audio.audioSourceHandle = Spark::AudioHandle(&stale);
    audio.audioSourceGeneration = 3;

    system.Update(world, 1.0f / 60.0f);

    // A handle the engine does not recognise as live is dropped, not dereferenced.
    // The bound generation stays as the "already played" marker.
    EXPECT_FALSE(audio.audioSourceHandle.IsValid());
    EXPECT_EQ(audio.audioSourceGeneration, 3u);
    EXPECT_FALSE(audio.isPlaying);
    EXPECT_TRUE(stale.Position.x == 0.0f); // The recycled source was never written through.
}

TEST(AudioECSBinding_PlayOnAwakeWithUnknownSoundStaysUnbound)
{
    AudioEngine engine;
    World world;
    Spark::ECS::AudioUpdateSystem system(&engine);

    const EntityID entity = world.CreateEntity("unknown_sound");
    world.AddComponent<Transform>(entity);
    auto& audio = world.AddComponent<AudioSourceComponent>(entity);
    audio.soundName = "wiring_no_such_sound";
    audio.playOnAwake = true;
    audio.is3D = true;

    system.Update(world, 1.0f / 60.0f);
    system.Update(world, 1.0f / 60.0f);

    // No registered sound: the bind attempt fails honestly and leaves the
    // component unbound instead of faking a playing state.
    EXPECT_FALSE(audio.audioSourceHandle.IsValid());
    EXPECT_FALSE(audio.isPlaying);
}

// ============================================================================
// Real playback binding (skips without an audio device)
// ============================================================================

TEST(AudioECSBinding_PlayOnAwakeBindsAuthoredComponentToLiveSource)
{
#ifndef _WIN32
    // Live-source binding rides on XAudio2 voices. Linux/macOS build the XAudio2
    // shim over miniaudio (or no-op stubs): Initialize() succeeds there but
    // LoadSound fails (observed on macOS CI), so the binding cannot be exercised.
    SKIP_TEST("XAudio2 voice semantics are Windows-only; the non-Windows audio shim cannot load sounds");
#endif

    AudioEngine engine;
    if (FAILED(engine.Initialize(2)))
    {
        SKIP_TEST("No audio device available for XAudio2 in this environment");
    }

    const std::wstring path = WriteSilentWav(L"spark_audio_ecs_binding_cue");
    if (path.empty())
    {
        engine.Shutdown();
        SKIP_TEST("Could not write a temporary WAV file");
    }
    ASSERT_TRUE(SUCCEEDED(engine.LoadSound("wiring_cue", path)));

    World world;
    Spark::ECS::AudioUpdateSystem system(&engine);

    const EntityID entity = world.CreateEntity("authored_audio");
    auto& transform = world.AddComponent<Transform>(entity);
    transform.position = {3.0f, 0.0f, 0.0f};
    auto& audio = world.AddComponent<AudioSourceComponent>(entity);
    audio.soundName = "wiring_cue";
    audio.playOnAwake = true;
    audio.is3D = true;
    audio.loop = true;
    audio.minDistance = 2.0f;
    audio.maxDistance = 40.0f;

    system.Update(world, 1.0f / 60.0f);

    // The authored component is bound to a live pooled source that carries the
    // component's spatial settings and starting position.
    ASSERT_TRUE(audio.audioSourceHandle.IsValid());
    EXPECT_TRUE(audio.isPlaying);
    auto* source = audio.audioSourceHandle.As<AudioSource>();
    ASSERT_TRUE(source != nullptr);
    EXPECT_TRUE(engine.IsSourceLive(source, audio.audioSourceGeneration));
    EXPECT_TRUE(source->Is3D);
    EXPECT_TRUE(source->MinDistance == 2.0f);
    EXPECT_TRUE(source->MaxDistance == 40.0f);
    EXPECT_TRUE(source->Position.x == 3.0f);

    // Moving the entity moves the source and yields a finite Doppler velocity.
    transform.position = {3.5f, 0.0f, 0.0f};
    system.Update(world, 0.5f);
    EXPECT_TRUE(source->Position.x == 3.5f);
    EXPECT_NEAR(source->Velocity.x, 1.0f, 1e-3f);

    // Stopping the source recycles it: the next tick drops the stale handle and,
    // because the component already played once, does not restart the cue.
    const uint32_t boundGeneration = audio.audioSourceGeneration;
    engine.StopSound(source);
    system.Update(world, 1.0f / 60.0f);
    EXPECT_FALSE(engine.IsSourceLive(source, boundGeneration));
    EXPECT_FALSE(audio.audioSourceHandle.IsValid());
    EXPECT_FALSE(audio.isPlaying);
    EXPECT_EQ(audio.audioSourceGeneration, boundGeneration);

    engine.StopAllSounds();
    engine.Shutdown();
}
