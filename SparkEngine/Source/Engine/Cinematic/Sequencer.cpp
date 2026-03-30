/**
 * @file Sequencer.cpp
 * @brief Implementation of the cinematic sequencer system
 */

#include "Sequencer.h"
#include "../../Utils/LogMacros.h"

#include <sstream>

namespace Spark::Cinematic
{

    // ============================================================================
    // Interpolation Helpers
    // ============================================================================

    namespace
    {

        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        XMFLOAT3 LerpVec3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
        {
            return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
        }

        /// Catmull-Rom spline interpolation for XMFLOAT3
        XMFLOAT3 CatmullRomVec3(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3, float t)
        {
            float t2 = t * t;
            float t3 = t2 * t;

            auto interp = [&](float v0, float v1, float v2, float v3) -> float
            {
                return 0.5f * ((2.0f * v1) + (-v0 + v2) * t + (2.0f * v0 - 5.0f * v1 + 4.0f * v2 - v3) * t2 +
                               (-v0 + 3.0f * v1 - 3.0f * v2 + v3) * t3);
            };

            return {interp(p0.x, p1.x, p2.x, p3.x), interp(p0.y, p1.y, p2.y, p3.y), interp(p0.z, p1.z, p2.z, p3.z)};
        }

        /// Cubic Bezier (Hermite-style with auto tangents) for XMFLOAT3
        XMFLOAT3 CubicBezierVec3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
        {
            // Simplified cubic ease: smoothstep-like interpolation
            float s = t * t * (3.0f - 2.0f * t);
            return LerpVec3(a, b, s);
        }

        /// Normalized time between two keyframes
        float NormalizedTime(float time, float t0, float t1)
        {
            if (t1 <= t0)
                return 0.0f;
            return std::clamp((time - t0) / (t1 - t0), 0.0f, 1.0f);
        }

        /// Evaluate a sorted vector of VectorKeyframes at a given time
        XMFLOAT3 EvaluateVectorKeys(const std::vector<VectorKeyframe>& keys, float time, const XMFLOAT3& defaultVal)
        {
            if (keys.empty())
                return defaultVal;
            if (time <= keys.front().time)
                return keys.front().value;
            if (time >= keys.back().time)
                return keys.back().value;

            // Find the two surrounding keyframes
            for (size_t i = 0; i + 1 < keys.size(); ++i)
            {
                if (time >= keys[i].time && time <= keys[i + 1].time)
                {
                    float t = NormalizedTime(time, keys[i].time, keys[i + 1].time);
                    switch (keys[i + 1].interpolation)
                    {
                    case InterpolationMode::Step:
                        return keys[i].value;
                    case InterpolationMode::CubicBezier:
                        return CubicBezierVec3(keys[i].value, keys[i + 1].value, t);
                    case InterpolationMode::Linear:
                    default:
                        return LerpVec3(keys[i].value, keys[i + 1].value, t);
                    }
                }
            }
            return keys.back().value;
        }

    } // anonymous namespace

    // ============================================================================
    // CameraPathTrack
    // ============================================================================

    float CameraPathTrack::GetDuration() const
    {
        return m_keyframes.empty() ? 0.0f : m_keyframes.back().time;
    }

