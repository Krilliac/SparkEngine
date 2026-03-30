/**
 * @file MusicManager.h
 * @brief Music playback system with crossfading, playlists, and dynamic intensity
 * @author Spark Engine Team
 * @date 2025
 *
 * Extends the AudioEngine with:
 * - Music playback with crossfading between tracks
 * - Playlist management with shuffle and loop modes
 * - Dynamic music layers (low/medium/high combat intensity)
 * - Mixer bus system (Master -> SFX / Voice / Music / Ambient)
 * - Audio occlusion via raycasts
 * - Reverb zones
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <queue>


namespace Spark::Audio
{

    // ============================================================================
    // Mixer Bus System
    // ============================================================================

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

    struct MixerBusSettings
    {
        float volume = 1.0f;
        bool muted = false;
        AudioBus parent = AudioBus::Master; ///< Parent bus for volume cascade
        float lowPassCutoff = 22000.0f;     ///< Low-pass filter cutoff Hz
        float highPassCutoff = 20.0f;       ///< High-pass filter cutoff Hz
    };

    class AudioBusMixer
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<AudioBusMixer>() instead")]]
        static AudioBusMixer& GetInstance();

        void SetBusVolume(AudioBus bus, float volume);
        float GetBusVolume(AudioBus bus) const;
        void SetBusMuted(AudioBus bus, bool muted);
        bool IsBusMuted(AudioBus bus) const;

        /// Get effective volume (cascaded through parent buses)
        float GetEffectiveVolume(AudioBus bus) const;

        /// Apply settings to a bus
        void SetBusSettings(AudioBus bus, const MixerBusSettings& settings);
        const MixerBusSettings& GetBusSettings(AudioBus bus) const;

        /// Fade a bus volume over time
        void FadeBus(AudioBus bus, float targetVolume, float duration);

        /// Update fades
        void Update(float deltaTime);

        /// Console integration
        std::string Console_GetMixerInfo() const;

      private:
        AudioBusMixer();
        MixerBusSettings m_buses[static_cast<int>(AudioBus::Count)];

        struct FadeState
        {
            AudioBus bus;
            float startVolume;
            float targetVolume;
            float duration;
            float elapsed;
        };
        std::vector<FadeState> m_activeFades;
    };

    // ============================================================================
    // Music Track & Playlist
    // ============================================================================

    struct MusicTrack
    {
        std::string name;
        std::string filepath;
        float bpm = 120.0f;         ///< Beats per minute (for sync)
        float loopStartTime = 0.0f; ///< Where to loop back to
        float loopEndTime = -1.0f;  ///< Where loop ends (-1 = end of file)
        bool loop = true;
        std::string nextTrack; ///< Name of next track to play automatically
    };

    enum class PlaylistMode
    {
        Sequential, ///< Play tracks in order
        Shuffle,    ///< Random order
        Loop,       ///< Loop entire playlist
        LoopOne     ///< Loop current track
    };

    struct Playlist
    {
        std::string name;
        std::vector<std::string> trackNames;
        PlaylistMode mode = PlaylistMode::Loop;
        int currentIndex = 0;

        std::string GetCurrentTrack() const;
        std::string GetNextTrack() const;
        void Advance();
    };

    // ============================================================================
    // Dynamic Music System (Intensity Layers)
    // ============================================================================

    enum class CombatIntensity
    {
        Exploration, ///< Calm ambient music
        LowThreat,   ///< Tension building
        Combat,      ///< Full combat music
        BossFight    ///< Maximum intensity
    };

    struct DynamicMusicState
    {
        std::string explorationTrack;
        std::string lowThreatTrack;
        std::string combatTrack;
        std::string bossFightTrack;
        CombatIntensity currentIntensity = CombatIntensity::Exploration;
        float transitionDuration = 2.0f; ///< Crossfade time between intensity levels
    };

    // ============================================================================
    // Audio Occlusion
    // ============================================================================

    struct OcclusionSettings
    {
        bool enabled = true;
        float maxOcclusionFactor = 0.8f;    ///< Maximum volume reduction (0-1)
        float lowPassWhenOccluded = 800.0f; ///< Low-pass cutoff when fully occluded
        int maxRays = 4;                    ///< Number of occlusion rays to cast
    };

    /**
     * @brief Simplified occlusion result for music system.
     *
     * Unlike AudioMixer::OcclusionResult (which has 4 fields), this only carries
     * the two values the music system needs. Named differently to avoid ODR
     * conflicts when both AudioMixer.h and MusicManager.h are included.
     */
    struct MusicOcclusionResult
    {
        float occlusionFactor; ///< 0 = no occlusion, 1 = fully occluded
        float lowPassCutoff;   ///< Resulting low-pass filter cutoff
    };

    // ============================================================================
    // Reverb Zone (Music)
    // ============================================================================

    /**
     * @brief Music-specific reverb zone with inline parameters.
     *
     * Unlike AudioMixer::ReverbZone (which references a separate ReverbParameters
     * struct), this inlines all reverb fields and adds a Type enum for presets.
     * Named differently to avoid ODR conflicts.
     */
    struct MusicReverbZone
    {
        std::string name;
        XMFLOAT3 position;
        XMFLOAT3 halfExtents; ///< Box-shaped zone
        float innerRadius;    ///< Full reverb within this radius
        float outerRadius;    ///< Reverb fades from inner to outer

        // Reverb parameters
        float decayTime = 1.0f; ///< Seconds for reverb to decay
        float earlyReflections = 0.5f;
        float lateReverbLevel = 0.3f;
        float diffusion = 0.8f;
        float density = 0.9f;
        float hfDecayRatio = 0.5f; ///< High-frequency decay relative to overall

        enum class Type
        {
            Small,
            Medium,
            Large,
            Hall,
            Cave,
            Outdoor
        } type = Type::Medium;
    };

    // ============================================================================
    // MusicManager
    // ============================================================================

    class MusicManager
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<MusicManager>() instead")]]
        static MusicManager& GetInstance();

        void Initialize();
        void Update(float deltaTime);
        void Shutdown();

        // Track management
        void RegisterTrack(const MusicTrack& track);
        void UnregisterTrack(const std::string& name);
        const MusicTrack* GetTrack(const std::string& name) const;

        // Playback
        void Play(const std::string& trackName, float fadeInDuration = 1.0f);
        void Stop(float fadeOutDuration = 1.0f);
        void Pause();
        void Resume();
        void CrossfadeTo(const std::string& trackName, float duration = 2.0f);
        bool IsPlaying() const { return m_isPlaying; }
        const std::string& GetCurrentTrackName() const { return m_currentTrack; }

        // Playlist management
        void RegisterPlaylist(const Playlist& playlist);
        void PlayPlaylist(const std::string& playlistName);
        void NextTrack();
        void PreviousTrack();
        void SetPlaylistMode(PlaylistMode mode);

        // Dynamic music
        void SetDynamicMusicState(const DynamicMusicState& state);
        void SetCombatIntensity(CombatIntensity intensity);
        CombatIntensity GetCombatIntensity() const { return m_dynamicState.currentIntensity; }

        // Reverb zones
        void AddReverbZone(const MusicReverbZone& zone);
        void RemoveReverbZone(const std::string& name);
        void UpdateListenerReverbZone(const XMFLOAT3& listenerPosition);

        // Audio occlusion
        void SetOcclusionSettings(const OcclusionSettings& settings);
        MusicOcclusionResult ComputeOcclusion(const XMFLOAT3& sourcePos, const XMFLOAT3& listenerPos) const;

        /// Console integration
        std::string Console_GetStatus() const;
        std::string Console_ListTracks() const;
        std::string Console_ListPlaylists() const;
        std::string Console_ListReverbZones() const;

      private:
        MusicManager() = default;

        void UpdateCrossfade(float deltaTime);
        void UpdateDynamicMusic(float deltaTime);

        // Tracks
        std::unordered_map<std::string, MusicTrack> m_tracks;

        // Playback state
        std::string m_currentTrack;
        std::string m_nextTrack; ///< Track being crossfaded to
        float m_crossfadeProgress = 0.0f;
        float m_crossfadeDuration = 0.0f;
        bool m_isCrossfading = false;
        bool m_isPlaying = false;
        bool m_isPaused = false;

        // Playlists
        std::unordered_map<std::string, Playlist> m_playlists;
        std::string m_activePlaylist;

        // Dynamic music
        DynamicMusicState m_dynamicState;
        CombatIntensity m_targetIntensity = CombatIntensity::Exploration;

        // Reverb zones
        std::vector<MusicReverbZone> m_reverbZones;
        const MusicReverbZone* m_activeReverbZone = nullptr;

        // Occlusion
        OcclusionSettings m_occlusionSettings;
    };

} // namespace Spark::Audio
