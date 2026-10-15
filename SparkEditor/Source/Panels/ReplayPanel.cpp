/**
 * @file ReplayPanel.cpp
 * @brief Implementation of the replay system editor panel
 */

#include "ReplayPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Engine/Replay/ReplaySystem.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/FileUtils.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <filesystem>
#include <optional>
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

            // The path is reopened through the replay system's narrow std::string file
            // API; the name is ImGui (UTF-8) text. path::string() throws on Windows for
            // a name the ANSI code page cannot spell, which ended the refresh; such a
            // replay has no narrow path that could load it, so it is not listed.
            std::optional<std::string> narrowPath = Spark::FileUtils::TryPathToNarrow(entry.path());
            std::optional<std::string> name = Spark::FileUtils::TryPathToUtf8(entry.path().filename());
            if (!narrowPath || !name)
            {
                continue;
            }

            ReplayFileInfo info;
            info.path = std::move(*narrowPath);
            info.name = std::move(*name);
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

        const bool hasReplay = duration > 0.0f;

        if (Spark::Ui::IconButton("rewind", ICON_FA_BACKWARD, "Rewind to start", false, hasReplay))
            replay.SeekTo(0.0f);
        ImGui::SameLine();

        if (state == Spark::PlaybackState::Playing)
        {
            if (Spark::Ui::IconButton("pause", ICON_FA_PAUSE, "Pause playback", true, hasReplay))
                replay.PausePlayback();
        }
        else
        {
            if (Spark::Ui::IconButton("play", ICON_FA_PLAY, "Play", false, hasReplay))
            {
                if (state == Spark::PlaybackState::Paused)
                    replay.ResumePlayback();
                else
                    replay.StartPlayback();
            }
        }
        ImGui::SameLine();

        if (Spark::Ui::IconButton("stop", ICON_FA_STOP, "Stop playback", false, hasReplay))
            replay.StopPlayback();
        ImGui::SameLine();

        if (Spark::Ui::IconButton("step", ICON_FA_STEP_FORWARD, "Step one frame", false, hasReplay))
            replay.SeekTo(replay.GetPlaybackTime() + 1.0f / 60.0f);

        ImGui::Separator();
        if (hasReplay)
        {
            if (ImGui::SliderFloat("Position", &m_scrubPosition, 0.0f, duration, "%.2f s"))
                replay.SeekTo(m_scrubPosition);
        }
        else
        {
            Spark::Ui::EmptyState(ICON_FA_FILM, "No replay loaded", "Load a file from the Files tab.");
        }

        if (ImGui::SliderFloat("Speed", &m_playbackSpeed, 0.1f, 4.0f, "%.1fx"))
            replay.SetPlaybackSpeed(m_playbackSpeed);
    }

    void ReplayPanel::RenderRecordingControls()
    {
        if (IsRecording())
        {
            Spark::Ui::TextRole(Spark::Ui::Role::ErrorText, ICON_FA_CIRCLE " RECORDING");
            if (Spark::Ui::SecondaryButton(ICON_FA_STOP " Stop Recording"))
            {
                StopRecording();
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "ReplayPanel: recording stopped (%zu frame(s) captured)",
                               GetCapturedFrameCount());
            }
        }
        else
        {
            if (Spark::Ui::PrimaryButton(ICON_FA_CIRCLE " Start Recording"))
            {
                StartRecording();
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "ReplayPanel: recording started");
            }
        }

        ImGui::Separator();
        Spark::Ui::KeyValue("Frames captured:", "%zu", GetCapturedFrameCount());
        Spark::Ui::KeyValue("Duration:", "%.2f s", GetDuration());
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("Frames are submitted by the running game session. A session that never submits "
                                 "frames records nothing, and the frame count above stays at zero.");
        ImGui::PopTextWrapPos();
    }

    void ReplayPanel::RenderReplayBrowser()
    {
        if (Spark::Ui::SecondaryButton(ICON_FA_REFRESH " Rescan"))
            RefreshReplayFiles(m_replayDirectory);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Spark::Ui::KeyValue("Directory:", "%s", m_replayDirectory.c_str());

        if (m_replayFiles.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_FOLDER_OPEN, "No replay files found",
                                  "Record a play session to create replay data.");
            return;
        }

        // The load action sits above the file table because the table fills the remaining height.
        ImGui::BeginDisabled(m_selectedReplay < 0);
        if (Spark::Ui::PrimaryButton(ICON_FA_DOWNLOAD " Load Selected") && m_selectedReplay >= 0)
        {
            LoadReplayFile(m_replayFiles[static_cast<size_t>(m_selectedReplay)].path);
        }
        ImGui::EndDisabled();

        if (Spark::Ui::BeginDataTable("ReplayFileTable", 2))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(90));
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
                Spark::Ui::TableNumber("%.1f KB", static_cast<double>(file.fileSizeBytes) / 1024.0);
            }
            ImGui::EndTable();
        }
    }

} // namespace SparkEditor
