#include "Core/Platform.h"
/**
 * @file MusicManager.cpp
 * @brief Music system implementation — crossfading, playlists, dynamic music, reverb
 */

#include "MusicManager.h"
#include "../Utils/ContainerUtils.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <cmath>
#include <algorithm>

using namespace DirectX;
namespace Spark::Audio
{

    // ============================================================================
    // Playlist
    // ============================================================================

    std::string Playlist::GetCurrentTrack() const
    {
        if (trackNames.empty() || currentIndex < 0 || currentIndex >= static_cast<int>(trackNames.size()))
            return "";
        return trackNames[currentIndex];
    }

    std::string Playlist::GetNextTrack() const
    {
        if (trackNames.empty())
            return "";
        int next = currentIndex + 1;
        if (next >= static_cast<int>(trackNames.size()))
        {
            if (mode == PlaylistMode::Loop || mode == PlaylistMode::Shuffle)
                next = 0;
            else
                return "";
        }
        return trackNames[next];
    }

    void Playlist::Advance()
    {
        if (trackNames.empty())
            return;
        if (mode == PlaylistMode::LoopOne)
            return;

        currentIndex++;
        if (currentIndex >= static_cast<int>(trackNames.size()))
        {
            if (mode == PlaylistMode::Loop || mode == PlaylistMode::Shuffle)
                currentIndex = 0;
            else
                currentIndex = static_cast<int>(trackNames.size()); // Past end
        }
    }

    // ============================================================================
    // AudioBusMixer
    // ============================================================================

    AudioBusMixer::AudioBusMixer()
    {
        // Initialize default bus hierarchy
        m_buses[static_cast<int>(AudioBus::Master)] = {1.0f, false, AudioBus::Master};
        m_buses[static_cast<int>(AudioBus::SFX)] = {1.0f, false, AudioBus::Master};
        m_buses[static_cast<int>(AudioBus::Music)] = {0.7f, false, AudioBus::Master};
        m_buses[static_cast<int>(AudioBus::Voice)] = {1.0f, false, AudioBus::Master};
        m_buses[static_cast<int>(AudioBus::Ambient)] = {0.5f, false, AudioBus::Master};
        m_buses[static_cast<int>(AudioBus::UI)] = {0.8f, false, AudioBus::Master};
    }

    AudioBusMixer& AudioBusMixer::GetInstance()
    {
        static AudioBusMixer instance;
        return instance;
    }

    void AudioBusMixer::SetBusVolume(AudioBus bus, float volume)
    {
        m_buses[static_cast<int>(bus)].volume = (std::max)(0.0f, (std::min)(2.0f, volume));
    }

    float AudioBusMixer::GetBusVolume(AudioBus bus) const
    {
        return m_buses[static_cast<int>(bus)].volume;
    }

    void AudioBusMixer::SetBusMuted(AudioBus bus, bool muted)
    {
        m_buses[static_cast<int>(bus)].muted = muted;
    }

    bool AudioBusMixer::IsBusMuted(AudioBus bus) const
    {
        return m_buses[static_cast<int>(bus)].muted;
    }

    float AudioBusMixer::GetEffectiveVolume(AudioBus bus) const
    {
        if (m_buses[static_cast<int>(bus)].muted)
            return 0.0f;

        float vol = m_buses[static_cast<int>(bus)].volume;
        AudioBus parent = m_buses[static_cast<int>(bus)].parent;

        // Cascade through parent chain (but stop if we reach Master or self)
        if (bus != AudioBus::Master && parent != bus)
        {
            vol *= GetEffectiveVolume(parent);
        }

        return vol;
    }

    void AudioBusMixer::SetBusSettings(AudioBus bus, const MixerBusSettings& settings)
    {
        m_buses[static_cast<int>(bus)] = settings;
    }

    const MixerBusSettings& AudioBusMixer::GetBusSettings(AudioBus bus) const
    {
        return m_buses[static_cast<int>(bus)];
    }

