// TestAudioEngine.cpp - Tests for audio engine state management
// Standalone implementations for CI testing (no XAudio2 dependency)

#include "TestFramework.h"
#include "TestCommonMath.h"
#include <cstdint>
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

    using TestMath::Vec3;

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

    struct PoolSlot
    {
        bool inUse = false;
        uint32_t sourceID = 0;
    };

    class AudioEngine
    {
      public:
        bool Initialize(size_t maxSources)
        {
            if (m_initialized)
                return false;
            m_maxSources = maxSources;
            m_pool.resize(maxSources);
            for (size_t i = 0; i < maxSources; ++i)
            {
                m_pool[i].inUse = false;
                m_pool[i].sourceID = static_cast<uint32_t>(i);
            }
            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            StopAllSources();
            m_sounds.clear();
            m_pool.clear();
            m_maxSources = 0;
            m_initialized = false;
        }

        bool IsInitialized() const { return m_initialized; }

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
            m.activeSources = GetActiveSourceCount();
            m.totalSoundsLoaded = static_cast<int>(m_sounds.size());
            m.masterVolume = m_masterVolume;
            m.sfxVolume = m_sfxVolume;
            m.musicVolume = m_musicVolume;
            m.voiceVolume = m_voiceVolume;
            return m;
        }

        // --- Object pool API ---

        /// Acquire a source from the pool. Returns -1 if pool is exhausted.
        int AcquireSource()
        {
            for (auto& slot : m_pool)
            {
                if (!slot.inUse)
                {
                    slot.inUse = true;
                    return static_cast<int>(slot.sourceID);
                }
            }
            return -1;
        }

        /// Release a source back into the pool for reuse.
        bool ReleaseSource(int sourceID)
        {
            if (sourceID < 0 || sourceID >= static_cast<int>(m_pool.size()))
                return false;
            m_pool[sourceID].inUse = false;
            return true;
        }

        void StopAllSources()
        {
            for (auto& slot : m_pool)
                slot.inUse = false;
        }

        int GetActiveSourceCount() const
        {
            int count = 0;
            for (const auto& slot : m_pool)
            {
                if (slot.inUse)
                    count++;
            }
            return count;
        }

        size_t GetMaxSources() const { return m_maxSources; }

      private:
        bool m_initialized = false;
        float m_masterVolume = 1.0f;
        float m_sfxVolume = 0.8f;
        float m_musicVolume = 0.6f;
        float m_voiceVolume = 1.0f;
        size_t m_maxSources = 0;
        ListenerState m_listener;
        std::unordered_map<std::string, SoundEntry> m_sounds;
        std::vector<PoolSlot> m_pool;
    };

} // namespace TestAudio

// =============================================================================
// Tests — Initialization and Shutdown
// =============================================================================

TEST(Audio_InitializeAndShutdown)
{
    TestAudio::AudioEngine audio;
    EXPECT_FALSE(audio.IsInitialized());

    EXPECT_TRUE(audio.Initialize(32));
    EXPECT_TRUE(audio.IsInitialized());
    EXPECT_EQ(audio.GetMaxSources(), (size_t)32);

    // Double-init should fail
    EXPECT_FALSE(audio.Initialize(16));

    audio.Shutdown();
    EXPECT_FALSE(audio.IsInitialized());
    EXPECT_EQ(audio.GetMaxSources(), (size_t)0);
    EXPECT_EQ(audio.GetActiveSourceCount(), 0);
}

TEST(Audio_ShutdownClearsLoadedSounds)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(8);
    audio.RegisterSound("explosion", 1.5f);
    audio.RegisterSound("footstep", 0.3f);
    EXPECT_EQ(audio.GetSoundCount(), (size_t)2);

    audio.Shutdown();
    EXPECT_EQ(audio.GetSoundCount(), (size_t)0);
}

TEST(Audio_ReinitializeAfterShutdown)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(16);
    audio.Shutdown();

    // Should be able to re-initialize after shutdown
    EXPECT_TRUE(audio.Initialize(8));
    EXPECT_TRUE(audio.IsInitialized());
    EXPECT_EQ(audio.GetMaxSources(), (size_t)8);
    audio.Shutdown();
}

// =============================================================================
// Tests — Volume Controls
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

