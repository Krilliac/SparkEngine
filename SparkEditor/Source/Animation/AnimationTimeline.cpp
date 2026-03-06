/**
 * @file AnimationTimeline.cpp
 * @brief Full implementation of AnimationTimeline, AnimationCurve, AnimationTrack, AnimationClip
 */

#include "AnimationTimeline.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cfloat>

namespace SparkEditor {

// ============================================================================
// AnimationCurve
// ============================================================================

XMFLOAT4 AnimationCurve::Evaluate(float time) const {
    if (keyframes.empty()) {
        return {0, 0, 0, 0};
    }

    // Before first keyframe
    if (time <= keyframes.front().time) {
        return keyframes.front().value;
    }

    // After last keyframe
    if (time >= keyframes.back().time) {
        return keyframes.back().value;
    }

    // Binary search for surrounding keyframes
    size_t lo = 0;
    size_t hi = keyframes.size() - 1;
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (keyframes[mid].time <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    const AnimationKeyframe& k0 = keyframes[lo];
    const AnimationKeyframe& k1 = keyframes[hi];

    float segmentDuration = k1.time - k0.time;
    if (segmentDuration <= 0.0f) {
        return k0.value;
    }

    float t = (time - k0.time) / segmentDuration;

    // Helper lambda for component-wise lerp
    auto lerpF4 = [](const XMFLOAT4& a, const XMFLOAT4& b, float s) -> XMFLOAT4 {
        return {
            a.x + (b.x - a.x) * s,
            a.y + (b.y - a.y) * s,
            a.z + (b.z - a.z) * s,
            a.w + (b.w - a.w) * s
        };
    };

    switch (k0.interpolation) {
        case AnimationKeyframe::LINEAR: {
            return lerpF4(k0.value, k1.value, t);
        }

        case AnimationKeyframe::BEZIER: {
            // Cubic bezier: B(t) = (1-t)^3*P0 + 3*(1-t)^2*t*P1 + 3*(1-t)*t^2*P2 + t^3*P3
            // P0 = k0.value, P3 = k1.value
            // P1 = k0.value + outTangent * (segmentDuration/3)
            // P2 = k1.value - inTangent * (segmentDuration/3)
            float oneMinusT = 1.0f - t;
            float b0 = oneMinusT * oneMinusT * oneMinusT;
            float b1 = 3.0f * oneMinusT * oneMinusT * t;
            float b2 = 3.0f * oneMinusT * t * t;
            float b3 = t * t * t;

            float tangentScale = segmentDuration / 3.0f;
            XMFLOAT4 result;
            result.x = b0 * k0.value.x + b1 * (k0.value.x + k0.outTangent.y * tangentScale)
                      + b2 * (k1.value.x - k1.inTangent.y * tangentScale) + b3 * k1.value.x;
            result.y = b0 * k0.value.y + b1 * (k0.value.y + k0.outTangent.y * tangentScale)
                      + b2 * (k1.value.y - k1.inTangent.y * tangentScale) + b3 * k1.value.y;
            result.z = b0 * k0.value.z + b1 * (k0.value.z + k0.outTangent.y * tangentScale)
                      + b2 * (k1.value.z - k1.inTangent.y * tangentScale) + b3 * k1.value.z;
            result.w = b0 * k0.value.w + b1 * (k0.value.w + k0.outTangent.y * tangentScale)
                      + b2 * (k1.value.w - k1.inTangent.y * tangentScale) + b3 * k1.value.w;
            return result;
        }

        case AnimationKeyframe::STEP: {
            return k0.value;
        }

        case AnimationKeyframe::EASE_IN: {
            // Quadratic ease in: t^2
            float eased = t * t;
            return lerpF4(k0.value, k1.value, eased);
        }

        case AnimationKeyframe::EASE_OUT: {
            // Quadratic ease out: 1 - (1-t)^2
            float eased = 1.0f - (1.0f - t) * (1.0f - t);
            return lerpF4(k0.value, k1.value, eased);
        }

        case AnimationKeyframe::EASE_IN_OUT: {
            // Cubic ease in-out: t < 0.5 ? 4t^3 : 1 - (-2t+2)^3/2
            float eased;
            if (t < 0.5f) {
                eased = 4.0f * t * t * t;
            } else {
                float f = -2.0f * t + 2.0f;
                eased = 1.0f - (f * f * f) / 2.0f;
            }
            return lerpF4(k0.value, k1.value, eased);
        }

        case AnimationKeyframe::CUSTOM:
        default: {
            return lerpF4(k0.value, k1.value, t);
        }
    }
}

void AnimationCurve::AddKeyframe(const AnimationKeyframe& keyframe) {
    // Insert maintaining sorted order by time
    auto it = std::lower_bound(keyframes.begin(), keyframes.end(), keyframe.time,
        [](const AnimationKeyframe& kf, float t) { return kf.time < t; });
    keyframes.insert(it, keyframe);
}

void AnimationCurve::RemoveKeyframe(size_t index) {
    if (index < keyframes.size()) {
        keyframes.erase(keyframes.begin() + static_cast<ptrdiff_t>(index));
    }
}

int AnimationCurve::FindKeyframe(float time, float tolerance) const {
    for (size_t i = 0; i < keyframes.size(); ++i) {
        if (std::abs(time - keyframes[i].time) < tolerance) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ============================================================================
// AnimationTrack
// ============================================================================

AnimationCurve* AnimationTrack::AddCurve(const std::string& propertyPath, const std::string& displayName) {
    auto curve = std::make_unique<AnimationCurve>();
    curve->propertyPath = propertyPath;
    curve->displayName = displayName;
    AnimationCurve* ptr = curve.get();
    curves.push_back(std::move(curve));
    return ptr;
}

void AnimationTrack::RemoveCurve(const std::string& propertyPath) {
    curves.erase(
        std::remove_if(curves.begin(), curves.end(),
            [&](const std::unique_ptr<AnimationCurve>& c) {
                return c->propertyPath == propertyPath;
            }),
        curves.end());
}

AnimationCurve* AnimationTrack::FindCurve(const std::string& propertyPath) {
    for (auto& curve : curves) {
        if (curve->propertyPath == propertyPath) {
            return curve.get();
        }
    }
    return nullptr;
}

// ============================================================================
// AnimationClip
// ============================================================================

AnimationTrack* AnimationClip::AddTrack(ObjectID objectID, const std::string& objectName) {
    // Check if track already exists
    if (auto* existing = FindTrack(objectID)) {
        return existing;
    }
    auto track = std::make_unique<AnimationTrack>();
    track->objectID = objectID;
    track->objectName = objectName;
    AnimationTrack* ptr = track.get();
    tracks.push_back(std::move(track));
    return ptr;
}

void AnimationClip::RemoveTrack(ObjectID objectID) {
    tracks.erase(
        std::remove_if(tracks.begin(), tracks.end(),
            [&](const std::unique_ptr<AnimationTrack>& t) {
                return t->objectID == objectID;
            }),
        tracks.end());
}

AnimationTrack* AnimationClip::FindTrack(ObjectID objectID) {
    for (auto& track : tracks) {
        if (track->objectID == objectID) {
            return track.get();
        }
    }
    return nullptr;
}

void AnimationClip::Evaluate(std::unordered_map<std::string, XMFLOAT4>& outValues) const {
    for (auto& track : tracks) {
        if (track->isMuted) continue;
        for (auto& curve : track->curves) {
            if (curve->isMuted) continue;
            outValues[curve->propertyPath] = curve->Evaluate(currentTime);
        }
    }
}

void AnimationClip::SetTime(float time) {
    currentTime = std::max(0.0f, std::min(time, duration));
}

// ============================================================================
// AnimationTimeline
// ============================================================================

AnimationTimeline::AnimationTimeline()
    : EditorPanel("Animation Timeline", "animation_timeline") {
}

AnimationTimeline::~AnimationTimeline() {
}

bool AnimationTimeline::Initialize() {
    CreateNewClip("Default Animation", 5.0f, 30.0f);
    m_isInitialized = true;
    return true;
}

void AnimationTimeline::Update(float deltaTime) {
    if (!m_currentClip) return;

    if (m_playbackState == PlaybackState::PLAYING) {
        float newTime = m_currentClip->currentTime + deltaTime * m_playbackSpeed;

        if (newTime >= m_currentClip->duration) {
            if (m_loopPlayback || m_currentClip->isLooping) {
                newTime = std::fmod(newTime, m_currentClip->duration);
            } else {
                newTime = m_currentClip->duration;
                m_playbackState = PlaybackState::STOPPED;
            }
        } else if (newTime < 0.0f) {
            if (m_loopPlayback || m_currentClip->isLooping) {
                newTime = m_currentClip->duration + std::fmod(newTime, m_currentClip->duration);
            } else {
                newTime = 0.0f;
                m_playbackState = PlaybackState::STOPPED;
            }
        }

        m_currentClip->SetTime(newTime);
        UpdateAnimationPreview();
    }
}

void AnimationTimeline::Render() {
    if (!m_isVisible) return;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Timeline", &m_isVisible)) {
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

    if (m_showCurveEditor) {
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
    } else {
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

void AnimationTimeline::Shutdown() {
    m_currentClip.reset();
    m_selection.Clear();
    m_playbackState = PlaybackState::STOPPED;
}

bool AnimationTimeline::HandleEvent(const std::string& eventType, void* eventData) {
    if (eventType == "play") {
        Play();
        return true;
    } else if (eventType == "pause") {
        Pause();
        return true;
    } else if (eventType == "stop") {
        Stop();
        return true;
    } else if (eventType == "delete_keyframes") {
        RemoveSelectedKeyframes();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Clip management
// ---------------------------------------------------------------------------

void AnimationTimeline::CreateNewClip(const std::string& name, float duration, float frameRate) {
    auto clip = std::make_unique<AnimationClip>();
    clip->name = name;
    clip->duration = duration;
    clip->frameRate = frameRate;
    clip->currentTime = 0.0f;
    m_currentClip = std::move(clip);
    m_viewStartTime = 0.0f;
    m_viewEndTime = duration;
    m_selection.Clear();
    m_playbackState = PlaybackState::STOPPED;
}

bool AnimationTimeline::LoadAnimationClip(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    auto clip = std::make_unique<AnimationClip>();
    std::string line;

    // Read header
    if (!std::getline(file, line) || line != "SPARK_ANIM") return false;

    // Name
    if (std::getline(file, line)) clip->name = line;
    // Duration, frameRate, looping
    if (std::getline(file, line)) {
        std::istringstream iss(line);
        iss >> clip->duration >> clip->frameRate;
        int looping = 0;
        if (iss >> looping) clip->isLooping = (looping != 0);
    }

    // Read tracks
    int trackCount = 0;
    if (std::getline(file, line)) {
        std::istringstream iss(line);
        iss >> trackCount;
    }

    for (int ti = 0; ti < trackCount; ++ti) {
        ObjectID objID = 0;
        std::string objName;
        int curveCount = 0;

        if (std::getline(file, line)) {
            std::istringstream iss(line);
            iss >> objID;
            std::getline(iss >> std::ws, objName);
        }
        if (std::getline(file, line)) {
            std::istringstream iss(line);
            iss >> curveCount;
        }

        AnimationTrack* track = clip->AddTrack(objID, objName);

        for (int ci = 0; ci < curveCount; ++ci) {
            std::string propPath, displayName;
            int kfCount = 0;

            if (std::getline(file, propPath)) {}
            if (std::getline(file, displayName)) {}
            if (std::getline(file, line)) {
                std::istringstream iss(line);
                iss >> kfCount;
            }

            AnimationCurve* curve = track->AddCurve(propPath, displayName);
            for (int ki = 0; ki < kfCount; ++ki) {
                if (std::getline(file, line)) {
                    AnimationKeyframe kf;
                    int interp = 0;
                    std::istringstream iss(line);
                    iss >> kf.time >> kf.value.x >> kf.value.y >> kf.value.z >> kf.value.w
                        >> kf.inTangent.x >> kf.inTangent.y
                        >> kf.outTangent.x >> kf.outTangent.y
                        >> interp;
                    kf.interpolation = static_cast<AnimationKeyframe::InterpolationType>(interp);
                    curve->AddKeyframe(kf);
                }
            }
        }
    }

    m_currentClip = std::move(clip);
    AutoFitView();
    m_selection.Clear();
    m_playbackState = PlaybackState::STOPPED;
    return true;
}

bool AnimationTimeline::SaveAnimationClip(const std::string& filePath) {
    if (!m_currentClip) return false;

    std::ofstream file(filePath);
    if (!file.is_open()) return false;

    // Header
    file << "SPARK_ANIM\n";
    file << m_currentClip->name << "\n";
    file << m_currentClip->duration << " " << m_currentClip->frameRate << " "
         << (m_currentClip->isLooping ? 1 : 0) << "\n";

    file << m_currentClip->tracks.size() << "\n";

    for (auto& track : m_currentClip->tracks) {
        file << track->objectID << " " << track->objectName << "\n";
        file << track->curves.size() << "\n";

        for (auto& curve : track->curves) {
            file << curve->propertyPath << "\n";
            file << curve->displayName << "\n";
            file << curve->keyframes.size() << "\n";

            for (auto& kf : curve->keyframes) {
                file << kf.time << " "
                     << kf.value.x << " " << kf.value.y << " "
                     << kf.value.z << " " << kf.value.w << " "
                     << kf.inTangent.x << " " << kf.inTangent.y << " "
                     << kf.outTangent.x << " " << kf.outTangent.y << " "
                     << static_cast<int>(kf.interpolation) << "\n";
            }
        }
    }

    return true;
}

void AnimationTimeline::SetCurrentClip(std::unique_ptr<AnimationClip> clip) {
    m_currentClip = std::move(clip);
    m_selection.Clear();
    m_playbackState = PlaybackState::STOPPED;
    if (m_currentClip) {
        AutoFitView();
    }
}

// ---------------------------------------------------------------------------
// Playback controls
// ---------------------------------------------------------------------------

void AnimationTimeline::Play() {
    if (!m_currentClip) return;
    m_playbackState = PlaybackState::PLAYING;
    m_currentClip->isPlaying = true;
    m_currentClip->isPaused = false;
}

void AnimationTimeline::Pause() {
    if (!m_currentClip) return;
    if (m_playbackState == PlaybackState::PLAYING) {
        m_playbackState = PlaybackState::PAUSED;
        m_currentClip->isPlaying = false;
        m_currentClip->isPaused = true;
    }
}

void AnimationTimeline::Stop() {
    if (!m_currentClip) return;
    m_playbackState = PlaybackState::STOPPED;
    m_currentClip->SetTime(0.0f);
    m_currentClip->isPlaying = false;
    m_currentClip->isPaused = false;
}

void AnimationTimeline::StepForward() {
    if (!m_currentClip) return;
    float frameTime = 1.0f / m_currentClip->frameRate;
    m_currentClip->SetTime(m_currentClip->currentTime + frameTime);
}

void AnimationTimeline::StepBackward() {
    if (!m_currentClip) return;
    float frameTime = 1.0f / m_currentClip->frameRate;
    m_currentClip->SetTime(m_currentClip->currentTime - frameTime);
}

void AnimationTimeline::GoToStart() {
    if (!m_currentClip) return;
    m_currentClip->SetTime(0.0f);
}

void AnimationTimeline::GoToEnd() {
    if (!m_currentClip) return;
    m_currentClip->SetTime(m_currentClip->duration);
}

void AnimationTimeline::SetPlaybackTime(float time) {
    if (!m_currentClip) return;
    m_currentClip->SetTime(time);
}

float AnimationTimeline::GetPlaybackTime() const {
    if (!m_currentClip) return 0.0f;
    return m_currentClip->currentTime;
}

void AnimationTimeline::SetRecording(bool recording) {
    if (recording) {
        m_playbackState = PlaybackState::RECORDING;
    } else {
        m_playbackState = PlaybackState::STOPPED;
    }
}

// ---------------------------------------------------------------------------
// Keyframe editing
// ---------------------------------------------------------------------------

void AnimationTimeline::AddKeyframe(ObjectID objectID, const std::string& propertyPath,
                                    const XMFLOAT4& value, float time) {
    if (!m_currentClip) return;

    float kfTime = (time < 0.0f) ? m_currentClip->currentTime : time;

    // Find or create track
    AnimationTrack* track = m_currentClip->FindTrack(objectID);
    if (!track) {
        track = m_currentClip->AddTrack(objectID, "Object_" + std::to_string(objectID));
    }

    // Find or create curve
    AnimationCurve* curve = track->FindCurve(propertyPath);
    if (!curve) {
        curve = track->AddCurve(propertyPath, propertyPath);
    }

    AnimationKeyframe kf;
    kf.time = kfTime;
    kf.value = value;
    kf.interpolation = AnimationKeyframe::LINEAR;
    curve->AddKeyframe(kf);
}

void AnimationTimeline::RemoveSelectedKeyframes() {
    if (!m_currentClip) return;

    // Build a set of pointers we need to remove
    for (auto& track : m_currentClip->tracks) {
        for (auto& curve : track->curves) {
            // Walk backwards to safely erase
            for (int i = static_cast<int>(curve->keyframes.size()) - 1; i >= 0; --i) {
                AnimationKeyframe* kfPtr = &curve->keyframes[static_cast<size_t>(i)];
                for (auto* sel : m_selection.selectedKeyframes) {
                    if (sel == kfPtr) {
                        curve->RemoveKeyframe(static_cast<size_t>(i));
                        break;
                    }
                }
            }
        }
    }
    m_selection.selectedKeyframes.clear();
}

void AnimationTimeline::SetKeyframeInterpolation(AnimationKeyframe::InterpolationType interpolationType) {
    for (auto* kf : m_selection.selectedKeyframes) {
        kf->interpolation = interpolationType;
    }
}

void AnimationTimeline::FrameSelected() {
    if (m_selection.selectedKeyframes.empty()) return;

    float minTime = FLT_MAX;
    float maxTime = -FLT_MAX;
    float minVal = FLT_MAX;
    float maxVal = -FLT_MAX;

    for (auto* kf : m_selection.selectedKeyframes) {
        minTime = std::min(minTime, kf->time);
        maxTime = std::max(maxTime, kf->time);
        minVal = std::min(minVal, kf->value.x);
        maxVal = std::max(maxVal, kf->value.x);
    }

    float padding = (maxTime - minTime) * 0.1f;
    if (padding < 0.1f) padding = 0.5f;
    m_viewStartTime = minTime - padding;
    m_viewEndTime = maxTime + padding;

    float valPadding = (maxVal - minVal) * 0.1f;
    if (valPadding < 0.1f) valPadding = 1.0f;
    m_curveViewMinValue = minVal - valPadding;
    m_curveViewMaxValue = maxVal + valPadding;
}

void AnimationTimeline::FrameAll() {
    if (!m_currentClip) return;

    float minTime = FLT_MAX;
    float maxTime = -FLT_MAX;
    float minVal = FLT_MAX;
    float maxVal = -FLT_MAX;
    bool hasKeyframes = false;

    for (auto& track : m_currentClip->tracks) {
        for (auto& curve : track->curves) {
            for (auto& kf : curve->keyframes) {
                hasKeyframes = true;
                minTime = std::min(minTime, kf.time);
                maxTime = std::max(maxTime, kf.time);
                minVal = std::min(minVal, kf.value.x);
                maxVal = std::max(maxVal, kf.value.x);
            }
        }
    }

    if (!hasKeyframes) {
        AutoFitView();
        return;
    }

    float padding = (maxTime - minTime) * 0.1f;
    if (padding < 0.1f) padding = 0.5f;
    m_viewStartTime = minTime - padding;
    m_viewEndTime = maxTime + padding;

    float valPadding = (maxVal - minVal) * 0.1f;
    if (valPadding < 0.1f) valPadding = 1.0f;
    m_curveViewMinValue = minVal - valPadding;
    m_curveViewMaxValue = maxVal + valPadding;
}

void AnimationTimeline::SetViewRange(float startTime, float endTime) {
    m_viewStartTime = startTime;
    m_viewEndTime = endTime;
    if (m_viewEndTime <= m_viewStartTime) {
        m_viewEndTime = m_viewStartTime + 0.1f;
    }
}

void AnimationTimeline::AutoFitView() {
    if (!m_currentClip) return;
    m_viewStartTime = 0.0f;
    m_viewEndTime = m_currentClip->duration;
    if (m_viewEndTime <= 0.0f) {
        m_viewEndTime = 5.0f;
    }
}

void AnimationTimeline::SetTimelineZoom(float zoom) {
    m_timelineZoom = std::max(0.1f, std::min(zoom, 100.0f));

    if (m_currentClip) {
        float center = (m_viewStartTime + m_viewEndTime) * 0.5f;
        float halfSpan = (m_currentClip->duration * 0.5f) / m_timelineZoom;
        m_viewStartTime = center - halfSpan;
        m_viewEndTime = center + halfSpan;
    }
}

// ---------------------------------------------------------------------------
// Coordinate mapping
// ---------------------------------------------------------------------------

float AnimationTimeline::TimeToScreen(float time, const XMFLOAT4& timelineRect) const {
    // timelineRect: x=left, y=top, z=width, w=height
    float viewDuration = m_viewEndTime - m_viewStartTime;
    if (viewDuration <= 0.0f) viewDuration = 1.0f;
    float t = (time - m_viewStartTime) / viewDuration;
    return timelineRect.x + t * timelineRect.z;
}

float AnimationTimeline::ScreenToTime(float screenX, const XMFLOAT4& timelineRect) const {
    float viewDuration = m_viewEndTime - m_viewStartTime;
    if (timelineRect.z <= 0.0f) return m_viewStartTime;
    float t = (screenX - timelineRect.x) / timelineRect.z;
    return m_viewStartTime + t * viewDuration;
}

float AnimationTimeline::ValueToScreen(float value, const XMFLOAT4& curveRect) const {
    // curveRect: x=left, y=top, z=width, w=height
    float valueRange = m_curveViewMaxValue - m_curveViewMinValue;
    if (valueRange <= 0.0f) valueRange = 1.0f;
    // Invert Y: higher values are higher on screen (lower Y)
    float t = (value - m_curveViewMinValue) / valueRange;
    return curveRect.y + (1.0f - t) * curveRect.w;
}

float AnimationTimeline::ScreenToValue(float screenY, const XMFLOAT4& curveRect) const {
    float valueRange = m_curveViewMaxValue - m_curveViewMinValue;
    if (curveRect.w <= 0.0f) return m_curveViewMinValue;
    float t = 1.0f - (screenY - curveRect.y) / curveRect.w;
    return m_curveViewMinValue + t * valueRange;
}

float AnimationTimeline::SnapToFrame(float time) const {
    if (!m_currentClip || m_currentClip->frameRate <= 0.0f) return time;
    float frameDuration = 1.0f / m_currentClip->frameRate;
    return std::round(time / frameDuration) * frameDuration;
}

void AnimationTimeline::CalculateAutoTangents(AnimationCurve* curve) {
    if (!curve) return;
    auto& kfs = curve->keyframes;
    size_t n = kfs.size();
    if (n < 2) return;

    for (size_t i = 0; i < n; ++i) {
        float slope;
        if (i == 0) {
            // Forward difference
            float dt = kfs[1].time - kfs[0].time;
            slope = (dt > 0.0f) ? (kfs[1].value.x - kfs[0].value.x) / dt : 0.0f;
        } else if (i == n - 1) {
            // Backward difference
            float dt = kfs[n - 1].time - kfs[n - 2].time;
            slope = (dt > 0.0f) ? (kfs[n - 1].value.x - kfs[n - 2].value.x) / dt : 0.0f;
        } else {
            // Catmull-Rom: average of slopes to neighbors
            float dt_total = kfs[i + 1].time - kfs[i - 1].time;
            slope = (dt_total > 0.0f) ? (kfs[i + 1].value.x - kfs[i - 1].value.x) / dt_total : 0.0f;
        }
        kfs[i].inTangent = {1.0f, slope};
        kfs[i].outTangent = {1.0f, slope};
    }
}

// ---------------------------------------------------------------------------
// Find keyframe at screen position
// ---------------------------------------------------------------------------

AnimationKeyframe* AnimationTimeline::FindKeyframeAtPosition(const XMFLOAT2& screenPos, float tolerance) {
    if (!m_currentClip) return nullptr;

    // We need the timeline rect to map. Use a rough rect from current ImGui state.
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    XMFLOAT4 timelineRect = {winPos.x + m_trackListWidth, winPos.y, winSize.x - m_trackListWidth, winSize.y};

    for (auto& track : m_currentClip->tracks) {
        for (auto& curve : track->curves) {
            for (auto& kf : curve->keyframes) {
                float sx = TimeToScreen(kf.time, timelineRect);
                float dx = screenPos.x - sx;
                float dy = screenPos.y - (timelineRect.y + timelineRect.w * 0.5f);
                if (std::sqrt(dx * dx + dy * dy) < tolerance) {
                    return &kf;
                }
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Render sub-methods
// ---------------------------------------------------------------------------

void AnimationTimeline::RenderPlaybackControls() {
    if (!m_currentClip) {
        ImGui::Text("No animation clip loaded");
        return;
    }

    // Transport controls
    if (ImGui::Button("|<")) GoToStart();
    ImGui::SameLine();
    if (ImGui::Button("<<")) StepBackward();
    ImGui::SameLine();

    if (m_playbackState == PlaybackState::PLAYING) {
        if (ImGui::Button("||")) Pause();
    } else {
        if (ImGui::Button(">")) Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("[]")) Stop();
    ImGui::SameLine();
    if (ImGui::Button(">>")) StepForward();
    ImGui::SameLine();
    if (ImGui::Button(">|")) GoToEnd();

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    // Recording toggle
    bool recording = IsRecording();
    if (recording) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("REC")) SetRecording(!recording);
    if (recording) ImGui::PopStyleColor();

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

void AnimationTimeline::RenderTimelineHeader() {
    if (!m_currentClip) return;

    ImGui::Text("Clip: %s", m_currentClip->name.c_str());
    ImGui::SameLine();
    ImGui::Text("  Duration: %.2fs  FPS: %.0f  Tracks: %zu",
        m_currentClip->duration, m_currentClip->frameRate, m_currentClip->tracks.size());

    ImGui::SameLine();
    if (ImGui::Button("Frame All")) FrameAll();
    ImGui::SameLine();
    if (ImGui::Button("Frame Sel")) FrameSelected();
}

void AnimationTimeline::RenderTrackList() {
    if (!m_currentClip) return;

    ImGui::Text("Tracks");
    ImGui::Separator();

    for (size_t i = 0; i < m_currentClip->tracks.size(); ++i) {
        auto& track = m_currentClip->tracks[i];
        ImGui::PushID(static_cast<int>(i));

        bool expanded = track->isExpanded;
        if (ImGui::TreeNodeEx(track->objectName.c_str(),
                expanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            track->isExpanded = true;

            // Mute / visibility toggles
            ImGui::SameLine();
            ImGui::Checkbox("V", &track->isVisible);
            ImGui::SameLine();
            ImGui::Checkbox("M", &track->isMuted);

            // Show curves
            for (size_t ci = 0; ci < track->curves.size(); ++ci) {
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
        } else {
            track->isExpanded = false;
        }

        ImGui::PopID();
    }
}

void AnimationTimeline::RenderTimelineEditor() {
    if (!m_currentClip) return;

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 10.0f || canvasSize.y < 10.0f) return;

    XMFLOAT4 timelineRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(
        ImVec2(timelineRect.x, timelineRect.y),
        ImVec2(timelineRect.x + timelineRect.z, timelineRect.y + timelineRect.w),
        ImGui::ColorConvertFloat4ToU32(ImVec4(
            m_timelineBackgroundColor.x, m_timelineBackgroundColor.y,
            m_timelineBackgroundColor.z, m_timelineBackgroundColor.w)));

    // Time ruler at top
    XMFLOAT4 rulerRect = {timelineRect.x, timelineRect.y, timelineRect.z, 24.0f};
    RenderTimeRuler(rulerRect);

    // Tracks
    float trackY = timelineRect.y + 24.0f;
    for (size_t i = 0; i < m_currentClip->tracks.size(); ++i) {
        auto& track = m_currentClip->tracks[i];
        if (!track->isVisible) continue;

        float trackBottom = trackY + m_trackHeight;
        if (trackBottom > timelineRect.y + timelineRect.w) break;

        XMFLOAT4 trackRect = {timelineRect.x, trackY, timelineRect.z, m_trackHeight};
        RenderTrack(track.get(), static_cast<int>(i));

        // Render keyframes for each visible curve
        for (auto& curve : track->curves) {
            if (curve->isVisible && !curve->isMuted) {
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

void AnimationTimeline::RenderCurveEditor() {
    if (!m_currentClip) return;

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 10.0f || canvasSize.y < 10.0f) return;

    XMFLOAT4 curveRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(
        ImVec2(curveRect.x, curveRect.y),
        ImVec2(curveRect.x + curveRect.z, curveRect.y + curveRect.w),
        IM_COL32(30, 30, 30, 255));

    // Draw grid lines for value axis
    float valueRange = m_curveViewMaxValue - m_curveViewMinValue;
    if (valueRange > 0.0f) {
        float step = 1.0f;
        if (valueRange > 20.0f) step = 5.0f;
        if (valueRange > 100.0f) step = 10.0f;

        float v = std::ceil(m_curveViewMinValue / step) * step;
        while (v <= m_curveViewMaxValue) {
            float sy = ValueToScreen(v, curveRect);
            drawList->AddLine(
                ImVec2(curveRect.x, sy),
                ImVec2(curveRect.x + curveRect.z, sy),
                IM_COL32(60, 60, 60, 255));
            char label[32];
            snprintf(label, sizeof(label), "%.1f", v);
            drawList->AddText(ImVec2(curveRect.x + 2, sy - 8), IM_COL32(150, 150, 150, 255), label);
            v += step;
        }
    }

    // Zero line
    float zeroY = ValueToScreen(0.0f, curveRect);
    if (zeroY >= curveRect.y && zeroY <= curveRect.y + curveRect.w) {
        drawList->AddLine(
            ImVec2(curveRect.x, zeroY),
            ImVec2(curveRect.x + curveRect.z, zeroY),
            IM_COL32(100, 100, 100, 255));
    }

    // Render curves
    for (auto& track : m_currentClip->tracks) {
        if (!track->isVisible || track->isMuted) continue;
        for (auto& curve : track->curves) {
            if (curve->isVisible && !curve->isMuted) {
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

void AnimationTimeline::RenderAnimationProperties() {
    if (!m_currentClip) return;

    ImGui::Text("Animation Properties");
    ImGui::Separator();

    char nameBuffer[256];
    strncpy(nameBuffer, m_currentClip->name.c_str(), sizeof(nameBuffer) - 1);
    nameBuffer[sizeof(nameBuffer) - 1] = '\0';
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
        m_currentClip->name = nameBuffer;
    }

    ImGui::DragFloat("Duration", &m_currentClip->duration, 0.1f, 0.1f, 600.0f, "%.2f s");
    ImGui::DragFloat("Frame Rate", &m_currentClip->frameRate, 1.0f, 1.0f, 120.0f, "%.0f fps");
    ImGui::Checkbox("Looping", &m_currentClip->isLooping);
}

void AnimationTimeline::RenderTrack(AnimationTrack* track, int trackIndex) {
    if (!track) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    // Track background (alternating colors)
    ImU32 bgColor = (trackIndex % 2 == 0)
        ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.18f, 0.18f, 1.0f))
        : ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.22f, 0.22f, 1.0f));

    float trackWidth = ImGui::GetContentRegionAvail().x;
    drawList->AddRectFilled(
        canvasPos,
        ImVec2(canvasPos.x + trackWidth, canvasPos.y + m_trackHeight),
        bgColor);

    // Track separator line
    drawList->AddLine(
        ImVec2(canvasPos.x, canvasPos.y + m_trackHeight),
        ImVec2(canvasPos.x + trackWidth, canvasPos.y + m_trackHeight),
        IM_COL32(80, 80, 80, 255));
}

void AnimationTimeline::RenderKeyframes(AnimationCurve* curve, const XMFLOAT4& trackRect) {
    if (!curve) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float centerY = trackRect.y + trackRect.w * 0.5f;
    float diamondSize = 5.0f;

    for (auto& kf : curve->keyframes) {
        float sx = TimeToScreen(kf.time, trackRect);

        // Skip keyframes outside visible range
        if (sx < trackRect.x - diamondSize || sx > trackRect.x + trackRect.z + diamondSize) continue;

        ImU32 color;
        if (kf.isSelected) {
            color = ImGui::ColorConvertFloat4ToU32(ImVec4(
                m_selectedKeyframeColor.x, m_selectedKeyframeColor.y,
                m_selectedKeyframeColor.z, m_selectedKeyframeColor.w));
        } else {
            color = ImGui::ColorConvertFloat4ToU32(ImVec4(
                m_keyframeColor.x, m_keyframeColor.y,
                m_keyframeColor.z, m_keyframeColor.w));
        }

        // Draw diamond shape
        ImVec2 points[4] = {
            ImVec2(sx, centerY - diamondSize),        // top
            ImVec2(sx + diamondSize, centerY),         // right
            ImVec2(sx, centerY + diamondSize),         // bottom
            ImVec2(sx - diamondSize, centerY)          // left
        };
        drawList->AddConvexPolyFilled(points, 4, color);
        drawList->AddPolyline(points, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 1.0f);
    }
}

void AnimationTimeline::RenderCurve(AnimationCurve* curve) {
    if (!curve || curve->keyframes.size() < 2) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    XMFLOAT4 curveRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

    // Determine curve color
    ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
        curve->color.x, curve->color.y, curve->color.z, curve->color.w));

    // Build polyline by evaluating at fine intervals
    const int numSamples = 200;
    float startTime = m_viewStartTime;
    float endTime = m_viewEndTime;
    float dt = (endTime - startTime) / static_cast<float>(numSamples);

    std::vector<ImVec2> points;
    points.reserve(numSamples + 1);

    for (int i = 0; i <= numSamples; ++i) {
        float t = startTime + i * dt;
        XMFLOAT4 val = curve->Evaluate(t);
        float sx = TimeToScreen(t, curveRect);
        float sy = ValueToScreen(val.x, curveRect);
        points.push_back(ImVec2(sx, sy));
    }

    if (points.size() >= 2) {
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), lineColor, ImDrawFlags_None, 1.5f);
    }

    // Draw keyframe handles in curve editor
    float diamondSize = 4.0f;
    for (auto& kf : curve->keyframes) {
        float sx = TimeToScreen(kf.time, curveRect);
        float sy = ValueToScreen(kf.value.x, curveRect);

        ImU32 kfColor = kf.isSelected
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(
                  m_selectedKeyframeColor.x, m_selectedKeyframeColor.y,
                  m_selectedKeyframeColor.z, m_selectedKeyframeColor.w))
            : ImGui::ColorConvertFloat4ToU32(ImVec4(
                  m_keyframeColor.x, m_keyframeColor.y,
                  m_keyframeColor.z, m_keyframeColor.w));

        // Diamond
        ImVec2 pts[4] = {
            ImVec2(sx, sy - diamondSize),
            ImVec2(sx + diamondSize, sy),
            ImVec2(sx, sy + diamondSize),
            ImVec2(sx - diamondSize, sy)
        };
        drawList->AddConvexPolyFilled(pts, 4, kfColor);

        // Tangent handles
        if (m_showCurveHandles && kf.isSelected && kf.interpolation == AnimationKeyframe::BEZIER) {
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

void AnimationTimeline::RenderTimeRuler(const XMFLOAT4& timelineRect) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Ruler background
    drawList->AddRectFilled(
        ImVec2(timelineRect.x, timelineRect.y),
        ImVec2(timelineRect.x + timelineRect.z, timelineRect.y + timelineRect.w),
        IM_COL32(40, 40, 40, 255));

    float viewDuration = m_viewEndTime - m_viewStartTime;
    if (viewDuration <= 0.0f) return;

    // Determine tick interval based on zoom
    float pixelsPerSecond = timelineRect.z / viewDuration;
    float minTickSpacing = 80.0f; // minimum pixels between major ticks

    // Find a nice tick interval
    float rawInterval = minTickSpacing / pixelsPerSecond;
    float magnitude = std::pow(10.0f, std::floor(std::log10(rawInterval)));
    float residual = rawInterval / magnitude;

    float tickInterval;
    if (residual <= 1.5f) tickInterval = magnitude;
    else if (residual <= 3.5f) tickInterval = 2.0f * magnitude;
    else if (residual <= 7.5f) tickInterval = 5.0f * magnitude;
    else tickInterval = 10.0f * magnitude;

    if (tickInterval <= 0.0f) tickInterval = 1.0f;

    // Draw tick marks
    float startTick = std::ceil(m_viewStartTime / tickInterval) * tickInterval;
    for (float t = startTick; t <= m_viewEndTime; t += tickInterval) {
        float sx = TimeToScreen(t, timelineRect);

        // Major tick
        drawList->AddLine(
            ImVec2(sx, timelineRect.y + timelineRect.w - 12),
            ImVec2(sx, timelineRect.y + timelineRect.w),
            IM_COL32(200, 200, 200, 255));

        // Label
        char label[32];
        if (m_showFrameNumbers && m_currentClip) {
            int frame = m_currentClip->TimeToFrame(t);
            snprintf(label, sizeof(label), "%d", frame);
        } else {
            snprintf(label, sizeof(label), "%.2f", t);
        }
        drawList->AddText(ImVec2(sx + 2, timelineRect.y + 2), IM_COL32(200, 200, 200, 255), label);

        // Minor ticks (subdivide into 5)
        float minorInterval = tickInterval / 5.0f;
        for (int m = 1; m < 5; ++m) {
            float mt = t + m * minorInterval;
            if (mt > m_viewEndTime) break;
            float msx = TimeToScreen(mt, timelineRect);
            drawList->AddLine(
                ImVec2(msx, timelineRect.y + timelineRect.w - 6),
                ImVec2(msx, timelineRect.y + timelineRect.w),
                IM_COL32(120, 120, 120, 255));
        }
    }

    // Bottom border
    drawList->AddLine(
        ImVec2(timelineRect.x, timelineRect.y + timelineRect.w),
        ImVec2(timelineRect.x + timelineRect.z, timelineRect.y + timelineRect.w),
        IM_COL32(80, 80, 80, 255));
}

void AnimationTimeline::RenderPlayhead(const XMFLOAT4& timelineRect) {
    if (!m_currentClip) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float sx = TimeToScreen(m_currentClip->currentTime, timelineRect);

    // Clamp playhead to visible area
    if (sx < timelineRect.x || sx > timelineRect.x + timelineRect.z) return;

    ImU32 playheadCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
        m_playheadColor.x, m_playheadColor.y, m_playheadColor.z, m_playheadColor.w));

    // Vertical line
    drawList->AddLine(
        ImVec2(sx, timelineRect.y),
        ImVec2(sx, timelineRect.y + timelineRect.w),
        playheadCol, 2.0f);

    // Triangle at top
    drawList->AddTriangleFilled(
        ImVec2(sx - 6, timelineRect.y),
        ImVec2(sx + 6, timelineRect.y),
        ImVec2(sx, timelineRect.y + 10),
        playheadCol);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void AnimationTimeline::HandleTimelineInput(const XMFLOAT4& timelineRect) {
    if (!m_currentClip) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;

    bool isHovered = ImGui::IsItemHovered();

    if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float clickTime = ScreenToTime(mousePos.x, timelineRect);

        // Check if clicking on playhead area (top 24 pixels)
        if (mousePos.y < timelineRect.y + 24.0f) {
            m_isDraggingPlayhead = true;
            if (m_snapToFrames) clickTime = SnapToFrame(clickTime);
            m_currentClip->SetTime(clickTime);
        } else {
            // Try to select a keyframe
            XMFLOAT2 mp = {mousePos.x, mousePos.y};
            bool additive = io.KeyShift;
            HandleKeyframeSelection(mp, additive);
        }
    }

    // Dragging playhead
    if (m_isDraggingPlayhead) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dragTime = ScreenToTime(mousePos.x, timelineRect);
            if (m_snapToFrames) dragTime = SnapToFrame(dragTime);
            m_currentClip->SetTime(dragTime);
        } else {
            m_isDraggingPlayhead = false;
        }
    }

    // Dragging keyframes
    if (m_isDraggingKeyframes) {
        HandleKeyframeDragging();
    }

    // Mouse wheel zoom
    if (isHovered && std::abs(io.MouseWheel) > 0.0f) {
        float zoomFactor = 1.0f + io.MouseWheel * 0.1f;
        float mouseTime = ScreenToTime(mousePos.x, timelineRect);

        float newStart = mouseTime + (m_viewStartTime - mouseTime) / zoomFactor;
        float newEnd = mouseTime + (m_viewEndTime - mouseTime) / zoomFactor;
        SetViewRange(newStart, newEnd);
    }

    // Middle mouse pan
    if (isHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        float panDelta = -io.MouseDelta.x * (m_viewEndTime - m_viewStartTime) / timelineRect.z;
        m_viewStartTime += panDelta;
        m_viewEndTime += panDelta;
    }

    // Right-click context menu
    if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##timeline_context");
    }
    if (ImGui::BeginPopup("##timeline_context")) {
        float clickTime = ScreenToTime(mousePos.x, timelineRect);
        if (ImGui::MenuItem("Add Keyframe Here")) {
            if (m_snapToFrames) clickTime = SnapToFrame(clickTime);
            // Add keyframe on the first selected or first available curve
            if (!m_currentClip->tracks.empty() && !m_currentClip->tracks[0]->curves.empty()) {
                auto& curve = m_currentClip->tracks[0]->curves[0];
                AnimationKeyframe kf;
                kf.time = clickTime;
                kf.value = curve->Evaluate(clickTime);
                curve->AddKeyframe(kf);
            }
        }
        if (ImGui::MenuItem("Delete Selected", nullptr, false, !m_selection.selectedKeyframes.empty())) {
            RemoveSelectedKeyframes();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Interpolation")) {
            if (ImGui::MenuItem("Linear")) SetKeyframeInterpolation(AnimationKeyframe::LINEAR);
            if (ImGui::MenuItem("Bezier")) SetKeyframeInterpolation(AnimationKeyframe::BEZIER);
            if (ImGui::MenuItem("Step")) SetKeyframeInterpolation(AnimationKeyframe::STEP);
            if (ImGui::MenuItem("Ease In")) SetKeyframeInterpolation(AnimationKeyframe::EASE_IN);
            if (ImGui::MenuItem("Ease Out")) SetKeyframeInterpolation(AnimationKeyframe::EASE_OUT);
            if (ImGui::MenuItem("Ease In/Out")) SetKeyframeInterpolation(AnimationKeyframe::EASE_IN_OUT);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Frame All")) FrameAll();
        if (ImGui::MenuItem("Frame Selected")) FrameSelected();
        if (ImGui::MenuItem("Auto Fit View")) AutoFitView();
        ImGui::EndPopup();
    }
}

void AnimationTimeline::HandleCurveEditorInput() {
    if (!m_currentClip) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;
    bool isHovered = ImGui::IsItemHovered();

    ImVec2 canvasPos = ImGui::GetItemRectMin();
    ImVec2 canvasSize = ImGui::GetItemRectSize();
    XMFLOAT4 curveRect = {canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y};

    // Mouse wheel zoom (vertical)
    if (isHovered && std::abs(io.MouseWheel) > 0.0f) {
        float zoomFactor = 1.0f + io.MouseWheel * 0.1f;
        float mouseVal = ScreenToValue(mousePos.y, curveRect);
        m_curveViewMinValue = mouseVal + (m_curveViewMinValue - mouseVal) / zoomFactor;
        m_curveViewMaxValue = mouseVal + (m_curveViewMaxValue - mouseVal) / zoomFactor;
    }

    // Middle mouse pan
    if (isHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        float panX = -io.MouseDelta.x * (m_viewEndTime - m_viewStartTime) / curveRect.z;
        float panY = io.MouseDelta.y * (m_curveViewMaxValue - m_curveViewMinValue) / curveRect.w;
        m_viewStartTime += panX;
        m_viewEndTime += panX;
        m_curveViewMinValue += panY;
        m_curveViewMaxValue += panY;
    }
}

void AnimationTimeline::HandleKeyframeSelection(const XMFLOAT2& mousePos, bool isAdditive) {
    if (!m_currentClip) return;

    AnimationKeyframe* hit = FindKeyframeAtPosition(mousePos, 8.0f);

    if (!isAdditive) {
        // Deselect all
        for (auto& track : m_currentClip->tracks) {
            for (auto& curve : track->curves) {
                for (auto& kf : curve->keyframes) {
                    kf.isSelected = false;
                }
            }
        }
        m_selection.selectedKeyframes.clear();
    }

    if (hit) {
        hit->isSelected = !hit->isSelected;
        if (hit->isSelected) {
            m_selection.selectedKeyframes.push_back(hit);
            m_isDraggingKeyframes = true;
            m_dragStartPos = mousePos;
        } else {
            // Remove from selection
            m_selection.selectedKeyframes.erase(
                std::remove(m_selection.selectedKeyframes.begin(),
                            m_selection.selectedKeyframes.end(), hit),
                m_selection.selectedKeyframes.end());
        }
    }
}

void AnimationTimeline::HandleKeyframeDragging() {
    if (!m_currentClip) return;

    ImGuiIO& io = ImGui::GetIO();

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_isDraggingKeyframes = false;
        return;
    }

    // Compute time delta from mouse delta
    ImVec2 canvasPos = ImGui::GetWindowPos();
    ImVec2 canvasSize = ImGui::GetWindowSize();
    XMFLOAT4 timelineRect = {canvasPos.x + m_trackListWidth, canvasPos.y, canvasSize.x - m_trackListWidth, canvasSize.y};

    float timeDelta = io.MouseDelta.x * (m_viewEndTime - m_viewStartTime) / timelineRect.z;

    for (auto* kf : m_selection.selectedKeyframes) {
        if (!kf->isLocked) {
            kf->time += timeDelta;
            if (m_snapToFrames) {
                kf->time = SnapToFrame(kf->time);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Preview / Apply / Record
// ---------------------------------------------------------------------------

void AnimationTimeline::UpdateAnimationPreview() {
    if (!m_currentClip) return;

    std::unordered_map<std::string, XMFLOAT4> values;
    m_currentClip->Evaluate(values);

    // The evaluated values would be applied to the scene in a full integration.
    // For now, just evaluate to keep the data current.
    ApplyAnimationToScene();
}

void AnimationTimeline::ApplyAnimationToScene() {
    if (!m_currentClip) return;

    // In a full engine integration, this would iterate through evaluated values
    // and apply them to the corresponding scene objects via the scene manager.
    // This is a stub integration point that the engine's runtime would fill in.
    std::unordered_map<std::string, XMFLOAT4> values;
    m_currentClip->Evaluate(values);

    // values now contains property path -> interpolated value mappings
    // e.g., "transform.position.x" -> {value, 0, 0, 0}
    // Engine would set these on the actual objects.
}

void AnimationTimeline::RecordKeyframes() {
    if (!m_currentClip) return;
    if (m_playbackState != PlaybackState::RECORDING) return;

    // In a full engine integration, this would sample current scene object
    // properties and create keyframes at the current time.
    // This requires access to the scene manager which is an external dependency.
}

} // namespace SparkEditor
