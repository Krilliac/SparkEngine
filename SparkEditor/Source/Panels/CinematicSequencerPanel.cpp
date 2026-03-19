/**
 * @file CinematicSequencerPanel.cpp
 * @brief Implementation of the cinematic sequence editor panel
 */

#include "CinematicSequencerPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace SparkEditor
{

    CinematicSequencerPanel::CinematicSequencerPanel() : EditorPanel("Cinematic Sequencer", "cinematic_sequencer_panel")
    {
    }

    bool CinematicSequencerPanel::Initialize()
    {
        std::cout << "Initializing Cinematic Sequencer panel\n";
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
                ImGui::TextDisabled("No sequences. Click 'New Sequence' to begin.");
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
        std::cout << "Shutting down Cinematic Sequencer panel\n";
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
            m_selectedTrack = -1;
            m_selectedKeyframe = -1;
        }

        ImGui::SameLine();

        if (!m_sequences.empty())
        {
            ImGui::SetNextItemWidth(200);
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
                m_isPlaying = !m_isPlaying;

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_STOP))
            {
                m_isPlaying = false;
                m_playheadTime = 0.0f;
            }

            ImGui::SameLine();
            ImGui::Checkbox(ICON_FA_REDO " Loop", &m_isLooping);

            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (m_selectedSequence >= 0)
            {
                auto& seq = m_sequences[m_selectedSequence];
                ImGui::DragFloat("Duration", &seq.duration, 0.1f, 0.1f, 600.0f, "%.1fs");
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
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
        float timelineWidth = seq.duration * 60.0f * m_zoom;
        float rowHeight = 24.0f;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        float canvasHeight = std::max(rowHeight * static_cast<float>(seq.tracks.size() + 1), 100.0f);

        // Time ruler
        for (float t = 0.0f; t <= seq.duration; t += 1.0f)
        {
            float x = canvasPos.x + t * 60.0f * m_zoom;
            drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasHeight),
                              IM_COL32(100, 100, 100, 200));

            char timeBuf[16];
            snprintf(timeBuf, sizeof(timeBuf), "%.0fs", t);
            drawList->AddText(ImVec2(x + 2, canvasPos.y), IM_COL32(200, 200, 200, 255), timeBuf);
        }

        // Playhead
        float playX = canvasPos.x + m_playheadTime * 60.0f * m_zoom;
        drawList->AddLine(ImVec2(playX, canvasPos.y), ImVec2(playX, canvasPos.y + canvasHeight),
                          IM_COL32(255, 80, 80, 255), 2.0f);

        // Track rows + keyframes
        for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
        {
            auto& track = seq.tracks[i];
            float rowY = canvasPos.y + rowHeight * static_cast<float>(i + 1);

            // Row background
            ImU32 rowColor = (i == m_selectedTrack) ? IM_COL32(60, 80, 120, 100) : IM_COL32(40, 40, 40, 80);
            drawList->AddRectFilled(ImVec2(canvasPos.x, rowY), ImVec2(canvasPos.x + timelineWidth, rowY + rowHeight),
                                    rowColor);

            // Keyframes
            for (int k = 0; k < static_cast<int>(track.keyframes.size()); ++k)
            {
                float kx = canvasPos.x + track.keyframes[k].time * 60.0f * m_zoom;
                float ky = rowY + rowHeight * 0.5f;
                float size = 5.0f;

                bool isSelected = (i == m_selectedTrack && k == m_selectedKeyframe);
                ImU32 kfColor = isSelected ? IM_COL32(255, 200, 50, 255) : IM_COL32(100, 200, 255, 255);

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
            m_playheadTime = std::clamp(mouseX / (60.0f * m_zoom), 0.0f, seq.duration);
        }

        // Right-click to add keyframe
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            float mouseX = ImGui::GetMousePos().x - canvasPos.x;
            float mouseY = ImGui::GetMousePos().y - canvasPos.y;
            float clickTime = mouseX / (60.0f * m_zoom);
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

        ImGui::Text(ICON_FA_EDIT " Track Properties");
        ImGui::InputText("Name", track.name, sizeof(track.name));
        ImGui::InputText("Actor", track.actorName, sizeof(track.actorName));
        ImGui::InputText("Property", track.propertyName, sizeof(track.propertyName));
        ImGui::Checkbox("Locked", &track.locked);

        ImGui::Text("Keyframes: %d", static_cast<int>(track.keyframes.size()));

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