    void AudioBusMixer::FadeBus(AudioBus bus, float targetVolume, float duration)
    {
        // Remove existing fade for this bus
        m_activeFades.erase(std::remove_if(m_activeFades.begin(), m_activeFades.end(),
                                           [bus](const FadeState& f) { return f.bus == bus; }),
                            m_activeFades.end());

        if (duration <= 0.0f)
        {
            SetBusVolume(bus, targetVolume);
            return;
        }

        FadeState fade;
        fade.bus = bus;
        fade.startVolume = GetBusVolume(bus);
        fade.targetVolume = targetVolume;
        fade.duration = duration;
        fade.elapsed = 0.0f;
        m_activeFades.push_back(fade);
    }

    void AudioBusMixer::Update(float deltaTime)
    {
        for (auto it = m_activeFades.begin(); it != m_activeFades.end();)
        {
            it->elapsed += deltaTime;
            float t = (std::min)(it->elapsed / it->duration, 1.0f);
            // Smooth interpolation
            t = t * t * (3.0f - 2.0f * t); // Smoothstep
            float vol = it->startVolume + (it->targetVolume - it->startVolume) * t;
            SetBusVolume(it->bus, vol);

            if (it->elapsed >= it->duration)
            {
                it = m_activeFades.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::string AudioBusMixer::Console_GetMixerInfo() const
    {
        const char* busNames[] = {"Master", "SFX", "Music", "Voice", "Ambient", "UI"};
        std::ostringstream ss;
        ss << "=== Audio Mixer ===\n";
        for (int i = 0; i < static_cast<int>(AudioBus::Count); ++i)
        {
            ss << "  " << busNames[i] << ": " << (m_buses[i].muted ? "[MUTED] " : "") << "Vol=" << m_buses[i].volume
               << " Eff=" << GetEffectiveVolume(static_cast<AudioBus>(i)) << "\n";
        }
        ss << "Active Fades: " << m_activeFades.size() << "\n";
        return ss.str();
    }

    // ============================================================================
    // MusicManager
    // ============================================================================

    MusicManager& MusicManager::GetInstance()
    {
        static MusicManager instance;
        return instance;
    }

    void MusicManager::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_LOG_INFO(Spark::LogCategory::Audio, "MusicManager::Initialize");
        m_isPlaying = false;
        m_isPaused = false;
        m_isCrossfading = false;
    }

    void MusicManager::Update(float deltaTime)
    {
        if (!m_isPlaying || m_isPaused)
            return;

        UpdateCrossfade(deltaTime);
        UpdateDynamicMusic(deltaTime);
        AudioBusMixer::GetInstance().Update(deltaTime);
    }

    void MusicManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_LOG_INFO(Spark::LogCategory::Audio, "MusicManager::Shutdown");
        Stop(0.0f);
        m_tracks.clear();
        m_playlists.clear();
        m_reverbZones.clear();
    }

    void MusicManager::RegisterTrack(const MusicTrack& track)
    {
        m_tracks[track.name] = track;
    }

    void MusicManager::UnregisterTrack(const std::string& name)
    {
        m_tracks.erase(name);
    }

    const MusicTrack* MusicManager::GetTrack(const std::string& name) const
    {
        auto it = m_tracks.find(name);
        return (it != m_tracks.end()) ? &it->second : nullptr;
    }

    void MusicManager::Play(const std::string& trackName, float fadeInDuration)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, trackName);
        if (!Spark::ContainerUtils::Contains(m_tracks, trackName))
            return;

        m_currentTrack = trackName;
        m_isPlaying = true;
        m_isPaused = false;