    void CameraPathTrack::AddKeyframe(const CameraKeyframe& kf)
    {
        auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), kf.time,
                                   [](const CameraKeyframe& k, float t) { return k.time < t; });
        m_keyframes.insert(it, kf);
    }

    void CameraPathTrack::RemoveKeyframe(int index)
    {
        if (index >= 0 && index < static_cast<int>(m_keyframes.size()))
            m_keyframes.erase(m_keyframes.begin() + index);
    }

    CameraKeyframe CameraPathTrack::Evaluate(float time) const
    {
        if (m_keyframes.empty())
            return {};
        if (m_keyframes.size() == 1 || time <= m_keyframes.front().time)
            return m_keyframes.front();
        if (time >= m_keyframes.back().time)
            return m_keyframes.back();

        // Find surrounding keyframes
        size_t idx = 0;
        for (size_t i = 0; i + 1 < m_keyframes.size(); ++i)
        {
            if (time >= m_keyframes[i].time && time <= m_keyframes[i + 1].time)
            {
                idx = i;
                break;
            }
        }

        const auto& a = m_keyframes[idx];
        const auto& b = m_keyframes[idx + 1];
        float t = NormalizedTime(time, a.time, b.time);

        CameraKeyframe result;
        result.time = time;

        switch (b.interpolation)
        {
        case InterpolationMode::Step:
            return a;

        case InterpolationMode::CatmullRom:
        {
            // Use surrounding keyframes for Catmull-Rom (clamp at boundaries)
            size_t i0 = (idx > 0) ? idx - 1 : 0;
            size_t i3 = std::min(idx + 2, m_keyframes.size() - 1);
            const auto& p0 = m_keyframes[i0];
            const auto& p3 = m_keyframes[i3];

            result.position = CatmullRomVec3(p0.position, a.position, b.position, p3.position, t);
            result.lookAt = CatmullRomVec3(p0.lookAt, a.lookAt, b.lookAt, p3.lookAt, t);
            result.fov = Lerp(a.fov, b.fov, t);
            result.roll = Lerp(a.roll, b.roll, t);
            break;
        }

        case InterpolationMode::CubicBezier:
            result.position = CubicBezierVec3(a.position, b.position, t);
            result.lookAt = CubicBezierVec3(a.lookAt, b.lookAt, t);
            result.fov = Lerp(a.fov, b.fov, t);
            result.roll = Lerp(a.roll, b.roll, t);
            break;

        case InterpolationMode::Linear:
        default:
            result.position = LerpVec3(a.position, b.position, t);
            result.lookAt = LerpVec3(a.lookAt, b.lookAt, t);
            result.fov = Lerp(a.fov, b.fov, t);
            result.roll = Lerp(a.roll, b.roll, t);
            break;
        }

        return result;
    }

    // ============================================================================
    // EntityTransformTrack
    // ============================================================================

    float EntityTransformTrack::GetDuration() const
    {
        float dur = 0.0f;
        if (!m_positionKeys.empty())
            dur = std::max(dur, m_positionKeys.back().time);
        if (!m_rotationKeys.empty())
            dur = std::max(dur, m_rotationKeys.back().time);
        if (!m_scaleKeys.empty())
            dur = std::max(dur, m_scaleKeys.back().time);
        return dur;
    }

    void EntityTransformTrack::AddPositionKeyframe(const VectorKeyframe& kf)
    {
        auto it = std::lower_bound(m_positionKeys.begin(), m_positionKeys.end(), kf.time,
                                   [](const VectorKeyframe& k, float t) { return k.time < t; });
        m_positionKeys.insert(it, kf);
    }

    void EntityTransformTrack::AddRotationKeyframe(const VectorKeyframe& kf)
    {
        auto it = std::lower_bound(m_rotationKeys.begin(), m_rotationKeys.end(), kf.time,
                                   [](const VectorKeyframe& k, float t) { return k.time < t; });
        m_rotationKeys.insert(it, kf);
    }

    void EntityTransformTrack::AddScaleKeyframe(const VectorKeyframe& kf)
    {
        auto it = std::lower_bound(m_scaleKeys.begin(), m_scaleKeys.end(), kf.time,
                                   [](const VectorKeyframe& k, float t) { return k.time < t; });
        m_scaleKeys.insert(it, kf);
    }

    XMFLOAT3 EntityTransformTrack::EvaluatePosition(float time) const
    {
        return EvaluateVectorKeys(m_positionKeys, time, {0, 0, 0});
    }

    XMFLOAT3 EntityTransformTrack::EvaluateRotation(float time) const
    {
        return EvaluateVectorKeys(m_rotationKeys, time, {0, 0, 0});
    }

    XMFLOAT3 EntityTransformTrack::EvaluateScale(float time) const
    {
        return EvaluateVectorKeys(m_scaleKeys, time, {1, 1, 1});
    }

    // ============================================================================
    // EntityPropertyTrack
    // ============================================================================

    float EntityPropertyTrack::GetDuration() const
    {
        return m_keyframes.empty() ? 0.0f : m_keyframes.back().time;
    }

    void EntityPropertyTrack::AddKeyframe(const PropertyKeyframe& kf)
    {
        auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), kf.time,
                                   [](const PropertyKeyframe& k, float t) { return k.time < t; });
        m_keyframes.insert(it, kf);
    }

    float EntityPropertyTrack::Evaluate(float time) const
    {
        if (m_keyframes.empty())
            return 0.0f;
        if (time <= m_keyframes.front().time)
            return m_keyframes.front().value;
        if (time >= m_keyframes.back().time)
            return m_keyframes.back().value;

        for (size_t i = 0; i + 1 < m_keyframes.size(); ++i)
        {
            if (time >= m_keyframes[i].time && time <= m_keyframes[i + 1].time)
            {
                float t = NormalizedTime(time, m_keyframes[i].time, m_keyframes[i + 1].time);
                switch (m_keyframes[i + 1].interpolation)
                {
                case InterpolationMode::Step:
                    return m_keyframes[i].value;
                case InterpolationMode::CubicBezier:
                {
                    float s = t * t * (3.0f - 2.0f * t);
                    return Lerp(m_keyframes[i].value, m_keyframes[i + 1].value, s);
                }
                case InterpolationMode::Linear:
                default:
                    return Lerp(m_keyframes[i].value, m_keyframes[i + 1].value, t);
                }
            }
        }
        return m_keyframes.back().value;
    }

    // ============================================================================
    // AudioCueTrack
    // ============================================================================

    float AudioCueTrack::GetDuration() const
    {
        return m_cues.empty() ? 0.0f : m_cues.back().time;
    }

    void AudioCueTrack::AddCue(const AudioCue& cue)
    {
        auto it = std::lower_bound(m_cues.begin(), m_cues.end(), cue.time,
                                   [](const AudioCue& c, float t) { return c.time < t; });
        m_cues.insert(it, cue);
    }

    std::vector<const AudioCue*> AudioCueTrack::GetTriggeredCues(float prevTime, float currentTime) const
    {
        std::vector<const AudioCue*> triggered;
        for (const auto& cue : m_cues)
        {
            if (cue.time > prevTime && cue.time <= currentTime)
                triggered.push_back(&cue);
        }
        return triggered;
    }

    // ============================================================================
    // EventTrack
    // ============================================================================

    float EventTrack::GetDuration() const
    {
        return m_cues.empty() ? 0.0f : m_cues.back().time;
    }

    void EventTrack::AddCue(const EventCue& cue)
    {
        auto it = std::lower_bound(m_cues.begin(), m_cues.end(), cue.time,
                                   [](const EventCue& c, float t) { return c.time < t; });
        m_cues.insert(it, cue);
    }

    std::vector<const EventCue*> EventTrack::GetTriggeredCues(float prevTime, float currentTime) const
    {
        std::vector<const EventCue*> triggered;
        for (const auto& cue : m_cues)
        {
            if (cue.time > prevTime && cue.time <= currentTime)
                triggered.push_back(&cue);
        }
        return triggered;
    }

    // ============================================================================
    // SubtitleTrack
    // ============================================================================

    float SubtitleTrack::GetDuration() const
    {
        float dur = 0.0f;
        for (const auto& s : m_subtitles)
            dur = std::max(dur, s.endTime);
        return dur;
    }

    void SubtitleTrack::AddSubtitle(const SubtitleCue& cue)
    {
        m_subtitles.push_back(cue);
    }

    const SubtitleCue* SubtitleTrack::GetActiveSubtitle(float time) const
    {
        for (const auto& s : m_subtitles)
        {
            if (time >= s.startTime && time <= s.endTime)
                return &s;
        }
        return nullptr;
    }

    // ============================================================================
    // FadeTrack
    // ============================================================================

    float FadeTrack::GetDuration() const
    {
        return m_keyframes.empty() ? 0.0f : m_keyframes.back().time;
    }

    void FadeTrack::AddKeyframe(const FadeKeyframe& kf)
    {
        auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), kf.time,
                                   [](const FadeKeyframe& k, float t) { return k.time < t; });
        m_keyframes.insert(it, kf);
    }

    FadeKeyframe FadeTrack::Evaluate(float time) const
    {
        if (m_keyframes.empty())
            return {0.0f, 0.0f, {0, 0, 0}, InterpolationMode::Linear};
        if (time <= m_keyframes.front().time)
            return m_keyframes.front();
        if (time >= m_keyframes.back().time)
            return m_keyframes.back();

        for (size_t i = 0; i + 1 < m_keyframes.size(); ++i)
        {
            if (time >= m_keyframes[i].time && time <= m_keyframes[i + 1].time)
            {
                float t = NormalizedTime(time, m_keyframes[i].time, m_keyframes[i + 1].time);
                const auto& a = m_keyframes[i];
                const auto& b = m_keyframes[i + 1];

                FadeKeyframe result;
                result.time = time;

                if (b.interpolation == InterpolationMode::Step)
                {
                    result.opacity = a.opacity;
                    result.color = a.color;
                }
                else
                {
                    if (b.interpolation == InterpolationMode::CubicBezier)
                        t = t * t * (3.0f - 2.0f * t);
                    result.opacity = Lerp(a.opacity, b.opacity, t);
                    result.color = LerpVec3(a.color, b.color, t);
                }
                return result;
            }
        }
        return m_keyframes.back();
    }

    // ============================================================================
    // Sequence
    // ============================================================================

    Sequence::Sequence(const std::string& name) : m_name(name)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Cinematic, "Sequence created: %s", name.c_str());
    }

    CameraPathTrack* Sequence::AddCameraTrack(const std::string& trackName)
    {
        auto track = std::make_unique<CameraPathTrack>();
        track->name = trackName;
        auto* ptr = track.get();
        m_tracks.push_back(std::move(track));
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
        if (m_playState == SequencePlayState::Paused)
        {
            m_playState = SequencePlayState::Playing;
            return;
        }
        m_currentTime = 0.0f;
        m_previousTime = 0.0f;
        m_playState = SequencePlayState::Playing;
        SPARK_LOG_INFO(Spark::LogCategory::Cinematic, "Sequence '%s' started (duration=%.2fs, tracks=%zu)",
                       m_name.c_str(), GetDuration(), m_tracks.size());
    }

    void Sequence::Pause()
    {
        if (m_playState == SequencePlayState::Playing)
        {
            m_playState = SequencePlayState::Paused;
            SPARK_LOG_INFO(Spark::LogCategory::Cinematic, "Sequence '%s' paused at %.2fs", m_name.c_str(),
                           m_currentTime);
        }
    }

    void Sequence::Stop()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Cinematic, "Sequence '%s' stopped", m_name.c_str());
        m_playState = SequencePlayState::Stopped;
        m_currentTime = 0.0f;
        m_previousTime = 0.0f;
        m_hasCameraState = false;
    }

    void Sequence::SetTime(float time)
    {
        float duration = GetDuration();
        m_currentTime = std::clamp(time, 0.0f, duration);
        m_previousTime = m_currentTime;
    }

    void Sequence::SetPlaybackSpeed(float speed)
    {
        m_playbackSpeed = speed;
    }

    void Sequence::SetLooping(bool loop)
    {
        m_looping = loop;
    }

    void Sequence::Update(float deltaTime)
    {
        if (m_playState != SequencePlayState::Playing)
            return;

        m_previousTime = m_currentTime;
        m_currentTime += deltaTime * m_playbackSpeed;

        float duration = GetDuration();

        // Process event and audio cue triggers
        for (const auto& track : m_tracks)
        {
            if (!track->enabled)
                continue;

            if (track->GetType() == TrackType::Event && m_eventCallback)
            {
                auto* eventTrack = static_cast<EventTrack*>(track.get());
                auto triggered = eventTrack->GetTriggeredCues(m_previousTime, m_currentTime);
                for (const auto* cue : triggered)
                    m_eventCallback(cue->eventName, cue->parameters);
            }
        }

        // Update cached camera state
        m_hasCameraState = false;
        for (const auto& track : m_tracks)
        {
            if (!track->enabled)
                continue;
            if (track->GetType() == TrackType::CameraPath)
            {
                auto* camTrack = static_cast<CameraPathTrack*>(track.get());
                if (!camTrack->GetKeyframes().empty())
                {
                    m_cachedCamera = camTrack->Evaluate(m_currentTime);
                    m_hasCameraState = true;
                    break; // Use first camera track
                }
            }
        }

        // Handle end of sequence
        if (m_currentTime >= duration)
        {
            if (m_looping)
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Cinematic, "Sequence '%s' looping", m_name.c_str());
                m_currentTime = 0.0f;
                m_previousTime = 0.0f;
            }
            else
            {
                SPARK_LOG_INFO(Spark::LogCategory::Cinematic, "Sequence '%s' finished (duration=%.2fs)", m_name.c_str(),
                               duration);
                m_currentTime = duration;
                m_playState = SequencePlayState::Stopped;
            }
        }
    }

    void Sequence::SetEventCallback(EventCallback callback)
    {
        m_eventCallback = std::move(callback);
    }

    float Sequence::GetDuration() const
    {
        float maxDur = 0.0f;
        for (const auto& track : m_tracks)
            maxDur = std::max(maxDur, track->GetDuration());
        return maxDur;
    }

    const CameraKeyframe* Sequence::GetCurrentCameraState() const
    {
        return m_hasCameraState ? &m_cachedCamera : nullptr;
    }

    const SubtitleCue* Sequence::GetCurrentSubtitle() const
    {
        for (const auto& track : m_tracks)
        {
            if (!track->enabled || track->GetType() != TrackType::Subtitle)
                continue;
            auto* subTrack = static_cast<SubtitleTrack*>(track.get());
            const SubtitleCue* active = subTrack->GetActiveSubtitle(m_currentTime);
            if (active)
                return active;
        }
        return nullptr;
    }

    FadeKeyframe Sequence::GetCurrentFade() const
    {
        for (const auto& track : m_tracks)
        {
            if (!track->enabled || track->GetType() != TrackType::Fade)
                continue;
            auto* fadeTrack = static_cast<FadeTrack*>(track.get());
            return fadeTrack->Evaluate(m_currentTime);
        }
        return {0.0f, 0.0f, {0, 0, 0}, InterpolationMode::Linear};
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
        SPARK_LOG_INFO(Spark::LogCategory::Cinematic, "Creating sequence '%s'", name.c_str());
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
        SPARK_LOG_DEBUG(Spark::LogCategory::Cinematic, "Removing sequence '%s'", name.c_str());
        m_sequences.erase(name);
    }

    bool SequencerManager::PlaySequence(const std::string& name)
    {
        auto* seq = GetSequence(name);
        if (!seq)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Cinematic, "PlaySequence failed: '%s' not found", name.c_str());
            return false;
        }
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
            seq->Stop();
    }

    void SequencerManager::Update(float deltaTime)
    {
        for (auto& [name, seq] : m_sequences)
        {
            if (seq->GetPlayState() == SequencePlayState::Playing)
                seq->Update(deltaTime);
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
        if (m_sequences.empty())
            return "No sequences registered.";

        std::ostringstream ss;
        ss << "=== Cinematic Sequences ===\n";
        for (const auto& [name, seq] : m_sequences)
        {
            const char* stateStr = "Stopped";
            if (seq->GetPlayState() == SequencePlayState::Playing)
                stateStr = "Playing";
            else if (seq->GetPlayState() == SequencePlayState::Paused)
                stateStr = "Paused";

            ss << "  " << name << " [" << stateStr << "] " << seq->GetDuration() << "s, " << seq->GetTracks().size()
               << " tracks\n";
        }
        return ss.str();
    }

    std::string SequencerManager::Console_GetSequenceInfo(const std::string& name) const
    {
        auto it = m_sequences.find(name);
        if (it == m_sequences.end())
            return "Sequence '" + name + "' not found.";

        const auto& seq = it->second;
        std::ostringstream ss;
        ss << "=== Sequence: " << name << " ===\n";
        ss << "Duration: " << seq->GetDuration() << "s\n";
        ss << "Current Time: " << seq->GetCurrentPlaybackTime() << "s\n";
        ss << "Speed: " << seq->GetPlaybackSpeed() << "x\n";
        ss << "Looping: " << (seq->IsLooping() ? "Yes" : "No") << "\n";
        ss << "Tracks: " << seq->GetTracks().size() << "\n";

        for (const auto& track : seq->GetTracks())
        {
            const char* typeStr = "Unknown";
            switch (track->GetType())
            {
            case TrackType::CameraPath:
                typeStr = "Camera";
                break;
            case TrackType::EntityTransform:
                typeStr = "Transform";
                break;
            case TrackType::EntityProperty:
                typeStr = "Property";
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
            ss << "  [" << typeStr << "] " << track->name << " (" << track->GetDuration() << "s)"
               << (track->enabled ? "" : " [DISABLED]") << "\n";
        }
        return ss.str();
    }

} // namespace Spark::Cinematic
