/**
 * @file AudioMixerPanel.h
 * @brief Audio mixer panel for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a visual audio mixing console with channel faders, bus routing,
 * effects chains, and real-time level metering. Designed for configuring
 * game audio balance — music, SFX, dialogue, ambience, and UI channels.
 */

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>
#include <algorithm>

#include <imgui.h>

#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    // =========================================================================
    // Audio mixer data model
    // =========================================================================

    /**
     * @brief Type of audio effect in an effect chain
     */
    enum class AudioEffectType
    {
        LowPass,    ///< Low-pass filter with cutoff frequency
        HighPass,   ///< High-pass filter with cutoff frequency
        Reverb,     ///< Convolution or algorithmic reverb
        Delay,      ///< Echo/delay effect
        Compressor, ///< Dynamic range compressor
        Limiter,    ///< Hard limiter (brick-wall)
        EQ3Band,    ///< 3-band parametric equalizer
        Distortion, ///< Soft/hard clipping distortion
        Chorus,     ///< Chorus/flanger modulation effect
        Panning     ///< Stereo panning control
    };

    /**
     * @brief Single audio effect with parameters
     */
    struct AudioEffect
    {
        AudioEffectType type = AudioEffectType::LowPass;
        std::string name;
        bool enabled = true;
        bool expanded = false; ///< Whether the effect's parameters are shown in the UI

        // Common parameters (effect-type determines which are used)
        float param1 = 0.0f;    ///< Cutoff freq, reverb time, delay time, threshold...
        float param2 = 0.0f;    ///< Resonance, wet mix, feedback, ratio...
        float param3 = 0.0f;    ///< Additional parameter
        float wetDryMix = 0.5f; ///< Wet/dry blend [0 = dry, 1 = fully wet]
    };

    /**
     * @brief A single audio channel/bus in the mixer
     */
    struct AudioMixerChannel
    {
        std::string name = "Channel";
        std::string icon;

        /// Volume level [0.0 = silence, 1.0 = unity, >1.0 = boost]
        float volume = 1.0f;

        /// Stereo pan [-1.0 = full left, 0.0 = center, 1.0 = full right]
        float pan = 0.0f;

        /// Whether this channel is muted
        bool muted = false;

        /// Whether this channel is soloed (only soloed channels play)
        bool soloed = false;

        /// Output bus name (empty = master bus)
        std::string outputBus;

        /// Effect chain for this channel
        std::vector<AudioEffect> effects;

        /// Real-time peak level for VU meter display (dB)
        float peakLevelLeft = -60.0f;
        float peakLevelRight = -60.0f;

        /// RMS level for averaging display (dB)
        float rmsLevelLeft = -60.0f;
        float rmsLevelRight = -60.0f;

        /// Whether the channel is a bus (groups other channels)
        bool isBus = false;

        /// Child channels routed to this bus
        std::vector<std::string> inputChannels;

        /**
         * @brief Get effective volume considering mute and solo states.
         * @param anySoloed Whether any channel in the mixer is soloed.
         * @return Effective volume in [0, volume] range.
         */
        float GetEffectiveVolume(bool anySoloed) const
        {
            if (muted)
                return 0.0f;
            if (anySoloed && !soloed && !isBus)
                return 0.0f;
            return volume;
        }
    };

    /**
     * @brief Audio mixer snapshot (saved configuration)
     */
    struct AudioMixerSnapshot
    {
        std::string name;
        std::unordered_map<std::string, float> channelVolumes;
        std::unordered_map<std::string, float> channelPans;
        std::unordered_map<std::string, bool> channelMutes;
        float transitionTime = 1.0f; ///< Blend time when transitioning to this snapshot
    };

    // =========================================================================
    // AudioMixerPanel
    // =========================================================================

    /**
     * @brief Audio mixer panel with channel faders, effects, and metering
     *
     * Provides a mixing console interface for game audio:
     * - Channel strip faders with VU meters for Master, Music, SFX, Dialogue,
     *   Ambience, UI, and custom channels
     * - Per-channel mute/solo/pan controls
     * - Effect chains with audio processors (reverb, EQ, compressor, etc.)
     * - Bus routing (channels can output to sub-mix buses)
     * - Mixer snapshots for different game states (menu, gameplay, cutscene)
     * - Real-time waveform and spectrum visualization
     * - Volume attenuation curves for distance-based falloff
     */
    class AudioMixerPanel : public EditorPanel
    {
      public:
        AudioMixerPanel();
        ~AudioMixerPanel() override;

        // EditorPanel interface
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        std::string GetTypeName() const override { return "AudioMixerPanel"; }

        /**
         * @brief Add a new channel to the mixer
         * @param name Channel display name
         * @param isBus Whether the channel is a bus
         */
        void AddChannel(const std::string& name, bool isBus = false);

        /**
         * @brief Remove a channel from the mixer
         * @param name Channel name to remove
         */
        void RemoveChannel(const std::string& name);

        /**
         * @brief Set channel volume
         * @param channelName Channel to modify
         * @param volume Volume level [0, 2]
         */
        void SetChannelVolume(const std::string& channelName, float volume);

        /**
         * @brief Get channel volume
         * @param channelName Channel to query
         * @return Volume level, or -1 if channel not found
         */
        float GetChannelVolume(const std::string& channelName) const;

        /**
         * @brief Save current mixer state as a named snapshot
         * @param snapshotName Name for the snapshot
         */
        void SaveSnapshot(const std::string& snapshotName);

        /**
         * @brief Load a mixer snapshot with optional transition
         * @param snapshotName Name of the snapshot to load
         * @param transitionTime Blend time in seconds (0 = instant)
         */
        void LoadSnapshot(const std::string& snapshotName, float transitionTime = 0.0f);

        /**
         * @brief Get all channel names
         * @return Vector of channel names in display order
         */
        std::vector<std::string> GetChannelNames() const;

      private:
        // Render sub-sections
        void RenderToolbar();
        void RenderChannelStrip(AudioMixerChannel& channel, float stripWidth);
        void RenderVUMeter(float peakLeft, float peakRight, float rmsLeft, float rmsRight, float height);
        void RenderEffectChain(AudioMixerChannel& channel);
        void RenderEffectEditor(AudioEffect& effect);
        void RenderSnapshotManager();
        void RenderBusRouting();

        // Helpers
        void InitializeDefaultChannels();
        bool IsAnySoloed() const;
        float VolumeToDb(float volume) const;
        float DbToVolume(float db) const;
        void SimulateMeterLevels(float deltaTime);
        ImVec4 GetMeterColor(float db) const;
        const char* GetEffectTypeName(AudioEffectType type) const;

      private:
        std::vector<AudioMixerChannel> m_channels;
        std::vector<AudioMixerSnapshot> m_snapshots;
        int m_selectedChannelIndex = -1;

        // UI state
        bool m_showEffectChain = false;
        bool m_showSnapshotManager = false;
        bool m_showBusRouting = false;
        float m_channelStripWidth = 80.0f;

        // Meter simulation (editor preview)
        float m_meterDecayRate = 20.0f; ///< dB per second decay rate
        float m_meterUpdateTimer = 0.0f;
        static constexpr float METER_UPDATE_INTERVAL = 0.033f; ///< ~30 Hz meter refresh

        // Snapshot transition
        bool m_isTransitioning = false;
        float m_transitionProgress = 0.0f;
        float m_transitionDuration = 0.0f;
        std::string m_targetSnapshotName;
    };

} // namespace SparkEditor
