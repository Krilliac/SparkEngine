/**
 * @file TestMusicManager.cpp
 * @brief Production MusicManager coverage: playlists, crossfades, buses, intensity, and reverb.
 */

#include "TestFramework.h"


#include "Audio/MusicManager.h"

#include <string>
#include <utility>

namespace
{
    using Spark::Audio::AudioBus;
    using Spark::Audio::AudioBusMixer;
    using Spark::Audio::CombatIntensity;
    using Spark::Audio::MixerBusSettings;
    using Spark::Audio::MusicManager;
    using Spark::Audio::MusicReverbZone;
    using Spark::Audio::MusicTrack;
    using Spark::Audio::OcclusionSettings;
    using Spark::Audio::Playlist;
    using Spark::Audio::PlaylistMode;

    void ResetProductionBusMixer()
    {
        auto& mixer = AudioBusMixer::GetInstance();
        mixer.Update(10000.0f);
        mixer.SetBusSettings(AudioBus::Master, {1.0f, false, AudioBus::Master});
        mixer.SetBusSettings(AudioBus::SFX, {1.0f, false, AudioBus::Master});
        mixer.SetBusSettings(AudioBus::Music, {0.7f, false, AudioBus::Master});
        mixer.SetBusSettings(AudioBus::Voice, {1.0f, false, AudioBus::Master});
        mixer.SetBusSettings(AudioBus::Ambient, {0.5f, false, AudioBus::Master});
        mixer.SetBusSettings(AudioBus::UI, {0.8f, false, AudioBus::Master});
    }

    class ProductionMusicScope
    {
      public:
        ProductionMusicScope()
        {
            manager.Shutdown();
            ResetProductionBusMixer();
            manager.Initialize();
        }

        ~ProductionMusicScope()
        {
            manager.Shutdown();
            ResetProductionBusMixer();
        }

        MusicManager& manager = MusicManager::GetInstance();
    };

    MusicTrack Track(std::string name, float bpm = 120.0f)
    {
        MusicTrack track;
        track.name = std::move(name);
        track.filepath = "Audio/" + track.name + ".ogg";
        track.bpm = bpm;
        return track;
    }

    MusicReverbZone MusicZone(std::string name, float x)
    {
        MusicReverbZone zone;
        zone.name = std::move(name);
        zone.position = {x, 0.0f, 0.0f};
        zone.halfExtents = {10.0f, 10.0f, 10.0f};
        zone.innerRadius = 3.0f;
        zone.outerRadius = 10.0f;
        zone.decayTime = 2.5f;
        zone.type = MusicReverbZone::Type::Hall;
        return zone;
    }
} // namespace

TEST(MusicManager_ProductionPlaylistValueSemantics)
{
    Playlist sequential;
    sequential.name = "Sequential";
    sequential.trackNames = {"A", "B", "C"};
    sequential.mode = PlaylistMode::Sequential;
    EXPECT_EQ(sequential.GetCurrentTrack(), std::string("A"));
    EXPECT_EQ(sequential.GetNextTrack(), std::string("B"));
    sequential.Advance();
    sequential.Advance();
    EXPECT_EQ(sequential.GetCurrentTrack(), std::string("C"));
    EXPECT_EQ(sequential.GetNextTrack(), std::string());
    sequential.Advance();
    EXPECT_EQ(sequential.GetCurrentTrack(), std::string());

    Playlist loop;
    loop.trackNames = {"A", "B"};
    loop.mode = PlaylistMode::Loop;
    loop.currentIndex = 1;
    EXPECT_EQ(loop.GetNextTrack(), std::string("A"));
    loop.Advance();
    EXPECT_EQ(loop.GetCurrentTrack(), std::string("A"));

    Playlist loopOne;
    loopOne.trackNames = {"A", "B"};
    loopOne.mode = PlaylistMode::LoopOne;
    loopOne.currentIndex = 1;
    loopOne.Advance();
    EXPECT_EQ(loopOne.GetCurrentTrack(), std::string("B"));

    Playlist empty;
    empty.Advance();
    EXPECT_EQ(empty.GetCurrentTrack(), std::string());
    EXPECT_EQ(empty.GetNextTrack(), std::string());
}