TEST(Audio_AllVolumeChannels)
{
    TestAudio::AudioEngine audio;

    audio.SetSFXVolume(0.3f);
    EXPECT_NEAR(audio.GetSFXVolume(), 0.3f, 0.001f);

    audio.SetMusicVolume(0.0f);
    EXPECT_NEAR(audio.GetMusicVolume(), 0.0f, 0.001f);

    audio.SetVoiceVolume(0.9f);
    EXPECT_NEAR(audio.GetVoiceVolume(), 0.9f, 0.001f);

    // Clamping on all channels
    audio.SetSFXVolume(-5.0f);
    EXPECT_NEAR(audio.GetSFXVolume(), 0.0f, 0.001f);

    audio.SetMusicVolume(100.0f);
    EXPECT_NEAR(audio.GetMusicVolume(), 1.0f, 0.001f);
}

TEST(Audio_EffectiveVolume)
{
    TestAudio::AudioEngine audio;
    audio.SetMasterVolume(0.5f);
    audio.SetSFXVolume(0.5f);

    float effective = audio.GetEffectiveVolume(1.0f);
    EXPECT_NEAR(effective, 0.25f, 0.001f);

    // Zero master means zero effective
    audio.SetMasterVolume(0.0f);
    EXPECT_NEAR(audio.GetEffectiveVolume(1.0f), 0.0f, 0.001f);
}

// =============================================================================
// Tests — 3D Audio / Listener
// =============================================================================

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

    // Negative clamps to zero
    audio.SetDopplerScale(-1.0f);
    EXPECT_GE(audio.GetListener().dopplerScale, 0.0f);
}

TEST(Audio_DistanceScale)
{
    TestAudio::AudioEngine audio;
    audio.SetDistanceScale(5.0f);
    EXPECT_NEAR(audio.GetListener().distanceScale, 5.0f, 0.001f);

    // Very small values clamp to minimum
    audio.SetDistanceScale(-10.0f);
    EXPECT_GT(audio.GetListener().distanceScale, 0.0f);
}

// =============================================================================
// Tests — Sound Loading
// =============================================================================

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

// =============================================================================
// Tests — Object Pool Behavior
// =============================================================================

TEST(Audio_PoolAcquireAndRelease)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(4);
    EXPECT_EQ(audio.GetActiveSourceCount(), 0);

    int id1 = audio.AcquireSource();
    int id2 = audio.AcquireSource();
    EXPECT_GE(id1, 0);
    EXPECT_GE(id2, 0);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(audio.GetActiveSourceCount(), 2);

    // Release one and verify count decreases
    EXPECT_TRUE(audio.ReleaseSource(id1));
    EXPECT_EQ(audio.GetActiveSourceCount(), 1);

    // Re-acquire should reuse the released slot
    int id3 = audio.AcquireSource();
    EXPECT_GE(id3, 0);
    EXPECT_EQ(audio.GetActiveSourceCount(), 2);

    audio.Shutdown();
}

TEST(Audio_PoolExhaustion)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(3);

    // Exhaust the pool
    int id0 = audio.AcquireSource();
    int id1 = audio.AcquireSource();
    int id2 = audio.AcquireSource();
    EXPECT_GE(id0, 0);
    EXPECT_GE(id1, 0);
    EXPECT_GE(id2, 0);
    EXPECT_EQ(audio.GetActiveSourceCount(), 3);

    // Pool is full — acquire should fail
    int idFail = audio.AcquireSource();
    EXPECT_EQ(idFail, -1);
    EXPECT_EQ(audio.GetActiveSourceCount(), 3);

    audio.Shutdown();
}

TEST(Audio_StopAllReleasesPool)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(4);

    audio.AcquireSource();
    audio.AcquireSource();
    audio.AcquireSource();
    EXPECT_EQ(audio.GetActiveSourceCount(), 3);

    audio.StopAllSources();
    EXPECT_EQ(audio.GetActiveSourceCount(), 0);

    // All slots should be available again
    int id = audio.AcquireSource();
    EXPECT_GE(id, 0);

    audio.Shutdown();
}

TEST(Audio_ReleaseInvalidSourceID)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(4);

    EXPECT_FALSE(audio.ReleaseSource(-1));
    EXPECT_FALSE(audio.ReleaseSource(100));
    EXPECT_EQ(audio.GetActiveSourceCount(), 0);

    audio.Shutdown();
}

// =============================================================================
// Tests — Metrics
// =============================================================================

TEST(Audio_Metrics)
{
    TestAudio::AudioEngine audio;
    audio.Initialize(8);
    audio.SetMasterVolume(0.7f);
    audio.RegisterSound("test", 1.0f);
    audio.AcquireSource();

    auto m = audio.GetMetrics();
    EXPECT_NEAR(m.masterVolume, 0.7f, 0.001f);
    EXPECT_EQ(m.totalSoundsLoaded, 1);
    EXPECT_EQ(m.activeSources, 1);

    audio.Shutdown();
}
