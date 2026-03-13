/**
 * @file AudioMixerPanel.cpp
 * @brief Implementation of the audio mixer panel for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a visual audio mixing console with channel faders, bus routing,
 * effects chains, and real-time level metering for game audio configuration.
 */

// Standard library headers first (MSVC /Zc:preprocessor compatibility)
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Project and third-party headers
#include "AudioMixerPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    // =========================================================================
    // File-scope constants
    // =========================================================================

    static constexpr float MIN_DB = -60.0f;
    static constexpr float MAX_DB = 6.0f;
    static constexpr float YELLOW_THRESHOLD_DB = -12.0f;
    static constexpr float RED_THRESHOLD_DB = -3.0f;
    static constexpr float DEFAULT_STRIP_WIDTH = 80.0f;

    static char s_newChannelNameBuffer[128] = "";
    static char s_snapshotNameBuffer[128] = "";

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================

    AudioMixerPanel::AudioMixerPanel() : EditorPanel("Audio Mixer", "audio_mixer_panel")
    {
        SetTitle(std::string(ICON_FA_VOLUME_UP) + "  Audio Mixer");
        SetIcon(ICON_FA_VOLUME_UP);
        m_channelStripWidth = DEFAULT_STRIP_WIDTH;
    }

    AudioMixerPanel::~AudioMixerPanel()
    {
        Shutdown();
    }

    // =========================================================================
    // EditorPanel interface
    // =========================================================================

    bool AudioMixerPanel::Initialize()
    {
        SPARK_TRACE_ENTER(LogCategory::Audio);
        if (m_isInitialized)
        {
            return true;
        }

        InitializeDefaultChannels();

        m_isInitialized = true;
        return true;
    }

    void AudioMixerPanel::Update(float deltaTime)
    {
        if (!m_isVisible)
        {
            return;
        }

        // Simulate VU meter levels for editor preview
        SimulateMeterLevels(deltaTime);

        // Handle snapshot transitions
        if (m_isTransitioning && m_transitionDuration > 0.0f)
        {
            m_transitionProgress += deltaTime / m_transitionDuration;
            if (m_transitionProgress >= 1.0f)
            {
                m_transitionProgress = 1.0f;
                m_isTransitioning = false;

                // Apply final snapshot values
                for (const auto& snapshot : m_snapshots)
                {
                    if (snapshot.name == m_targetSnapshotName)
                    {
                        for (auto& channel : m_channels)
                        {
                            auto volIt = snapshot.channelVolumes.find(channel.name);
                            if (volIt != snapshot.channelVolumes.end())
                            {
                                channel.volume = volIt->second;
                            }
                            auto panIt = snapshot.channelPans.find(channel.name);
                            if (panIt != snapshot.channelPans.end())
                            {
                                channel.pan = panIt->second;
                            }
                            auto muteIt = snapshot.channelMutes.find(channel.name);
                            if (muteIt != snapshot.channelMutes.end())
                            {
                                channel.muted = muteIt->second;
                            }
                        }
                        break;
                    }
                }
            }
            else
            {
                // Interpolate toward target snapshot
                for (const auto& snapshot : m_snapshots)
                {
                    if (snapshot.name == m_targetSnapshotName)
                    {
                        float t = m_transitionProgress;
                        for (auto& channel : m_channels)
                        {
                            auto volIt = snapshot.channelVolumes.find(channel.name);
                            if (volIt != snapshot.channelVolumes.end())
                            {
                                float target = volIt->second;
                                channel.volume = channel.volume + (target - channel.volume) * t;
                            }
                            auto panIt = snapshot.channelPans.find(channel.name);
                            if (panIt != snapshot.channelPans.end())
                            {
                                float target = panIt->second;
                                channel.pan = channel.pan + (target - channel.pan) * t;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    void AudioMixerPanel::Render()
    {
        if (!m_isVisible)
        {
            return;
        }

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderToolbar();

        ImGui::Separator();

        // Main mixer area — horizontal scroll for channel strips
        float availableHeight = ImGui::GetContentRegionAvail().y;
        float sidebarWidth = 0.0f;

        // Reserve space for optional side panels
        if (m_showEffectChain && m_selectedChannelIndex >= 0)
        {
            sidebarWidth = 280.0f;
        }

        // Channel strips region
        float channelAreaWidth = ImGui::GetContentRegionAvail().x - sidebarWidth;
        if (channelAreaWidth < 100.0f)
        {
            channelAreaWidth = 100.0f;
        }

        ImGui::BeginChild("##ChannelStrips", ImVec2(channelAreaWidth, availableHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        bool anySoloed = IsAnySoloed();
        for (size_t i = 0; i < m_channels.size(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }

            ImGui::PushID(static_cast<int>(i));

            // Highlight selected channel
            bool isSelected = (static_cast<int>(i) == m_selectedChannelIndex);
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.22f, 0.30f, 1.0f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.14f, 0.16f, 1.0f));
            }

            // Dim channels that are effectively muted by solo
            bool isDimmed = anySoloed && !m_channels[i].soloed && !m_channels[i].isBus;
            if (isDimmed || m_channels[i].muted)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            }

            RenderChannelStrip(m_channels[i], m_channelStripWidth);

            if (isDimmed || m_channels[i].muted)
            {
                ImGui::PopStyleVar();
            }

            ImGui::PopStyleColor();

            // Select channel on click
            if (ImGui::IsItemClicked())
            {
                m_selectedChannelIndex = static_cast<int>(i);
            }

            ImGui::PopID();
        }

        ImGui::EndChild();

        // Side panel: effect chain editor
        if (m_showEffectChain && m_selectedChannelIndex >= 0 &&
            m_selectedChannelIndex < static_cast<int>(m_channels.size()))
        {
            ImGui::SameLine();
            ImGui::BeginChild("##EffectChainPanel", ImVec2(sidebarWidth, availableHeight), true);
            RenderEffectChain(m_channels[m_selectedChannelIndex]);
            ImGui::EndChild();
        }

        // Popup panels
        if (m_showSnapshotManager)
        {
            RenderSnapshotManager();
        }

        if (m_showBusRouting)
        {
            RenderBusRouting();
        }

        EndPanel();
    }

    void AudioMixerPanel::Shutdown()
    {
        m_channels.clear();
        m_snapshots.clear();
        m_selectedChannelIndex = -1;
        m_isTransitioning = false;
        m_isInitialized = false;
    }

    // =========================================================================
    // Public API
    // =========================================================================

    void AudioMixerPanel::AddChannel(const std::string& name, bool isBus)
    {
        // Prevent duplicate names
        for (const auto& ch : m_channels)
        {
            if (ch.name == name)
            {
                std::cout << "[AudioMixer] Channel '" << name << "' already exists.\n";
                return;
            }
        }

        AudioMixerChannel channel;
        channel.name = name;
        channel.isBus = isBus;
        channel.volume = 1.0f;
        channel.pan = 0.0f;
        channel.icon = isBus ? ICON_FA_SITEMAP : ICON_FA_VOLUME_UP;

        // Insert before master (master is always first)
        if (!m_channels.empty() && m_channels[0].name == "Master")
        {
            m_channels.insert(m_channels.begin() + 1, channel);
        }
        else
        {
            m_channels.push_back(channel);
        }

        SetModified(true);
        NotifyStateChange();
    }

    void AudioMixerPanel::RemoveChannel(const std::string& name)
    {
        // Do not allow removing the master channel
        if (name == "Master")
        {
            std::cout << "[AudioMixer] Cannot remove Master channel.\n";
            return;
        }

        auto it = std::find_if(m_channels.begin(), m_channels.end(),
                               [&name](const AudioMixerChannel& ch) { return ch.name == name; });

        if (it != m_channels.end())
        {
            int removedIndex = static_cast<int>(std::distance(m_channels.begin(), it));
            m_channels.erase(it);

            // Adjust selection
            if (m_selectedChannelIndex >= static_cast<int>(m_channels.size()))
            {
                m_selectedChannelIndex = static_cast<int>(m_channels.size()) - 1;
            }
            else if (m_selectedChannelIndex == removedIndex)
            {
                m_selectedChannelIndex = -1;
            }

            SetModified(true);
            NotifyStateChange();
        }
    }

    void AudioMixerPanel::SetChannelVolume(const std::string& channelName, float volume)
    {
        for (auto& channel : m_channels)
        {
            if (channel.name == channelName)
            {
                channel.volume = std::clamp(volume, 0.0f, 2.0f);
                SetModified(true);
                return;
            }
        }
    }

    float AudioMixerPanel::GetChannelVolume(const std::string& channelName) const
    {
        for (const auto& channel : m_channels)
        {
            if (channel.name == channelName)
            {
                return channel.volume;
            }
        }
        return -1.0f;
    }

    void AudioMixerPanel::SaveSnapshot(const std::string& snapshotName)
    {
        // Overwrite existing snapshot with the same name
        for (auto& snapshot : m_snapshots)
        {
            if (snapshot.name == snapshotName)
            {
                snapshot.channelVolumes.clear();
                snapshot.channelPans.clear();
                snapshot.channelMutes.clear();
                for (const auto& ch : m_channels)
                {
                    snapshot.channelVolumes[ch.name] = ch.volume;
                    snapshot.channelPans[ch.name] = ch.pan;
                    snapshot.channelMutes[ch.name] = ch.muted;
                }
                return;
            }
        }

        // Create new snapshot
        AudioMixerSnapshot snapshot;
        snapshot.name = snapshotName;
        for (const auto& ch : m_channels)
        {
            snapshot.channelVolumes[ch.name] = ch.volume;
            snapshot.channelPans[ch.name] = ch.pan;
            snapshot.channelMutes[ch.name] = ch.muted;
        }
        m_snapshots.push_back(snapshot);
    }

    void AudioMixerPanel::LoadSnapshot(const std::string& snapshotName, float transitionTime)
    {
        // Verify snapshot exists
        bool found = false;
        for (const auto& snapshot : m_snapshots)
        {
            if (snapshot.name == snapshotName)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            std::cout << "[AudioMixer] Snapshot '" << snapshotName << "' not found.\n";
            return;
        }

        if (transitionTime <= 0.0f)
        {
            // Instant load
            for (const auto& snapshot : m_snapshots)
            {
                if (snapshot.name == snapshotName)
                {
                    for (auto& channel : m_channels)
                    {
                        auto volIt = snapshot.channelVolumes.find(channel.name);
                        if (volIt != snapshot.channelVolumes.end())
                        {
                            channel.volume = volIt->second;
                        }
                        auto panIt = snapshot.channelPans.find(channel.name);
                        if (panIt != snapshot.channelPans.end())
                        {
                            channel.pan = panIt->second;
                        }
                        auto muteIt = snapshot.channelMutes.find(channel.name);
                        if (muteIt != snapshot.channelMutes.end())
                        {
                            channel.muted = muteIt->second;
                        }
                    }
                    break;
                }
            }
        }
        else
        {
            // Begin timed transition
            m_isTransitioning = true;
            m_transitionProgress = 0.0f;
            m_transitionDuration = transitionTime;
            m_targetSnapshotName = snapshotName;
        }
    }

    std::vector<std::string> AudioMixerPanel::GetChannelNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_channels.size());
        for (const auto& channel : m_channels)
        {
            names.push_back(channel.name);
        }
        return names;
    }

    // =========================================================================
    // Initialization
    // =========================================================================

    void AudioMixerPanel::InitializeDefaultChannels()
    {
        m_channels.clear();

        auto addDefault = [this](const std::string& name, const char* icon, bool isBus, float defaultVol)
        {
            AudioMixerChannel ch;
            ch.name = name;
            ch.icon = icon;
            ch.isBus = isBus;
            ch.volume = defaultVol;
            ch.pan = 0.0f;
            m_channels.push_back(ch);
        };

        addDefault("Master", ICON_FA_VOLUME_UP, true, 1.0f);
        addDefault("Music", ICON_FA_FILM, false, 0.8f);
        addDefault("SFX", ICON_FA_BOLT, false, 1.0f);
        addDefault("Dialogue", ICON_FA_USER, false, 1.0f);
        addDefault("Ambience", ICON_FA_GLOBE, false, 0.7f);
        addDefault("UI", ICON_FA_GAMEPAD, false, 0.9f);

        // Route non-master channels to Master bus
        for (size_t i = 1; i < m_channels.size(); ++i)
        {
            m_channels[i].outputBus = "Master";
        }

        // Register child inputs on the Master bus
        for (size_t i = 1; i < m_channels.size(); ++i)
        {
            m_channels[0].inputChannels.push_back(m_channels[i].name);
        }
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void AudioMixerPanel::RenderToolbar()
    {
        // Add channel button
        if (ImGui::Button(ICON_FA_PLUS " Add Channel"))
        {
            ImGui::OpenPopup("##AddChannelPopup");
        }

        if (ImGui::BeginPopup("##AddChannelPopup"))
        {
            ImGui::Text("Channel Name:");
            ImGui::InputText("##NewChannelName", s_newChannelNameBuffer, sizeof(s_newChannelNameBuffer));

            static bool newChannelIsBus = false;
            ImGui::Checkbox("Bus", &newChannelIsBus);

            if (ImGui::Button("Add") && s_newChannelNameBuffer[0] != '\0')
            {
                AddChannel(s_newChannelNameBuffer, newChannelIsBus);
                s_newChannelNameBuffer[0] = '\0';
                newChannelIsBus = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        // Remove selected channel
        bool canRemove = m_selectedChannelIndex > 0 && m_selectedChannelIndex < static_cast<int>(m_channels.size());
        if (!canRemove)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(ICON_FA_MINUS " Remove"))
        {
            if (canRemove)
            {
                RemoveChannel(m_channels[m_selectedChannelIndex].name);
            }
        }
        if (!canRemove)
        {
            ImGui::EndDisabled();
        }

        SparkEditor::VerticalSeparator();

        // Toggle snapshot manager
        if (ImGui::Button(ICON_FA_SAVE " Snapshots"))
        {
            m_showSnapshotManager = !m_showSnapshotManager;
        }

        ImGui::SameLine();

        // Toggle bus routing view
        if (ImGui::Button(ICON_FA_SITEMAP " Bus Routing"))
        {
            m_showBusRouting = !m_showBusRouting;
        }

        ImGui::SameLine();

        // Toggle effect chain sidebar
        if (ImGui::Button(ICON_FA_SLIDERS " Effects"))
        {
            m_showEffectChain = !m_showEffectChain;
        }

        SparkEditor::VerticalSeparator();

        // Strip width slider
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderFloat("##StripWidth", &m_channelStripWidth, 60.0f, 140.0f, "Width: %.0f");

        // Transition indicator
        if (m_isTransitioning)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), ICON_FA_SPINNER " Transitioning to '%s' (%.0f%%)",
                               m_targetSnapshotName.c_str(), m_transitionProgress * 100.0f);
        }
    }

    // =========================================================================
    // Channel Strip
    // =========================================================================

    void AudioMixerPanel::RenderChannelStrip(AudioMixerChannel& channel, float stripWidth)
    {
        float stripHeight = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild(("##Strip_" + channel.name).c_str(), ImVec2(stripWidth, stripHeight), true,
                          ImGuiWindowFlags_NoScrollbar);

        // Channel name header
        float textWidth = ImGui::CalcTextSize(channel.name.c_str()).x;
        float centeredX = (stripWidth - textWidth) * 0.5f;
        if (centeredX > 0.0f)
        {
            ImGui::SetCursorPosX(centeredX);
        }

        if (!channel.icon.empty())
        {
            ImGui::Text("%s %s", channel.icon.c_str(), channel.name.c_str());
        }
        else
        {
            ImGui::Text("%s", channel.name.c_str());
        }

        if (channel.isBus)
        {
            float busLabelWidth = ImGui::CalcTextSize("[BUS]").x;
            ImGui::SetCursorPosX((stripWidth - busLabelWidth) * 0.5f);
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "[BUS]");
        }

        ImGui::Separator();

        // --- dB readout ---
        float db = VolumeToDb(channel.volume);
        char dbText[32];
        if (channel.volume <= 0.0001f)
        {
            snprintf(dbText, sizeof(dbText), "-inf dB");
        }
        else
        {
            snprintf(dbText, sizeof(dbText), "%.1f dB", db);
        }
        float dbTextWidth = ImGui::CalcTextSize(dbText).x;
        ImGui::SetCursorPosX((stripWidth - dbTextWidth) * 0.5f);
        ImGui::TextColored(db > 0.0f ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", dbText);

        // --- Volume fader (vertical slider) ---
        float faderHeight = stripHeight - 220.0f;
        if (faderHeight < 60.0f)
        {
            faderHeight = 60.0f;
        }

        float faderWidth = stripWidth - 40.0f;
        if (faderWidth < 20.0f)
        {
            faderWidth = 20.0f;
        }

        // Center the fader + meter horizontally
        float meterWidth = 16.0f;
        float totalWidth = faderWidth * 0.4f + 4.0f + meterWidth;
        float startX = (stripWidth - totalWidth) * 0.5f;
        ImGui::SetCursorPosX(startX);

        // Fader slider (vertical)
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.2f, 0.2f, 0.24f, 1.0f));

        ImGui::VSliderFloat(("##Fader_" + channel.name).c_str(), ImVec2(faderWidth * 0.4f, faderHeight),
                            &channel.volume, 0.0f, 2.0f, "");

        ImGui::PopStyleColor(4);

        // Tooltip with precise value
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s: %.2f (%.1f dB)", channel.name.c_str(), channel.volume, VolumeToDb(channel.volume));
        }
        ImGui::EndGroup();

        // VU meter beside fader
        ImGui::SameLine(0.0f, 4.0f);
        RenderVUMeter(channel.peakLevelLeft, channel.peakLevelRight, channel.rmsLevelLeft, channel.rmsLevelRight,
                      faderHeight);

        // --- Pan knob ---
        ImGui::Spacing();
        ImGui::Text("Pan");
        ImGui::SetNextItemWidth(stripWidth - 16.0f);
        ImGui::SliderFloat(("##Pan_" + channel.name).c_str(), &channel.pan, -1.0f, 1.0f, "%.2f");

        if (ImGui::IsItemHovered())
        {
            const char* panLabel = "C";
            if (channel.pan < -0.01f)
            {
                panLabel = "L";
            }
            else if (channel.pan > 0.01f)
            {
                panLabel = "R";
            }
            ImGui::SetTooltip("Pan: %.0f%% %s", std::abs(channel.pan) * 100.0f, panLabel);
        }

        // Double-click to reset pan to center
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        {
            channel.pan = 0.0f;
        }

        ImGui::Spacing();

        // --- Mute / Solo buttons ---
        float buttonWidth = (stripWidth - 24.0f) * 0.5f;
        if (buttonWidth < 20.0f)
        {
            buttonWidth = 20.0f;
        }

        // Mute button
        if (channel.muted)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        }

        if (ImGui::Button(("M##Mute_" + channel.name).c_str(), ImVec2(buttonWidth, 0.0f)))
        {
            channel.muted = !channel.muted;
            SetModified(true);
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // Solo button
        if (channel.soloed)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.7f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.8f, 0.2f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        }

        if (ImGui::Button(("S##Solo_" + channel.name).c_str(), ImVec2(buttonWidth, 0.0f)))
        {
            channel.soloed = !channel.soloed;
            SetModified(true);
        }
        ImGui::PopStyleColor(2);

        // --- Output bus label ---
        if (!channel.outputBus.empty())
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), ICON_FA_CHEVRON_RIGHT " %s", channel.outputBus.c_str());
        }

        ImGui::EndChild();
    }

    // =========================================================================
    // VU Meter
    // =========================================================================

    void AudioMixerPanel::RenderVUMeter(float peakLeft, float peakRight, float rmsLeft, float rmsRight, float height)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();

        float meterBarWidth = 6.0f;
        float meterGap = 2.0f;
        float totalWidth = meterBarWidth * 2.0f + meterGap;

        // Background
        drawList->AddRectFilled(pos, ImVec2(pos.x + totalWidth, pos.y + height), IM_COL32(20, 20, 24, 255));

        // Draw dB scale ticks on the side
        float dbRange = MAX_DB - MIN_DB;
        float tickValues[] = {0.0f, -6.0f, -12.0f, -24.0f, -48.0f};
        for (float tickDb : tickValues)
        {
            float normalized = (tickDb - MIN_DB) / dbRange;
            float yPos = pos.y + height - (normalized * height);
            drawList->AddLine(ImVec2(pos.x, yPos), ImVec2(pos.x + totalWidth, yPos), IM_COL32(80, 80, 80, 200), 1.0f);
        }

        // Helper lambda: draw one meter bar with peak + RMS
        auto drawBar = [&](float xOffset, float peakDb, float rmsDb)
        {
            float peakNorm = std::clamp((peakDb - MIN_DB) / dbRange, 0.0f, 1.0f);
            float rmsNorm = std::clamp((rmsDb - MIN_DB) / dbRange, 0.0f, 1.0f);

            float peakHeight = peakNorm * height;
            float rmsHeight = rmsNorm * height;

            float barX = pos.x + xOffset;

            // Draw the RMS filled bar in segments (green -> yellow -> red)
            float segmentBottomY = pos.y + height;

            // Green zone: MIN_DB to YELLOW_THRESHOLD_DB
            float greenTopNorm = std::clamp((YELLOW_THRESHOLD_DB - MIN_DB) / dbRange, 0.0f, 1.0f);
            float greenFillNorm = std::min(rmsNorm, greenTopNorm);
            if (greenFillNorm > 0.0f)
            {
                float greenTopY = segmentBottomY - greenFillNorm * height;
                drawList->AddRectFilled(ImVec2(barX, greenTopY), ImVec2(barX + meterBarWidth, segmentBottomY),
                                        IM_COL32(30, 200, 60, 255));
            }

            // Yellow zone: YELLOW_THRESHOLD_DB to RED_THRESHOLD_DB
            if (rmsNorm > greenTopNorm)
            {
                float yellowTopNorm = std::clamp((RED_THRESHOLD_DB - MIN_DB) / dbRange, 0.0f, 1.0f);
                float yellowFillNorm = std::min(rmsNorm, yellowTopNorm);
                float yellowBottomY = segmentBottomY - greenTopNorm * height;
                float yellowTopY = segmentBottomY - yellowFillNorm * height;
                drawList->AddRectFilled(ImVec2(barX, yellowTopY), ImVec2(barX + meterBarWidth, yellowBottomY),
                                        IM_COL32(220, 200, 30, 255));
            }

            // Red zone: RED_THRESHOLD_DB to MAX_DB
            float redBottomNorm = std::clamp((RED_THRESHOLD_DB - MIN_DB) / dbRange, 0.0f, 1.0f);
            if (rmsNorm > redBottomNorm)
            {
                float redBottomY = segmentBottomY - redBottomNorm * height;
                float redTopY = segmentBottomY - rmsNorm * height;
                drawList->AddRectFilled(ImVec2(barX, redTopY), ImVec2(barX + meterBarWidth, redBottomY),
                                        IM_COL32(230, 40, 40, 255));
            }

            // Peak indicator line (thin bright line at peak level)
            if (peakHeight > 1.0f)
            {
                float peakY = segmentBottomY - peakHeight;
                ImVec4 peakColor = GetMeterColor(peakDb);
                ImU32 peakCol = IM_COL32(static_cast<int>(peakColor.x * 255), static_cast<int>(peakColor.y * 255),
                                         static_cast<int>(peakColor.z * 255), 255);
                drawList->AddRectFilled(ImVec2(barX, peakY), ImVec2(barX + meterBarWidth, peakY + 2.0f), peakCol);
            }
        };

        // Draw left and right bars
        drawBar(0.0f, peakLeft, rmsLeft);
        drawBar(meterBarWidth + meterGap, peakRight, rmsRight);

        // Advance cursor past the meter
        ImGui::Dummy(ImVec2(totalWidth, height));
    }

    // =========================================================================
    // Effect Chain
    // =========================================================================

    void AudioMixerPanel::RenderEffectChain(AudioMixerChannel& channel)
    {
        ImGui::Text(ICON_FA_SLIDERS " Effects: %s", channel.name.c_str());
        ImGui::Separator();

        if (channel.effects.empty())
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No effects in chain.");
        }

        int removeIndex = -1;

        for (size_t i = 0; i < channel.effects.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));

            auto& effect = channel.effects[i];

            // Enable/disable toggle
            ImGui::Checkbox(("##Enable_" + std::to_string(i)).c_str(), &effect.enabled);
            ImGui::SameLine();

            // Effect header — click to expand/collapse
            const char* typeName = GetEffectTypeName(effect.type);
            std::string label = effect.name.empty() ? typeName : effect.name;

            if (!effect.enabled)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            }

            bool expanded = ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            effect.expanded = expanded;

            if (!effect.enabled)
            {
                ImGui::PopStyleVar();
            }

            if (expanded)
            {
                ImGui::Indent(16.0f);
                RenderEffectEditor(effect);

                // Remove button
                if (ImGui::Button(ICON_FA_TRASH " Remove##effect"))
                {
                    removeIndex = static_cast<int>(i);
                }
                ImGui::Unindent(16.0f);
            }

            ImGui::PopID();
        }

        // Deferred removal
        if (removeIndex >= 0 && removeIndex < static_cast<int>(channel.effects.size()))
        {
            channel.effects.erase(channel.effects.begin() + removeIndex);
            SetModified(true);
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Add effect dropdown
        if (ImGui::Button(ICON_FA_PLUS " Add Effect"))
        {
            ImGui::OpenPopup("##AddEffectPopup");
        }

        if (ImGui::BeginPopup("##AddEffectPopup"))
        {
            struct EffectEntry
            {
                AudioEffectType type;
                const char* name;
            };

            static const EffectEntry effectTypes[] = {
                {AudioEffectType::LowPass, "Low-Pass Filter"}, {AudioEffectType::HighPass, "High-Pass Filter"},
                {AudioEffectType::Reverb, "Reverb"},           {AudioEffectType::Delay, "Delay"},
                {AudioEffectType::Compressor, "Compressor"},   {AudioEffectType::Limiter, "Limiter"},
                {AudioEffectType::EQ3Band, "3-Band EQ"},       {AudioEffectType::Distortion, "Distortion"},
                {AudioEffectType::Chorus, "Chorus"},           {AudioEffectType::Panning, "Panning"},
            };

            for (const auto& entry : effectTypes)
            {
                if (ImGui::Selectable(entry.name))
                {
                    AudioEffect newEffect;
                    newEffect.type = entry.type;
                    newEffect.name = entry.name;
                    newEffect.enabled = true;

                    // Set sensible defaults per type
                    switch (entry.type)
                    {
                    case AudioEffectType::LowPass:
                        newEffect.param1 = 5000.0f;
                        newEffect.param2 = 0.707f;
                        break;
                    case AudioEffectType::HighPass:
                        newEffect.param1 = 200.0f;
                        newEffect.param2 = 0.707f;
                        break;
                    case AudioEffectType::Reverb:
                        newEffect.param1 = 2.0f;
                        newEffect.param2 = 0.5f;
                        newEffect.wetDryMix = 0.3f;
                        break;
                    case AudioEffectType::Delay:
                        newEffect.param1 = 0.25f;
                        newEffect.param2 = 0.4f;
                        newEffect.wetDryMix = 0.3f;
                        break;
                    case AudioEffectType::Compressor:
                        newEffect.param1 = -20.0f;
                        newEffect.param2 = 4.0f;
                        newEffect.param3 = 6.0f;
                        break;
                    case AudioEffectType::Limiter:
                        newEffect.param1 = -1.0f;
                        newEffect.param2 = 0.0f;
                        break;
                    case AudioEffectType::EQ3Band:
                        newEffect.param1 = 0.0f;
                        newEffect.param2 = 0.0f;
                        newEffect.param3 = 0.0f;
                        break;
                    case AudioEffectType::Distortion:
                        newEffect.param1 = 0.3f;
                        newEffect.param2 = 0.5f;
                        break;
                    case AudioEffectType::Chorus:
                        newEffect.param1 = 0.5f;
                        newEffect.param2 = 2.0f;
                        newEffect.wetDryMix = 0.3f;
                        break;
                    case AudioEffectType::Panning:
                        newEffect.param1 = 0.0f;
                        break;
                    }

                    channel.effects.push_back(newEffect);
                    SetModified(true);
                }
            }
            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Effect Editor
    // =========================================================================

    void AudioMixerPanel::RenderEffectEditor(AudioEffect& effect)
    {
        float itemWidth = ImGui::GetContentRegionAvail().x - 8.0f;
        if (itemWidth < 80.0f)
        {
            itemWidth = 80.0f;
        }
        ImGui::PushItemWidth(itemWidth);

        switch (effect.type)
        {
        case AudioEffectType::LowPass:
            ImGui::SliderFloat("Cutoff (Hz)", &effect.param1, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Resonance (Q)", &effect.param2, 0.1f, 20.0f, "%.2f");
            break;

        case AudioEffectType::HighPass:
            ImGui::SliderFloat("Cutoff (Hz)", &effect.param1, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Resonance (Q)", &effect.param2, 0.1f, 20.0f, "%.2f");
            break;

        case AudioEffectType::Reverb:
            ImGui::SliderFloat("Decay Time (s)", &effect.param1, 0.1f, 20.0f, "%.1f s");
            ImGui::SliderFloat("Pre-Delay (s)", &effect.param2, 0.0f, 0.5f, "%.3f s");
            ImGui::SliderFloat("Wet/Dry Mix", &effect.wetDryMix, 0.0f, 1.0f, "%.2f");
            break;

        case AudioEffectType::Delay:
            ImGui::SliderFloat("Delay Time (s)", &effect.param1, 0.001f, 5.0f, "%.3f s", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Feedback", &effect.param2, 0.0f, 0.95f, "%.2f");
            ImGui::SliderFloat("Wet/Dry Mix", &effect.wetDryMix, 0.0f, 1.0f, "%.2f");
            break;

        case AudioEffectType::Compressor:
            ImGui::SliderFloat("Threshold (dB)", &effect.param1, -60.0f, 0.0f, "%.1f dB");
            ImGui::SliderFloat("Ratio", &effect.param2, 1.0f, 20.0f, "%.1f:1");
            ImGui::SliderFloat("Makeup Gain (dB)", &effect.param3, 0.0f, 30.0f, "%.1f dB");
            break;

        case AudioEffectType::Limiter:
            ImGui::SliderFloat("Ceiling (dB)", &effect.param1, -20.0f, 0.0f, "%.1f dB");
            ImGui::SliderFloat("Release (ms)", &effect.param2, 1.0f, 500.0f, "%.0f ms");
            break;

        case AudioEffectType::EQ3Band:
            ImGui::SliderFloat("Low (dB)", &effect.param1, -12.0f, 12.0f, "%.1f dB");
            ImGui::SliderFloat("Mid (dB)", &effect.param2, -12.0f, 12.0f, "%.1f dB");
            ImGui::SliderFloat("High (dB)", &effect.param3, -12.0f, 12.0f, "%.1f dB");
            break;

        case AudioEffectType::Distortion:
            ImGui::SliderFloat("Drive", &effect.param1, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Tone", &effect.param2, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Wet/Dry Mix", &effect.wetDryMix, 0.0f, 1.0f, "%.2f");
            break;

        case AudioEffectType::Chorus:
            ImGui::SliderFloat("Depth", &effect.param1, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Rate (Hz)", &effect.param2, 0.1f, 10.0f, "%.1f Hz");
            ImGui::SliderFloat("Wet/Dry Mix", &effect.wetDryMix, 0.0f, 1.0f, "%.2f");
            break;

        case AudioEffectType::Panning:
            ImGui::SliderFloat("Pan", &effect.param1, -1.0f, 1.0f, "%.2f");
            break;
        }

        ImGui::PopItemWidth();
    }

    // =========================================================================
    // Snapshot Manager
    // =========================================================================

    void AudioMixerPanel::RenderSnapshotManager()
    {
        ImGui::SetNextWindowSize(ImVec2(340.0f, 300.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(ICON_FA_SAVE " Snapshot Manager", &m_showSnapshotManager))
        {
            ImGui::End();
            return;
        }

        // Save new snapshot
        ImGui::InputText("##SnapshotName", s_snapshotNameBuffer, sizeof(s_snapshotNameBuffer));
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SAVE " Save") && s_snapshotNameBuffer[0] != '\0')
        {
            SaveSnapshot(s_snapshotNameBuffer);
            s_snapshotNameBuffer[0] = '\0';
        }

        ImGui::Separator();

        // List existing snapshots
        if (m_snapshots.empty())
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No snapshots saved.");
        }

        int deleteIndex = -1;
        for (size_t i = 0; i < m_snapshots.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));

            const auto& snapshot = m_snapshots[i];
            ImGui::Text("%s", snapshot.name.c_str());
            ImGui::SameLine();

            // Instant load
            if (ImGui::SmallButton("Load"))
            {
                LoadSnapshot(snapshot.name, 0.0f);
            }
            ImGui::SameLine();

            // Transition load
            if (ImGui::SmallButton("Blend (1s)"))
            {
                LoadSnapshot(snapshot.name, 1.0f);
            }
            ImGui::SameLine();

            // Delete
            if (ImGui::SmallButton(ICON_FA_TRASH))
            {
                deleteIndex = static_cast<int>(i);
            }

            // Show snapshot channel count
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%zu ch)", snapshot.channelVolumes.size());

            ImGui::PopID();
        }

        // Deferred deletion
        if (deleteIndex >= 0 && deleteIndex < static_cast<int>(m_snapshots.size()))
        {
            m_snapshots.erase(m_snapshots.begin() + deleteIndex);
        }

        ImGui::End();
    }

    // =========================================================================
    // Bus Routing
    // =========================================================================

    void AudioMixerPanel::RenderBusRouting()
    {
        ImGui::SetNextWindowSize(ImVec2(400.0f, 350.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(ICON_FA_SITEMAP " Bus Routing", &m_showBusRouting))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped("Channels are routed to bus outputs. Each non-bus channel can target one output bus.");

        ImGui::Separator();

        // Gather bus names for combo
        std::vector<std::string> busNames;
        busNames.push_back("(None)");
        for (const auto& ch : m_channels)
        {
            if (ch.isBus)
            {
                busNames.push_back(ch.name);
            }
        }

        // Display routing table
        if (ImGui::BeginTable("##RoutingTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Output Bus", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto& channel : m_channels)
            {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s %s", channel.icon.c_str(), channel.name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(channel.isBus ? ImVec4(0.5f, 0.8f, 1.0f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                                   "%s", channel.isBus ? "Bus" : "Ch");

                ImGui::TableSetColumnIndex(2);
                if (channel.isBus)
                {
                    // Buses show their inputs
                    if (!channel.inputChannels.empty())
                    {
                        std::string inputList;
                        for (size_t j = 0; j < channel.inputChannels.size(); ++j)
                        {
                            if (j > 0)
                            {
                                inputList += ", ";
                            }
                            inputList += channel.inputChannels[j];
                        }
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), ICON_FA_CHEVRON_LEFT " %s",
                                           inputList.c_str());
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "(no inputs)");
                    }
                }
                else
                {
                    // Non-bus channels get a combo to select output
                    ImGui::PushID(channel.name.c_str());

                    int currentBusIndex = 0;
                    for (size_t b = 1; b < busNames.size(); ++b)
                    {
                        if (busNames[b] == channel.outputBus)
                        {
                            currentBusIndex = static_cast<int>(b);
                            break;
                        }
                    }

                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##OutputBus", busNames[currentBusIndex].c_str()))
                    {
                        for (size_t b = 0; b < busNames.size(); ++b)
                        {
                            bool isSelected = (static_cast<int>(b) == currentBusIndex);
                            if (ImGui::Selectable(busNames[b].c_str(), isSelected))
                            {
                                // Remove from old bus
                                for (auto& bus : m_channels)
                                {
                                    if (bus.isBus)
                                    {
                                        auto& inputs = bus.inputChannels;
                                        inputs.erase(std::remove(inputs.begin(), inputs.end(), channel.name),
                                                     inputs.end());
                                    }
                                }

                                // Assign new bus
                                if (b == 0)
                                {
                                    channel.outputBus.clear();
                                }
                                else
                                {
                                    channel.outputBus = busNames[b];

                                    // Add to new bus inputs
                                    for (auto& bus : m_channels)
                                    {
                                        if (bus.name == channel.outputBus && bus.isBus)
                                        {
                                            bus.inputChannels.push_back(channel.name);
                                            break;
                                        }
                                    }
                                }
                                SetModified(true);
                            }
                            if (isSelected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::PopID();
                }
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    bool AudioMixerPanel::IsAnySoloed() const
    {
        for (const auto& channel : m_channels)
        {
            if (channel.soloed)
            {
                return true;
            }
        }
        return false;
    }

    float AudioMixerPanel::VolumeToDb(float volume) const
    {
        if (volume <= 0.0001f)
        {
            return MIN_DB;
        }
        return 20.0f * std::log10(volume);
    }

    float AudioMixerPanel::DbToVolume(float db) const
    {
        if (db <= MIN_DB)
        {
            return 0.0f;
        }
        return std::pow(10.0f, db / 20.0f);
    }

    void AudioMixerPanel::SimulateMeterLevels(float deltaTime)
    {
        m_meterUpdateTimer += deltaTime;
        if (m_meterUpdateTimer < METER_UPDATE_INTERVAL)
        {
            return;
        }
        m_meterUpdateTimer = 0.0f;

        bool anySoloed = IsAnySoloed();

        for (auto& channel : m_channels)
        {
            float effectiveVol = channel.GetEffectiveVolume(anySoloed);

            if (effectiveVol <= 0.0001f)
            {
                // Decay toward silence
                channel.peakLevelLeft =
                    std::max(channel.peakLevelLeft - m_meterDecayRate * METER_UPDATE_INTERVAL, MIN_DB);
                channel.peakLevelRight =
                    std::max(channel.peakLevelRight - m_meterDecayRate * METER_UPDATE_INTERVAL, MIN_DB);
                channel.rmsLevelLeft =
                    std::max(channel.rmsLevelLeft - m_meterDecayRate * METER_UPDATE_INTERVAL, MIN_DB);
                channel.rmsLevelRight =
                    std::max(channel.rmsLevelRight - m_meterDecayRate * METER_UPDATE_INTERVAL, MIN_DB);
                continue;
            }

            // Simulate a signal with some randomness for visual interest
            float baseDb = VolumeToDb(effectiveVol);

            // Random fluctuation range
            float fluctuation = 12.0f;
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            float pseudoRandom = static_cast<float>(now % 10000) / 10000.0f;
            float pseudoRandom2 = static_cast<float>((now / 7) % 10000) / 10000.0f;

            float targetPeakL = baseDb - fluctuation * (1.0f - pseudoRandom);
            float targetPeakR = baseDb - fluctuation * (1.0f - pseudoRandom2);
            float targetRmsL = targetPeakL - 3.0f - pseudoRandom * 4.0f;
            float targetRmsR = targetPeakR - 3.0f - pseudoRandom2 * 4.0f;

            // Clamp
            targetPeakL = std::clamp(targetPeakL, MIN_DB, MAX_DB);
            targetPeakR = std::clamp(targetPeakR, MIN_DB, MAX_DB);
            targetRmsL = std::clamp(targetRmsL, MIN_DB, MAX_DB);
            targetRmsR = std::clamp(targetRmsR, MIN_DB, MAX_DB);

            // Peak: fast attack, slow decay
            if (targetPeakL > channel.peakLevelLeft)
            {
                channel.peakLevelLeft = targetPeakL;
            }
            else
            {
                channel.peakLevelLeft =
                    std::max(channel.peakLevelLeft - m_meterDecayRate * METER_UPDATE_INTERVAL, targetPeakL);
            }

            if (targetPeakR > channel.peakLevelRight)
            {
                channel.peakLevelRight = targetPeakR;
            }
            else
            {
                channel.peakLevelRight =
                    std::max(channel.peakLevelRight - m_meterDecayRate * METER_UPDATE_INTERVAL, targetPeakR);
            }

            // RMS: smoothed average
            float rmsSmooth = 0.3f;
            channel.rmsLevelLeft += (targetRmsL - channel.rmsLevelLeft) * rmsSmooth;
            channel.rmsLevelRight += (targetRmsR - channel.rmsLevelRight) * rmsSmooth;
        }
    }

    ImVec4 AudioMixerPanel::GetMeterColor(float db) const
    {
        if (db >= RED_THRESHOLD_DB)
        {
            return ImVec4(0.9f, 0.15f, 0.15f, 1.0f); // Red
        }
        if (db >= YELLOW_THRESHOLD_DB)
        {
            return ImVec4(0.86f, 0.78f, 0.12f, 1.0f); // Yellow
        }
        return ImVec4(0.12f, 0.78f, 0.24f, 1.0f); // Green
    }

    const char* AudioMixerPanel::GetEffectTypeName(AudioEffectType type) const
    {
        switch (type)
        {
        case AudioEffectType::LowPass:
            return "Low-Pass Filter";
        case AudioEffectType::HighPass:
            return "High-Pass Filter";
        case AudioEffectType::Reverb:
            return "Reverb";
        case AudioEffectType::Delay:
            return "Delay";
        case AudioEffectType::Compressor:
            return "Compressor";
        case AudioEffectType::Limiter:
            return "Limiter";
        case AudioEffectType::EQ3Band:
            return "3-Band EQ";
        case AudioEffectType::Distortion:
            return "Distortion";
        case AudioEffectType::Chorus:
            return "Chorus";
        case AudioEffectType::Panning:
            return "Panning";
        default:
            return "Unknown";
        }
    }

} // namespace SparkEditor
