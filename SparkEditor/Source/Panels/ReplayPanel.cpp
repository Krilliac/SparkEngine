/**
 * @file ReplayPanel.cpp
 * @brief Implementation of the replay system editor panel
 */

#include "ReplayPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <filesystem>
#include <imgui.h>

namespace SparkEditor
{

    ReplayPanel::ReplayPanel() : EditorPanel("Replay", "replay_panel") {}

    Spark::ReplaySystem& ReplayPanel::System() const
    {
        if (auto* context = ::EngineContext::Get())
        {
            if (auto* replay = context->GetReplay())
            {
                return *replay;
            }
        }
        return Spark::ReplaySystem::GetInstance();
    }

    bool ReplayPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ReplayPanel initialized");
        RefreshReplayFiles(m_replayDirectory);
        m_playbackSpeed = System().GetPlaybackSpeed();
        return true;
    }

    void ReplayPanel::Update(float deltaTime)
    {
        if (IsDrivingPlayback())
        {
            System().UpdatePlayback(deltaTime);
        }
        m_scrubPosition = System().GetPlaybackTime();
    }

    bool ReplayPanel::IsDrivingPlayback() const
    {
        return ::EngineContext::Get() == nullptr;
    }

    void ReplayPanel::StartRecording()
    {
        System().StartRecording();
    }

    void ReplayPanel::StopRecording()
    {
        System().StopRecording();
    }

    bool ReplayPanel::IsRecording() const
    {
        return System().IsRecording();
    }

    size_t ReplayPanel::GetCapturedFrameCount() const
    {
        return System().GetFrameCount();
    }

    float ReplayPanel::GetDuration() const
    {
        return System().GetDuration();
    }

    bool ReplayPanel::LoadReplayFile(const std::string& filePath)
    {
        const bool loaded = System().LoadFromFile(filePath);
        if (!loaded)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "ReplayPanel: failed to load replay '%s'", filePath.c_str());
        }
        m_scrubPosition = System().GetPlaybackTime();
        return loaded;
    }

    size_t ReplayPanel::RefreshReplayFiles(const std::string& directory)
    {
        m_replayDirectory = directory;
        m_replayFiles.clear();
        m_selectedReplay = -1;

        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(directory, ec) || ec)
        {
            return 0;
        }

        for (const auto& entry : fs::directory_iterator(directory, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file() || entry.path().extension() != ".replay")
                continue;

            ReplayFileInfo info;
            info.path = entry.path().string();
            info.name = entry.path().filename().string();
            std::error_code sizeEc;
            info.fileSizeBytes = fs::file_size(entry.path(), sizeEc);
            if (sizeEc)
                info.fileSizeBytes = 0;
            m_replayFiles.push_back(std::move(info));
        }

        std::sort(m_replayFiles.begin(), m_replayFiles.end(),
                  [](const ReplayFileInfo& a, const ReplayFileInfo& b) { return a.name < b.name; });
        return m_replayFiles.size();
    }

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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ReplayPanel shutting down");
    }

    void ReplayPanel::RenderTransportControls()
    {
        Spark::ReplaySystem& replay = System();
        const float duration = replay.GetDuration();
        const Spark::PlaybackState state = replay.GetPlaybackState();

        ImGui::BeginDisabled(duration <= 0.0f);

        if (ImGui::Button(ICON_FA_BACKWARD "##rewind"))
            replay.SeekTo(0.0f);
        ImGui::SameLine();

        if (state == Spark::PlaybackState::Playing)
        {
            if (ImGui::Button(ICON_FA_PAUSE "##pause"))
                replay.PausePlayback();
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY "##play"))
            {
                if (state == Spark::PlaybackState::Paused)
                    replay.ResumePlayback();
                else
                    replay.StartPlayback();
            }
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_STOP "##stop"))
            replay.StopPlayback();
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_STEP_FORWARD "##step"))
            replay.SeekTo(replay.GetPlaybackTime() + 1.0f / 60.0f);

        ImGui::EndDisabled();

        ImGui::Separator();
        if (duration > 0.0f)
        {
            if (ImGui::SliderFloat("Position", &m_scrubPosition, 0.0f, duration, "%.2f s"))
                replay.SeekTo(m_scrubPosition);
        }
        else
        {
            ImGui::TextDisabled("No replay loaded. Load a file from the Files tab.");
        }

        if (ImGui::SliderFloat("Speed", &m_playbackSpeed, 0.1f, 4.0f, "%.1fx"))
            replay.SetPlaybackSpeed(m_playbackSpeed);
    }

    void ReplayPanel::RenderRecordingControls()
    {
        if (IsRecording())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), ICON_FA_CIRCLE " RECORDING");
            if (ImGui::Button(ICON_FA_STOP " Stop Recording"))
            {
                StopRecording();
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "ReplayPanel: recording stopped (%zu frame(s) captured)",
                               GetCapturedFrameCount());
            }
        }
        else
        {
            if (ImGui::Button(ICON_FA_CIRCLE " Start Recording"))
            {
                StartRecording();
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "ReplayPanel: recording started");
            }
        }

        ImGui::Separator();
        ImGui::Text("Frames captured: %zu", GetCapturedFrameCount());
        ImGui::Text("Duration: %.2f s", GetDuration());
        ImGui::Separator();
        ImGui::TextDisabled("Frames are submitted by the running game session.");
        ImGui::TextDisabled("A session that never submits frames records nothing,");
        ImGui::TextDisabled("and the frame count above stays at zero.");
    }

    void ReplayPanel::RenderReplayBrowser()
    {
        ImGui::Text("Directory: %s", m_replayDirectory.c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REFRESH " Rescan"))
            RefreshReplayFiles(m_replayDirectory);

        if (m_replayFiles.empty())
        {
            ImGui::TextDisabled("No replay files found.");
            ImGui::TextDisabled("Record a play session to create replay data.");
            return;
        }

        if (ImGui::BeginTable("ReplayFileTable", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_replayFiles.size()); ++i)
            {
                const ReplayFileInfo& file = m_replayFiles[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                const bool selected = (m_selectedReplay == i);
                if (ImGui::Selectable(file.name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedReplay = i;

                ImGui::TableNextColumn();
                ImGui::Text("%.1f KB", static_cast<double>(file.fileSizeBytes) / 1024.0);
            }
            ImGui::EndTable();
        }

        ImGui::BeginDisabled(m_selectedReplay < 0);
        if (ImGui::Button(ICON_FA_DOWNLOAD " Load Selected") && m_selectedReplay >= 0)
        {
            LoadReplayFile(m_replayFiles[static_cast<size_t>(m_selectedReplay)].path);
        }
        ImGui::EndDisabled();
    }

} // namespace SparkEditor
