/**
 * @file TestAudioMixerBus.cpp
 * @brief Production AudioMixer coverage: hierarchy, routing, reverb, occlusion, and DSP state.
 */

#include "TestFramework.h"


#include "Audio/AudioMixer.h"

#include <algorithm>
#include <string>
#include <utility>

namespace
{
    Spark::Audio::ReverbZone MakeProductionReverbZone(std::string name, int priority = 0)
    {
        Spark::Audio::ReverbZone zone;
        zone.name = std::move(name);
        zone.position = {0.0f, 0.0f, 0.0f};
        zone.halfExtents = {20.0f, 20.0f, 20.0f};
        zone.innerRadius = 5.0f;
        zone.outerRadius = 20.0f;
        zone.reverb.preset = Spark::Audio::ReverbPreset::Cave;
        zone.reverb.decayTime = 3.0f;
        zone.reverb.wetDryMix = 0.8f;
        zone.priority = priority;
        return zone;
    }
} // namespace

TEST(AudioMixer_ProductionBusHierarchyAndVolumeRouting)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();

    const auto names = mixer.GetBusNames();
    for (const std::string expected : {"Master", "SFX", "Music", "Voice", "Ambient", "UI"})
    {
        EXPECT_TRUE(std::find(names.begin(), names.end(), expected) != names.end());
        EXPECT_NEAR(mixer.GetBusVolume(expected), 1.0f, 0.001f);
    }

    mixer.SetBusVolume("Master", 0.5f);
    mixer.SetBusVolume("SFX", 0.8f);
    mixer.CreateBus("Weapons", "SFX");
    mixer.SetBusVolume("Weapons", 0.5f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Weapons"), 0.2f, 0.001f);

    mixer.SetBusMuted("SFX", true);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Weapons"), 0.0f, 0.001f);
    mixer.SetBusMuted("SFX", false);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Weapons"), 0.2f, 0.001f);
}

TEST(AudioMixer_ProductionClampsAndIgnoresUnknownBuses)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.SetBusVolume("SFX", 1.5f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 1.0f, 0.001f);
    mixer.SetBusVolume("SFX", -0.5f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 0.0f, 0.001f);
    EXPECT_NEAR(mixer.GetBusVolume("Missing"), 0.0f, 0.001f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Missing"), 0.0f, 0.001f);

    mixer.SetBusVolume("Missing", 0.5f);
    mixer.SetBusMuted("Missing", true);
    mixer.SetBusSolo("Missing", true);
}

TEST(AudioMixer_ProductionSoloMuteAndEffectsAppearInStatus)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.SetBusSolo("SFX", true);
    mixer.SetBusMuted("Music", true);

    Spark::Audio::DSPEffect effect;
    effect.type = Spark::Audio::DSPEffectType::Compressor;
    effect.param1 = 0.25f;
    mixer.AddBusEffect("SFX", effect);
    mixer.AddBusEffect("Missing", effect);

    const std::string status = mixer.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "Buses: 6");
    EXPECT_STR_CONTAINS(status, "SFX: vol=1 [SOLO] (1 effects)");
    EXPECT_STR_CONTAINS(status, "Music: vol=1 [MUTED]");

    mixer.ClearBusEffects("SFX");
    mixer.ClearBusEffects("Missing");
    EXPECT_TRUE(mixer.Console_GetStatus().find("effects") == std::string::npos);
}

