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
     * Provides master/SFX/music volume controls, mix bus hierarchy,
     * active sound source monitoring, and reverb zone configuration.
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
            bool muted = false;
            bool solo = false;
        };

        struct ActiveSoundInfo
        {
            char name[128] = {};
            float volume = 1.0f;
            bool is3D = false;
            bool looping = false;
            float position[3] = {};
        };

        void RenderVolumeControls();
        void RenderMixBuses();
        void RenderActiveSounds();
        void RenderReverbZones();

        float m_masterVolume = 1.0f;
        float m_sfxVolume = 1.0f;
        float m_musicVolume = 1.0f;
        float m_voiceVolume = 1.0f;
        float m_ambientVolume = 1.0f;

        std::vector<MixBusInfo> m_buses;
        std::vector<ActiveSoundInfo> m_activeSounds;
        int m_selectedBus = -1;
        int m_activeSoundCount = 0;
        int m_loadedSoundCount = 0;
        bool m_showReverbZones = false;
    };

} // namespace SparkEditor
