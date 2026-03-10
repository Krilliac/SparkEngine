/**
 * @file Sequencer.cpp
 * @brief Cinematic sequencer implementation — timeline evaluation, track interpolation
 */

#include "Sequencer.h"
#include <sstream>
#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace Spark::Cinematic
{

    // ============================================================================
    // Helper: Linear interpolation for float
    // ============================================================================

    static float LerpF(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    static XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    // ============================================================================
    // Helper: Cubic Bezier ease curve
    // ============================================================================

    /**
     * @brief Evaluate a cubic bezier ease curve defined by control points
     *        (0,0), (cx1,cy1), (cx2,cy2), (1,1).
     *
     * Uses a standard ease-in-out curve: P1=(0.42, 0), P2=(0.58, 1).
     * The input t in [0,1] is remapped through the bezier curve to produce
     * a smoothly eased parameter.
     *
     * We solve for the bezier parameter u such that Bx(u) = t using
     * Newton-Raphson iteration, then return By(u).
     */
    static float CubicBezierEase(float t)
    {
        // Control points for a smooth ease-in-out
        constexpr float cx1 = 0.42f;
        constexpr float cy1 = 0.0f;
        constexpr float cx2 = 0.58f;
        constexpr float cy2 = 1.0f;

        if (t <= 0.0f)
            return 0.0f;
        if (t >= 1.0f)
            return 1.0f;

        // Bezier coefficients for the x component: B(u) = 3(1-u)^2*u*cx1 + 3(1-u)*u^2*cx2 + u^3
        // Expanded: B(u) = (3*cx1)*u - (6*cx1 - 3*cx2)*u^2 + (3*cx1 - 3*cx2 + 1)*u^3
        float ax = 3.0f * cx1;
        float bx = 3.0f * (cx2 - cx1) - 3.0f * cx1;
        float cxCoeff = 1.0f - 3.0f * cx2 + 3.0f * cx1;

        // Newton-Raphson: find u where x(u) = t
        float u = t; // Initial guess
        for (int i = 0; i < 8; ++i)
        {
            float xVal = ((cxCoeff * u + bx) * u + ax) * u - t;
            float dxVal = (3.0f * cxCoeff * u + 2.0f * bx) * u + ax;
            if (std::abs(dxVal) < 1e-7f)
                break;
            u -= xVal / dxVal;
            u = (std::max)(0.0f, (std::min)(1.0f, u));
        }

        // Evaluate y(u) with the same bezier formula using cy control points
        float ay = 3.0f * cy1;
        float by = 3.0f * (cy2 - cy1) - 3.0f * cy1;
        float cyCoeff = 1.0f - 3.0f * cy2 + 3.0f * cy1;
        return ((cyCoeff * u + by) * u + ay) * u;
    }

    /**
     * @brief Apply cubic bezier easing to a float interpolation.
     */
    static float CubicBezierLerpF(float a, float b, float t)
    {
        return LerpF(a, b, CubicBezierEase(t));
    }

    /**
     * @brief Apply cubic bezier easing to a vector interpolation.
     */
    static XMFLOAT3 CubicBezierLerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
    {
        float eased = CubicBezierEase(t);
        return LerpFloat3(a, b, eased);
    }

    // ============================================================================
    // CameraPathTrack
    // ============================================================================

    float CameraPathTrack::GetDuration() const
    {
        if (m_keyframes.empty())
            return 0.0f;
        return m_keyframes.back().time;
    }

    void CameraPathTrack::AddKeyframe(const CameraKeyframe& kf)
    {
        m_keyframes.push_back(kf);
        std::sort(m_keyframes.begin(), m_keyframes.end(),
                  [](const CameraKeyframe& a, const CameraKeyframe& b) { return a.time < b.time; });
    }

    void CameraPathTrack::RemoveKeyframe(int index)
    {
        if (index >= 0 && index < static_cast<int>(m_keyframes.size()))
        {
            m_keyframes.erase(m_keyframes.begin() + index);
        }
    }

    CameraKeyframe CameraPathTrack::Evaluate(float time) const
    {
        if (m_keyframes.empty())
        {
            return {0.0f, {0, 0, 0}, {0, 0, -1}, 60.0f, 0.0f};
        }
        if (m_keyframes.size() == 1 || time <= m_keyframes.front().time)
        {
            return m_keyframes.front();
        }
        if (time >= m_keyframes.back().time)
        {
            return m_keyframes.back();
        }

        // Find the two surrounding keyframes
        int idx = 0;
        for (int i = 0; i < static_cast<int>(m_keyframes.size()) - 1; ++i)
        {
            if (time >= m_keyframes[i].time && time < m_keyframes[i + 1].time)
            {
                idx = i;
                break;
            }
        }

        const auto& kf0 = m_keyframes[idx];
        const auto& kf1 = m_keyframes[idx + 1];
        float segmentDuration = kf1.time - kf0.time;
        float t = (segmentDuration > 0.0001f) ? (time - kf0.time) / segmentDuration : 0.0f;

        CameraKeyframe result;
        result.time = time;

        if (kf0.interpolation == InterpolationMode::Step)
        {
            result.position = kf0.position;
            result.lookAt = kf0.lookAt;
            result.fov = kf0.fov;
            result.roll = kf0.roll;
            result.interpolation = kf0.interpolation;
            return result;
        }
        else if (kf0.interpolation == InterpolationMode::CubicBezier)
        {
            result.position = CubicBezierLerpFloat3(kf0.position, kf1.position, t);
            result.lookAt = CubicBezierLerpFloat3(kf0.lookAt, kf1.lookAt, t);
        }
        else if (kf0.interpolation == InterpolationMode::CatmullRom && m_keyframes.size() >= 4)
        {
            // Catmull-Rom needs 4 points
            int i0 = (std::max)(idx - 1, 0);
            int i3 = (std::min)(idx + 2, static_cast<int>(m_keyframes.size()) - 1);
            result.position = CatmullRomInterpolate(m_keyframes[i0].position, kf0.position, kf1.position,
                                                    m_keyframes[i3].position, t);
            result.lookAt =
                CatmullRomInterpolate(m_keyframes[i0].lookAt, kf0.lookAt, kf1.lookAt, m_keyframes[i3].lookAt, t);
        }
        else
        {
            // Linear interpolation (default)
            result.position = LerpFloat3(kf0.position, kf1.position, t);
            result.lookAt = LerpFloat3(kf0.lookAt, kf1.lookAt, t);
        }

        if (kf0.interpolation == InterpolationMode::CubicBezier)
        {
            result.fov = CubicBezierLerpF(kf0.fov, kf1.fov, t);
            result.roll = CubicBezierLerpF(kf0.roll, kf1.roll, t);
        }
        else
        {
            result.fov = LerpF(kf0.fov, kf1.fov, t);
            result.roll = LerpF(kf0.roll, kf1.roll, t);
        }
        result.interpolation = kf0.interpolation;

        return result;
    }

    XMFLOAT3 CameraPathTrack::CatmullRomInterpolate(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2,
                                                    const XMFLOAT3& p3, float t) const
    {

        float t2 = t * t;
        float t3 = t2 * t;

        XMFLOAT3 result;
        result.x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                           (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
        result.y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                           (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
        result.z = 0.5f * ((2.0f * p1.z) + (-p0.z + p2.z) * t + (2.0f * p0.z - 5.0f * p1.z + 4.0f * p2.z - p3.z) * t2 +
                           (-p0.z + 3.0f * p1.z - 3.0f * p2.z + p3.z) * t3);

        return result;
    }

    // ============================================================================
    // EntityTransformTrack
    // ============================================================================

    float EntityTransformTrack::GetDuration() const
    {
        float maxTime = 0.0f;
        auto getMax = [&](const auto& keys)
        {
            if (!keys.empty())
                maxTime = (std::max)(maxTime, keys.back().time);
        };
        getMax(m_positionKeys);
        getMax(m_rotationKeys);
        getMax(m_scaleKeys);
        return maxTime;
    }

    void EntityTransformTrack::AddPositionKeyframe(const VectorKeyframe& kf)
    {
        m_positionKeys.push_back(kf);
        std::sort(m_positionKeys.begin(), m_positionKeys.end(),
                  [](const VectorKeyframe& a, const VectorKeyframe& b) { return a.time < b.time; });
    }

    void EntityTransformTrack::AddRotationKeyframe(const VectorKeyframe& kf)
    {
        m_rotationKeys.push_back(kf);
        std::sort(m_rotationKeys.begin(), m_rotationKeys.end(),
                  [](const VectorKeyframe& a, const VectorKeyframe& b) { return a.time < b.time; });
    }

    void EntityTransformTrack::AddScaleKeyframe(const VectorKeyframe& kf)
    {
        m_scaleKeys.push_back(kf);
        std::sort(m_scaleKeys.begin(), m_scaleKeys.end(),
                  [](const VectorKeyframe& a, const VectorKeyframe& b) { return a.time < b.time; });
    }

    XMFLOAT3 EntityTransformTrack::EvaluatePosition(float time) const
    {
        return EvaluateVectorKeys(m_positionKeys, time);
    }

    XMFLOAT3 EntityTransformTrack::EvaluateRotation(float time) const
    {
        return EvaluateVectorKeys(m_rotationKeys, time);
    }

    XMFLOAT3 EntityTransformTrack::EvaluateScale(float time) const
    {
        XMFLOAT3 result = EvaluateVectorKeys(m_scaleKeys, time);
        if (m_scaleKeys.empty())
            return {1.0f, 1.0f, 1.0f};
        return result;
    }

    XMFLOAT3 EntityTransformTrack::EvaluateVectorKeys(const std::vector<VectorKeyframe>& keys, float time) const
    {
        if (keys.empty())
            return {0, 0, 0};
        if (keys.size() == 1 || time <= keys.front().time)
            return keys.front().value;
        if (time >= keys.back().time)
            return keys.back().value;

        for (size_t i = 0; i + 1 < keys.size(); ++i)
        {
            if (time >= keys[i].time && time < keys[i + 1].time)
            {
                float segDur = keys[i + 1].time - keys[i].time;
                float t = (segDur > 0.0001f) ? (time - keys[i].time) / segDur : 0.0f;

                if (keys[i].interpolation == InterpolationMode::Step)
                {
                    return keys[i].value;
                }
                if (keys[i].interpolation == InterpolationMode::CubicBezier)
                {
                    return CubicBezierLerpFloat3(keys[i].value, keys[i + 1].value, t);
                }
                return LerpFloat3(keys[i].value, keys[i + 1].value, t);
            }
        }
        return keys.back().value;
    }

    // ============================================================================
    // EntityPropertyTrack
    // ============================================================================

    float EntityPropertyTrack::GetDuration() const
    {
        if (m_keyframes.empty())
            return 0.0f;
        return m_keyframes.back().time;
    }

    void EntityPropertyTrack::AddKeyframe(const PropertyKeyframe& kf)
    {
        m_keyframes.push_back(kf);
        std::sort(m_keyframes.begin(), m_keyframes.end(),
                  [](const PropertyKeyframe& a, const PropertyKeyframe& b) { return a.time < b.time; });
    }

    float EntityPropertyTrack::Evaluate(float time) const
    {
        if (m_keyframes.empty())
            return 0.0f;
        if (m_keyframes.size() == 1 || time <= m_keyframes.front().time)
            return m_keyframes.front().value;
        if (time >= m_keyframes.back().time)
            return m_keyframes.back().value;

        for (size_t i = 0; i + 1 < m_keyframes.size(); ++i)
        {
            if (time >= m_keyframes[i].time && time < m_keyframes[i + 1].time)
            {
                float segDur = m_keyframes[i + 1].time - m_keyframes[i].time;
                float t = (segDur > 0.0001f) ? (time - m_keyframes[i].time) / segDur : 0.0f;

                if (m_keyframes[i].interpolation == InterpolationMode::Step)
                {
                    return m_keyframes[i].value;
                }
                if (m_keyframes[i].interpolation == InterpolationMode::CubicBezier)
                {
                    return CubicBezierLerpF(m_keyframes[i].value, m_keyframes[i + 1].value, t);
                }
                return LerpF(m_keyframes[i].value, m_keyframes[i + 1].value, t);
            }
        }
        return m_keyframes.back().value;
    }

    // ============================================================================
    // AudioCueTrack
    // ============================================================================

    float AudioCueTrack::GetDuration() const
    {
        if (m_cues.empty())
            return 0.0f;
        return m_cues.back().time;
    }

    void AudioCueTrack::AddCue(const AudioCue& cue)
    {
        m_cues.push_back(cue);
        std::sort(m_cues.begin(), m_cues.end(), [](const AudioCue& a, const AudioCue& b) { return a.time < b.time; });
    }

    std::vector<const AudioCue*> AudioCueTrack::GetTriggeredCues(float prevTime, float currentTime) const
    {
        std::vector<const AudioCue*> triggered;
        for (const auto& cue : m_cues)
        {
            if (cue.time > prevTime && cue.time <= currentTime)
            {
                triggered.push_back(&cue);
            }
        }
        return triggered;
    }

    // ============================================================================
    // EventTrack
    // ============================================================================

    float EventTrack::GetDuration() const
    {
        if (m_cues.empty())
            return 0.0f;
        return m_cues.back().time;
    }

    void EventTrack::AddCue(const EventCue& cue)
    {
        m_cues.push_back(cue);
        std::sort(m_cues.begin(), m_cues.end(), [](const EventCue& a, const EventCue& b) { return a.time < b.time; });
    }

    std::vector<const EventCue*> EventTrack::GetTriggeredCues(float prevTime, float currentTime) const
    {
        std::vector<const EventCue*> triggered;
        for (const auto& cue : m_cues)
        {
            if (cue.time > prevTime && cue.time <= currentTime)
            {
                triggered.push_back(&cue);
            }
        }
        return triggered;
    }

    // ============================================================================
    // SubtitleTrack
    // ============================================================================

    float SubtitleTrack::GetDuration() const
    {
        if (m_subtitles.empty())
            return 0.0f;
        return m_subtitles.back().endTime;
    }

    void SubtitleTrack::AddSubtitle(const SubtitleCue& cue)
    {
        m_subtitles.push_back(cue);
        std::sort(m_subtitles.begin(), m_subtitles.end(),
                  [](const SubtitleCue& a, const SubtitleCue& b) { return a.startTime < b.startTime; });
    }

    const SubtitleCue* SubtitleTrack::GetActiveSubtitle(float time) const
    {
        for (const auto& sub : m_subtitles)
        {
            if (time >= sub.startTime && time < sub.endTime)
            {
                return &sub;
            }
        }
        return nullptr;
    }

    // ============================================================================
    // FadeTrack
    // ============================================================================

    float FadeTrack::GetDuration() const
    {
        if (m_keyframes.empty())
            return 0.0f;
        return m_keyframes.back().time;
    }

    void FadeTrack::AddKeyframe(const FadeKeyframe& kf)
    {
        m_keyframes.push_back(kf);
        std::sort(m_keyframes.begin(), m_keyframes.end(),
                  [](const FadeKeyframe& a, const FadeKeyframe& b) { return a.time < b.time; });
    }

    FadeKeyframe FadeTrack::Evaluate(float time) const
    {
        if (m_keyframes.empty())
            return {0.0f, 0.0f, {0, 0, 0}};
        if (m_keyframes.size() == 1 || time <= m_keyframes.front().time)
            return m_keyframes.front();
        if (time >= m_keyframes.back().time)
            return m_keyframes.back();

        for (size_t i = 0; i + 1 < m_keyframes.size(); ++i)
        {
            if (time >= m_keyframes[i].time && time < m_keyframes[i + 1].time)
            {
                float segDur = m_keyframes[i + 1].time - m_keyframes[i].time;
                float t = (segDur > 0.0001f) ? (time - m_keyframes[i].time) / segDur : 0.0f;

                FadeKeyframe result;
                result.time = time;

                if (m_keyframes[i].interpolation == InterpolationMode::Step)
                {
                    result.opacity = m_keyframes[i].opacity;
                    result.color = m_keyframes[i].color;
                }
                else if (m_keyframes[i].interpolation == InterpolationMode::CubicBezier)
                {
                    result.opacity = CubicBezierLerpF(m_keyframes[i].opacity, m_keyframes[i + 1].opacity, t);
                    result.color = CubicBezierLerpFloat3(m_keyframes[i].color, m_keyframes[i + 1].color, t);
                }
                else
                {
                    result.opacity = LerpF(m_keyframes[i].opacity, m_keyframes[i + 1].opacity, t);
                    result.color = LerpFloat3(m_keyframes[i].color, m_keyframes[i + 1].color, t);
                }
                return result;
            }
        }
        return m_keyframes.back();
    }

    // ============================================================================
    // Sequence
    // ============================================================================

    Sequence::Sequence(const std::string& name) : m_name(name) {}

    CameraPathTrack* Sequence::AddCameraTrack(const std::string& trackName)
    {
        auto track = std::make_unique<CameraPathTrack>();
        track->name = trackName;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        m_hasCameraTrack = true;
        return ptr;
    }

    EntityTransformTrack* Sequence::AddEntityTransformTrack(const std::string& trackName, uint32_t entityID)
    {
        auto track = std::make_unique<EntityTransformTrack>();
        track->name = trackName;
        track->targetEntityID = entityID;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        return ptr;
    }

    EntityPropertyTrack* Sequence::AddEntityPropertyTrack(const std::string& trackName, uint32_t entityID,
                                                          const std::string& property)
    {
        auto track = std::make_unique<EntityPropertyTrack>();
        track->name = trackName;
        track->targetEntityID = entityID;
        track->propertyName = property;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        return ptr;
    }

    AudioCueTrack* Sequence::AddAudioCueTrack(const std::string& trackName)
    {
        auto track = std::make_unique<AudioCueTrack>();
        track->name = trackName;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        return ptr;
    }

    EventTrack* Sequence::AddEventTrack(const std::string& trackName)
    {
        auto track = std::make_unique<EventTrack>();
        track->name = trackName;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        return ptr;
    }

    SubtitleTrack* Sequence::AddSubtitleTrack(const std::string& trackName)
    {
        auto track = std::make_unique<SubtitleTrack>();
        track->name = trackName;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        return ptr;
    }

    FadeTrack* Sequence::AddFadeTrack(const std::string& trackName)
    {
        auto track = std::make_unique<FadeTrack>();
        track->name = trackName;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
        return ptr;
    }

    void Sequence::Play()
    {
        if (m_playState == SequencePlayState::Stopped)
        {
            m_currentTime = 0.0f;
            m_previousTime = 0.0f;
        }
        m_playState = SequencePlayState::Playing;
    }

    void Sequence::Pause()
    {
        if (m_playState == SequencePlayState::Playing)
        {
            m_playState = SequencePlayState::Paused;
        }
    }

    void Sequence::Stop()
    {
        m_playState = SequencePlayState::Stopped;
        m_currentTime = 0.0f;
        m_previousTime = 0.0f;
    }

    void Sequence::SetTime(float time)
    {
        m_previousTime = m_currentTime;
        m_currentTime = (std::max)(0.0f, (std::min)(time, GetDuration()));
    }

    float Sequence::GetDuration() const
    {
        float maxDuration = 0.0f;
        for (const auto& track : m_tracks)
        {
            if (track->enabled)
            {
                maxDuration = (std::max)(maxDuration, track->GetDuration());
            }
        }
        return maxDuration;
    }

    void Sequence::Update(float deltaTime)
    {
        if (m_playState != SequencePlayState::Playing)
            return;

        m_previousTime = m_currentTime;
        m_currentTime += deltaTime * m_playbackSpeed;

        float duration = GetDuration();
        if (m_currentTime >= duration)
        {
            if (m_looping)
            {
                m_currentTime = std::fmod(m_currentTime, duration);
                m_previousTime = 0.0f;
            }
            else
            {
                m_currentTime = duration;
                m_playState = SequencePlayState::Stopped;
            }
        }

        // Process event and audio cue tracks
        for (const auto& track : m_tracks)
        {
            if (!track->enabled)
                continue;

            if (track->GetType() == TrackType::Event)
            {
                auto* eventTrack = static_cast<EventTrack*>(track.get());
                auto triggered = eventTrack->GetTriggeredCues(m_previousTime, m_currentTime);
                for (const auto* cue : triggered)
                {
                    if (m_eventCallback)
                    {
                        m_eventCallback(cue->eventName, cue->parameters);
                    }
                }
            }
        }
    }

    const CameraKeyframe* Sequence::GetCurrentCameraState() const
    {
        for (const auto& track : m_tracks)
        {
            if (track->enabled && track->GetType() == TrackType::CameraPath)
            {
                auto* camTrack = static_cast<CameraPathTrack*>(track.get());
                m_currentCamera = camTrack->Evaluate(m_currentTime);
                return &m_currentCamera;
            }
        }
        return nullptr;
    }

    const SubtitleCue* Sequence::GetCurrentSubtitle() const
    {
        for (const auto& track : m_tracks)
        {
            if (track->enabled && track->GetType() == TrackType::Subtitle)
            {
                auto* subTrack = static_cast<SubtitleTrack*>(track.get());
                return subTrack->GetActiveSubtitle(m_currentTime);
            }
        }
        return nullptr;
    }

    FadeKeyframe Sequence::GetCurrentFade() const
    {
        for (const auto& track : m_tracks)
        {
            if (track->enabled && track->GetType() == TrackType::Fade)
            {
                auto* fadeTrack = static_cast<FadeTrack*>(track.get());
                return fadeTrack->Evaluate(m_currentTime);
            }
        }
        return {0.0f, 0.0f, {0, 0, 0}};
    }

    // ============================================================================
    // SequencerManager
    // ============================================================================

    SequencerManager& SequencerManager::GetInstance()
    {
        static SequencerManager instance;
        return instance;
    }

    Sequence* SequencerManager::CreateSequence(const std::string& name)
    {
        auto seq = std::make_unique<Sequence>(name);
        auto* ptr = seq.get();
        m_sequences[name] = std::move(seq);
        return ptr;
    }

    Sequence* SequencerManager::GetSequence(const std::string& name)
    {
        auto it = m_sequences.find(name);
        return (it != m_sequences.end()) ? it->second.get() : nullptr;
    }

    void SequencerManager::RemoveSequence(const std::string& name)
    {
        m_sequences.erase(name);
    }

    bool SequencerManager::PlaySequence(const std::string& name)
    {
        auto* seq = GetSequence(name);
        if (!seq)
            return false;
        seq->Play();
        return true;
    }

    void SequencerManager::StopSequence(const std::string& name)
    {
        auto* seq = GetSequence(name);
        if (seq)
            seq->Stop();
    }

    void SequencerManager::PauseSequence(const std::string& name)
    {
        auto* seq = GetSequence(name);
        if (seq)
            seq->Pause();
    }

    void SequencerManager::StopAll()
    {
        for (auto& [name, seq] : m_sequences)
        {
            seq->Stop();
        }
    }

    void SequencerManager::Update(float deltaTime)
    {
        for (auto& [name, seq] : m_sequences)
        {
            if (seq->GetPlayState() == SequencePlayState::Playing)
            {
                seq->Update(deltaTime);
            }
        }
    }

    bool SequencerManager::IsAnyCutscenePlaying() const
    {
        for (const auto& [name, seq] : m_sequences)
        {
            if (seq->GetPlayState() == SequencePlayState::Playing)
                return true;
        }
        return false;
    }

    Sequence* SequencerManager::GetActiveSequence()
    {
        for (auto& [name, seq] : m_sequences)
        {
            if (seq->GetPlayState() == SequencePlayState::Playing)
                return seq.get();
        }
        return nullptr;
    }

    std::string SequencerManager::Console_ListSequences() const
    {
        std::ostringstream ss;
        ss << "=== Sequences (" << m_sequences.size() << ") ===\n";
        for (const auto& [name, seq] : m_sequences)
        {
            const char* stateStr = "Stopped";
            if (seq->GetPlayState() == SequencePlayState::Playing)
                stateStr = "Playing";
            else if (seq->GetPlayState() == SequencePlayState::Paused)
                stateStr = "Paused";

            ss << "  " << name << " [" << stateStr << "] " << seq->GetTracks().size() << " tracks, "
               << seq->GetDuration() << "s\n";
        }
        return ss.str();
    }

    std::string SequencerManager::Console_GetSequenceInfo(const std::string& name) const
    {
        auto it = m_sequences.find(name);
        if (it == m_sequences.end())
            return "Sequence '" + name + "' not found\n";

        const auto& seq = it->second;
        std::ostringstream ss;
        ss << "=== Sequence: " << name << " ===\n";
        ss << "Duration: " << seq->GetDuration() << "s\n";
        ss << "Current Time: " << seq->GetCurrentTime() << "s\n";
        ss << "Speed: " << seq->GetPlaybackSpeed() << "x\n";
        ss << "Looping: " << (seq->IsLooping() ? "Yes" : "No") << "\n";
        ss << "Tracks:\n";
        for (const auto& track : seq->GetTracks())
        {
            const char* typeStr = "Unknown";
            switch (track->GetType())
            {
            case TrackType::CameraPath:
                typeStr = "Camera";
                break;
            case TrackType::EntityProperty:
                typeStr = "Property";
                break;
            case TrackType::EntityTransform:
                typeStr = "Transform";
                break;
            case TrackType::AudioCue:
                typeStr = "Audio";
                break;
            case TrackType::Event:
                typeStr = "Event";
                break;
            case TrackType::Subtitle:
                typeStr = "Subtitle";
                break;
            case TrackType::Fade:
                typeStr = "Fade";
                break;
            }
            ss << "  [" << typeStr << "] " << track->GetName() << " (" << track->GetDuration() << "s)"
               << (track->enabled ? "" : " DISABLED") << "\n";
        }
        return ss.str();
    }

    void SequencerManager::Console_PlaySequence(const std::string& name)
    {
        PlaySequence(name);
    }

    void SequencerManager::Console_StopSequence(const std::string& name)
    {
        auto* seq = GetSequence(name);
        if (seq)
            seq->Stop();
    }

    void SequencerManager::Console_SetTime(const std::string& name, float time)
    {
        auto* seq = GetSequence(name);
        if (seq)
            seq->SetTime(time);
    }

} // namespace Spark::Cinematic
