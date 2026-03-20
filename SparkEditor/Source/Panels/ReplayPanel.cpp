/**
 * @file ReplayPanel.cpp
 * @brief Implementation of the replay system editor panel
 */

#include "ReplayPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    ReplayPanel::ReplayPanel() : EditorPanel("Replay", "replay_panel") {}

    bool ReplayPanel::Initialize()
    {
        std::cout << "Initializing Replay panel\n";
        return true;
    }

    void ReplayPanel::Update(float /*deltaTime*/) {}

    void ReplayPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("ReplayTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_PLAY " Playback"))
                {
                    RenderTransportControls();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CIRCLE " Recording"))
                {
                    RenderRecordingControls();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_FOLDER " Files"))
                {
                    RenderReplayBrowser();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void ReplayPanel::Shutdown()
    {
        std::cout << "Shutting down Replay panel\n";
    }

    void ReplayPanel::RenderTransportControls()
    {
        // Transport buttons
        if (ImGui::Button(ICON_FA_BACKWARD "##rewind"))
            m_playbackPosition = 0.0f;
        ImGui::SameLine();

        if (m_isPlaying && !m_isPaused)
        {
            if (ImGui::Button(ICON_FA_PAUSE "##pause"))
                m_isPaused = true;
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY "##play"))
            {
                m_isPlaying = true;
                m_isPaused = false;
            }
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_STOP "##stop"))
        {
            m_isPlaying = false;
            m_isPaused = false;
            m_playbackPosition = 0.0f;
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_STEP_FORWARD "##step"))
        {
            m_playbackPosition += 1.0f / 60.0f; // Step one frame at 60fps
        }

        // Timeline scrubber
        ImGui::Separator();
        if (m_replayDuration > 0.0f)
        {
            ImGui::SliderFloat("Position", &m_playbackPosition, 0.0f, m_replayDuration, "%.2f s");
        }
        else
        {
            ImGui::TextDisabled("No replay loaded.");
        }

        // Speed control
        ImGui::SliderFloat("Speed", &m_playbackSpeed, 0.1f, 4.0f, "%.1fx");
    }

    void ReplayPanel::RenderRecordingControls()
    {
        if (m_isRecording)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), ICON_FA_CIRCLE " RECORDING");
            if (ImGui::Button(ICON_FA_STOP " Stop Recording"))
                m_isRecording = false;
        }
        else
        {
            if (ImGui::Button(ICON_FA_CIRCLE " Start Recording"))
                m_isRecording = true;
        }

        ImGui::Separator();
        ImGui::TextDisabled("Recording captures entity transforms, events,");
        ImGui::TextDisabled("and input state each frame during Play mode.");
    }

    void ReplayPanel::RenderReplayBrowser()
    {
        if (m_replayFiles.empty())
        {
            ImGui::TextDisabled("No replay files found.");
            ImGui::TextDisabled("Record a play session to create replay data.");
            return;
        }

        if (ImGui::BeginTable("ReplayFileTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_replayFiles.size()); ++i)
            {
                auto& file = m_replayFiles[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                bool selected = (m_selectedReplay == i);
                if (ImGui::Selectable(file.name, selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedReplay = i;

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(file.sceneName);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f s", file.duration);
                ImGui::TableNextColumn();
                ImGui::Text("%d KB", file.fileSizeKB);
            }
            ImGui::EndTable();
        }
    }

} // namespace SparkEditor
