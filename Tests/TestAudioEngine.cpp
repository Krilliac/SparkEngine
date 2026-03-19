// TestAudioEngine.cpp - Tests for audio engine state management
// Standalone implementations for CI testing (no XAudio2 dependency)

#include "TestFramework.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace TestAudio
{

    struct AudioMetrics
    {
        int activeSources = 0;
        int totalSoundsLoaded = 0;
        float masterVolume = 1.0f;
        float sfxVolume = 1.0f;
        float musicVolume = 1.0f;
        float voiceVolume = 1.0f;
    };

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct ListenerState
    {
        Vec3 position;
        Vec3 forward;
        Vec3 up;
        float dopplerScale = 1.0f;
        float distanceScale = 1.0f;
    };

    struct SoundEntry
    {
        std::string name;
        bool loaded = false;
        float duration = 0.0f;
    };

    class AudioEngine
    {
      public:
        void SetMasterVolume(float v) { m_masterVolume = std::clamp(v, 0.0f, 1.0f); }
        void SetSFXVolume(float v) { m_sfxVolume = std::clamp(v, 0.0f, 1.0f); }
        void SetMusicVolume(float v) { m_musicVolume = std::clamp(v, 0.0f, 1.0f); }
        void SetVoiceVolume(float v) { m_voiceVolume = std::clamp(v, 0.0f, 1.0f); }

        float GetMasterVolume() const { return m_masterVolume; }
        float GetSFXVolume() const { return m_sfxVolume; }
        float GetMusicVolume() const { return m_musicVolume; }
        float GetVoiceVolume() const { return m_voiceVolume; }

        float GetEffectiveVolume(float sourceVol) const { return sourceVol * m_masterVolume * m_sfxVolume; }

        void SetListenerPosition(float x, float y, float z) { m_listener.position = {x, y, z}; }

        void SetListenerOrientation(float fx, float fy, float fz, float ux, float uy, float uz)
        {
            m_listener.forward = {fx, fy, fz};
            m_listener.up = {ux, uy, uz};
        }

        void SetDopplerScale(float s) { m_listener.dopplerScale = std::max(0.0f, s); }
        void SetDistanceScale(float s) { m_listener.distanceScale = std::max(0.001f, s); }

        const ListenerState& GetListener() const { return m_listener; }

        bool RegisterSound(const std::string& name, float duration)
        {
            if (name.empty() || m_sounds.count(name))
                return false;
            m_sounds[name] = {name, true, duration};
            return true;
        }

        void UnregisterSound(const std::string& name) { m_sounds.erase(name); }

        bool HasSound(const std::string& name) const { return m_sounds.count(name) > 0; }

        size_t GetSoundCount() const { return m_sounds.size(); }

        AudioMetrics GetMetrics() const
        {
            AudioMetrics m;
            m.activeSources = m_activeSources;
            m.totalSoundsLoaded = static_cast<int>(m_sounds.size());
            m.masterVolume = m_masterVolume;
            m.sfxVolume = m_sfxVolume;
            m.musicVolume = m_musicVolume;
            m.voiceVolume = m_voiceVolume;
            return m;
        }

        void SimulatePlaySource() { m_activeSources++; }
        void SimulateStopSource()
        {
            if (m_activeSources > 0)
                m_activeSources--;
        }
        void StopAllSources() { m_activeSources = 0; }
        int GetActiveSourceCount() const { return m_activeSources; }

      private:
        float m_masterVolume = 1.0f;
        float m_sfxVolume = 0.8f;
        float m_musicVolume = 0.6f;
        float m_voiceVolume = 1.0f;
        ListenerState m_listener;
        std::unordered_map<std::string, SoundEntry> m_sounds;
        int m_activeSources = 0;
    };

} // namespace TestAudio

// =============================================================================
// Tests
// =============================================================================

TEST(Audio_DefaultVolumes)
{
    TestAudio::AudioEngine audio;
    EXPECT_NEAR(audio.GetMasterVolume(), 1.0f, 0.001f);
    EXPECT_NEAR(audio.GetSFXVolume(), 0.8f, 0.001f);
    EXPECT_NEAR(audio.GetMusicVolume(), 0.6f, 0.001f);
    EXPECT_NEAR(audio.GetVoiceVolume(), 1.0f, 0.001f);
}