TEST(AudioMixer_ProductionReverbZonesSortBlendAndRemove)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    auto low = MakeProductionReverbZone("Low", 0);
    auto high = MakeProductionReverbZone("High", 4);
    high.reverb.decayTime = 5.0f;
    mixer.AddReverbZone(low);
    mixer.AddReverbZone(high);

    ASSERT_EQ(mixer.GetReverbZones().size(), static_cast<size_t>(2));
    EXPECT_EQ(mixer.GetReverbZones().front().name, std::string("High"));

    const auto inside = mixer.GetReverbAtPosition({0.0f, 0.0f, 0.0f});
    EXPECT_EQ(static_cast<int>(inside.preset), static_cast<int>(Spark::Audio::ReverbPreset::Custom));
    EXPECT_GT(inside.decayTime, 0.0f);
    EXPECT_GT(inside.wetDryMix, 0.0f);

    const auto outside = mixer.GetReverbAtPosition({100.0f, 0.0f, 0.0f});
    EXPECT_EQ(static_cast<int>(outside.preset), static_cast<int>(Spark::Audio::ReverbPreset::None));
    EXPECT_STR_CONTAINS(mixer.Console_ListReverbZones(), "High");

    mixer.RemoveReverbZone("High");
    mixer.RemoveReverbZone("Missing");
    ASSERT_EQ(mixer.GetReverbZones().size(), static_cast<size_t>(1));
    EXPECT_EQ(mixer.GetReverbZones().front().name, std::string("Low"));
}

TEST(AudioMixer_ProductionReverbHonorsBoundsBlendAndEnabledState)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    auto zone = MakeProductionReverbZone("Blend");
    zone.halfExtents = {20.0f, 2.0f, 2.0f};
    mixer.AddReverbZone(zone);

    const auto inner = mixer.GetReverbAtPosition({0.0f, 0.0f, 0.0f});
    const auto transition = mixer.GetReverbAtPosition({15.0f, 0.0f, 0.0f});
    EXPECT_EQ(static_cast<int>(inner.preset), static_cast<int>(Spark::Audio::ReverbPreset::Custom));
    EXPECT_EQ(static_cast<int>(transition.preset), static_cast<int>(Spark::Audio::ReverbPreset::Custom));

    const auto outsideBox = mixer.GetReverbAtPosition({0.0f, 3.0f, 0.0f});
    EXPECT_EQ(static_cast<int>(outsideBox.preset), static_cast<int>(Spark::Audio::ReverbPreset::None));

    Spark::Audio::AudioMixer disabledMixer;
    disabledMixer.Initialize();
    zone.enabled = false;
    disabledMixer.AddReverbZone(zone);
    const auto disabled = disabledMixer.GetReverbAtPosition({0.0f, 0.0f, 0.0f});
    EXPECT_EQ(static_cast<int>(disabled.preset), static_cast<int>(Spark::Audio::ReverbPreset::None));
}

TEST(AudioMixer_ProductionOcclusionFallbackAndUpdateAreStable)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.Update({1.0f, 2.0f, 3.0f}, 1.0f / 60.0f);

    mixer.SetOcclusionEnabled(true);
    EXPECT_TRUE(mixer.IsOcclusionEnabled());
    const auto enabled = mixer.CalculateOcclusion({0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f});
    EXPECT_NEAR(enabled.occlusionFactor, 0.0f, 0.001f);
    EXPECT_NEAR(enabled.volumeScale, 1.0f, 0.001f);
    EXPECT_NEAR(enabled.lowPassCutoff, 22000.0f, 0.001f);
    EXPECT_EQ(enabled.wallCount, 0);

    mixer.SetOcclusionEnabled(false);
    EXPECT_FALSE(mixer.IsOcclusionEnabled());
    const auto disabled = mixer.CalculateOcclusion({0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f});
    EXPECT_NEAR(disabled.volumeScale, 1.0f, 0.001f);
}

TEST(AudioMixer_ProductionSnapshotRestoresVolumes)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.SetBusVolume("Music", 0.35f);
    mixer.SetBusVolume("SFX", 0.65f);
    mixer.SaveSnapshot("Gameplay");

    mixer.SetBusVolume("Music", 0.9f);
    mixer.SetBusVolume("SFX", 0.1f);
    mixer.RestoreSnapshot("Gameplay", 2.0f);
    EXPECT_NEAR(mixer.GetBusVolume("Music"), 0.35f, 0.001f);
    EXPECT_NEAR(mixer.GetBusVolume("SFX"), 0.65f, 0.001f);

    mixer.RestoreSnapshot("Missing");
    EXPECT_NEAR(mixer.GetBusVolume("Music"), 0.35f, 0.001f);
    EXPECT_STR_CONTAINS(mixer.Console_GetStatus(), "Snapshots: 1");
}
