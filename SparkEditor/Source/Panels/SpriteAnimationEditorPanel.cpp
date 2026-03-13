/**
 * @file SpriteAnimationEditorPanel.cpp
 * @brief Implementation of the Sprite Animation Editor panel
 * @author Spark Engine Team
 * @date 2026
 */

#include "SpriteAnimationEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace SparkEditor
{

    SpriteAnimationEditorPanel::SpriteAnimationEditorPanel()
        : EditorPanel("Sprite Animation", "sprite_animation_editor_panel")
    {
    }

    bool SpriteAnimationEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Sprite Animation Editor panel\n";
        return true;
    }

    void SpriteAnimationEditorPanel::Update(float deltaTime)
    {
        // Advance preview animation
        if (m_isPlaying && m_selectedClipIndex >= 0 && m_selectedClipIndex < static_cast<int>(m_clips.size()))
        {
            const auto& clip = m_clips[m_selectedClipIndex];
            if (!clip.frames.empty())
            {
                m_previewTimer += deltaTime * m_previewSpeed;
                int frameIdx = m_previewFrame % static_cast<int>(clip.frames.size());
                float frameDur = clip.frames[frameIdx].duration;

                while (m_previewTimer >= frameDur && frameDur > 0.0f)
                {
                    m_previewTimer -= frameDur;
                    m_previewFrame++;

                    if (m_previewFrame >= static_cast<int>(clip.frames.size()))
                    {
                        if (clip.loop)
                        {
                            m_previewFrame = 0;
                        }
                        else
                        {
                            m_previewFrame = static_cast<int>(clip.frames.size()) - 1;
                            m_isPlaying = false;
                        }
                    }

                    frameIdx = m_previewFrame % static_cast<int>(clip.frames.size());
                    frameDur = clip.frames[frameIdx].duration;
                }
            }
        }
    }

    void SpriteAnimationEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            // Layout: clip list (left), preview (center), frame properties (right)
            float panelWidth = ImGui::GetContentRegionAvail().x;
            float clipListWidth = (std::min)(panelWidth * 0.2f, 160.0f);
            float previewWidth = panelWidth * 0.35f;

            // Top section: clip list + preview + properties
            float topHeight = ImGui::GetContentRegionAvail().y * 0.55f;

            ImGui::BeginChild("ClipList", ImVec2(clipListWidth, topHeight), true);
            RenderClipList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("AnimPreview", ImVec2(previewWidth, topHeight), true);
            RenderPreview();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("FrameProps", ImVec2(0, topHeight), true);
            RenderFrameProperties();
            ImGui::EndChild();

            // Bottom section: timeline
            ImGui::BeginChild("Timeline", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            RenderTimeline();
            ImGui::EndChild();

            // Auto-slice dialog
            if (m_showAutoSlice)
                RenderAutoSliceDialog();

            EndPanel();
        }
    }

    void SpriteAnimationEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Sprite Animation Editor panel\n";
    }

    void SpriteAnimationEditorPanel::RenderToolbar()
    {
        // Playback controls
        if (m_isPlaying)
        {
            if (ImGui::Button(ICON_FA_PAUSE " Pause"))
                m_isPlaying = false;
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY " Play"))
            {
                m_isPlaying = true;
                if (m_previewFrame < 0)
                    m_previewFrame = 0;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP " Stop"))
        {
            m_isPlaying = false;
            m_previewFrame = 0;
            m_previewTimer = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STEP_FORWARD " Step"))
        {
            m_isPlaying = false;
            if (m_selectedClipIndex >= 0 && m_selectedClipIndex < static_cast<int>(m_clips.size()))
            {
                m_previewFrame++;
                if (m_previewFrame >= static_cast<int>(m_clips[m_selectedClipIndex].frames.size()))
                    m_previewFrame = 0;
            }
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("Speed", &m_previewSpeed, 0.05f, 0.1f, 5.0f, "%.1fx");

        ImGui::SameLine();
        ImGui::Checkbox("Onion Skin", &m_onionSkinning);
        if (m_onionSkinning)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragInt("Frames", &m_onionSkinFrames, 0.1f, 1, 5);
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TH " Auto-Slice"))
            m_showAutoSlice = true;
    }

    void SpriteAnimationEditorPanel::RenderClipList()
    {
        ImGui::Text(ICON_FA_FILM " Animation Clips");
        ImGui::Separator();

        if (ImGui::Button(ICON_FA_PLUS " Add Clip"))
        {
            AnimClip newClip;
            newClip.name = "Clip " + std::to_string(m_clips.size());
            m_clips.push_back(newClip);
            m_selectedClipIndex = static_cast<int>(m_clips.size()) - 1;
        }

        ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_clips.size()); ++i)
        {
            bool selected = (i == m_selectedClipIndex);
            std::string label = m_clips[i].name + " (" + std::to_string(m_clips[i].frames.size()) + " frames)";

            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedClipIndex = i;
                m_selectedFrameIndex = -1;
                m_previewFrame = 0;
                m_previewTimer = 0.0f;
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem())
            {
                char nameBuf[128];
                std::strncpy(nameBuf, m_clips[i].name.c_str(), sizeof(nameBuf) - 1);
                nameBuf[sizeof(nameBuf) - 1] = '\0';
                if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
                    m_clips[i].name = nameBuf;

                ImGui::Checkbox("Loop", &m_clips[i].loop);

                if (ImGui::MenuItem("Duplicate"))
                {
                    AnimClip copy = m_clips[i];
                    copy.name += " (copy)";
                    m_clips.push_back(copy);
                }

                if (ImGui::MenuItem("Delete"))
                {
                    m_clips.erase(m_clips.begin() + i);
                    if (m_selectedClipIndex >= static_cast<int>(m_clips.size()))
                        m_selectedClipIndex = static_cast<int>(m_clips.size()) - 1;
                    ImGui::EndPopup();
                    return;
                }

                ImGui::EndPopup();
            }
        }
    }

    void SpriteAnimationEditorPanel::RenderTimeline()
    {
        ImGui::Text(ICON_FA_STREAM " Timeline");

        if (m_selectedClipIndex < 0 || m_selectedClipIndex >= static_cast<int>(m_clips.size()))
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Select a clip to edit its timeline");
            return;
        }

        auto& clip = m_clips[m_selectedClipIndex];

        // Add frame button
        if (ImGui::Button(ICON_FA_PLUS_SQUARE " Add Frame"))
        {
            AnimFrame frame;
            int numFrames = static_cast<int>(clip.frames.size());
            // Auto-compute sourceRect for sprite sheet layout
            frame.sourceRect[0] = static_cast<float>(numFrames % 8) / 8.0f;
            frame.sourceRect[1] = static_cast<float>(numFrames / 8) / 8.0f;
            frame.sourceRect[2] = frame.sourceRect[0] + 1.0f / 8.0f;
            frame.sourceRect[3] = frame.sourceRect[1] + 1.0f / 8.0f;
            clip.frames.push_back(frame);
        }
        ImGui::SameLine();

        ImGui::Text("Frames: %d", static_cast<int>(clip.frames.size()));

        float totalDuration = 0.0f;
        for (const auto& f : clip.frames)
            totalDuration += f.duration;
        ImGui::SameLine();
        ImGui::Text("| Duration: %.2fs", totalDuration);

        ImGui::Separator();

        // Draw timeline frames
        float frameWidth = 60.0f * m_timelineZoom;
        float frameHeight = 50.0f;
        ImVec2 timelinePos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (int i = 0; i < static_cast<int>(clip.frames.size()); ++i)
        {
            float px = timelinePos.x + i * (frameWidth + 4.0f);
            float py = timelinePos.y;

            // Frame background
            bool isCurrentPreview = (i == m_previewFrame);
            bool isSelected = (i == m_selectedFrameIndex);

            ImU32 bgCol = IM_COL32(60, 60, 80, 255);
            if (isCurrentPreview)
                bgCol = IM_COL32(80, 120, 200, 255);
            if (isSelected)
                bgCol = IM_COL32(200, 120, 40, 255);

            drawList->AddRectFilled(ImVec2(px, py), ImVec2(px + frameWidth, py + frameHeight), bgCol, 4.0f);
            drawList->AddRect(ImVec2(px, py), ImVec2(px + frameWidth, py + frameHeight), IM_COL32(180, 180, 180, 200),
                              4.0f);

            // Frame number
            char frameLabel[32];
            std::snprintf(frameLabel, sizeof(frameLabel), "%d", i);
            drawList->AddText(ImVec2(px + 4, py + 2), IM_COL32(255, 255, 255, 200), frameLabel);

            // Duration label
            char durLabel[32];
            std::snprintf(durLabel, sizeof(durLabel), "%.0fms", clip.frames[i].duration * 1000.0f);
            drawList->AddText(ImVec2(px + 4, py + frameHeight - 16), IM_COL32(200, 200, 200, 180), durLabel);

            // Playhead indicator
            if (isCurrentPreview && m_isPlaying)
            {
                drawList->AddTriangleFilled(ImVec2(px + frameWidth * 0.5f - 5, py - 8),
                                            ImVec2(px + frameWidth * 0.5f + 5, py - 8),
                                            ImVec2(px + frameWidth * 0.5f, py - 2), IM_COL32(0, 255, 100, 255));
            }
        }

        float totalWidth = clip.frames.size() * (frameWidth + 4.0f);
        ImGui::Dummy(ImVec2(totalWidth, frameHeight + 12.0f));

        // Handle click to select frame
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            int clickedFrame = static_cast<int>((mousePos.x - timelinePos.x) / (frameWidth + 4.0f));
            if (clickedFrame >= 0 && clickedFrame < static_cast<int>(clip.frames.size()))
            {
                m_selectedFrameIndex = clickedFrame;
                m_previewFrame = clickedFrame;
            }
        }

        // Timeline zoom
        if (ImGui::IsItemHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (std::abs(wheel) > 0.01f)
            {
                m_timelineZoom *= (wheel > 0) ? 1.1f : 0.9f;
                m_timelineZoom = std::clamp(m_timelineZoom, 0.3f, 4.0f);
            }
        }
    }

    void SpriteAnimationEditorPanel::RenderPreview()
    {
        ImGui::Text(ICON_FA_EYE " Preview");
        ImGui::Separator();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float previewSize = (std::min)(avail.x, avail.y - 20) * 0.9f;

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Checkerboard background
        float gridStep = 8.0f;
        for (float y = 0; y < previewSize; y += gridStep)
        {
            for (float x = 0; x < previewSize; x += gridStep)
            {
                int ix = static_cast<int>(x / gridStep);
                int iy = static_cast<int>(y / gridStep);
                ImU32 col = ((ix + iy) % 2 == 0) ? IM_COL32(200, 200, 200, 255) : IM_COL32(150, 150, 150, 255);
                drawList->AddRectFilled(ImVec2(canvasPos.x + x, canvasPos.y + y),
                                        ImVec2(canvasPos.x + (std::min)(x + gridStep, previewSize),
                                               canvasPos.y + (std::min)(y + gridStep, previewSize)),
                                        col);
            }
        }

        // Onion skinning
        if (m_onionSkinning && m_selectedClipIndex >= 0 && m_selectedClipIndex < static_cast<int>(m_clips.size()))
        {
            const auto& clip = m_clips[m_selectedClipIndex];
            for (int offset = -m_onionSkinFrames; offset <= m_onionSkinFrames; ++offset)
            {
                if (offset == 0)
                    continue;
                int frameIdx = m_previewFrame + offset;
                if (frameIdx < 0 || frameIdx >= static_cast<int>(clip.frames.size()))
                    continue;

                float alpha = 0.15f / std::abs(static_cast<float>(offset));
                ImU32 ghostCol = (offset < 0) ? IM_COL32(100, 100, 255, static_cast<int>(alpha * 255))
                                              : IM_COL32(255, 100, 100, static_cast<int>(alpha * 255));

                float margin = previewSize * 0.1f;
                drawList->AddRectFilled(ImVec2(canvasPos.x + margin, canvasPos.y + margin),
                                        ImVec2(canvasPos.x + previewSize - margin, canvasPos.y + previewSize - margin),
                                        ghostCol, 4.0f);
            }
        }

        // Current frame
        float margin = previewSize * 0.1f;
        drawList->AddRectFilled(ImVec2(canvasPos.x + margin, canvasPos.y + margin),
                                ImVec2(canvasPos.x + previewSize - margin, canvasPos.y + previewSize - margin),
                                IM_COL32(100, 150, 255, 200), 4.0f);

        // Frame counter
        char frameText[64];
        std::snprintf(frameText, sizeof(frameText), "Frame: %d", m_previewFrame);
        drawList->AddText(ImVec2(canvasPos.x + 4, canvasPos.y + previewSize - 16), IM_COL32(255, 255, 255, 200),
                          frameText);

        ImGui::Dummy(ImVec2(previewSize, previewSize));
    }

    void SpriteAnimationEditorPanel::RenderFrameProperties()
    {
        ImGui::Text(ICON_FA_SLIDERS_H " Frame Properties");
        ImGui::Separator();

        if (m_selectedClipIndex < 0 || m_selectedClipIndex >= static_cast<int>(m_clips.size()) ||
            m_selectedFrameIndex < 0 ||
            m_selectedFrameIndex >= static_cast<int>(m_clips[m_selectedClipIndex].frames.size()))
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Select a frame");
            return;
        }

        auto& frame = m_clips[m_selectedClipIndex].frames[m_selectedFrameIndex];

        ImGui::Text("Frame %d of %d", m_selectedFrameIndex + 1,
                    static_cast<int>(m_clips[m_selectedClipIndex].frames.size()));

        ImGui::Spacing();

        ImGui::DragFloat("Duration (s)", &frame.duration, 0.01f, 0.001f, 10.0f, "%.3f");
        float durationMs = frame.duration * 1000.0f;
        if (ImGui::DragFloat("Duration (ms)", &durationMs, 1.0f, 1.0f, 10000.0f, "%.0f"))
            frame.duration = durationMs / 1000.0f;

        ImGui::Spacing();
        ImGui::DragFloat2("UV Min", &frame.sourceRect[0], 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat2("UV Max", &frame.sourceRect[2], 0.01f, 0.0f, 1.0f);

        ImGui::Spacing();

        if (ImGui::Button("Delete Frame"))
        {
            auto& frames = m_clips[m_selectedClipIndex].frames;
            if (m_selectedFrameIndex < static_cast<int>(frames.size()))
            {
                frames.erase(frames.begin() + m_selectedFrameIndex);
                if (m_selectedFrameIndex >= static_cast<int>(frames.size()))
                    m_selectedFrameIndex = static_cast<int>(frames.size()) - 1;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate"))
        {
            auto& frames = m_clips[m_selectedClipIndex].frames;
            if (m_selectedFrameIndex < static_cast<int>(frames.size()))
            {
                frames.insert(frames.begin() + m_selectedFrameIndex + 1, frames[m_selectedFrameIndex]);
                m_selectedFrameIndex++;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Batch operations
        ImGui::Text("Batch Operations:");
        static float batchDuration = 0.1f;
        ImGui::DragFloat("Batch Duration", &batchDuration, 0.01f, 0.001f, 10.0f);
        if (ImGui::Button("Set All Frame Durations"))
        {
            for (auto& f : m_clips[m_selectedClipIndex].frames)
                f.duration = batchDuration;
        }

        if (ImGui::Button("Reverse Frames"))
        {
            auto& frames = m_clips[m_selectedClipIndex].frames;
            std::reverse(frames.begin(), frames.end());
        }
    }

    void SpriteAnimationEditorPanel::RenderAutoSliceDialog()
    {
        ImGui::SetNextWindowSize(ImVec2(350, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Auto-Slice Sprite Sheet", &m_showAutoSlice, ImGuiWindowFlags_NoDocking))
        {
            ImGui::Text("Automatically create animation frames from a sprite sheet grid.");
            ImGui::Spacing();

            ImGui::DragInt("Columns", &m_autoSliceCols, 0.1f, 1, 64);
            ImGui::DragInt("Rows", &m_autoSliceRows, 0.1f, 1, 64);
            ImGui::DragFloat("Frame Duration (s)", &m_autoSliceFrameDuration, 0.01f, 0.001f, 10.0f, "%.3f");

            ImGui::Spacing();
            int totalFrames = m_autoSliceCols * m_autoSliceRows;
            ImGui::Text("Total frames: %d", totalFrames);
            ImGui::Text("Total duration: %.2fs", totalFrames * m_autoSliceFrameDuration);

            ImGui::Spacing();

            if (ImGui::Button("Generate Frames"))
            {
                if (m_selectedClipIndex >= 0 && m_selectedClipIndex < static_cast<int>(m_clips.size()))
                {
                    auto& clip = m_clips[m_selectedClipIndex];
                    clip.frames.clear();

                    for (int y = 0; y < m_autoSliceRows; ++y)
                    {
                        for (int x = 0; x < m_autoSliceCols; ++x)
                        {
                            AnimFrame frame;
                            frame.sourceRect[0] = static_cast<float>(x) / m_autoSliceCols;
                            frame.sourceRect[1] = static_cast<float>(y) / m_autoSliceRows;
                            frame.sourceRect[2] = static_cast<float>(x + 1) / m_autoSliceCols;
                            frame.sourceRect[3] = static_cast<float>(y + 1) / m_autoSliceRows;
                            frame.duration = m_autoSliceFrameDuration;
                            clip.frames.push_back(frame);
                        }
                    }
                    m_showAutoSlice = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                m_showAutoSlice = false;
        }
        ImGui::End();
    }

} // namespace SparkEditor
