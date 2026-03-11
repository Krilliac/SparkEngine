/**
 * @file AudioMixer.h
 * @brief Audio mixer with mix buses, reverb zones, and occlusion
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Extends the AudioEngine with professional audio mixing features:
 * - **Mix Buses**: Named volume groups (Master, SFX, Music, Voice, Ambient)
 *   with per-bus volume, mute, and solo controls.
 * - **Reverb Zones**: Spatial volumes that apply reverb presets to sounds
 *   played within them (cave, hall, room, outdoor).
 * - **Audio Occlusion**: Raycasted obstruction between listener and source,
 *   applying low-pass filtering and volume attenuation.
 * - **DSP Effects**: Per-bus effect chain (EQ, compressor, delay, chorus).
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <functional>

namespace Spark::Audio
{

    // =============================================================================
    // MixBus
    // =============================================================================

    /**
 * @brief A named audio channel group with volume and effect controls.
 */
    struct MixBus
    {
        std::string name;      ///< Bus name (e.g. "Master", "SFX", "Music")
        float volume = 1.0f;   ///< Bus volume multiplier (0.0 - 1.0)
        bool muted = false;    ///< Mute all sounds on this bus
        bool solo = false;     ///< Solo this bus (mute all others)
        std::string parentBus; ///< Parent bus name (empty = direct to Master)

        /** @brief Get effective volume including parent chain. */
        float GetEffectiveVolume() const { return muted ? 0.0f : volume; }
    };

    // =============================================================================
    // ReverbPreset
    // =============================================================================

    /**
 * @brief Predefined reverb environment settings.
 */
    enum class ReverbPreset
    {
        None,       ///< No reverb
        SmallRoom,  ///< Small enclosed space
        MediumRoom, ///< Medium room (office, bedroom)
        LargeRoom,  ///< Large open room (warehouse)
        Hall,       ///< Concert hall / cathedral
        Cave,       ///< Natural cave with long decay
        Sewer,      ///< Underground tunnel
        Outdoor,    ///< Open outdoor environment
        Forest,     ///< Forest with diffuse reflections
        Underwater, ///< Submerged in water
        Custom      ///< User-defined parameters
    };

    // =============================================================================
    // ReverbParameters
    // =============================================================================

    /**
 * @brief Detailed reverb configuration parameters.
 */
    struct ReverbParameters
    {
        ReverbPreset preset = ReverbPreset::None;
        float decayTime = 1.0f;        ///< Reverb tail length in seconds
        float earlyReflections = 0.5f; ///< Early reflection level (0.0 - 1.0)
        float lateReverb = 0.5f;       ///< Late reverb level (0.0 - 1.0)
        float diffusion = 0.7f;        ///< Reverb diffusion (0.0 - 1.0)
        float density = 0.8f;          ///< Reverb density (0.0 - 1.0)
        float roomSize = 0.5f;         ///< Virtual room size factor
        float wetDryMix = 0.3f;        ///< Wet/dry ratio (0.0 = dry, 1.0 = fully wet)
        float highFreqDamping = 0.5f;  ///< High frequency absorption (0.0 - 1.0)
    };

    // =============================================================================
    // ReverbZone
    // =============================================================================

    /**
 * @brief A spatial volume that applies reverb to sounds within it.
 */
    struct ReverbZone
    {
        std::string name;                       ///< Zone identifier
        DirectX::XMFLOAT3 position{0, 0, 0};    ///< World-space center
        DirectX::XMFLOAT3 halfExtents{5, 5, 5}; ///< Box half-extents
        float innerRadius = 3.0f;               ///< Full-effect radius
        float outerRadius = 5.0f;               ///< Blend-out radius
        ReverbParameters reverb;                ///< Reverb settings for this zone
        int priority = 0;                       ///< Higher priority zones override lower
        bool enabled = true;                    ///< Whether this zone is active
    };

    // =============================================================================
    // OcclusionResult
    // =============================================================================

    /**
 * @brief Result of an audio occlusion query between listener and source.
 */
    struct OcclusionResult
    {
        float occlusionFactor = 0.0f;   ///< 0.0 = unobstructed, 1.0 = fully blocked
        float lowPassCutoff = 22000.0f; ///< Low-pass filter frequency (Hz)
        float volumeScale = 1.0f;       ///< Volume multiplier from occlusion
        int wallCount = 0;              ///< Number of walls between listener and source
    };

    // =============================================================================
    // DSPEffectType
    // =============================================================================

    /**
 * @brief Types of DSP effects available for mix buses.
 */
    enum class DSPEffectType
    {
        LowPass,    ///< Low-pass filter
        HighPass,   ///< High-pass filter
        BandPass,   ///< Band-pass filter
        Equalizer,  ///< Multi-band equalizer
        Compressor, ///< Dynamic range compressor
        Limiter,    ///< Hard limiter
        Delay,      ///< Echo/delay effect
        Chorus,     ///< Chorus/flanger effect
        Distortion  ///< Distortion/overdrive
    };

    /**
 * @brief Configuration for a single DSP effect in a bus chain.
 */
    struct DSPEffect
    {
        DSPEffectType type = DSPEffectType::LowPass;
        float param1 = 0.0f; ///< Effect-specific parameter 1
        float param2 = 0.0f; ///< Effect-specific parameter 2
        float param3 = 0.0f; ///< Effect-specific parameter 3
        float wetDry = 1.0f; ///< Wet/dry mix (0.0 = bypass, 1.0 = full effect)
        bool enabled = true; ///< Whether this effect is active
    };

    // =============================================================================
    // AudioMixer
    // =============================================================================

    /**
 * @class AudioMixer
 * @brief Central audio mixing system with buses, reverb, and occlusion.
 *
 * Works alongside AudioEngine to provide advanced audio features. The
 * AudioEngine handles raw playback; the AudioMixer handles routing, effects,
 * and spatial processing.
 */
    class AudioMixer
    {
      public:
        AudioMixer();
        ~AudioMixer() = default;

        /**
     * @brief Initialize the mixer with default buses.
     *
     * Creates Master, SFX, Music, Voice, and Ambient buses.
     */
        void Initialize();

        /**
     * @brief Update the mixer (called once per frame).
     * @param listenerPos World-space listener position.
     * @param deltaTime   Frame time in seconds.
     */
        void Update(const DirectX::XMFLOAT3& listenerPos, float deltaTime);

        // --- Mix Buses ---

        /**
     * @brief Create a new mix bus.
     * @param name      Bus name.
     * @param parentBus Parent bus name (empty = Master).
     */
        void CreateBus(const std::string& name, const std::string& parentBus = "");

        /**
     * @brief Set the volume of a bus.
     * @param busName Bus name.
     * @param volume  Volume level (0.0 - 1.0).
     */
        void SetBusVolume(const std::string& busName, float volume);

        /** @brief Get the volume of a bus. */
        float GetBusVolume(const std::string& busName) const;

        /** @brief Mute or unmute a bus. */
        void SetBusMuted(const std::string& busName, bool muted);

        /** @brief Solo a bus (mutes all others). */
        void SetBusSolo(const std::string& busName, bool solo);

        /**
     * @brief Get the effective volume of a bus (including parent chain).
     * @param busName Bus name.
     * @return Combined volume from this bus through all parents to Master.
     */
        float GetEffectiveBusVolume(const std::string& busName) const;

        /** @brief Get all bus names. */
        std::vector<std::string> GetBusNames() const;

        // --- DSP Effects ---

        /**
     * @brief Add a DSP effect to a bus.
     * @param busName Bus name.
     * @param effect  DSP effect configuration.
     */
        void AddBusEffect(const std::string& busName, const DSPEffect& effect);

        /**
     * @brief Remove all effects from a bus.
     * @param busName Bus name.
     */
        void ClearBusEffects(const std::string& busName);

        // --- Reverb Zones ---

        /**
     * @brief Add a reverb zone to the world.
     * @param zone Reverb zone definition.
     */
        void AddReverbZone(const ReverbZone& zone);

        /**
     * @brief Remove a reverb zone by name.
     * @param name Zone name.
     */
        void RemoveReverbZone(const std::string& name);

        /**
     * @brief Get the active reverb parameters at a world position.
     * @param position World-space position to query.
     * @return Blended reverb parameters from all overlapping zones.
     */
        ReverbParameters GetReverbAtPosition(const DirectX::XMFLOAT3& position) const;

        /** @brief Get all reverb zones. */
        const std::vector<ReverbZone>& GetReverbZones() const { return m_reverbZones; }

        // --- Occlusion ---

        /**
     * @brief Calculate occlusion between two positions.
     * @param listenerPos Listener world position.
     * @param sourcePos   Sound source world position.
     * @return Occlusion result with volume scale and filter settings.
     *
     * @note Requires PhysicsSystem for raycasting. Falls back to
     *       unobstructed if physics is unavailable.
     */
        OcclusionResult CalculateOcclusion(const DirectX::XMFLOAT3& listenerPos,
                                           const DirectX::XMFLOAT3& sourcePos) const;

        /** @brief Enable or disable audio occlusion globally. */
        void SetOcclusionEnabled(bool enabled) { m_occlusionEnabled = enabled; }

        /** @brief Check if occlusion is enabled. */
        bool IsOcclusionEnabled() const { return m_occlusionEnabled; }

        // --- Snapshots ---

        /**
     * @brief Save current mixer state as a named snapshot.
     * @param name Snapshot name.
     */
        void SaveSnapshot(const std::string& name);

        /**
     * @brief Restore a named snapshot, optionally blending over time.
     * @param name     Snapshot name.
     * @param blendTime Transition time in seconds (0 = instant).
     */
        void RestoreSnapshot(const std::string& name, float blendTime = 0.0f);

        // --- Console integration ---

        /** @brief Get mixer status (console integration). */
        std::string Console_GetStatus() const;

        /** @brief List all reverb zones (console integration). */
        std::string Console_ListReverbZones() const;

      private:
        struct BusState
        {
            MixBus bus;
            std::vector<DSPEffect> effects;
        };

        struct Snapshot
        {
            std::unordered_map<std::string, float> busVolumes;
        };

        std::unordered_map<std::string, BusState> m_buses;
        std::vector<ReverbZone> m_reverbZones;
        std::unordered_map<std::string, Snapshot> m_snapshots;
        bool m_occlusionEnabled = true;
        DirectX::XMFLOAT3 m_listenerPosition{0, 0, 0};
    };

} // namespace Spark::Audio