TEST(MusicManager_ProductionBusMixerCascadesMutesClampsAndReports)
{
    ResetProductionBusMixer();
    auto& mixer = AudioBusMixer::GetInstance();
    mixer.SetBusVolume(AudioBus::Master, 0.5f);
    mixer.SetBusVolume(AudioBus::Music, 0.8f);
    EXPECT_NEAR(mixer.GetEffectiveVolume(AudioBus::Music), 0.4f, 0.001f);

    mixer.SetBusMuted(AudioBus::Master, true);
    EXPECT_TRUE(mixer.IsBusMuted(AudioBus::Master));
    EXPECT_NEAR(mixer.GetEffectiveVolume(AudioBus::Music), 0.0f, 0.001f);
    mixer.SetBusMuted(AudioBus::Master, false);

    mixer.SetBusVolume(AudioBus::Music, 3.0f);
    EXPECT_NEAR(mixer.GetBusVolume(AudioBus::Music), 2.0f, 0.001f);
    mixer.SetBusVolume(AudioBus::Music, -1.0f);
    EXPECT_NEAR(mixer.GetBusVolume(AudioBus::Music), 0.0f, 0.001f);
    EXPECT_STR_CONTAINS(mixer.Console_GetMixerInfo(), "Active Fades: 0");
    ResetProductionBusMixer();
}

TEST(MusicManager_ProductionBusSettingsAndFades)
{
    ResetProductionBusMixer();
    auto& mixer = AudioBusMixer::GetInstance();
    MixerBusSettings settings;
    settings.volume = 0.2f;
    settings.parent = AudioBus::Master;
    settings.lowPassCutoff = 12000.0f;
    mixer.SetBusSettings(AudioBus::SFX, settings);
    EXPECT_NEAR(mixer.GetBusSettings(AudioBus::SFX).lowPassCutoff, 12000.0f, 0.001f);

    mixer.FadeBus(AudioBus::SFX, 0.8f, 2.0f);
    mixer.Update(1.0f);
    EXPECT_NEAR(mixer.GetBusVolume(AudioBus::SFX), 0.5f, 0.001f);
    mixer.Update(1.0f);
    EXPECT_NEAR(mixer.GetBusVolume(AudioBus::SFX), 0.8f, 0.001f);

    mixer.FadeBus(AudioBus::SFX, 0.3f, 0.0f);
    EXPECT_NEAR(mixer.GetBusVolume(AudioBus::SFX), 0.3f, 0.001f);
    mixer.FadeBus(AudioBus::SFX, 0.9f, 10.0f);
    mixer.FadeBus(AudioBus::SFX, 0.4f, 0.0f);
    EXPECT_NEAR(mixer.GetBusVolume(AudioBus::SFX), 0.4f, 0.001f);
    ResetProductionBusMixer();
}

TEST(MusicManager_ProductionTrackLifecycleAndPlaybackState)
{
    ProductionMusicScope scope;
    auto& music = scope.manager;
    music.RegisterTrack(Track("Calm", 90.0f));
    music.RegisterTrack(Track("Battle", 150.0f));

    const MusicTrack* calm = music.GetTrack("Calm");
    ASSERT_TRUE(calm != nullptr);
    EXPECT_NEAR(calm->bpm, 90.0f, 0.001f);
    EXPECT_TRUE(music.GetTrack("Missing") == nullptr);
    EXPECT_STR_CONTAINS(music.Console_ListTracks(), "Calm [90 BPM");

    music.Play("Missing", 0.0f);
    EXPECT_FALSE(music.IsPlaying());
    music.Play("Calm", 0.0f);
    EXPECT_TRUE(music.IsPlaying());
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("Calm"));
    music.Pause();
    EXPECT_STR_CONTAINS(music.Console_GetStatus(), "Paused: Yes");
    music.Resume();
    EXPECT_STR_CONTAINS(music.Console_GetStatus(), "Paused: No");

    music.Stop(0.0f);
    EXPECT_FALSE(music.IsPlaying());
    music.UnregisterTrack("Calm");
    music.UnregisterTrack("Missing");
    EXPECT_TRUE(music.GetTrack("Calm") == nullptr);
}

TEST(MusicManager_ProductionCrossfadeCompletesThroughUpdate)
{
    ProductionMusicScope scope;
    auto& music = scope.manager;
    music.RegisterTrack(Track("CrossfadeA"));
    music.RegisterTrack(Track("CrossfadeB"));
    music.Play("CrossfadeA", 0.5f);
    music.CrossfadeTo("Missing", 2.0f);
    music.CrossfadeTo("CrossfadeA", 2.0f);
    music.CrossfadeTo("CrossfadeB", 2.0f);

    music.Update(1.0f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("CrossfadeA"));
    EXPECT_STR_CONTAINS(music.Console_GetStatus(), "Crossfading to: CrossfadeB (50%");
    music.Update(1.0f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("CrossfadeB"));
    EXPECT_TRUE(music.Console_GetStatus().find("Crossfading to") == std::string::npos);

    music.CrossfadeTo("CrossfadeA", 0.0f);
    music.Update(0.0f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("CrossfadeA"));
}

