/**
 * @file CinematicSequencerPanel.cpp
 * @brief Implementation of the cinematic sequence editor panel
 */

#include "CinematicSequencerPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    CinematicSequencerPanel::CinematicSequencerPanel() : EditorPanel("Cinematic Sequencer", "cinematic_sequencer_panel")
    {
    }

    bool CinematicSequencerPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "CinematicSequencerPanel initialized");
        return true;
    }

    void CinematicSequencerPanel::Update(float deltaTime)
    {
        if (m_isPlaying && m_selectedSequence >= 0 && m_selectedSequence < static_cast<int>(m_sequences.size()))
        {
            auto& seq = m_sequences[m_selectedSequence];
            m_playheadTime += deltaTime;

            if (m_playheadTime >= seq.duration)
            {
                if (m_isLooping)
                    m_playheadTime = 0.0f;
                else
                {
                    m_playheadTime = seq.duration;
                    m_isPlaying = false;
                }
            }
        }
    }

    void CinematicSequencerPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            if (m_sequences.empty())
            {
                Spark::Ui::EmptyState(ICON_FA_FILM, "No sequences", "Click 'New Sequence' to begin.");
            }
            else if (m_selectedSequence >= 0 && m_selectedSequence < static_cast<int>(m_sequences.size()))
            {
                // Split layout: track list on left, timeline on right
                float panelWidth = ImGui::GetContentRegionAvail().x;
                float trackListWidth = panelWidth * 0.25f;

                ImGui::BeginChild("TrackList", ImVec2(trackListWidth, 0), true);
                RenderTrackList();
                ImGui::EndChild();

                ImGui::SameLine();

                ImGui::BeginChild("Timeline", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                RenderTimeline();
                ImGui::EndChild();
            }
        }
        EndPanel();
    }

    void CinematicSequencerPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "CinematicSequencerPanel shutting down");
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void CinematicSequencerPanel::RenderToolbar()
    {
        // Sequence selector
        if (ImGui::Button(ICON_FA_PLUS " New Sequence"))
        {
            CinematicSequence seq;
            snprintf(seq.name, sizeof(seq.name), "Sequence_%d", static_cast<int>(m_sequences.size() + 1));
            m_sequences.push_back(seq);
            m_selectedSequence = static_cast<int>(m_sequences.size()) - 1;
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "CinematicSequencerPanel: created sequence '%s'", seq.name);
            m_selectedTrack = -1;
            m_selectedKeyframe = -1;
        }

        ImGui::SameLine();

        if (!m_sequences.empty())
        {
            ImGui::SetNextItemWidth(Spark::Ui::Px(200));
            if (ImGui::BeginCombo("##SeqSelect",
                                  m_selectedSequence >= 0 ? m_sequences[m_selectedSequence].name : "Select..."))
            {
                for (int i = 0; i < static_cast<int>(m_sequences.size()); ++i)
                {
                    bool selected = (i == m_selectedSequence);
                    if (ImGui::Selectable(m_sequences[i].name, selected))
                    {
                        m_selectedSequence = i;
                        m_selectedTrack = -1;
                        m_selectedKeyframe = -1;
                        m_playheadTime = 0.0f;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();

            // Playback controls
            if (ImGui::Button(m_isPlaying ? ICON_FA_PAUSE : ICON_FA_PLAY))
            {
                m_isPlaying = !m_isPlaying;
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "CinematicSequencerPanel: playback %s",
                                m_isPlaying ? "started" : "paused");
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_STOP))
            {
                m_isPlaying = false;
                m_playheadTime = 0.0f;
            }

            ImGui::SameLine();
            ImGui::Checkbox(ICON_FA_REDO " Loop", &m_isLooping);

            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Px(80));
            if (m_selectedSequence >= 0)
            {
                auto& seq = m_sequences[m_selectedSequence];
                ImGui::DragFloat("Duration", &seq.duration, 0.1f, 0.1f, 600.0f, "%.1fs");
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Px(60));
            ImGui::DragFloat("Zoom", &m_zoom, 0.05f, 0.1f, 10.0f, "%.1fx");
        }
    }

    // =========================================================================
    // Track List (left pane)
    // =========================================================================

    void CinematicSequencerPanel::RenderTrackList()
    {
        if (m_selectedSequence < 0)
            return;

        auto& seq = m_sequences[m_selectedSequence];

        ImGui::Text(ICON_FA_LIST " Tracks (%d)", static_cast<int>(seq.tracks.size()));
        ImGui::Separator();

        if (ImGui::Button(ICON_FA_PLUS " Add Track"))
        {
            CinematicTrack track;
            snprintf(track.name, sizeof(track.name), "Track_%d", static_cast<int>(seq.tracks.size() + 1));
            strncpy(track.propertyName, "Position", sizeof(track.propertyName) - 1);
            seq.tracks.push_back(track);
        }

        for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
        {
            auto& track = seq.tracks[i];
            bool selected = (i == m_selectedTrack);

            ImGui::PushID(i);

            // Mute/lock icons
            if (ImGui::Checkbox("##mute", &track.muted))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Track '%s' %s", track.name,
                                track.muted ? "muted" : "unmuted");
            }
            ImGui::SameLine();

            if (ImGui::Selectable(track.name, selected))
            {
                m_selectedTrack = i;
                m_selectedKeyframe = -1;
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Rename"))
                {
                    // Could open rename popup
                }
                if (ImGui::MenuItem("Delete"))
                {
                    seq.tracks.erase(seq.tracks.begin() + i);
                    if (m_selectedTrack >= static_cast<int>(seq.tracks.size()))
                        m_selectedTrack = static_cast<int>(seq.tracks.size()) - 1;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    return;
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        // Property editor for selected track
        if (m_selectedTrack >= 0 && m_selectedTrack < static_cast<int>(seq.tracks.size()))
        {
            ImGui::Separator();
            RenderPropertyEditor();
        }
    }

    // =========================================================================
    // Timeline (right pane)
    // =========================================================================

    void CinematicSequencerPanel::RenderTimeline()
    {
        if (m_selectedSequence < 0)
            return;

        auto& seq = m_sequences[m_selectedSequence];
        // One second of timeline spans 60 logical pixels at zoom 1; rows are 24 logical pixels.
        const float pixelsPerSecond = Spark::Ui::Px(60.0f) * m_zoom;
        float timelineWidth = seq.duration * pixelsPerSecond;
        const float rowHeight = Spark::Ui::Px(24.0f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        float canvasHeight = std::max(rowHeight * static_cast<float>(seq.tracks.size() + 1), Spark::Ui::Px(100.0f));

        // Time ruler
        for (float t = 0.0f; t <= seq.duration; t += 1.0f)
        {
            float x = canvasPos.x + t * pixelsPerSecond;
            drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasHeight),
                              Spark::Ui::Col(Spark::Ui::Role::BorderSubtle));

            char timeBuf[16];
            snprintf(timeBuf, sizeof(timeBuf), "%.0fs", t);
            drawList->AddText(ImVec2(x + Spark::Ui::Px(2.0f), canvasPos.y),
                              Spark::Ui::Col(Spark::Ui::Role::TextSecondary), timeBuf);
        }

        // Playhead
        float playX = canvasPos.x + m_playheadTime * pixelsPerSecond;
        drawList->AddLine(ImVec2(playX, canvasPos.y), ImVec2(playX, canvasPos.y + canvasHeight),
                          Spark::Ui::Col(Spark::Ui::Role::AccentFill), Spark::Ui::Px(2.0f));

        // Track rows + keyframes
        for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
        {
            auto& track = seq.tracks[i];
            float rowY = canvasPos.y + rowHeight * static_cast<float>(i + 1);

            // Row background
            ImU32 rowColor = (i == m_selectedTrack) ? Spark::Ui::Col(Spark::Ui::Role::SurfaceSelected)
                                                    : Spark::Ui::Col(Spark::Ui::Role::SurfaceRaised);
            drawList->AddRectFilled(ImVec2(canvasPos.x, rowY), ImVec2(canvasPos.x + timelineWidth, rowY + rowHeight),
                                    rowColor);

            // Keyframes
            for (int k = 0; k < static_cast<int>(track.keyframes.size()); ++k)
            {
                float kx = canvasPos.x + track.keyframes[k].time * pixelsPerSecond;
                float ky = rowY + rowHeight * 0.5f;

                // The selected keyframe is drawn larger and brighter so selection is not colour-only.
                bool isSelected = (i == m_selectedTrack && k == m_selectedKeyframe);
                const float size = Spark::Ui::Px(isSelected ? 7.0f : 5.0f);
                ImU32 kfColor = isSelected ? Spark::Ui::Col(Spark::Ui::Role::TextPrimary)
                                           : Spark::Ui::Col(Spark::Ui::Role::Secondary);

                // Diamond shape
                drawList->AddQuadFilled(ImVec2(kx, ky - size), ImVec2(kx + size, ky), ImVec2(kx, ky + size),
                                        ImVec2(kx - size, ky), kfColor);
            }
        }

        // Clickable region for playhead scrubbing
        ImGui::InvisibleButton("TimelineArea", ImVec2(timelineWidth, canvasHeight));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            float mouseX = ImGui::GetMousePos().x - canvasPos.x;
            m_playheadTime = std::clamp(mouseX / pixelsPerSecond, 0.0f, seq.duration);
        }

        // Right-click to add keyframe
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            float mouseX = ImGui::GetMousePos().x - canvasPos.x;
            float mouseY = ImGui::GetMousePos().y - canvasPos.y;
            float clickTime = mouseX / pixelsPerSecond;
            int clickTrack = static_cast<int>((mouseY / rowHeight) - 1.0f);

            if (clickTrack >= 0 && clickTrack < static_cast<int>(seq.tracks.size()) && clickTime >= 0.0f &&
                clickTime <= seq.duration)
            {
                CinematicKeyframe kf;
                kf.time = clickTime;
                seq.tracks[clickTrack].keyframes.push_back(kf);

                // Sort keyframes by time
                auto& kfs = seq.tracks[clickTrack].keyframes;
                std::sort(kfs.begin(), kfs.end(),
                          [](const CinematicKeyframe& a, const CinematicKeyframe& b) { return a.time < b.time; });

                m_selectedTrack = clickTrack;
            }
        }
    }

    // =========================================================================
    // Property Editor (below track list)
    // =========================================================================

    void CinematicSequencerPanel::RenderPropertyEditor()
    {
        if (m_selectedSequence < 0 || m_selectedTrack < 0)
            return;

        auto& seq = m_sequences[m_selectedSequence];
        if (m_selectedTrack >= static_cast<int>(seq.tracks.size()))
            return;

        auto& track = seq.tracks[m_selectedTrack];

        Spark::Ui::SubHeader(ICON_FA_EDIT " Track Properties");
        ImGui::InputText("Name", track.name, sizeof(track.name));
        ImGui::InputText("Actor", track.actorName, sizeof(track.actorName));
        ImGui::InputText("Property", track.propertyName, sizeof(track.propertyName));
        ImGui::Checkbox("Locked", &track.locked);

        Spark::Ui::KeyValue("Keyframes", "%d", static_cast<int>(track.keyframes.size()));

        // List keyframes
        for (int k = 0; k < static_cast<int>(track.keyframes.size()); ++k)
        {
            ImGui::PushID(k);
            auto& kf = track.keyframes[k];

            bool selected = (k == m_selectedKeyframe);
            char label[32];
            snprintf(label, sizeof(label), "%.2fs", kf.time);

            if (ImGui::Selectable(label, selected))
                m_selectedKeyframe = k;

            if (selected)
            {
                ImGui::Indent();
                ImGui::DragFloat("Time", &kf.time, 0.01f, 0.0f, seq.duration);
                ImGui::DragFloat4("Value", kf.value, 0.1f);
                ImGui::Unindent();
            }

            ImGui::PopID();
        }
    }

} // namespace SparkEditor