        if (fadeInDuration > 0.0f)
        {
            AudioBusMixer::GetInstance().FadeBus(AudioBus::Music, 0.7f, fadeInDuration);
        }
    }

    void MusicManager::Stop(float fadeOutDuration)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        if (fadeOutDuration > 0.0f)
        {
            AudioBusMixer::GetInstance().FadeBus(AudioBus::Music, 0.0f, fadeOutDuration);
        }
        m_isPlaying = false;
        m_isCrossfading = false;
    }

    void MusicManager::Pause()
    {
        m_isPaused = true;
    }
    void MusicManager::Resume()
    {
        m_isPaused = false;
    }

    void MusicManager::CrossfadeTo(const std::string& trackName, float duration)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, trackName);
        if (!Spark::ContainerUtils::Contains(m_tracks, trackName))
            return;
        if (trackName == m_currentTrack)
            return;

        m_nextTrack = trackName;
        m_crossfadeDuration = duration;
        m_crossfadeProgress = 0.0f;
        m_isCrossfading = true;
    }

    void MusicManager::RegisterPlaylist(const Playlist& playlist)
    {
        m_playlists[playlist.name] = playlist;
    }

    void MusicManager::PlayPlaylist(const std::string& playlistName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Audio, playlistName);
        auto it = m_playlists.find(playlistName);
        if (it == m_playlists.end())
            return;

        m_activePlaylist = playlistName;
        it->second.currentIndex = 0;
        std::string firstTrack = it->second.GetCurrentTrack();
        if (!firstTrack.empty())
            Play(firstTrack);
    }

    void MusicManager::NextTrack()
    {
        if (m_activePlaylist.empty())
            return;
        auto it = m_playlists.find(m_activePlaylist);
        if (it == m_playlists.end())
            return;

        it->second.Advance();
        std::string next = it->second.GetCurrentTrack();
        if (!next.empty())
            CrossfadeTo(next);
    }

    void MusicManager::PreviousTrack()
    {
        if (m_activePlaylist.empty())
            return;
        auto it = m_playlists.find(m_activePlaylist);
        if (it == m_playlists.end())
            return;

        if (it->second.currentIndex > 0)
            it->second.currentIndex--;
        std::string prev = it->second.GetCurrentTrack();
        if (!prev.empty())
            CrossfadeTo(prev);
    }

    void MusicManager::SetPlaylistMode(PlaylistMode mode)
    {
        if (m_activePlaylist.empty())
            return;
        auto it = m_playlists.find(m_activePlaylist);
        if (it != m_playlists.end())
            it->second.mode = mode;
    }

    void MusicManager::SetDynamicMusicState(const DynamicMusicState& state)
    {
        m_dynamicState = state;
    }

    void MusicManager::SetCombatIntensity(CombatIntensity intensity)
    {
        if (intensity == m_dynamicState.currentIntensity)
            return;

        m_targetIntensity = intensity;
        std::string targetTrack;
        switch (intensity)
        {
        case CombatIntensity::Exploration:
            targetTrack = m_dynamicState.explorationTrack;
            break;
        case CombatIntensity::LowThreat:
            targetTrack = m_dynamicState.lowThreatTrack;
            break;
        case CombatIntensity::Combat:
            targetTrack = m_dynamicState.combatTrack;
            break;
        case CombatIntensity::BossFight:
            targetTrack = m_dynamicState.bossFightTrack;
            break;
        }

        if (!targetTrack.empty())
        {
            CrossfadeTo(targetTrack, m_dynamicState.transitionDuration);
        }
        m_dynamicState.currentIntensity = intensity;
    }

    void MusicManager::AddReverbZone(const MusicReverbZone& zone)
    {
        // Invalidate cached pointer — push_back may reallocate the vector
        m_activeReverbZone = nullptr;
        m_reverbZones.push_back(zone);
    }

    void MusicManager::RemoveReverbZone(const std::string& name)
    {
        // Invalidate cached pointer — erase invalidates iterators/pointers into the vector
        m_activeReverbZone = nullptr;
        m_reverbZones.erase(std::remove_if(m_reverbZones.begin(), m_reverbZones.end(),
                                           [&name](const MusicReverbZone& z) { return z.name == name; }),
                            m_reverbZones.end());
    }

    void MusicManager::UpdateListenerReverbZone(const XMFLOAT3& listenerPosition)
    {
        m_activeReverbZone = nullptr;
        float bestDist = 1e30f;

        for (const auto& zone : m_reverbZones)
        {
            float dx = std::abs(listenerPosition.x - zone.position.x);
            float dy = std::abs(listenerPosition.y - zone.position.y);
            float dz = std::abs(listenerPosition.z - zone.position.z);

            // Check if inside the zone's box
            if (dx <= zone.halfExtents.x && dy <= zone.halfExtents.y && dz <= zone.halfExtents.z)
            {
                float dist = dx * dx + dy * dy + dz * dz;
                if (dist < bestDist)
                {
                    bestDist = dist;
                    m_activeReverbZone = &zone;
                }
            }
        }
    }

    void MusicManager::SetOcclusionSettings(const OcclusionSettings& settings)
    {
        m_occlusionSettings = settings;
    }

    MusicOcclusionResult MusicManager::ComputeOcclusion(const XMFLOAT3& sourcePos, const XMFLOAT3& listenerPos) const
    {
        MusicOcclusionResult result{0.0f, 22000.0f};
        if (!m_occlusionSettings.enabled)
            return result;

        // In a full implementation, this would raycast against the physics world.
        // For now, return no occlusion — the integration point is ready.
        return result;
    }

    void MusicManager::UpdateCrossfade(float deltaTime)
    {
        if (!m_isCrossfading)
            return;

        m_crossfadeProgress += deltaTime;
        float t = (m_crossfadeDuration > 0.0f) ? (std::min)(m_crossfadeProgress / m_crossfadeDuration, 1.0f) : 1.0f;

        if (t >= 1.0f)
        {
            m_currentTrack = m_nextTrack;
            m_nextTrack.clear();
            m_isCrossfading = false;
        }
    }

    void MusicManager::UpdateDynamicMusic(float deltaTime)
    {
        // Dynamic music intensity is handled by SetCombatIntensity
        // which triggers crossfades. This update step can be used
        // for additional logic like automatic intensity detection.
    }

    std::string MusicManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Music Manager ===\n";
        ss << "Playing: " << (m_isPlaying ? "Yes" : "No") << "\n";
        ss << "Paused: " << (m_isPaused ? "Yes" : "No") << "\n";
        ss << "Current Track: " << (m_currentTrack.empty() ? "None" : m_currentTrack) << "\n";
        if (m_isCrossfading)
        {
            float pct = (m_crossfadeDuration > 0.0f) ? (m_crossfadeProgress / m_crossfadeDuration * 100.0f) : 100.0f;
            ss << "Crossfading to: " << m_nextTrack << " (" << pct << "%)\n";
        }
        ss << "Active Playlist: " << (m_activePlaylist.empty() ? "None" : m_activePlaylist) << "\n";
        ss << "Combat Intensity: ";
        switch (m_dynamicState.currentIntensity)
        {
        case CombatIntensity::Exploration:
            ss << "Exploration";
            break;
        case CombatIntensity::LowThreat:
            ss << "Low Threat";
            break;
        case CombatIntensity::Combat:
            ss << "Combat";
            break;
        case CombatIntensity::BossFight:
            ss << "Boss Fight";
            break;
        }
        ss << "\nReverb Zones: " << m_reverbZones.size() << "\n";
        if (m_activeReverbZone)
            ss << "Active Reverb: " << m_activeReverbZone->name << "\n";
        return ss.str();
    }

    std::string MusicManager::Console_ListTracks() const
    {
        std::ostringstream ss;
        ss << "=== Music Tracks (" << m_tracks.size() << ") ===\n";
        for (const auto& [name, track] : m_tracks)
        {
            ss << "  " << name << " [" << track.bpm << " BPM" << (track.loop ? ", Loop" : "") << "]\n";
        }
        return ss.str();
    }

    std::string MusicManager::Console_ListPlaylists() const
    {
        std::ostringstream ss;
        ss << "=== Playlists (" << m_playlists.size() << ") ===\n";
        for (const auto& [name, pl] : m_playlists)
        {
            ss << "  " << name << " [" << pl.trackNames.size() << " tracks]\n";
        }
        return ss.str();
    }

    std::string MusicManager::Console_ListReverbZones() const
    {
        std::ostringstream ss;
        ss << "=== Reverb Zones (" << m_reverbZones.size() << ") ===\n";
        for (const auto& zone : m_reverbZones)
        {
            ss << "  " << zone.name << " [Decay: " << zone.decayTime << "s]\n";
        }
        return ss.str();
    }

} // namespace Spark::Audio
