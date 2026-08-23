/**
 * @file TestSequencerAudioWiring.cpp
 * @brief Production-linked Sequencer-to-audio-service contract tests.
 */

#include "TestFramework.h"
#include "Audio/IAudioBackend.h"
#include "Engine/Cinematic/Sequencer.h"

#include <string>
#include <thread>
#include <vector>

namespace
{
    struct AudioCall
    {
        std::string name;
        float volume = 0.0f;
        bool is3D = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::thread::id thread;
    };

    class RecordingAudioBackend final : public Spark::Audio::IAudioBackend
    {
      public:
        bool Initialize(size_t) override { return true; }
        void Shutdown() override {}
        void Update(float) override {}
        bool LoadSound(const std::string&, const std::string&) override { return true; }
        void UnloadSound(const std::string&) override {}

        uint32_t PlaySound(const std::string& name, float volume, float, bool) override
        {
            calls.push_back({name, volume, false, 0.0f, 0.0f, 0.0f, std::this_thread::get_id()});
            return static_cast<uint32_t>(calls.size());
        }

        uint32_t PlaySound3D(const std::string& name, float x, float y, float z, float volume, float, bool) override
        {
            calls.push_back({name, volume, true, x, y, z, std::this_thread::get_id()});
            return static_cast<uint32_t>(calls.size());
        }

        void StopSound(uint32_t) override {}
        void StopAllSounds() override {}
        void PauseAllSounds() override {}
        void ResumeAllSounds() override {}
        void SetMasterVolume(float volume) override { masterVolume = volume; }
        void SetSFXVolume(float volume) override { sfxVolume = volume; }
        void SetMusicVolume(float volume) override { musicVolume = volume; }
        float GetMasterVolume() const override { return masterVolume; }
        float GetSFXVolume() const override { return sfxVolume; }
        float GetMusicVolume() const override { return musicVolume; }
        void SetListenerPosition(float, float, float) override {}
        void SetListenerOrientation(float, float, float, float, float, float) override {}
        void SetListenerVelocity(float, float, float) override {}
        void Set3DEnabled(bool) override {}
        bool IsAvailable() const override { return true; }

        std::vector<AudioCall> calls;
        float masterVolume = 1.0f;
        float sfxVolume = 1.0f;
        float musicVolume = 1.0f;
    };

    Spark::Cinematic::Sequence* FreshSequence(const char* name)
    {
        auto& manager = Spark::Cinematic::SequencerManager::GetInstance();
        manager.StopAll();
        manager.RemoveSequence(name);
        return manager.CreateSequence(name);
    }

    void Cleanup(const char* name)
    {
        auto& manager = Spark::Cinematic::SequencerManager::GetInstance();
        manager.StopAll();
        manager.RemoveSequence(name);
        manager.SetAudioBackend(nullptr);
    }
} // namespace

TEST(SequencerAudio_WiredPlaybackDispatchesOnUpdateCallerExactlyOnce)
{
    constexpr const char* name = "AudioWiring_Playback";
    auto& manager = Spark::Cinematic::SequencerManager::GetInstance();
    RecordingAudioBackend backend;
    manager.SetAudioBackend(&backend);

    auto* sequence = FreshSequence(name);
    auto* track = sequence->AddAudioCueTrack("SFX");
    track->AddCue({0.5f, "cinematic_boom", 0.7f, true, {1.0f, 2.0f, 3.0f}});

    int observerCount = 0;
    std::thread::id observerThread;
    sequence->SetAudioCallback(
        [&](const Spark::Cinematic::AudioCue&)
        {
            ++observerCount;
            observerThread = std::this_thread::get_id();
        });
    sequence->Play();

    std::thread worker([&] { manager.Update(0.75f); });
    const std::thread::id workerThread = worker.get_id();
    worker.join();

    EXPECT_EQ(observerCount, 1);
    EXPECT_TRUE(observerThread == workerThread);
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(0));

    const std::thread::id updateThread = std::this_thread::get_id();
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(1));
    EXPECT_TRUE(backend.calls[0].name == "cinematic_boom");
    EXPECT_TRUE(backend.calls[0].is3D);
    EXPECT_TRUE(backend.calls[0].volume == 0.7f);
    EXPECT_TRUE(backend.calls[0].x == 1.0f && backend.calls[0].y == 2.0f && backend.calls[0].z == 3.0f);
    EXPECT_TRUE(backend.calls[0].thread == updateThread);

    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(1));
    Cleanup(name);
}

TEST(SequencerAudio_NoServiceConsumesCuesWithoutDeferredReplay)
{
    constexpr const char* name = "AudioWiring_Headless";
    auto& manager = Spark::Cinematic::SequencerManager::GetInstance();
    manager.SetAudioBackend(nullptr);

    auto* sequence = FreshSequence(name);
    auto* track = sequence->AddAudioCueTrack("SFX");
    track->AddCue({0.25f, "headless_safe", 1.0f, false, {0.0f, 0.0f, 0.0f}});
    sequence->Play();
    manager.Update(0.5f);
    manager.DispatchPendingAudioCues();

    RecordingAudioBackend backend;
    manager.SetAudioBackend(&backend);
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(0));

    sequence->Stop();
    sequence->Play();
    manager.Update(0.5f);
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(1));
    Cleanup(name);
}

TEST(SequencerAudio_SeekIsSilentButRewindAndReplayCanRetrigger)
{
    constexpr const char* name = "AudioWiring_SeekReplay";
    auto& manager = Spark::Cinematic::SequencerManager::GetInstance();
    RecordingAudioBackend backend;
    manager.SetAudioBackend(&backend);

    auto* sequence = FreshSequence(name);
    auto* track = sequence->AddAudioCueTrack("SFX");
    track->AddCue({1.0f, "timeline_hit", 1.0f, false, {0.0f, 0.0f, 0.0f}});
    track->AddCue({5.0f, "timeline_tail", 1.0f, false, {0.0f, 0.0f, 0.0f}});

    sequence->Play();
    manager.Update(1.1f);
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(1));

    manager.Update(0.1f);
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(1));

    sequence->SetTime(2.0f); // forward scrub does not synthesize missed cues
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(1));

    sequence->SetTime(0.5f); // rewind makes a future crossing eligible again
    sequence->Pause();
    sequence->Play();
    manager.Update(0.6f);
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(2));

    sequence->Stop();
    sequence->Play();
    manager.Update(1.1f);
    manager.DispatchPendingAudioCues();
    manager.DispatchPendingAudioCues();
    EXPECT_EQ(backend.calls.size(), static_cast<size_t>(3));
    EXPECT_TRUE(backend.calls[0].name == "timeline_hit" && backend.calls[1].name == "timeline_hit" &&
                backend.calls[2].name == "timeline_hit");
    Cleanup(name);
}