TEST(Audio_SetVolumeClamped)
{
    TestAudio::AudioEngine audio;

    audio.SetMasterVolume(0.5f);
    EXPECT_NEAR(audio.GetMasterVolume(), 0.5f, 0.001f);

    audio.SetMasterVolume(-1.0f);
    EXPECT_NEAR(audio.GetMasterVolume(), 0.0f, 0.001f);

    audio.SetMasterVolume(5.0f);
    EXPECT_NEAR(audio.GetMasterVolume(), 1.0f, 0.001f);
}

TEST(Audio_EffectiveVolume)
{
    TestAudio::AudioEngine audio;
    audio.SetMasterVolume(0.5f);
    audio.SetSFXVolume(0.5f);

    float effective = audio.GetEffectiveVolume(1.0f);
    EXPECT_NEAR(effective, 0.25f, 0.001f);
}

TEST(Audio_ListenerPosition)
{
    TestAudio::AudioEngine audio;
    audio.SetListenerPosition(10.0f, 5.0f, -3.0f);

    auto& listener = audio.GetListener();
    EXPECT_NEAR(listener.position.x, 10.0f, 0.001f);
    EXPECT_NEAR(listener.position.y, 5.0f, 0.001f);
    EXPECT_NEAR(listener.position.z, -3.0f, 0.001f);
}

TEST(Audio_ListenerOrientation)
{
    TestAudio::AudioEngine audio;
    audio.SetListenerOrientation(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);

    auto& listener = audio.GetListener();
    EXPECT_NEAR(listener.forward.z, 1.0f, 0.001f);
    EXPECT_NEAR(listener.up.y, 1.0f, 0.001f);
}

TEST(Audio_DopplerScale)
{
    TestAudio::AudioEngine audio;
    audio.SetDopplerScale(2.0f);
    EXPECT_NEAR(audio.GetListener().dopplerScale, 2.0f, 0.001f);

    audio.SetDopplerScale(-1.0f);
    EXPECT_GE(audio.GetListener().dopplerScale, 0.0f);
}

TEST(Audio_SoundRegistration)
{
    TestAudio::AudioEngine audio;

    EXPECT_TRUE(audio.RegisterSound("explosion", 1.5f));
    EXPECT_TRUE(audio.HasSound("explosion"));
    EXPECT_EQ(audio.GetSoundCount(), (size_t)1);

    // Duplicate registration should fail
    EXPECT_FALSE(audio.RegisterSound("explosion", 2.0f));
    EXPECT_EQ(audio.GetSoundCount(), (size_t)1);

    // Empty name should fail
    EXPECT_FALSE(audio.RegisterSound("", 1.0f));
}

TEST(Audio_SoundUnregister)
{
    TestAudio::AudioEngine audio;
    audio.RegisterSound("footstep", 0.3f);
    audio.RegisterSound("gunshot", 0.5f);

    EXPECT_EQ(audio.GetSoundCount(), (size_t)2);

    audio.UnregisterSound("footstep");
    EXPECT_FALSE(audio.HasSound("footstep"));
    EXPECT_EQ(audio.GetSoundCount(), (size_t)1);
}

TEST(Audio_ActiveSources)
{
    TestAudio::AudioEngine audio;
    EXPECT_EQ(audio.GetActiveSourceCount(), 0);

    audio.SimulatePlaySource();
    audio.SimulatePlaySource();
    audio.SimulatePlaySource();
    EXPECT_EQ(audio.GetActiveSourceCount(), 3);

    audio.SimulateStopSource();
    EXPECT_EQ(audio.GetActiveSourceCount(), 2);

    audio.StopAllSources();
    EXPECT_EQ(audio.GetActiveSourceCount(), 0);
}

TEST(Audio_Metrics)
{
    TestAudio::AudioEngine audio;
    audio.SetMasterVolume(0.7f);
    audio.RegisterSound("test", 1.0f);
    audio.SimulatePlaySource();

    auto m = audio.GetMetrics();
    EXPECT_NEAR(m.masterVolume, 0.7f, 0.001f);
    EXPECT_EQ(m.totalSoundsLoaded, 1);
    EXPECT_EQ(m.activeSources, 1);
}
