/**
 * @file AnimationTimelineUI.cpp
 * @brief ImGui rendering and input handling for AnimationTimeline
 */

#include "AnimationTimeline.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DirectX;
namespace SparkEditor
{

    // ---------------------------------------------------------------------------
    // Top-level render and lifecycle
    // ---------------------------------------------------------------------------

    void AnimationTimeline::Render()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_isVisible)
            return;

        ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Animation Timeline", &m_isVisible))
        {
            ImGui::End();
            return;
        }

        RenderPlaybackControls();
        ImGui::Separator();
        RenderTimelineHeader();
        ImGui::Separator();

        // Split: track list on left, timeline on right
        float availWidth = ImGui::GetContentRegionAvail().x;
        float availHeight = ImGui::GetContentRegionAvail().y;

        if (m_showCurveEditor)
        {
            // Top half: dope sheet, bottom half: curve editor
            float halfHeight = availHeight * 0.5f;

            ImGui::BeginChild("##TrackAndTimeline", ImVec2(0, halfHeight), true);
            {
                ImGui::BeginChild("##TrackList", ImVec2(m_trackListWidth, 0), true);
                RenderTrackList();
                ImGui::EndChild();

                ImGui::SameLine();

                ImGui::BeginChild("##TimelineArea", ImVec2(0, 0), true);
                RenderTimelineEditor();
                ImGui::EndChild();
            }
            ImGui::EndChild();

            ImGui::BeginChild("##CurveEditor", ImVec2(0, 0), true);
            RenderCurveEditor();
            ImGui::EndChild();
        }
        else
        {
            ImGui::BeginChild("##TrackList", ImVec2(m_trackListWidth, 0), true);
            RenderTrackList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("##TimelineArea", ImVec2(0, 0), true);
            RenderTimelineEditor();
            ImGui::EndChild();
        }

        ImGui::End();
    }

    bool AnimationTimeline::HandleEvent(const std::string& eventType, void* eventData)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "AnimationTimeline handling event: %s", eventType.c_str());
        if (eventType == "play")
        {
            Play();
            return true;
        }
        else if (eventType == "pause")
        {
            Pause();
            return true;
        }
        else if (eventType == "stop")
        {
            Stop();
            return true;
        }
        else if (eventType == "delete_keyframes")
        {
            RemoveSelectedKeyframes();
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Render sub-methods
    // ---------------------------------------------------------------------------

    void AnimationTimeline::RenderPlaybackControls()
    {
        if (!m_currentClip)
        {
            ImGui::Text("No animation clip loaded");
            return;
        }

        // Transport controls
        if (ImGui::Button("|<"))
            GoToStart();
        ImGui::SameLine();
        if (ImGui::Button("<<"))
            StepBackward();
        ImGui::SameLine();

        if (m_playbackState == PlaybackState::PLAYING)
        {
            if (ImGui::Button("||"))
                Pause();
        }
        else
        {
            if (ImGui::Button(">"))
                Play();
        }
        ImGui::SameLine();
        if (ImGui::Button("[]"))
            Stop();
        ImGui::SameLine();
        if (ImGui::Button(">>"))
            StepForward();
        ImGui::SameLine();
        if (ImGui::Button(">|"))
            GoToEnd();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // Recording toggle
        bool recording = IsRecording();
        if (recording)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("REC"))
            SetRecording(!recording);
        if (recording)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // Time display
        float time = m_currentClip->currentTime;
        int frame = m_currentClip->TimeToFrame(time);
        ImGui::Text("Time: %.2f  Frame: %d / %d", time, frame, m_currentClip->GetFrameCount());

        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("Speed", &m_playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2fx");

        ImGui::SameLine();
        ImGui::Checkbox("Loop", &m_loopPlayback);

        ImGui::SameLine();
        ImGui::Checkbox("Curve Editor", &m_showCurveEditor);

        ImGui::SameLine();
        ImGui::Checkbox("Snap", &m_snapToFrames);
    }

    void AnimationTimeline::RenderTimelineHeader()
    {
        if (!m_currentClip)
            return;

        ImGui::Text("Clip: %s", m_currentClip->name.c_str());
        ImGui::SameLine();
        ImGui::Text("  Duration: %.2fs  FPS: %.0f  Tracks: %zu", m_currentClip->duration, m_currentClip->frameRate,
                    m_currentClip->tracks.size());

        ImGui::SameLine();
        if (ImGui::Button("Frame All"))
            FrameAll();
        ImGui::SameLine();
        if (ImGui::Button("Frame Sel"))
            FrameSelected();
    }

    void AnimationTimeline::RenderTrackList()
    {
        if (!m_currentClip)
            return;

        ImGui::Text("Tracks");
        ImGui::Separator();

        for (size_t i = 0; i < m_currentClip->tracks.size(); ++i)
        {
            auto& track = m_currentClip->tracks[i];
            ImGui::PushID(static_cast<int>(i));

            bool expanded = track->isExpanded;
            if (ImGui::TreeNodeEx(track->objectName.c_str(), expanded ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            {
                track->isExpanded = true;

                // Mute / visibility toggles
                ImGui::SameLine();
                ImGui::Checkbox("V", &track->isVisible);
                ImGui::SameLine();
                ImGui::Checkbox("M", &track->isMuted);

                // Show curves
                for (size_t ci = 0; ci < track->curves.size(); ++ci)
                {
                    auto& curve = track->curves[ci];
                    ImGui::PushID(static_cast<int>(ci));

                    ImGui::BulletText("%s (%zu keys)", curve->displayName.c_str(), curve->keyframes.size());
                    ImGui::SameLine();
                    ImGui::Checkbox("V##c", &curve->isVisible);
                    ImGui::SameLine();
                    ImGui::Checkbox("M##c", &curve->isMuted);

                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            else
            {
                track->isExpanded = false;
            }

            ImGui::PopID();
        }
    }

    void AnimationTimeline::RenderTimelineEditor()
    {
        if (!m_currentClip)
            return;

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 10.0f || canvasSize.y < 10.0f)
            return;

        XMFLOAT4 timelineRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(
            ImVec2(timelineRect.x, timelineRect.y),
            ImVec2(timelineRect.x + timelineRect.z, timelineRect.y + timelineRect.w),
            ImGui::ColorConvertFloat4ToU32(ImVec4(m_timelineBackgroundColor.x, m_timelineBackgroundColor.y,
                                                  m_timelineBackgroundColor.z, m_timelineBackgroundColor.w)));

        // Time ruler at top
        XMFLOAT4 rulerRect = {timelineRect.x, timelineRect.y, timelineRect.z, 24.0f};
        RenderTimeRuler(rulerRect);

        // Tracks
        float trackY = timelineRect.y + 24.0f;
        for (size_t i = 0; i < m_currentClip->tracks.size(); ++i)
        {
            auto& track = m_currentClip->tracks[i];
            if (!track->isVisible)
                continue;

            float trackBottom = trackY + m_trackHeight;
            if (trackBottom > timelineRect.y + timelineRect.w)
                break;

            RenderTrack(track.get(), static_cast<int>(i));

            // Render keyframes for each visible curve
            for (auto& curve : track->curves)
            {
                if (curve->isVisible && !curve->isMuted)
                {
                    XMFLOAT4 kfRect = {timelineRect.x, trackY, timelineRect.z, m_trackHeight};
                    RenderKeyframes(curve.get(), kfRect);
                }
            }

            trackY += m_trackHeight;
        }

        // Playhead
        RenderPlayhead(timelineRect);

        // Handle input (invisible button over the area)
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::InvisibleButton("##timeline_input", canvasSize);
        HandleTimelineInput(timelineRect);
    }

    void AnimationTimeline::RenderCurveEditor()
    {
        if (!m_currentClip)
            return;

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 10.0f || canvasSize.y < 10.0f)
            return;

        XMFLOAT4 curveRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(ImVec2(curveRect.x, curveRect.y),
                                ImVec2(curveRect.x + curveRect.z, curveRect.y + curveRect.w),
                                IM_COL32(30, 30, 30, 255));

        // Draw grid lines for value axis
        float valueRange = m_curveViewMaxValue - m_curveViewMinValue;
        if (valueRange > 0.0f)
        {
            float step = 1.0f;
            if (valueRange > 20.0f)
                step = 5.0f;
            if (valueRange > 100.0f)
                step = 10.0f;

            float v = std::ceil(m_curveViewMinValue / step) * step;
            while (v <= m_curveViewMaxValue)
            {
                float sy = ValueToScreen(v, curveRect);
                drawList->AddLine(ImVec2(curveRect.x, sy), ImVec2(curveRect.x + curveRect.z, sy),
                                  IM_COL32(60, 60, 60, 255));
                char label[32];
                snprintf(label, sizeof(label), "%.1f", v);
                drawList->AddText(ImVec2(curveRect.x + 2, sy - 8), IM_COL32(150, 150, 150, 255), label);
                v += step;
            }
        }

        // Zero line
        float zeroY = ValueToScreen(0.0f, curveRect);
        if (zeroY >= curveRect.y && zeroY <= curveRect.y + curveRect.w)
        {
            drawList->AddLine(ImVec2(curveRect.x, zeroY), ImVec2(curveRect.x + curveRect.z, zeroY),
                              IM_COL32(100, 100, 100, 255));
        }

        // Render curves
        for (auto& track : m_currentClip->tracks)
        {
            if (!track->isVisible || track->isMuted)
                continue;
            for (auto& curve : track->curves)
            {
                if (curve->isVisible && !curve->isMuted)
                {
                    RenderCurve(curve.get());
                }
            }
        }

        // Playhead in curve editor
        RenderPlayhead(curveRect);

        // Handle curve editor input
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::InvisibleButton("##curve_input", canvasSize);
        HandleCurveEditorInput();
    }

    void AnimationTimeline::RenderAnimationProperties()
    {
        if (!m_currentClip)
            return;

        ImGui::Text("Animation Properties");
        ImGui::Separator();

        char nameBuffer[256];
        strncpy(nameBuffer, m_currentClip->name.c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Animation clip renamed to '%s'", nameBuffer);
            m_currentClip->name = nameBuffer;
        }

        ImGui::DragFloat("Duration", &m_currentClip->duration, 0.1f, 0.1f, 600.0f, "%.2f s");
        ImGui::DragFloat("Frame Rate", &m_currentClip->frameRate, 1.0f, 1.0f, 120.0f, "%.0f fps");
        ImGui::Checkbox("Looping", &m_currentClip->isLooping);
    }

    void AnimationTimeline::RenderTrack(AnimationTrack* track, int trackIndex)
    {
        if (!track)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();

        // Track background (alternating colors)
        ImU32 bgColor = (trackIndex % 2 == 0) ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.18f, 0.18f, 1.0f))
                                              : ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.22f, 0.22f, 1.0f));

        float trackWidth = ImGui::GetContentRegionAvail().x;
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + trackWidth, canvasPos.y + m_trackHeight), bgColor);

        // Track separator line
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + m_trackHeight),
                          ImVec2(canvasPos.x + trackWidth, canvasPos.y + m_trackHeight), IM_COL32(80, 80, 80, 255));
    }

    void AnimationTimeline::RenderKeyframes(AnimationCurve* curve, const XMFLOAT4& trackRect)
    {
        if (!curve)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float centerY = trackRect.y + trackRect.w * 0.5f;
        float diamondSize = 5.0f;

        for (auto& kf : curve->keyframes)
        {
            float sx = TimeToScreen(kf.time, trackRect);

            // Skip keyframes outside visible range
            if (sx < trackRect.x - diamondSize || sx > trackRect.x + trackRect.z + diamondSize)
                continue;

            ImU32 color;
            if (kf.isSelected)
            {
                color = ImGui::ColorConvertFloat4ToU32(ImVec4(m_selectedKeyframeColor.x, m_selectedKeyframeColor.y,
                                                              m_selectedKeyframeColor.z, m_selectedKeyframeColor.w));
            }
            else
            {
                color = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(m_keyframeColor.x, m_keyframeColor.y, m_keyframeColor.z, m_keyframeColor.w));
            }

            // Draw diamond shape
            ImVec2 points[4] = {
                ImVec2(sx, centerY - diamondSize), // top
                ImVec2(sx + diamondSize, centerY), // right
                ImVec2(sx, centerY + diamondSize), // bottom
                ImVec2(sx - diamondSize, centerY)  // left
            };
            drawList->AddConvexPolyFilled(points, 4, color);
            drawList->AddPolyline(points, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 1.0f);
        }
    }

    void AnimationTimeline::RenderCurve(AnimationCurve* curve)
    {
        if (!curve || curve->keyframes.size() < 2)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        XMFLOAT4 curveRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

        // Determine curve color
        ImU32 lineColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(curve->color.x, curve->color.y, curve->color.z, curve->color.w));

        // Build polyline by evaluating at fine intervals
        const int numSamples = 200;
        float startTime = m_viewStartTime;
        float endTime = m_viewEndTime;
        float dt = (endTime - startTime) / static_cast<float>(numSamples);

        std::vector<ImVec2> points;
        points.reserve(numSamples + 1);

        for (int i = 0; i <= numSamples; ++i)
        {
            float t = startTime + i * dt;
            XMFLOAT4 val = curve->Evaluate(t);
            float sx = TimeToScreen(t, curveRect);
            float sy = ValueToScreen(val.x, curveRect);
            points.push_back(ImVec2(sx, sy));
        }

        if (points.size() >= 2)
        {
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()), lineColor, ImDrawFlags_None, 1.5f);
        }

        // Draw keyframe handles in curve editor
        float diamondSize = 4.0f;
        for (auto& kf : curve->keyframes)
        {
            float sx = TimeToScreen(kf.time, curveRect);
            float sy = ValueToScreen(kf.value.x, curveRect);

            ImU32 kfColor =
                kf.isSelected
                    ? ImGui::ColorConvertFloat4ToU32(ImVec4(m_selectedKeyframeColor.x, m_selectedKeyframeColor.y,
                                                            m_selectedKeyframeColor.z, m_selectedKeyframeColor.w))
                    : ImGui::ColorConvertFloat4ToU32(
                          ImVec4(m_keyframeColor.x, m_keyframeColor.y, m_keyframeColor.z, m_keyframeColor.w));

            // Diamond
            ImVec2 pts[4] = {ImVec2(sx, sy - diamondSize), ImVec2(sx + diamondSize, sy), ImVec2(sx, sy + diamondSize),
                             ImVec2(sx - diamondSize, sy)};
            drawList->AddConvexPolyFilled(pts, 4, kfColor);

            // Tangent handles
            if (m_showCurveHandles && kf.isSelected && kf.interpolation == AnimationKeyframe::BEZIER)
            {
                float tangentScale = 30.0f;
                // In tangent
                ImVec2 inHandle(sx - tangentScale * kf.inTangent.x, sy - tangentScale * kf.inTangent.y);
                drawList->AddLine(ImVec2(sx, sy), inHandle, IM_COL32(100, 200, 100, 200), 1.0f);
                drawList->AddCircleFilled(inHandle, 3.0f, IM_COL32(100, 200, 100, 255));

                // Out tangent
                ImVec2 outHandle(sx + tangentScale * kf.outTangent.x, sy - tangentScale * kf.outTangent.y);
                drawList->AddLine(ImVec2(sx, sy), outHandle, IM_COL32(200, 100, 100, 200), 1.0f);
                drawList->AddCircleFilled(outHandle, 3.0f, IM_COL32(200, 100, 100, 255));
            }
        }
    }

    void AnimationTimeline::RenderTimeRuler(const XMFLOAT4& timelineRect)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Ruler background
        drawList->AddRectFilled(ImVec2(timelineRect.x, timelineRect.y),
                                ImVec2(timelineRect.x + timelineRect.z, timelineRect.y + timelineRect.w),
                                IM_COL32(40, 40, 40, 255));

        float viewDuration = m_viewEndTime - m_viewStartTime;
        if (viewDuration <= 0.0f)
            return;

        // Determine tick interval based on zoom
        float pixelsPerSecond = timelineRect.z / viewDuration;
        float minTickSpacing = 80.0f; // minimum pixels between major ticks

        // Find a nice tick interval
        float rawInterval = minTickSpacing / pixelsPerSecond;
        float magnitude = std::pow(10.0f, std::floor(std::log10(rawInterval)));
        float residual = rawInterval / magnitude;

        float tickInterval;
        if (residual <= 1.5f)
            tickInterval = magnitude;
        else if (residual <= 3.5f)
            tickInterval = 2.0f * magnitude;
        else if (residual <= 7.5f)
            tickInterval = 5.0f * magnitude;
        else
            tickInterval = 10.0f * magnitude;

        if (tickInterval <= 0.0f)
            tickInterval = 1.0f;

        // Draw tick marks
        float startTick = std::ceil(m_viewStartTime / tickInterval) * tickInterval;
        for (float t = startTick; t <= m_viewEndTime; t += tickInterval)
        {
            float sx = TimeToScreen(t, timelineRect);

            // Major tick
            drawList->AddLine(ImVec2(sx, timelineRect.y + timelineRect.w - 12),
                              ImVec2(sx, timelineRect.y + timelineRect.w), IM_COL32(200, 200, 200, 255));

            // Label
            char label[32];
            if (m_showFrameNumbers && m_currentClip)
            {
                int frame = m_currentClip->TimeToFrame(t);
                snprintf(label, sizeof(label), "%d", frame);
            }
            else
            {
                snprintf(label, sizeof(label), "%.2f", t);
            }
            drawList->AddText(ImVec2(sx + 2, timelineRect.y + 2), IM_COL32(200, 200, 200, 255), label);

            // Minor ticks (subdivide into 5)
            float minorInterval = tickInterval / 5.0f;
            for (int m = 1; m < 5; ++m)
            {
                float mt = t + m * minorInterval;
                if (mt > m_viewEndTime)
                    break;
                float msx = TimeToScreen(mt, timelineRect);
                drawList->AddLine(ImVec2(msx, timelineRect.y + timelineRect.w - 6),
                                  ImVec2(msx, timelineRect.y + timelineRect.w), IM_COL32(120, 120, 120, 255));
            }
        }

        // Bottom border
        drawList->AddLine(ImVec2(timelineRect.x, timelineRect.y + timelineRect.w),
                          ImVec2(timelineRect.x + timelineRect.z, timelineRect.y + timelineRect.w),
                          IM_COL32(80, 80, 80, 255));
    }

    void AnimationTimeline::RenderPlayhead(const XMFLOAT4& timelineRect)
    {
        if (!m_currentClip)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float sx = TimeToScreen(m_currentClip->currentTime, timelineRect);

        // Clamp playhead to visible area
        if (sx < timelineRect.x || sx > timelineRect.x + timelineRect.z)
            return;

        ImU32 playheadCol = ImGui::ColorConvertFloat4ToU32(
            ImVec4(m_playheadColor.x, m_playheadColor.y, m_playheadColor.z, m_playheadColor.w));

        // Vertical line
        drawList->AddLine(ImVec2(sx, timelineRect.y), ImVec2(sx, timelineRect.y + timelineRect.w), playheadCol, 2.0f);

        // Triangle at top
        drawList->AddTriangleFilled(ImVec2(sx - 6, timelineRect.y), ImVec2(sx + 6, timelineRect.y),
                                    ImVec2(sx, timelineRect.y + 10), playheadCol);
    }

    // ---------------------------------------------------------------------------
    // Input handling
    // ---------------------------------------------------------------------------

    void AnimationTimeline::HandleTimelineInput(const XMFLOAT4& timelineRect)
    {
        if (!m_currentClip)
            return;

        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mousePos = io.MousePos;

        bool isHovered = ImGui::IsItemHovered();

        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            float clickTime = ScreenToTime(mousePos.x, timelineRect);

            // Check if clicking on playhead area (top 24 pixels)
            if (mousePos.y < timelineRect.y + 24.0f)
            {
                m_isDraggingPlayhead = true;
                if (m_snapToFrames)
                    clickTime = SnapToFrame(clickTime);
                m_currentClip->SetTime(clickTime);
            }
            else
            {
                // Try to select a keyframe
                XMFLOAT2 mp = {mousePos.x, mousePos.y};
                bool additive = io.KeyShift;
                HandleKeyframeSelection(mp, additive);
            }
        }

        // Dragging playhead
        if (m_isDraggingPlayhead)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                float dragTime = ScreenToTime(mousePos.x, timelineRect);
                if (m_snapToFrames)
                    dragTime = SnapToFrame(dragTime);
                m_currentClip->SetTime(dragTime);
            }
            else
            {
                m_isDraggingPlayhead = false;
            }
        }

        // Dragging keyframes
        if (m_isDraggingKeyframes)
        {
            HandleKeyframeDragging();
        }

        // Mouse wheel zoom
        if (isHovered && std::abs(io.MouseWheel) > 0.0f)
        {
            float zoomFactor = 1.0f + io.MouseWheel * 0.1f;
            float mouseTime = ScreenToTime(mousePos.x, timelineRect);

            float newStart = mouseTime + (m_viewStartTime - mouseTime) / zoomFactor;
            float newEnd = mouseTime + (m_viewEndTime - mouseTime) / zoomFactor;
            SetViewRange(newStart, newEnd);
        }

        // Middle mouse pan
        if (isHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            float panDelta = -io.MouseDelta.x * (m_viewEndTime - m_viewStartTime) / timelineRect.z;
            m_viewStartTime += panDelta;
            m_viewEndTime += panDelta;
        }

        // Right-click context menu
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("##timeline_context");
        }
        if (ImGui::BeginPopup("##timeline_context"))
        {
            float clickTime = ScreenToTime(mousePos.x, timelineRect);
            if (ImGui::MenuItem("Add Keyframe Here"))
            {
                if (m_snapToFrames)
                    clickTime = SnapToFrame(clickTime);
                // Add keyframe on the first selected or first available curve
                if (!m_currentClip->tracks.empty() && !m_currentClip->tracks[0]->curves.empty())
                {
                    auto& curve = m_currentClip->tracks[0]->curves[0];
                    AnimationKeyframe kf;
                    kf.time = clickTime;
                    kf.value = curve->Evaluate(clickTime);
                    curve->AddKeyframe(kf);
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Added keyframe via context menu at time=%.3f",
                                    clickTime);
                }
            }
            if (ImGui::MenuItem("Delete Selected", nullptr, false, !m_selection.selectedKeyframes.empty()))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Deleting %zu selected keyframes",
                                m_selection.selectedKeyframes.size());
                RemoveSelectedKeyframes();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Interpolation"))
            {
                if (ImGui::MenuItem("Linear"))
                    SetKeyframeInterpolation(AnimationKeyframe::LINEAR);
                if (ImGui::MenuItem("Bezier"))
                    SetKeyframeInterpolation(AnimationKeyframe::BEZIER);
                if (ImGui::MenuItem("Step"))
                    SetKeyframeInterpolation(AnimationKeyframe::STEP);
                if (ImGui::MenuItem("Ease In"))
                    SetKeyframeInterpolation(AnimationKeyframe::EASE_IN);
                if (ImGui::MenuItem("Ease Out"))
                    SetKeyframeInterpolation(AnimationKeyframe::EASE_OUT);
                if (ImGui::MenuItem("Ease In/Out"))
                    SetKeyframeInterpolation(AnimationKeyframe::EASE_IN_OUT);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Frame All"))
                FrameAll();
            if (ImGui::MenuItem("Frame Selected"))
                FrameSelected();
            if (ImGui::MenuItem("Auto Fit View"))
                AutoFitView();
            ImGui::EndPopup();
        }
    }

    void AnimationTimeline::HandleCurveEditorInput()
    {
        if (!m_currentClip)
            return;

        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mousePos = io.MousePos;
        bool isHovered = ImGui::IsItemHovered();

        ImVec2 canvasPos = ImGui::GetItemRectMin();
        ImVec2 canvasSize = ImGui::GetItemRectSize();
        XMFLOAT4 curveRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

        // Mouse wheel zoom (vertical)
        if (isHovered && std::abs(io.MouseWheel) > 0.0f)
        {
            float zoomFactor = 1.0f + io.MouseWheel * 0.1f;
            float mouseVal = ScreenToValue(mousePos.y, curveRect);
            m_curveViewMinValue = mouseVal + (m_curveViewMinValue - mouseVal) / zoomFactor;
            m_curveViewMaxValue = mouseVal + (m_curveViewMaxValue - mouseVal) / zoomFactor;
        }

        // Middle mouse pan
        if (isHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            float panX = -io.MouseDelta.x * (m_viewEndTime - m_viewStartTime) / curveRect.z;
            float panY = io.MouseDelta.y * (m_curveViewMaxValue - m_curveViewMinValue) / curveRect.w;
            m_viewStartTime += panX;
            m_viewEndTime += panX;
            m_curveViewMinValue += panY;
            m_curveViewMaxValue += panY;
        }
    }

    void AnimationTimeline::HandleKeyframeSelection(const XMFLOAT2& mousePos, bool isAdditive)
    {
        if (!m_currentClip)
            return;

        AnimationKeyframe* hit = FindKeyframeAtPosition(mousePos, 8.0f);

        if (!isAdditive)
        {
            // Deselect all
            for (auto& track : m_currentClip->tracks)
            {
                for (auto& curve : track->curves)
                {
                    for (auto& kf : curve->keyframes)
                    {
                        kf.isSelected = false;
                    }
                }
            }
            m_selection.selectedKeyframes.clear();
        }

        if (hit)
        {
            hit->isSelected = !hit->isSelected;
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Keyframe %s at time=%.3f",
                            hit->isSelected ? "selected" : "deselected", hit->time);
            if (hit->isSelected)
            {
                m_selection.selectedKeyframes.push_back(hit);
                m_isDraggingKeyframes = true;
                m_dragStartPos = mousePos;
            }
            else
            {
                // Remove from selection
                m_selection.selectedKeyframes.erase(
                    std::remove(m_selection.selectedKeyframes.begin(), m_selection.selectedKeyframes.end(), hit),
                    m_selection.selectedKeyframes.end());
            }
        }
    }

    void AnimationTimeline::HandleKeyframeDragging()
    {
        if (!m_currentClip)
            return;

        ImGuiIO& io = ImGui::GetIO();

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_isDraggingKeyframes = false;
            return;
        }

        // Compute time delta from mouse delta
        ImVec2 canvasPos = ImGui::GetWindowPos();
        ImVec2 canvasSize = ImGui::GetWindowSize();
        XMFLOAT4 timelineRect = {canvasPos.x + m_trackListWidth, canvasPos.y, canvasSize.x - m_trackListWidth,
                                 canvasSize.y};

        float timeDelta = io.MouseDelta.x * (m_viewEndTime - m_viewStartTime) / timelineRect.z;

        for (auto* kf : m_selection.selectedKeyframes)
        {
            if (!kf->isLocked)
            {
                kf->time += timeDelta;
                if (m_snapToFrames)
                {
                    kf->time = SnapToFrame(kf->time);
                }
            }
        }
    }

    AnimationKeyframe* AnimationTimeline::FindKeyframeAtPosition(const XMFLOAT2& screenPos, float tolerance)
    {
        if (!m_currentClip)
            return nullptr;

        // We need the timeline rect to map. Use a rough rect from current ImGui state.
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        XMFLOAT4 timelineRect = {winPos.x + m_trackListWidth, winPos.y, winSize.x - m_trackListWidth, winSize.y};

        for (auto& track : m_currentClip->tracks)
        {
            for (auto& curve : track->curves)
            {
                for (auto& kf : curve->keyframes)
                {
                    float sx = TimeToScreen(kf.time, timelineRect);
                    float dx = screenPos.x - sx;
                    float dy = screenPos.y - (timelineRect.y + timelineRect.w * 0.5f);
                    if (std::sqrt(dx * dx + dy * dy) < tolerance)
                    {
                        return &kf;
                    }
                }
            }
        }
        return nullptr;
    }

} // namespace SparkEditor