TEST(MusicManager_ProductionPlaylistNavigationUsesRegisteredTracks)
{
    ProductionMusicScope scope;
    auto& music = scope.manager;
    music.RegisterTrack(Track("PlaylistA"));
    music.RegisterTrack(Track("PlaylistB"));
    music.RegisterTrack(Track("PlaylistC"));

    Playlist playlist;
    playlist.name = "ProductionPlaylist";
    playlist.trackNames = {"PlaylistA", "PlaylistB", "PlaylistC"};
    playlist.mode = PlaylistMode::Loop;
    music.RegisterPlaylist(playlist);
    music.PlayPlaylist("Missing");
    music.PlayPlaylist("ProductionPlaylist");
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("PlaylistA"));
    EXPECT_STR_CONTAINS(music.Console_ListPlaylists(), "ProductionPlaylist [3 tracks]");

    music.NextTrack();
    music.Update(2.0f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("PlaylistB"));
    music.PreviousTrack();
    music.Update(2.0f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("PlaylistA"));
    music.SetPlaylistMode(PlaylistMode::Sequential);
}

TEST(MusicManager_ProductionDynamicIntensityTransitionsTracks)
{
    ProductionMusicScope scope;
    auto& music = scope.manager;
    music.RegisterTrack(Track("Explore"));
    music.RegisterTrack(Track("Threat"));
    music.RegisterTrack(Track("Combat"));
    music.RegisterTrack(Track("Boss"));
    music.Play("Explore", 0.0f);

    Spark::Audio::DynamicMusicState state;
    state.explorationTrack = "Explore";
    state.lowThreatTrack = "Threat";
    state.combatTrack = "Combat";
    state.bossFightTrack = "Boss";
    state.transitionDuration = 0.5f;
    music.SetDynamicMusicState(state);

    music.SetCombatIntensity(CombatIntensity::Exploration);
    music.SetCombatIntensity(CombatIntensity::LowThreat);
    music.Update(0.5f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("Threat"));
    music.SetCombatIntensity(CombatIntensity::Combat);
    music.Update(0.5f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("Combat"));
    music.SetCombatIntensity(CombatIntensity::BossFight);
    music.Update(0.5f);
    EXPECT_EQ(music.GetCurrentTrackName(), std::string("Boss"));
    EXPECT_EQ(static_cast<int>(music.GetCombatIntensity()), static_cast<int>(CombatIntensity::BossFight));
    EXPECT_STR_CONTAINS(music.Console_GetStatus(), "Combat Intensity: Boss Fight");
}

TEST(MusicManager_ProductionReverbZonesTrackNearestListenerZone)
{
    ProductionMusicScope scope;
    auto& music = scope.manager;
    music.AddReverbZone(MusicZone("Hall", 0.0f));
    music.AddReverbZone(MusicZone("Cave", 8.0f));
    music.UpdateListenerReverbZone({7.5f, 0.0f, 0.0f});

    EXPECT_STR_CONTAINS(music.Console_GetStatus(), "Active Reverb: Cave");
    EXPECT_STR_CONTAINS(music.Console_ListReverbZones(), "Hall [Decay: 2.5s]");
    music.RemoveReverbZone("Cave");
    music.RemoveReverbZone("Missing");
    music.UpdateListenerReverbZone({100.0f, 0.0f, 0.0f});
    EXPECT_TRUE(music.Console_GetStatus().find("Active Reverb") == std::string::npos);
}

TEST(MusicManager_ProductionOcclusionFallbackHonorsEnabledSetting)
{
    ProductionMusicScope scope;
    auto& music = scope.manager;
    OcclusionSettings settings;
    settings.enabled = true;
    music.SetOcclusionSettings(settings);
    const auto enabled = music.ComputeOcclusion({20.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(enabled.occlusionFactor, 0.0f, 0.001f);
    EXPECT_NEAR(enabled.lowPassCutoff, 22000.0f, 0.001f);

    settings.enabled = false;
    music.SetOcclusionSettings(settings);
    const auto disabled = music.ComputeOcclusion({20.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(disabled.occlusionFactor, 0.0f, 0.001f);
    EXPECT_NEAR(disabled.lowPassCutoff, 22000.0f, 0.001f);
}
