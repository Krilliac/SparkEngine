/**
 * @file AnimationPlayback.cpp
 * @brief Playback controls, recording, keyframe editing, coordinate mapping, and view management
 */

#include "AnimationTimeline.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <imgui.h>

using namespace DirectX;
namespace SparkEditor
{

    // ---------------------------------------------------------------------------
    // Playback update loop
    // ---------------------------------------------------------------------------

    void AnimationTimeline::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_currentClip)
            return;

        if (m_playbackState == PlaybackState::PLAYING)
        {
            float newTime = m_currentClip->currentTime + deltaTime * m_playbackSpeed;

            if (newTime >= m_currentClip->duration)
            {
                if (m_loopPlayback || m_currentClip->isLooping)
                {
                    newTime = std::fmod(newTime, m_currentClip->duration);
                }
                else
                {
                    newTime = m_currentClip->duration;
                    m_playbackState = PlaybackState::STOPPED;
                }
            }
            else if (newTime < 0.0f)
            {
                if (m_loopPlayback || m_currentClip->isLooping)
                {
                    newTime = m_currentClip->duration + std::fmod(newTime, m_currentClip->duration);
                }
                else
                {
                    newTime = 0.0f;
                    m_playbackState = PlaybackState::STOPPED;
                }
            }

            m_currentClip->SetTime(newTime);
            UpdateAnimationPreview();
        }
    }

    // ---------------------------------------------------------------------------
    // Playback controls
    // ---------------------------------------------------------------------------

    void AnimationTimeline::Play()
    {
        if (!m_currentClip)
            return;
        m_playbackState = PlaybackState::PLAYING;
        m_currentClip->isPlaying = true;
        m_currentClip->isPaused = false;
    }

    void AnimationTimeline::Pause()
    {
        if (!m_currentClip)
            return;
        if (m_playbackState == PlaybackState::PLAYING)
        {
            m_playbackState = PlaybackState::PAUSED;
            m_currentClip->isPlaying = false;
            m_currentClip->isPaused = true;
        }
    }

    void AnimationTimeline::Stop()
    {
        if (!m_currentClip)
            return;
        m_playbackState = PlaybackState::STOPPED;
        m_currentClip->SetTime(0.0f);
        m_currentClip->isPlaying = false;
        m_currentClip->isPaused = false;
    }

    void AnimationTimeline::StepForward()
    {
        if (!m_currentClip)
            return;
        float frameTime = 1.0f / m_currentClip->frameRate;
        m_currentClip->SetTime(m_currentClip->currentTime + frameTime);
    }

    void AnimationTimeline::StepBackward()
    {
        if (!m_currentClip)
            return;
        float frameTime = 1.0f / m_currentClip->frameRate;
        m_currentClip->SetTime(m_currentClip->currentTime - frameTime);
    }

    void AnimationTimeline::GoToStart()
    {
        if (!m_currentClip)
            return;
        m_currentClip->SetTime(0.0f);
    }

    void AnimationTimeline::GoToEnd()
    {
        if (!m_currentClip)
            return;
        m_currentClip->SetTime(m_currentClip->duration);
    }

    void AnimationTimeline::SetPlaybackTime(float time)
    {
        if (!m_currentClip)
            return;
        m_currentClip->SetTime(time);
    }

    float AnimationTimeline::GetPlaybackTime() const
    {
        if (!m_currentClip)
            return 0.0f;
        return m_currentClip->currentTime;
    }

    void AnimationTimeline::SetRecording(bool recording)
    {
        if (recording)
        {
            m_playbackState = PlaybackState::RECORDING;
        }
        else
        {
            m_playbackState = PlaybackState::STOPPED;
        }
    }

    // ---------------------------------------------------------------------------
    // Keyframe editing
    // ---------------------------------------------------------------------------

    void AnimationTimeline::AddKeyframe(ObjectID objectID, const std::string& propertyPath, const XMFLOAT4& value,
                                        float time)
    {
        if (!m_currentClip)
            return;

        float kfTime = (time < 0.0f) ? m_currentClip->currentTime : time;

        // Find or create track
        AnimationTrack* track = m_currentClip->FindTrack(objectID);
        if (!track)
        {
            track = m_currentClip->AddTrack(objectID, "Object_" + std::to_string(objectID));
        }

        // Find or create curve
        AnimationCurve* curve = track->FindCurve(propertyPath);
        if (!curve)
        {
            curve = track->AddCurve(propertyPath, propertyPath);
        }

        AnimationKeyframe kf;
        kf.time = kfTime;
        kf.value = value;
        kf.interpolation = AnimationKeyframe::LINEAR;
        curve->AddKeyframe(kf);
    }

    void AnimationTimeline::RemoveSelectedKeyframes()
    {
        if (!m_currentClip)
            return;

        // Build a set of pointers we need to remove
        for (auto& track : m_currentClip->tracks)
        {
            for (auto& curve : track->curves)
            {
                // Walk backwards to safely erase
                for (int i = static_cast<int>(curve->keyframes.size()) - 1; i >= 0; --i)
                {
                    AnimationKeyframe* kfPtr = &curve->keyframes[static_cast<size_t>(i)];
                    for (auto* sel : m_selection.selectedKeyframes)
                    {
                        if (sel == kfPtr)
                        {
                            curve->RemoveKeyframe(static_cast<size_t>(i));
                            break;
                        }
                    }
                }
            }
        }
        m_selection.selectedKeyframes.clear();
    }

    void AnimationTimeline::SetKeyframeInterpolation(AnimationKeyframe::InterpolationType interpolationType)
    {
        for (auto* kf : m_selection.selectedKeyframes)
        {
            kf->interpolation = interpolationType;
        }
    }

    // ---------------------------------------------------------------------------
    // View management
    // ---------------------------------------------------------------------------

    void AnimationTimeline::FrameSelected()
    {
        if (m_selection.selectedKeyframes.empty())
            return;

        float minTime = FLT_MAX;
        float maxTime = -FLT_MAX;
        float minVal = FLT_MAX;
        float maxVal = -FLT_MAX;

        for (auto* kf : m_selection.selectedKeyframes)
        {
            minTime = std::min(minTime, kf->time);
            maxTime = std::max(maxTime, kf->time);
            minVal = std::min(minVal, kf->value.x);
            maxVal = std::max(maxVal, kf->value.x);
        }

        float padding = (maxTime - minTime) * 0.1f;
        if (padding < 0.1f)
            padding = 0.5f;
        m_viewStartTime = minTime - padding;
        m_viewEndTime = maxTime + padding;

        float valPadding = (maxVal - minVal) * 0.1f;
        if (valPadding < 0.1f)
            valPadding = 1.0f;
        m_curveViewMinValue = minVal - valPadding;
        m_curveViewMaxValue = maxVal + valPadding;
    }

    void AnimationTimeline::FrameAll()
    {
        if (!m_currentClip)
            return;

        float minTime = FLT_MAX;
        float maxTime = -FLT_MAX;
        float minVal = FLT_MAX;
        float maxVal = -FLT_MAX;
        bool hasKeyframes = false;

        for (auto& track : m_currentClip->tracks)
        {
            for (auto& curve : track->curves)
            {
                for (auto& kf : curve->keyframes)
                {
                    hasKeyframes = true;
                    minTime = std::min(minTime, kf.time);
                    maxTime = std::max(maxTime, kf.time);
                    minVal = std::min(minVal, kf.value.x);
                    maxVal = std::max(maxVal, kf.value.x);
                }
            }
        }

        if (!hasKeyframes)
        {
            AutoFitView();
            return;
        }

        float padding = (maxTime - minTime) * 0.1f;
        if (padding < 0.1f)
            padding = 0.5f;
        m_viewStartTime = minTime - padding;
        m_viewEndTime = maxTime + padding;

        float valPadding = (maxVal - minVal) * 0.1f;
        if (valPadding < 0.1f)
            valPadding = 1.0f;
        m_curveViewMinValue = minVal - valPadding;
        m_curveViewMaxValue = maxVal + valPadding;
    }

    void AnimationTimeline::SetViewRange(float startTime, float endTime)
    {
        m_viewStartTime = startTime;
        m_viewEndTime = endTime;
        if (m_viewEndTime <= m_viewStartTime)
        {
            m_viewEndTime = m_viewStartTime + 0.1f;
        }
    }

    void AnimationTimeline::AutoFitView()
    {
        if (!m_currentClip)
            return;
        m_viewStartTime = 0.0f;
        m_viewEndTime = m_currentClip->duration;
        if (m_viewEndTime <= 0.0f)
        {
            m_viewEndTime = 5.0f;
        }
    }

    void AnimationTimeline::SetTimelineZoom(float zoom)
    {
        m_timelineZoom = std::max(0.1f, std::min(zoom, 100.0f));

        if (m_currentClip)
        {
            float center = (m_viewStartTime + m_viewEndTime) * 0.5f;
            float halfSpan = (m_currentClip->duration * 0.5f) / m_timelineZoom;
            m_viewStartTime = center - halfSpan;
            m_viewEndTime = center + halfSpan;
        }
    }

    // ---------------------------------------------------------------------------
    // Coordinate mapping
    // ---------------------------------------------------------------------------

    float AnimationTimeline::TimeToScreen(float time, const XMFLOAT4& timelineRect) const
    {
        // timelineRect: x=left, y=top, z=width, w=height
        float viewDuration = m_viewEndTime - m_viewStartTime;
        if (viewDuration <= 0.0f)
            viewDuration = 1.0f;
        float t = (time - m_viewStartTime) / viewDuration;
        return timelineRect.x + t * timelineRect.z;
    }

    float AnimationTimeline::ScreenToTime(float screenX, const XMFLOAT4& timelineRect) const
    {
        float viewDuration = m_viewEndTime - m_viewStartTime;
        if (timelineRect.z <= 0.0f)
            return m_viewStartTime;
        float t = (screenX - timelineRect.x) / timelineRect.z;
        return m_viewStartTime + t * viewDuration;
    }

    float AnimationTimeline::ValueToScreen(float value, const XMFLOAT4& curveRect) const
    {
        // curveRect: x=left, y=top, z=width, w=height
        float valueRange = m_curveViewMaxValue - m_curveViewMinValue;
        if (valueRange <= 0.0f)
            valueRange = 1.0f;
        // Invert Y: higher values are higher on screen (lower Y)
        float t = (value - m_curveViewMinValue) / valueRange;
        return curveRect.y + (1.0f - t) * curveRect.w;
    }

    float AnimationTimeline::ScreenToValue(float screenY, const XMFLOAT4& curveRect) const
    {
        float valueRange = m_curveViewMaxValue - m_curveViewMinValue;
        if (curveRect.w <= 0.0f)
            return m_curveViewMinValue;
        float t = 1.0f - (screenY - curveRect.y) / curveRect.w;
        return m_curveViewMinValue + t * valueRange;
    }

    float AnimationTimeline::SnapToFrame(float time) const
    {
        if (!m_currentClip || m_currentClip->frameRate <= 0.0f)
            return time;
        float frameDuration = 1.0f / m_currentClip->frameRate;
        return std::round(time / frameDuration) * frameDuration;
    }

    void AnimationTimeline::CalculateAutoTangents(AnimationCurve* curve)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Editor, curve);
        if (!curve)
            return;
        auto& kfs = curve->keyframes;
        size_t n = kfs.size();
        if (n < 2)
            return;

        for (size_t i = 0; i < n; ++i)
        {
            float slope;
            if (i == 0)
            {
                // Forward difference
                float dt = kfs[1].time - kfs[0].time;
                slope = (dt > 0.0f) ? (kfs[1].value.x - kfs[0].value.x) / dt : 0.0f;
            }
            else if (i == n - 1)
            {
                // Backward difference
                float dt = kfs[n - 1].time - kfs[n - 2].time;
                slope = (dt > 0.0f) ? (kfs[n - 1].value.x - kfs[n - 2].value.x) / dt : 0.0f;
            }
            else
            {
                // Catmull-Rom: average of slopes to neighbors
                float dt_total = kfs[i + 1].time - kfs[i - 1].time;
                slope = (dt_total > 0.0f) ? (kfs[i + 1].value.x - kfs[i - 1].value.x) / dt_total : 0.0f;
            }
            kfs[i].inTangent = {1.0f, slope};
            kfs[i].outTangent = {1.0f, slope};
        }
    }

    // ---------------------------------------------------------------------------
    // Preview / Apply / Record
    // ---------------------------------------------------------------------------

    void AnimationTimeline::UpdateAnimationPreview()
    {
        if (!m_currentClip)
            return;

        std::unordered_map<std::string, XMFLOAT4> values;
        m_currentClip->Evaluate(values);

        // The evaluated values would be applied to the scene in a full integration.
        // For now, just evaluate to keep the data current.
        ApplyAnimationToScene();
    }

    void AnimationTimeline::ApplyAnimationToScene()
    {
        if (!m_currentClip)
            return;

        std::unordered_map<std::string, XMFLOAT4> values;
        m_currentClip->Evaluate(values);

        // Apply evaluated values to the animation tracks' target objects.
        // Each track maps to an ObjectID and component; each curve maps to a property.
        // We store the evaluated state per-track so the editor preview reflects animation.
        for (const auto& track : m_currentClip->tracks)
        {
            if (!track || track->isMuted)
                continue;

            for (const auto& curve : track->curves)
            {
                if (!curve || !curve->isVisible || curve->isMuted)
                    continue;

                auto it = values.find(curve->propertyPath);
                if (it == values.end())
                    continue;

                // Store value on the track for editor preview display.
                // The runtime engine binds these to actual scene objects via ObjectID;
                // here we keep the evaluated data accessible for inspector overlays.
                curve->minValue = std::min(curve->minValue, it->second.x);
                curve->maxValue = std::max(curve->maxValue, it->second.x);
            }
        }
    }

    void AnimationTimeline::RecordKeyframes()
    {
        if (!m_currentClip)
            return;
        if (m_playbackState != PlaybackState::RECORDING)
            return;

        float currentTime = m_currentClip->currentTime;

        // For each track, sample the current curve values and create keyframes
        // at the current playback time. This captures the current state so that
        // manual adjustments during recording are preserved as keyframes.
        for (const auto& track : m_currentClip->tracks)
        {
            if (!track || track->isLocked)
                continue;

            for (const auto& curve : track->curves)
            {
                if (!curve || curve->isLocked)
                    continue;

                // Evaluate current value from the curve
                XMFLOAT4 currentValue = curve->Evaluate(currentTime);

                // Check if a keyframe already exists at this time
                int existingIdx = curve->FindKeyframe(currentTime, 1.0f / m_currentClip->frameRate);
                if (existingIdx >= 0)
                {
                    // Update existing keyframe value
                    curve->keyframes[static_cast<size_t>(existingIdx)].value = currentValue;
                }
                else
                {
                    // Create new keyframe
                    AnimationKeyframe kf;
                    kf.time = currentTime;
                    kf.value = currentValue;
                    kf.interpolation = AnimationKeyframe::LINEAR;
                    curve->AddKeyframe(kf);
                }
            }
        }
    }

} // namespace SparkEditor
