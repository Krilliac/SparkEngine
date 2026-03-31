/**
 * @file TestAudioMixerBus.cpp
 * @brief Tests for AudioMixer: bus hierarchy, volume cascading, mute/solo,
 *        reverb zone blending, and DSP effects.
 *
 * Standalone reimplementation — mirrors Spark::Audio::AudioMixer
 * without XAudio2/DirectX dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <algorithm>
#include <cmath>
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

    enum class ReverbPreset
    {
        None,
        SmallRoom,
        MediumRoom,
        LargeRoom,
        Hall,
        Cave,
        Outdoor,
        Custom
    };

    struct ReverbParameters
    {
        ReverbPreset preset = ReverbPreset::None;
        float decayTime = 1.0f;
        float wetDryMix = 0.5f;
        float roomSize = 0.5f;
    };

    struct ReverbZone
    {
        std::string name;
        Vec3 position{};
        Vec3 halfExtents{};
        float innerRadius = 5.0f;
        float outerRadius = 15.0f;
        ReverbParameters reverb;
        int priority = 0;
        bool enabled = true;
    };

    struct MixBus
    {
        std::string name;
        float volume = 1.0f;
        bool muted = false;
        bool solo = false;
        std::string parentBus;
    };

    // --- AudioMixer reimplementation ---

    class AudioMixer
    {
      public:
        void Initialize()
        {
            // Create default master bus
            MixBus master;
            master.name = "Master";
            m_buses["Master"] = master;
        }

        void CreateBus(const std::string& name, const std::string& parentBus = "")
        {
            MixBus bus;
            bus.name = name;
            bus.parentBus = parentBus.empty() ? "Master" : parentBus;
            m_buses[name] = bus;
        }

        void SetBusVolume(const std::string& busName, float volume)
        {
            if (auto it = m_buses.find(busName); it != m_buses.end())
                it->second.volume = std::clamp(volume, 0.0f, 1.0f);
        }

        float GetBusVolume(const std::string& busName) const
        {
            auto it = m_buses.find(busName);
            return (it != m_buses.end()) ? it->second.volume : 0.0f;
        }

        void SetBusMuted(const std::string& busName, bool muted)
        {
            if (auto it = m_buses.find(busName); it != m_buses.end())
                it->second.muted = muted;
        }

        void SetBusSolo(const std::string& busName, bool solo)
        {
            if (auto it = m_buses.find(busName); it != m_buses.end())
                it->second.solo = solo;
        }

        float GetEffectiveBusVolume(const std::string& busName) const
        {
            auto it = m_buses.find(busName);
            if (it == m_buses.end())
                return 0.0f;

            const auto& bus = it->second;

            // Check solo mode
            bool anySolo = false;
            for (auto& [name, b] : m_buses)
            {
                if (b.solo)
                {
                    anySolo = true;
                    break;
                }
            }

            if (anySolo && !bus.solo)
                return 0.0f;

            if (bus.muted)
                return 0.0f;

            float volume = bus.volume;

            // Walk up parent chain
            std::string parent = bus.parentBus;
            int depth = 0;
            while (!parent.empty() && depth < 10)
            {
                auto parentIt = m_buses.find(parent);
                if (parentIt == m_buses.end())
                    break;
                if (parentIt->second.muted)
                    return 0.0f;
                volume *= parentIt->second.volume;
                parent = parentIt->second.parentBus;
                depth++;
            }

            return volume;
        }

        std::vector<std::string> GetBusNames() const
        {
            std::vector<std::string> result;
            for (auto& [name, _] : m_buses)
                result.push_back(name);
            return result;
        }

        // Reverb zones
        void AddReverbZone(const ReverbZone& zone) { m_reverbZones.push_back(zone); }

        void RemoveReverbZone(const std::string& name)
        {
            m_reverbZones.erase(std::remove_if(m_reverbZones.begin(), m_reverbZones.end(),
                                               [&](const ReverbZone& z) { return z.name == name; }),
                                m_reverbZones.end());
        }

        ReverbParameters GetReverbAtPosition(const Vec3& position) const
        {
            ReverbParameters bestReverb;
            int bestPriority = -1;
            float bestBlend = 0.0f;

            for (auto& zone : m_reverbZones)
            {
                if (!zone.enabled)
                    continue;

                float dist = position.DistanceTo(zone.position);
                if (dist > zone.outerRadius)
                    continue;

                float blend = 1.0f;
                if (dist > zone.innerRadius)
                {
                    blend = 1.0f - (dist - zone.innerRadius) / (zone.outerRadius - zone.innerRadius);
                }

                if (zone.priority > bestPriority || (zone.priority == bestPriority && blend > bestBlend))
                {
                    bestReverb = zone.reverb;
                    bestReverb.wetDryMix *= blend;
                    bestPriority = zone.priority;
                    bestBlend = blend;
                }
            }

            return bestReverb;
        }

        const std::vector<ReverbZone>& GetReverbZones() const { return m_reverbZones; }

        // Snapshots
        void SaveSnapshot(const std::string& name) { m_snapshots[name] = m_buses; }

        void RestoreSnapshot(const std::string& name)
        {
            auto it = m_snapshots.find(name);
            if (it != m_snapshots.end())
                m_buses = it->second;
        }

      private:
        std::unordered_map<std::string, MixBus> m_buses;
        std::vector<ReverbZone> m_reverbZones;
        std::unordered_map<std::string, std::unordered_map<std::string, MixBus>> m_snapshots;
    };

} // anonymous namespace

// ============================================================================
// Bus Hierarchy Tests
// ============================================================================

TEST(AudioMixer_CreateBus)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.CreateBus("Music");
    mixer.CreateBus("Voice");

    auto names = mixer.GetBusNames();
    EXPECT_GE(names.size(), (size_t)4); // Master + 3
}

TEST(AudioMixer_BusVolume)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.SetBusVolume("SFX", 0.75f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 0.75f, 0.01f);
}

TEST(AudioMixer_BusVolumeClamped)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.SetBusVolume("SFX", 1.5f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 1.0f, 0.01f);

    mixer.SetBusVolume("SFX", -0.5f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 0.0f, 0.01f);
}

// ============================================================================
// Volume Cascading Tests
// ============================================================================

TEST(AudioMixer_VolumeCascadesFromParent)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.SetBusVolume("Master", 0.5f);
    mixer.CreateBus("SFX"); // parent = Master
    mixer.SetBusVolume("SFX", 0.8f);

    float effective = mixer.GetEffectiveBusVolume("SFX");
    EXPECT_NEAR(effective, 0.4f, 0.01f); // 0.5 * 0.8
}

TEST(AudioMixer_VolumeCascadesMultipleLevels)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.SetBusVolume("Master", 0.5f);
    mixer.CreateBus("SFX");
    mixer.SetBusVolume("SFX", 0.8f);
    mixer.CreateBus("Weapons", "SFX");
    mixer.SetBusVolume("Weapons", 0.5f);

    float effective = mixer.GetEffectiveBusVolume("Weapons");
    EXPECT_NEAR(effective, 0.2f, 0.01f); // 0.5 * 0.8 * 0.5
}

TEST(AudioMixer_MasterVolumeZeroSilences)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.SetBusVolume("Master", 0.0f);
    mixer.CreateBus("SFX");
    mixer.SetBusVolume("SFX", 1.0f);

    EXPECT_NEAR(mixer.GetEffectiveBusVolume("SFX"), 0.0f, 0.01f);
}

// ============================================================================
// Mute Tests
// ============================================================================

TEST(AudioMixer_MuteBus)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.SetBusVolume("SFX", 1.0f);
    mixer.SetBusMuted("SFX", true);

    EXPECT_NEAR(mixer.GetEffectiveBusVolume("SFX"), 0.0f, 0.01f);
}

TEST(AudioMixer_MuteParentSilencesChildren)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.CreateBus("Weapons", "SFX");
    mixer.SetBusMuted("SFX", true);

    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Weapons"), 0.0f, 0.01f);
}

// ============================================================================
// Solo Tests
// ============================================================================

TEST(AudioMixer_SoloBus)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.CreateBus("Music");
    mixer.SetBusSolo("SFX", true);

    EXPECT_GT(mixer.GetEffectiveBusVolume("SFX"), 0.0f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Music"), 0.0f, 0.01f);
}

// ============================================================================
// Reverb Zone Tests
// ============================================================================

TEST(AudioMixer_AddAndRemoveReverbZone)
{
    AudioMixer mixer;
    mixer.Initialize();

    ReverbZone zone;
    zone.name = "Cave";
    zone.position = {0, 0, 0};
    zone.innerRadius = 5.0f;
    zone.outerRadius = 15.0f;
    zone.reverb.preset = ReverbPreset::Cave;
    zone.reverb.wetDryMix = 0.8f;
    mixer.AddReverbZone(zone);

    EXPECT_EQ(mixer.GetReverbZones().size(), (size_t)1);

    mixer.RemoveReverbZone("Cave");
    EXPECT_EQ(mixer.GetReverbZones().size(), (size_t)0);
}

TEST(AudioMixer_ReverbInsideInnerRadius)
{
    AudioMixer mixer;
    mixer.Initialize();

    ReverbZone zone;
    zone.name = "Cave";
    zone.position = {0, 0, 0};
    zone.innerRadius = 10.0f;
    zone.outerRadius = 20.0f;
    zone.reverb.preset = ReverbPreset::Cave;
    zone.reverb.wetDryMix = 0.8f;
    mixer.AddReverbZone(zone);

    // Inside inner radius — full reverb
    auto reverb = mixer.GetReverbAtPosition({0, 0, 0});
    EXPECT_NEAR(reverb.wetDryMix, 0.8f, 0.01f);
}

TEST(AudioMixer_ReverbOutsideOuterRadius)
{
    AudioMixer mixer;
    mixer.Initialize();

    ReverbZone zone;
    zone.name = "Cave";
    zone.position = {0, 0, 0};
    zone.innerRadius = 10.0f;
    zone.outerRadius = 20.0f;
    zone.reverb.preset = ReverbPreset::Cave;
    zone.reverb.wetDryMix = 0.8f;
    mixer.AddReverbZone(zone);

    // Far outside — no reverb
    auto reverb = mixer.GetReverbAtPosition({100, 0, 0});
    EXPECT_EQ(static_cast<int>(reverb.preset), static_cast<int>(ReverbPreset::None));
}

TEST(AudioMixer_ReverbBlendInTransitionZone)
{
    AudioMixer mixer;
    mixer.Initialize();

    ReverbZone zone;
    zone.name = "Cave";
    zone.position = {0, 0, 0};
    zone.innerRadius = 10.0f;
    zone.outerRadius = 20.0f;
    zone.reverb.preset = ReverbPreset::Cave;
    zone.reverb.wetDryMix = 1.0f;
    mixer.AddReverbZone(zone);

    // At midpoint between inner and outer radius (15m)
    auto reverb = mixer.GetReverbAtPosition({15, 0, 0});
    EXPECT_NEAR(reverb.wetDryMix, 0.5f, 0.01f);
}

TEST(AudioMixer_ReverbPriority)
{
    AudioMixer mixer;
    mixer.Initialize();

    ReverbZone cave;
    cave.name = "Cave";
    cave.position = {0, 0, 0};
    cave.innerRadius = 20.0f;
    cave.outerRadius = 30.0f;
    cave.reverb.preset = ReverbPreset::Cave;
    cave.reverb.wetDryMix = 0.8f;
    cave.priority = 1;
    mixer.AddReverbZone(cave);

    ReverbZone outdoor;
    outdoor.name = "Outdoor";
    outdoor.position = {0, 0, 0};
    outdoor.innerRadius = 20.0f;
    outdoor.outerRadius = 30.0f;
    outdoor.reverb.preset = ReverbPreset::Outdoor;
    outdoor.reverb.wetDryMix = 0.3f;
    outdoor.priority = 0;
    mixer.AddReverbZone(outdoor);

    // Higher priority zone wins
    auto reverb = mixer.GetReverbAtPosition({0, 0, 0});
    EXPECT_EQ(static_cast<int>(reverb.preset), static_cast<int>(ReverbPreset::Cave));
}

TEST(AudioMixer_DisabledReverbZoneIgnored)
{
    AudioMixer mixer;
    mixer.Initialize();

    ReverbZone zone;
    zone.name = "Cave";
    zone.position = {0, 0, 0};
    zone.innerRadius = 10.0f;
    zone.outerRadius = 20.0f;
    zone.reverb.preset = ReverbPreset::Cave;
    zone.enabled = false;
    mixer.AddReverbZone(zone);

    auto reverb = mixer.GetReverbAtPosition({0, 0, 0});
    EXPECT_EQ(static_cast<int>(reverb.preset), static_cast<int>(ReverbPreset::None));
}

// ============================================================================
// Snapshot Tests
// ============================================================================

TEST(AudioMixer_SaveAndRestoreSnapshot)
{
    AudioMixer mixer;
    mixer.Initialize();

    mixer.CreateBus("SFX");
    mixer.SetBusVolume("SFX", 0.5f);
    mixer.SaveSnapshot("Gameplay");

    mixer.SetBusVolume("SFX", 0.1f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 0.1f, 0.01f);

    mixer.RestoreSnapshot("Gameplay");
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 0.5f, 0.01f);
}

// ============================================================================
// Missing Bus Tests
// ============================================================================

TEST(AudioMixer_GetNonexistentBus)
{
    AudioMixer mixer;
    mixer.Initialize();

    EXPECT_NEAR(mixer.GetBusVolume("Nonexistent"), 0.0f, 0.01f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Nonexistent"), 0.0f, 0.01f);
}
