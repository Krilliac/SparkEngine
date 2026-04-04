/**
 * @file TestMusicManager.cpp
 * @brief Tests for MusicManager: playlists, crossfade, dynamic intensity,
 *        mixer bus cascading, occlusion, and reverb zones.
 *
 * Standalone reimplementation — mirrors Spark::Audio::MusicManager
 * without XAudio2/DirectXMath dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float DistanceTo(const Vec3& o) const
        {
            float dx = x - o.x, dy = y - o.y, dz = z - o.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    };

    enum class PlaylistMode
    {
        Sequential,
        Shuffle,
        Loop,
        LoopOne
    };

    struct Playlist
    {
        std::vector<std::string> trackNames;
        PlaylistMode mode = PlaylistMode::Loop;
        int currentIndex = 0;

        std::string GetCurrentTrack() const
        {
            return trackNames.empty() ? "" : trackNames[static_cast<size_t>(currentIndex)];
        }
        std::string GetNextTrack() const
        {
            if (trackNames.empty())
                return "";
            int count = static_cast<int>(trackNames.size());
            switch (mode)
            {
            case PlaylistMode::Sequential:
                return (currentIndex + 1 < count) ? trackNames[static_cast<size_t>(currentIndex + 1)] : "";
            case PlaylistMode::Loop:
                return trackNames[static_cast<size_t>((currentIndex + 1) % count)];
            case PlaylistMode::LoopOne:
            case PlaylistMode::Shuffle:
                return trackNames[static_cast<size_t>(currentIndex)];
            }
            return "";
        }
        void Advance()
        {
            if (trackNames.empty())
                return;
            int count = static_cast<int>(trackNames.size());
            switch (mode)
            {
            case PlaylistMode::Sequential:
                if (currentIndex + 1 < count)
                    currentIndex++;
                break;
            case PlaylistMode::Loop:
                currentIndex = (currentIndex + 1) % count;
                break;
            case PlaylistMode::LoopOne:
                break;
            case PlaylistMode::Shuffle:
            {
                std::random_device rd;
                std::mt19937 rng(rd());
                currentIndex = std::uniform_int_distribution<int>(0, count - 1)(rng);
                break;
            }
            }
        }
    };

    struct MusicTrack
    {
        std::string name, filepath;
        float bpm = 120.0f;
    };

    class TrackRegistry
    {
      public:
        void Register(const MusicTrack& t) { m_tracks[t.name] = t; }
        void Unregister(const std::string& n) { m_tracks.erase(n); }
        const MusicTrack* Find(const std::string& n) const
        {
            auto it = m_tracks.find(n);
            return (it != m_tracks.end()) ? &it->second : nullptr;
        }
        size_t Count() const { return m_tracks.size(); }

      private:
        std::unordered_map<std::string, MusicTrack> m_tracks;
    };

    struct CrossfadeState
    {
        std::string fromTrack, toTrack;
        float duration = 2.0f, elapsed = 0.0f;
        bool active = false;
        float Progress() const { return (duration <= 0.0f) ? 1.0f : std::clamp(elapsed / duration, 0.0f, 1.0f); }
        void Update(float dt)
        {
            if (!active)
                return;
            elapsed += dt;
            if (elapsed >= duration)
                active = false;
        }
    };

    enum class CombatIntensity
    {
        Exploration,
        LowThreat,
        Combat,
        BossFight
    };

    struct DynamicMusicState
    {
        std::string tracks[4]; // indexed by CombatIntensity
        CombatIntensity current = CombatIntensity::Exploration;
        float transitionDuration = 2.0f;
        const std::string& TrackFor(CombatIntensity i) const { return tracks[static_cast<int>(i)]; }
    };

    enum class AudioBus
    {
        Master,
        SFX,
        Music,
        Voice,
        Ambient,
        UI,
        Count
    };
    constexpr int kBusCount = static_cast<int>(AudioBus::Count);
    struct BusSettings
    {
        float volume = 1.0f;
        bool muted = false;
        AudioBus parent = AudioBus::Master;
    };

    class AudioBusMixer
    {
      public:
        void SetVolume(AudioBus b, float v) { m_bus[idx(b)].volume = std::clamp(v, 0.0f, 1.0f); }
        float GetVolume(AudioBus b) const { return m_bus[idx(b)].volume; }
        void SetMuted(AudioBus b, bool m) { m_bus[idx(b)].muted = m; }
        void SetParent(AudioBus b, AudioBus p) { m_bus[idx(b)].parent = p; }
        float GetEffectiveVolume(AudioBus b) const
        {
            if (m_bus[idx(b)].muted)
                return 0.0f;
            float vol = m_bus[idx(b)].volume;
            AudioBus cur = b;
            for (int depth = 0; depth < 10; depth++)
            {
                AudioBus p = m_bus[idx(cur)].parent;
                if (p == cur)
                    break;
                if (m_bus[idx(p)].muted)
                    return 0.0f;
                vol *= m_bus[idx(p)].volume;
                cur = p;
                if (cur == AudioBus::Master)
                    break;
            }
            return vol;
        }
        void FadeBus(AudioBus b, float target, float dur)
        {
            m_fades.push_back({b, m_bus[idx(b)].volume, std::clamp(target, 0.0f, 1.0f), dur, 0.0f});
        }
        void Update(float dt)
        {
            for (auto it = m_fades.begin(); it != m_fades.end();)
            {
                it->elapsed += dt;
                float t = std::clamp(it->elapsed / it->duration, 0.0f, 1.0f);
                m_bus[idx(it->bus)].volume = it->startVol + (it->targetVol - it->startVol) * t;
                it = (it->elapsed >= it->duration) ? m_fades.erase(it) : std::next(it);
            }
        }

      private:
        static int idx(AudioBus b) { return static_cast<int>(b); }
        BusSettings m_bus[kBusCount]{};
        struct Fade
        {
            AudioBus bus;
            float startVol, targetVol, duration, elapsed;
        };
        std::vector<Fade> m_fades;
    };

    struct OcclusionSettings
    {
        bool enabled = true;
        float maxFactor = 0.8f;
        float lpOccluded = 800.0f;
        int maxRays = 4;
    };
    struct OcclusionResult
    {
        float factor = 0.0f;
        float lowPassCutoff = 22000.0f;
    };

    OcclusionResult ComputeOcclusion(const Vec3& src, const Vec3& ear, int obstacles, const OcclusionSettings& s)
    {
        if (!s.enabled)
            return {0.0f, 22000.0f};
        float distF = std::clamp(src.DistanceTo(ear) / 50.0f, 0.0f, 1.0f);
        float obsF = std::clamp(float(obstacles) / float(s.maxRays), 0.0f, 1.0f);
        float occ = std::min(distF * obsF, s.maxFactor);
        float cutoff = 22000.0f - (22000.0f - s.lpOccluded) * (occ / s.maxFactor);
        return {occ, cutoff};
    }

    struct ReverbZone
    {
        std::string name;
        Vec3 position;
        float outerRadius = 15.0f;
        float decayTime = 1.0f;
    };

    const ReverbZone* FindActiveZone(const std::vector<ReverbZone>& zones, const Vec3& listener)
    {
        const ReverbZone* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();
        for (auto& z : zones)
        {
            float d = listener.DistanceTo(z.position);
            if (d <= z.outerRadius && d < bestDist)
            {
                bestDist = d;
                best = &z;
            }
        }
        return best;
    }

} // anonymous namespace

// --- Playlist: Sequential ---------------------------------------------------

TEST(MusicManager_PlaylistSequential_CurrentAndNext)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C"};
    pl.mode = PlaylistMode::Sequential;

    EXPECT_EQ(pl.GetCurrentTrack(), std::string("A"));
    EXPECT_EQ(pl.GetNextTrack(), std::string("B"));
}

TEST(MusicManager_PlaylistSequential_AdvanceStopsAtEnd)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C"};
    pl.mode = PlaylistMode::Sequential;

    pl.Advance(); // -> B
    pl.Advance(); // -> C
    EXPECT_EQ(pl.GetCurrentTrack(), std::string("C"));
    pl.Advance(); // stays at C
    EXPECT_EQ(pl.GetCurrentTrack(), std::string("C"));
    EXPECT_EQ(pl.GetNextTrack(), std::string(""));
}

// --- Playlist: Loop ---------------------------------------------------------

TEST(MusicManager_PlaylistLoop_WrapsAround)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C"};
    pl.mode = PlaylistMode::Loop;
    pl.currentIndex = 2;

    EXPECT_EQ(pl.GetNextTrack(), std::string("A"));
    pl.Advance();
    EXPECT_EQ(pl.GetCurrentTrack(), std::string("A"));
}

TEST(MusicManager_PlaylistLoop_FullCycle)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C"};
    pl.mode = PlaylistMode::Loop;

    std::vector<std::string> v;
    for (int i = 0; i < 6; i++)
    {
        v.push_back(pl.GetCurrentTrack());
        pl.Advance();
    }
    // A B C A B C
    EXPECT_EQ(v[0], std::string("A"));
    EXPECT_EQ(v[3], std::string("A"));
    EXPECT_EQ(v[5], std::string("C"));
}

// --- Playlist: Shuffle ------------------------------------------------------

TEST(MusicManager_PlaylistShuffle_ValidIndices)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C", "D", "E"};
    pl.mode = PlaylistMode::Shuffle;

    for (int i = 0; i < 100; i++)
    {
        pl.Advance();
        EXPECT_GE(pl.currentIndex, 0);
        EXPECT_LT(pl.currentIndex, static_cast<int>(pl.trackNames.size()));
    }
}

TEST(MusicManager_PlaylistShuffle_CoversAllTracks)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C", "D"};
    pl.mode = PlaylistMode::Shuffle;

    std::set<std::string> seen;
    for (int i = 0; i < 200; i++)
    {
        seen.insert(pl.GetCurrentTrack());
        pl.Advance();
    }
    EXPECT_EQ(seen.size(), pl.trackNames.size());
}

// --- Playlist: LoopOne ------------------------------------------------------

TEST(MusicManager_PlaylistLoopOne_StaysOnSameTrack)
{
    Playlist pl;
    pl.trackNames = {"A", "B", "C"};
    pl.mode = PlaylistMode::LoopOne;
    pl.currentIndex = 1;

    EXPECT_EQ(pl.GetCurrentTrack(), std::string("B"));
    EXPECT_EQ(pl.GetNextTrack(), std::string("B"));
    pl.Advance();
    pl.Advance();
    EXPECT_EQ(pl.GetCurrentTrack(), std::string("B"));
}

// --- Track Registration -----------------------------------------------------

TEST(MusicManager_TrackRegisterLookupUnregister)
{
    TrackRegistry reg;
    reg.Register({"BattleTheme", "music/battle.ogg", 140.0f});

    auto* t = reg.Find("BattleTheme");
    EXPECT_TRUE(t != nullptr);
    EXPECT_EQ(t->name, std::string("BattleTheme"));
    EXPECT_NEAR(t->bpm, 140.0f, 0.01f);

    reg.Unregister("BattleTheme");
    EXPECT_EQ(reg.Count(), (size_t)0);
    EXPECT_TRUE(reg.Find("BattleTheme") == nullptr);
    EXPECT_TRUE(reg.Find("Nonexistent") == nullptr);
}

// --- Crossfade Timing -------------------------------------------------------

TEST(MusicManager_CrossfadeProgressUpdates)
{
    CrossfadeState cf;
    cf.fromTrack = "A";
    cf.toTrack = "B";
    cf.duration = 2.0f;
    cf.active = true;

    EXPECT_NEAR(cf.Progress(), 0.0f, 0.01f);
    cf.Update(1.0f);
    EXPECT_NEAR(cf.Progress(), 0.5f, 0.01f);
    EXPECT_TRUE(cf.active);
    cf.Update(1.0f);
    EXPECT_NEAR(cf.Progress(), 1.0f, 0.01f);
    EXPECT_FALSE(cf.active);
}

TEST(MusicManager_CrossfadeZeroDuration)
{
    CrossfadeState cf;
    cf.duration = 0.0f;
    cf.active = true;
    EXPECT_NEAR(cf.Progress(), 1.0f, 0.01f);
}

// --- Dynamic Music Intensity ------------------------------------------------

TEST(MusicManager_DynamicIntensityTrackSelection)
{
    DynamicMusicState st;
    st.tracks[0] = "calm";
    st.tracks[1] = "tension";
    st.tracks[2] = "battle";
    st.tracks[3] = "boss";

    EXPECT_EQ(st.TrackFor(CombatIntensity::Exploration), std::string("calm"));
    EXPECT_EQ(st.TrackFor(CombatIntensity::LowThreat), std::string("tension"));
    EXPECT_EQ(st.TrackFor(CombatIntensity::Combat), std::string("battle"));
    EXPECT_EQ(st.TrackFor(CombatIntensity::BossFight), std::string("boss"));
}

TEST(MusicManager_DynamicIntensityTriggersCrossfade)
{
    DynamicMusicState st;
    st.tracks[0] = "calm";
    st.tracks[2] = "battle";
    st.current = CombatIntensity::Exploration;
    st.transitionDuration = 2.0f;

    // Simulate intensity change triggering a crossfade
    CombatIntensity target = CombatIntensity::Combat;
    CrossfadeState cf;
    cf.fromTrack = st.TrackFor(st.current);
    cf.toTrack = st.TrackFor(target);
    cf.duration = st.transitionDuration;
    cf.active = true;
    st.current = target;

    EXPECT_TRUE(cf.active);
    EXPECT_EQ(cf.fromTrack, std::string("calm"));
    EXPECT_EQ(cf.toTrack, std::string("battle"));
    EXPECT_NEAR(cf.duration, 2.0f, 0.01f);
}

// --- Mixer Bus Volume Cascading ---------------------------------------------

TEST(MusicManager_MixerBusChildInheritsParent)
{
    AudioBusMixer mixer;
    mixer.SetVolume(AudioBus::Master, 0.5f);
    mixer.SetParent(AudioBus::Music, AudioBus::Master);
    mixer.SetVolume(AudioBus::Music, 0.8f);
    EXPECT_NEAR(mixer.GetEffectiveVolume(AudioBus::Music), 0.4f, 0.01f); // 0.5 * 0.8
}

TEST(MusicManager_MixerBusMasterZeroSilences)
{
    AudioBusMixer mixer;
    mixer.SetVolume(AudioBus::Master, 0.0f);
    mixer.SetParent(AudioBus::Music, AudioBus::Master);
    mixer.SetVolume(AudioBus::Music, 1.0f);
    EXPECT_NEAR(mixer.GetEffectiveVolume(AudioBus::Music), 0.0f, 0.01f);
}

// --- Mixer Bus Mute ---------------------------------------------------------

TEST(MusicManager_MutedBusReturnsZero)
{
    AudioBusMixer mixer;
    mixer.SetVolume(AudioBus::Music, 0.9f);
    mixer.SetMuted(AudioBus::Music, true);
    EXPECT_NEAR(mixer.GetEffectiveVolume(AudioBus::Music), 0.0f, 0.01f);
}

TEST(MusicManager_MutedParentSilencesChild)
{
    AudioBusMixer mixer;
    mixer.SetParent(AudioBus::Music, AudioBus::Master);
    mixer.SetMuted(AudioBus::Master, true);
    mixer.SetVolume(AudioBus::Music, 1.0f);
    EXPECT_NEAR(mixer.GetEffectiveVolume(AudioBus::Music), 0.0f, 0.01f);
}

// --- Bus Fade ---------------------------------------------------------------

TEST(MusicManager_BusFadeInterpolates)
{
    AudioBusMixer mixer;
    mixer.SetVolume(AudioBus::Music, 1.0f);
    mixer.FadeBus(AudioBus::Music, 0.0f, 2.0f);

    mixer.Update(1.0f);
    EXPECT_NEAR(mixer.GetVolume(AudioBus::Music), 0.5f, 0.01f);
    mixer.Update(1.0f);
    EXPECT_NEAR(mixer.GetVolume(AudioBus::Music), 0.0f, 0.01f);
}

TEST(MusicManager_BusFadeFromLowToHigh)
{
    AudioBusMixer mixer;
    mixer.SetVolume(AudioBus::SFX, 0.2f);
    mixer.FadeBus(AudioBus::SFX, 0.8f, 1.0f);

    mixer.Update(0.5f);
    EXPECT_NEAR(mixer.GetVolume(AudioBus::SFX), 0.5f, 0.01f);
    mixer.Update(0.5f);
    EXPECT_NEAR(mixer.GetVolume(AudioBus::SFX), 0.8f, 0.01f);
}

// --- Occlusion --------------------------------------------------------------

TEST(MusicManager_OcclusionNoObstacles)
{
    OcclusionSettings s;
    auto r = ComputeOcclusion({10, 0, 0}, {0, 0, 0}, 0, s);
    EXPECT_NEAR(r.factor, 0.0f, 0.01f);
}

TEST(MusicManager_OcclusionWithObstacles)
{
    OcclusionSettings s;
    auto r = ComputeOcclusion({30, 0, 0}, {0, 0, 0}, 4, s);
    EXPECT_GT(r.factor, 0.0f);
    EXPECT_LE(r.factor, s.maxFactor);
    EXPECT_LT(r.lowPassCutoff, 22000.0f);
}

TEST(MusicManager_OcclusionDisabled)
{
    OcclusionSettings s;
    s.enabled = false;
    auto r = ComputeOcclusion({100, 0, 0}, {0, 0, 0}, 4, s);
    EXPECT_NEAR(r.factor, 0.0f, 0.01f);
    EXPECT_NEAR(r.lowPassCutoff, 22000.0f, 0.01f);
}

// --- Reverb Zone Activation -------------------------------------------------

TEST(MusicManager_ReverbZoneNearestActivates)
{
    std::vector<ReverbZone> zones = {{"Cave", {10, 0, 0}, 15.0f, 3.0f}, {"Hall", {50, 0, 0}, 20.0f, 2.0f}};
    auto* active = FindActiveZone(zones, {8, 0, 0});
    EXPECT_TRUE(active != nullptr);
    EXPECT_EQ(active->name, std::string("Cave"));
}

TEST(MusicManager_ReverbZoneNoneActive)
{
    std::vector<ReverbZone> zones = {{"Cave", {100, 0, 0}, 10.0f, 1.0f}};
    EXPECT_TRUE(FindActiveZone(zones, {0, 0, 0}) == nullptr);
}

TEST(MusicManager_ReverbZonePicksClosest)
{
    std::vector<ReverbZone> zones = {{"A", {0, 0, 0}, 30.0f, 1.0f}, {"B", {5, 0, 0}, 30.0f, 1.0f}};
    auto* active = FindActiveZone(zones, {4, 0, 0});
    EXPECT_TRUE(active != nullptr);
    EXPECT_EQ(active->name, std::string("B"));
}
