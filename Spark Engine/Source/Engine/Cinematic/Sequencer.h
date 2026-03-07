/**
 * @file Sequencer.h
 * @brief Cinematic sequencer system for cutscenes, camera paths, and scripted events
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a timeline-based sequencer for:
 * - Camera path animations (spline-based)
 * - Entity property keyframes
 * - Audio cue triggers
 * - Scripted event callbacks
 * - Subtitle/text display
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstdint>


namespace Spark::Cinematic {

// ============================================================================
// Timeline Keyframes
// ============================================================================

enum class InterpolationMode {
    Step,       ///< No interpolation — snap to value
    Linear,     ///< Linear interpolation
    CubicBezier,///< Cubic bezier curve
    CatmullRom  ///< Catmull-Rom spline
};

struct CameraKeyframe {
    float time;
    XMFLOAT3 position;
    XMFLOAT3 lookAt;
    float fov;
    float roll;  ///< Roll angle in degrees
    InterpolationMode interpolation = InterpolationMode::CatmullRom;
};

struct PropertyKeyframe {
    float time;
    float value;
    InterpolationMode interpolation = InterpolationMode::Linear;
};

struct VectorKeyframe {
    float time;
    XMFLOAT3 value;
    InterpolationMode interpolation = InterpolationMode::Linear;
};

// ============================================================================
// Sequencer Tracks
// ============================================================================

enum class TrackType {
    CameraPath,     ///< Camera animation track
    EntityProperty, ///< Animate a float property on an entity
    EntityTransform,///< Animate entity position/rotation/scale
    AudioCue,       ///< Trigger audio at specific times
    Event,          ///< Fire script/code callbacks at specific times
    Subtitle,       ///< Display subtitle text
    Fade            ///< Screen fade in/out
};

struct AudioCue {
    float time;
    std::string soundName;
    float volume = 1.0f;
    bool is3D = false;
    XMFLOAT3 position{0, 0, 0};
};

struct EventCue {
    float time;
    std::string eventName;
    std::string parameters;  ///< JSON or simple key=value string
};

struct SubtitleCue {
    float startTime;
    float endTime;
    std::string text;
    std::string speaker;
    XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct FadeKeyframe {
    float time;
    float opacity;  ///< 0.0 = fully transparent, 1.0 = fully black
    XMFLOAT3 color{0, 0, 0};
    InterpolationMode interpolation = InterpolationMode::Linear;
};

// ============================================================================
// Track Base
// ============================================================================

class SequencerTrack {
public:
    virtual ~SequencerTrack() = default;
    virtual TrackType GetType() const = 0;
    virtual const char* GetName() const = 0;
    virtual float GetDuration() const = 0;

    bool enabled = true;
    std::string name;
};

// ============================================================================
// Camera Path Track
// ============================================================================

class CameraPathTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::CameraPath; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    void AddKeyframe(const CameraKeyframe& kf);
    void RemoveKeyframe(int index);

    /// Evaluate camera state at time t
    CameraKeyframe Evaluate(float time) const;

    /// Get all keyframes
    const std::vector<CameraKeyframe>& GetKeyframes() const { return m_keyframes; }

private:
    std::vector<CameraKeyframe> m_keyframes;

    XMFLOAT3 CatmullRomInterpolate(const XMFLOAT3& p0, const XMFLOAT3& p1,
                                     const XMFLOAT3& p2, const XMFLOAT3& p3, float t) const;
};

// ============================================================================
// Entity Transform Track
// ============================================================================

class EntityTransformTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::EntityTransform; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    uint32_t targetEntityID = 0;  ///< Entity to animate

    void AddPositionKeyframe(const VectorKeyframe& kf);
    void AddRotationKeyframe(const VectorKeyframe& kf);
    void AddScaleKeyframe(const VectorKeyframe& kf);

    XMFLOAT3 EvaluatePosition(float time) const;
    XMFLOAT3 EvaluateRotation(float time) const;
    XMFLOAT3 EvaluateScale(float time) const;

private:
    std::vector<VectorKeyframe> m_positionKeys;
    std::vector<VectorKeyframe> m_rotationKeys;
    std::vector<VectorKeyframe> m_scaleKeys;

    XMFLOAT3 EvaluateVectorKeys(const std::vector<VectorKeyframe>& keys, float time) const;
};

// ============================================================================
// Entity Property Track
// ============================================================================

class EntityPropertyTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::EntityProperty; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    uint32_t targetEntityID = 0;
    std::string propertyName;  ///< E.g., "light.intensity", "material.opacity"

    void AddKeyframe(const PropertyKeyframe& kf);
    float Evaluate(float time) const;

    const std::vector<PropertyKeyframe>& GetKeyframes() const { return m_keyframes; }

private:
    std::vector<PropertyKeyframe> m_keyframes;
};

// ============================================================================
// Audio Cue Track
// ============================================================================

class AudioCueTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::AudioCue; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    void AddCue(const AudioCue& cue);
    const std::vector<AudioCue>& GetCues() const { return m_cues; }

    /// Get cues that should fire between prevTime and currentTime
    std::vector<const AudioCue*> GetTriggeredCues(float prevTime, float currentTime) const;

private:
    std::vector<AudioCue> m_cues;
};

// ============================================================================
// Event Track
// ============================================================================

class EventTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::Event; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    void AddCue(const EventCue& cue);
    const std::vector<EventCue>& GetCues() const { return m_cues; }

    std::vector<const EventCue*> GetTriggeredCues(float prevTime, float currentTime) const;

private:
    std::vector<EventCue> m_cues;
};

// ============================================================================
// Subtitle Track
// ============================================================================

class SubtitleTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::Subtitle; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    void AddSubtitle(const SubtitleCue& cue);

    /// Get the active subtitle at a given time (null if none)
    const SubtitleCue* GetActiveSubtitle(float time) const;
    const std::vector<SubtitleCue>& GetSubtitles() const { return m_subtitles; }

private:
    std::vector<SubtitleCue> m_subtitles;
};

// ============================================================================
// Fade Track
// ============================================================================

class FadeTrack : public SequencerTrack {
public:
    TrackType GetType() const override { return TrackType::Fade; }
    const char* GetName() const override { return name.c_str(); }
    float GetDuration() const override;

    void AddKeyframe(const FadeKeyframe& kf);
    FadeKeyframe Evaluate(float time) const;

private:
    std::vector<FadeKeyframe> m_keyframes;
};

// ============================================================================
// Sequence (a complete cutscene)
// ============================================================================

enum class SequencePlayState {
    Stopped,
    Playing,
    Paused
};

using EventCallback = std::function<void(const std::string& eventName, const std::string& params)>;

class Sequence {
public:
    explicit Sequence(const std::string& name);

    /// Track management
    CameraPathTrack* AddCameraTrack(const std::string& trackName);
    EntityTransformTrack* AddEntityTransformTrack(const std::string& trackName, uint32_t entityID);
    EntityPropertyTrack* AddEntityPropertyTrack(const std::string& trackName, uint32_t entityID, const std::string& property);
    AudioCueTrack* AddAudioCueTrack(const std::string& trackName);
    EventTrack* AddEventTrack(const std::string& trackName);
    SubtitleTrack* AddSubtitleTrack(const std::string& trackName);
    FadeTrack* AddFadeTrack(const std::string& trackName);

    /// Playback control
    void Play();
    void Pause();
    void Stop();
    void SetTime(float time);
    void SetPlaybackSpeed(float speed) { m_playbackSpeed = speed; }
    void SetLooping(bool loop) { m_looping = loop; }

    /// Update — call each frame
    void Update(float deltaTime);

    /// Event callbacks
    void SetEventCallback(EventCallback callback) { m_eventCallback = callback; }

    /// Getters
    const std::string& GetName() const { return m_name; }
    float GetCurrentTime() const { return m_currentTime; }
    float GetDuration() const;
    SequencePlayState GetPlayState() const { return m_playState; }
    float GetPlaybackSpeed() const { return m_playbackSpeed; }
    bool IsLooping() const { return m_looping; }

    /// Get current evaluated state
    const CameraKeyframe* GetCurrentCameraState() const;
    const SubtitleCue* GetCurrentSubtitle() const;
    FadeKeyframe GetCurrentFade() const;

    /// Track access
    const std::vector<std::unique_ptr<SequencerTrack>>& GetTracks() const { return m_tracks; }

private:
    std::string m_name;
    std::vector<std::unique_ptr<SequencerTrack>> m_tracks;

    float m_currentTime = 0.0f;
    float m_previousTime = 0.0f;
    float m_playbackSpeed = 1.0f;
    bool m_looping = false;
    SequencePlayState m_playState = SequencePlayState::Stopped;
    EventCallback m_eventCallback;

    // Cached current state
    mutable CameraKeyframe m_currentCamera;
    mutable bool m_hasCameraTrack = false;
};

// ============================================================================
// Sequencer Manager
// ============================================================================

class SequencerManager {
public:
    static SequencerManager& GetInstance();

    /// Create a new sequence
    Sequence* CreateSequence(const std::string& name);

    /// Get a sequence by name
    Sequence* GetSequence(const std::string& name);

    /// Remove a sequence
    void RemoveSequence(const std::string& name);

    /// Play a sequence by name
    bool PlaySequence(const std::string& name);

    /// Stop all playing sequences
    void StopAll();

    /// Update all active sequences
    void Update(float deltaTime);

    /// Check if any sequence is playing
    bool IsAnyCutscenePlaying() const;

    /// Get the currently active sequence (first playing one)
    Sequence* GetActiveSequence();

    /// Console integration
    std::string Console_ListSequences() const;
    std::string Console_GetSequenceInfo(const std::string& name) const;
    void Console_PlaySequence(const std::string& name);
    void Console_StopSequence(const std::string& name);
    void Console_SetTime(const std::string& name, float time);

private:
    SequencerManager() = default;
    std::unordered_map<std::string, std::unique_ptr<Sequence>> m_sequences;
};

} // namespace Spark::Cinematic
