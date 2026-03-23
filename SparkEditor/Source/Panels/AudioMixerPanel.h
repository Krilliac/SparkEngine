/**
 * @file AudioMixerPanel.h
 * @brief Audio mixer and sound management panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for managing audio buses, volumes, and sound playback
     *
     * Provides master/SFX/music volume controls, mix bus hierarchy with
     * VU meters, DSP effect toggles, active sound monitoring, sound bank
     * browsing, and reverb zone configuration.
     */
    class AudioMixerPanel : public EditorPanel
    {
      public:
        AudioMixerPanel();
        ~AudioMixerPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct MixBusInfo
        {
            char name[64] = {};
            float volume = 1.0f;
            float peakLevel = 0.0f; // simulated VU meter level [0, 1]
            bool muted = false;
            bool solo = false;
            int parentBus = -1; // -1 = root (Master)

            // DSP effects
            bool reverbEnabled = false;
            bool eqEnabled = false;
            bool compressorEnabled = false;
        };

        struct ActiveSoundInfo
        {
            char name[128] = {};
            float volume = 1.0f;
            bool is3D = false;
            bool looping = false;
            float position[3] = {};
        };

        struct SoundBankEntry
        {
            char name[128] = {};
            int soundCount = 0;
            float sizeKB = 0.0f;
            bool loaded = false;
        };

        void RenderVolumeControls();
        void RenderMixBuses();
        void RenderActiveSounds();
        void RenderReverbZones();
        void RenderSoundBanks();

        float m_masterVolume = 1.0f;
        float m_sfxVolume = 1.0f;
        float m_musicVolume = 1.0f;
        float m_voiceVolume = 1.0f;
        float m_ambientVolume = 1.0f;

        std::vector<MixBusInfo> m_buses;
        std::vector<ActiveSoundInfo> m_activeSounds;
        std::vector<SoundBankEntry> m_soundBanks;
        int m_selectedBus = -1;
        int m_activeSoundCount = 0;
        int m_loadedSoundCount = 0;
        bool m_showReverbZones = false;

        // Listener info
        float m_listenerPos[3] = {};
        float m_listenerFwd[3] = {0.0f, 0.0f, 1.0f};
    };

} // namespace SparkEditor
