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
        const int parentIndices[] = {-1, 0, 0, 0, 0, 0}; // All route to Master
        for (int i = 0; i < 6; ++i)
        {
            MixBusInfo bus;
            snprintf(bus.name, sizeof(bus.name), "%s", busNames[i]);
            bus.parentBus = parentIndices[i];
            m_buses.push_back(bus);
        }
        return true;
    }

    void AudioMixerPanel::Update(float /*deltaTime*/)
    {
        // Simulate VU meter decay for visual feedback
        for (auto& bus : m_buses)
        {
            bus.peakLevel *= 0.95f;
            if (bus.peakLevel < 0.01f)
                bus.peakLevel = 0.0f;
        }
    }

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
                if (ImGui::BeginTabItem(ICON_FA_DATABASE " Sound Banks"))
                {
                    RenderSoundBanks();
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

        // Listener position
        ImGui::Separator();
        ImGui::Text(ICON_FA_HEADPHONES " Listener Position: (%.1f, %.1f, %.1f)", m_listenerPos[0], m_listenerPos[1],
                    m_listenerPos[2]);
        ImGui::Text("Forward: (%.2f, %.2f, %.2f)", m_listenerFwd[0], m_listenerFwd[1], m_listenerFwd[2]);
    }

    void AudioMixerPanel::RenderMixBuses()
    {
        if (ImGui::BeginTable("MixBusTable", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Bus", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Volume");
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Solo", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Route", ImGuiTableColumnFlags_WidthFixed, 70);
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

                // VU meter
                ImGui::TableNextColumn();
                ImVec4 meterColor = bus.peakLevel > 0.9f   ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
                                    : bus.peakLevel > 0.7f ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                                           : ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, meterColor);
                ImGui::ProgressBar(bus.peakLevel, ImVec2(-1, 0));
                ImGui::PopStyleColor();

                ImGui::TableNextColumn();
                ImGui::Checkbox("##mute", &bus.muted);

                ImGui::TableNextColumn();
                ImGui::Checkbox("##solo", &bus.solo);

                ImGui::TableNextColumn();
                if (bus.parentBus >= 0 && bus.parentBus < static_cast<int>(m_buses.size()))
                    ImGui::TextDisabled("-> %s", m_buses[bus.parentBus].name);
                else
                    ImGui::TextDisabled("(root)");

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        // DSP effects for selected bus
        if (m_selectedBus >= 0 && m_selectedBus < static_cast<int>(m_buses.size()))
        {
            auto& bus = m_buses[m_selectedBus];
            ImGui::Separator();
            ImGui::Text(ICON_FA_SLIDERS " DSP Effects: %s", bus.name);

            ImGui::Checkbox("Reverb", &bus.reverbEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("EQ", &bus.eqEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("Compressor", &bus.compressorEnabled);
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

        if (ImGui::BeginTable("ActiveSoundsTable", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Sound");
            ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("3D", ImGuiTableColumnFlags_WidthFixed, 30);
            ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed, 30);
            ImGui::TableSetupColumn("Position");
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
                ImGui::TableNextColumn();
                if (sound.is3D)
                    ImGui::Text("(%.1f, %.1f, %.1f)", sound.position[0], sound.position[1], sound.position[2]);
                else
                    ImGui::TextDisabled("-");
            }

            ImGui::EndTable();
        }
    }

    void AudioMixerPanel::RenderSoundBanks()
    {
        ImGui::Text("Sound Banks: %d", static_cast<int>(m_soundBanks.size()));
        ImGui::Separator();

        if (m_soundBanks.empty())
        {
            ImGui::TextDisabled("No sound banks loaded.");
            ImGui::TextDisabled("Sound banks are loaded automatically from the project's Audio/ directory.");
            return;
        }

        if (ImGui::BeginTable("SoundBankTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Bank Name");
            ImGui::TableSetupColumn("Sounds", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (const auto& bank : m_soundBanks)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bank.name);
                ImGui::TableNextColumn();
                ImGui::Text("%d", bank.soundCount);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f KB", bank.sizeKB);
                ImGui::TableNextColumn();
                ImGui::TextColored(bank.loaded ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   bank.loaded ? ICON_FA_CHECK : ICON_FA_TIMES);
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

        if (ImGui::Button(ICON_FA_PLUS " Add Reverb Zone Entity"))
        {
            ImGui::TextDisabled("Creates a new entity with a ReverbZone component");
        }
    }

} // namespace SparkEditor
