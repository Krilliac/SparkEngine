/**
 * @file AudioMixerPanel.cpp
 * @brief Implementation of the audio mixer management panel
 */

#include "AudioMixerPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>

namespace SparkEditor
{

    AudioMixerPanel::AudioMixerPanel() : EditorPanel("Audio Mixer", "audio_mixer_panel") {}

    bool AudioMixerPanel::Initialize()
    {
        std::cout << "Initializing Audio Mixer panel\n";

        // Default mix buses matching engine AudioBus enum
        const char* busNames[] = {"Master", "SFX", "Music", "Voice", "Ambient", "UI"};
        for (const char* name : busNames)
        {
            MixBusInfo bus;
            snprintf(bus.name, sizeof(bus.name), "%s", name);
            m_buses.push_back(bus);
        }
        return true;
    }

    void AudioMixerPanel::Update(float /*deltaTime*/) {}

    void AudioMixerPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("AudioMixerTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_VOLUME_UP " Volumes"))
                {
                    RenderVolumeControls();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_SLIDERS " Mix Buses"))
                {
                    RenderMixBuses();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_PLAY " Active Sounds"))
                {
                    RenderActiveSounds();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GLOBE " Reverb Zones"))
                {
                    RenderReverbZones();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void AudioMixerPanel::Shutdown()
    {
        std::cout << "Shutting down Audio Mixer panel\n";
    }

    void AudioMixerPanel::RenderVolumeControls()
    {
        ImGui::Text("Master Volume Controls");
        ImGui::Separator();

        ImGui::SliderFloat("Master", &m_masterVolume, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("SFX", &m_sfxVolume, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Music", &m_musicVolume, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Voice", &m_voiceVolume, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Ambient", &m_ambientVolume, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        ImGui::Text("Active Sources: %d | Loaded Sounds: %d", m_activeSoundCount, m_loadedSoundCount);
    }

    void AudioMixerPanel::RenderMixBuses()
    {
        if (ImGui::BeginTable("MixBusTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Bus", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Volume");
            ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Solo", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_buses.size()); ++i)
            {
                auto& bus = m_buses[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                bool selected = (m_selectedBus == i);
                if (ImGui::Selectable(bus.name, selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedBus = i;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##vol", &bus.volume, 0.0f, 1.0f, "%.2f");

                ImGui::TableNextColumn();
                ImGui::Checkbox("##mute", &bus.muted);

                ImGui::TableNextColumn();
                ImGui::Checkbox("##solo", &bus.solo);

                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    }

    void AudioMixerPanel::RenderActiveSounds()
    {
        ImGui::Text("Active Sound Sources");
        ImGui::Separator();

        if (m_activeSounds.empty())
        {
            ImGui::TextDisabled("No active sounds (enter Play mode to monitor)");
            return;
        }

        if (ImGui::BeginTable("ActiveSoundsTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Sound");
            ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("3D", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableHeadersRow();

            for (const auto& sound : m_activeSounds)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sound.name);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", sound.volume);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sound.is3D ? ICON_FA_CHECK : "-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sound.looping ? ICON_FA_CHECK : "-");
            }

            ImGui::EndTable();
        }
    }

    void AudioMixerPanel::RenderReverbZones()
    {
        ImGui::Checkbox("Show Reverb Zones in Viewport", &m_showReverbZones);
        ImGui::Separator();

        ImGui::TextDisabled("Reverb zones are configured per-scene.");
        ImGui::TextDisabled("Use the Inspector to edit zone properties on selected entities.");

        if (ImGui::Button(ICON_FA_PLUS " Add Reverb Zone"))
        {
            // Placeholder — would create a reverb zone entity in the scene
        }
    }

} // namespace SparkEditor
