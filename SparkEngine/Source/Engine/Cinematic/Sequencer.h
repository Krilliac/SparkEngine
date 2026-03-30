/**
 * @file Sequencer.h
 * @brief Timeline-based cinematic sequencer for cutscenes and scripted events
 *
 * Provides camera paths, entity transforms, property animation, audio cues,
 * event triggers, subtitles, and screen fades — all driven by a timeline.
 */

#pragma once

#include "Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Cinematic
{

    // ============================================================================
    // Enums
    // ============================================================================

    enum class TrackType
    {
        CameraPath,
        EntityProperty,
        EntityTransform,
        AudioCue,
        Event,
        Subtitle,
        Fade
    };

    enum class InterpolationMode
    {
        Step,
        Linear,
        CubicBezier,
        CatmullRom
    };

    enum class SequencePlayState
    {
        Stopped,
        Playing,
        Paused
    };

    // ============================================================================
    // Keyframe / Cue Structures
    // ============================================================================

    struct CameraKeyframe
    {
        float time = 0.0f;
        XMFLOAT3 position{0, 0, 0};
        XMFLOAT3 lookAt{0, 0, 0};
        float fov = 60.0f;
        float roll = 0.0f;
        InterpolationMode interpolation = InterpolationMode::CatmullRom;
    };

    struct VectorKeyframe
    {
        float time = 0.0f;
        XMFLOAT3 value{0, 0, 0};
        InterpolationMode interpolation = InterpolationMode::Linear;
    };

    struct PropertyKeyframe
    {
        float time = 0.0f;
        float value = 0.0f;
        InterpolationMode interpolation = InterpolationMode::Linear;
    };

    struct FadeKeyframe
    {
        float time = 0.0f;
        float opacity = 0.0f;
        XMFLOAT3 color{0, 0, 0};
        InterpolationMode interpolation = InterpolationMode::Linear;
    };

    struct AudioCue
    {
        float time = 0.0f;
        std::string soundName;
        float volume = 1.0f;
        bool is3D = false;
        XMFLOAT3 position{0, 0, 0};
    };

    struct EventCue
    {
        float time = 0.0f;
        std::string eventName;
        std::string parameters;
    };

    struct SubtitleCue
    {
        float startTime = 0.0f;
        float endTime = 0.0f;
        std::string text;
        std::string speaker;
        XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
    };

    // ============================================================================
    // Base Track
    // ============================================================================

    class SequencerTrack
    {
      public:
        virtual ~SequencerTrack() = default;
        virtual TrackType GetType() const = 0;
        virtual float GetDuration() const = 0;

        bool enabled = true;
        std::string name;
    };

    // ============================================================================
    // Track Implementations
    // ============================================================================

    class CameraPathTrack : public SequencerTrack
    {
      public:
        TrackType GetType() const override { return TrackType::CameraPath; }
        float GetDuration() const override;

        void AddKeyframe(const CameraKeyframe& kf);
        void RemoveKeyframe(int index);
        CameraKeyframe Evaluate(float time) const;
        const std::vector<CameraKeyframe>& GetKeyframes() const { return m_keyframes; }

      private:
        std::vector<CameraKeyframe> m_keyframes;
    };

    class EntityTransformTrack : public SequencerTrack
    {
      public:
        uint32_t targetEntityID = 0;

        TrackType GetType() const override { return TrackType::EntityTransform; }
        float GetDuration() const override;

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
    };

    class EntityPropertyTrack : public SequencerTrack
    {
      public:
        uint32_t targetEntityID = 0;
        std::string propertyName;

        TrackType GetType() const override { return TrackType::EntityProperty; }
        float GetDuration() const override;

        void AddKeyframe(const PropertyKeyframe& kf);
        float Evaluate(float time) const;
        const std::vector<PropertyKeyframe>& GetKeyframes() const { return m_keyframes; }

      private:
        std::vector<PropertyKeyframe> m_keyframes;
    };

    class AudioCueTrack : public SequencerTrack
    {
      public:
        TrackType GetType() const override { return TrackType::AudioCue; }
        float GetDuration() const override;

        void AddCue(const AudioCue& cue);
        const std::vector<AudioCue>& GetCues() const { return m_cues; }
        std::vector<const AudioCue*> GetTriggeredCues(float prevTime, float currentTime) const;

      private:
        std::vector<AudioCue> m_cues;
    };

    class EventTrack : public SequencerTrack
    {
      public:
        TrackType GetType() const override { return TrackType::Event; }
        float GetDuration() const override;

        void AddCue(const EventCue& cue);
        const std::vector<EventCue>& GetCues() const { return m_cues; }
        std::vector<const EventCue*> GetTriggeredCues(float prevTime, float currentTime) const;

      private:
        std::vector<EventCue> m_cues;
    };

    class SubtitleTrack : public SequencerTrack
    {
      public:
        TrackType GetType() const override { return TrackType::Subtitle; }
        float GetDuration() const override;

        void AddSubtitle(const SubtitleCue& cue);
        const SubtitleCue* GetActiveSubtitle(float time) const;
        const std::vector<SubtitleCue>& GetSubtitles() const { return m_subtitles; }

      private:
        std::vector<SubtitleCue> m_subtitles;
    };

    class FadeTrack : public SequencerTrack
    {
      public:
        TrackType GetType() const override { return TrackType::Fade; }
        float GetDuration() const override;

        void AddKeyframe(const FadeKeyframe& kf);
        FadeKeyframe Evaluate(float time) const;

      private:
        std::vector<FadeKeyframe> m_keyframes;
    };

    // ============================================================================
    // Sequence
    // ============================================================================

    using EventCallback = std::function<void(const std::string& eventName, const std::string& params)>;

    class Sequence
    {
      public:
        explicit Sequence(const std::string& name);

        // Track management — returns raw pointer for immediate use (Sequence owns the track)
        CameraPathTrack* AddCameraTrack(const std::string& trackName);
        EntityTransformTrack* AddEntityTransformTrack(const std::string& trackName, uint32_t entityID);
        EntityPropertyTrack* AddEntityPropertyTrack(const std::string& trackName, uint32_t entityID,
                                                    const std::string& property);
        AudioCueTrack* AddAudioCueTrack(const std::string& trackName);
        EventTrack* AddEventTrack(const std::string& trackName);
        SubtitleTrack* AddSubtitleTrack(const std::string& trackName);
        FadeTrack* AddFadeTrack(const std::string& trackName);

        // Playback control
        void Play();
        void Pause();
        void Stop();
        void SetTime(float time);
        void SetPlaybackSpeed(float speed);
        void SetLooping(bool loop);

        // Per-frame update
        void Update(float deltaTime);

        // Event callback
        void SetEventCallback(EventCallback callback);

        // State queries
        const std::string& GetName() const { return m_name; }
        float GetCurrentPlaybackTime() const { return m_currentTime; }
        float GetDuration() const;
        SequencePlayState GetPlayState() const { return m_playState; }
        float GetPlaybackSpeed() const { return m_playbackSpeed; }
        bool IsLooping() const { return m_looping; }

        // Current evaluated state (for rendering)
        const CameraKeyframe* GetCurrentCameraState() const;
        const SubtitleCue* GetCurrentSubtitle() const;
        FadeKeyframe GetCurrentFade() const;

        // Track access
        const std::vector<std::unique_ptr<SequencerTrack>>& GetTracks() const { return m_tracks; }

      private:
        std::string m_name;
        std::vector<std::unique_ptr<SequencerTrack>> m_tracks;
        EventCallback m_eventCallback;

        SequencePlayState m_playState = SequencePlayState::Stopped;
        float m_currentTime = 0.0f;
        float m_previousTime = 0.0f;
        float m_playbackSpeed = 1.0f;
        bool m_looping = false;

        // Cached state from last Update()
        mutable CameraKeyframe m_cachedCamera{};
        mutable bool m_hasCameraState = false;
    };

    // ============================================================================
    // SequencerManager (singleton)
    // ============================================================================

    class SequencerManager
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<SequencerManager>() instead")]]
        static SequencerManager& GetInstance();

        Sequence* CreateSequence(const std::string& name);
        Sequence* GetSequence(const std::string& name);
        void RemoveSequence(const std::string& name);

        bool PlaySequence(const std::string& name);
        void StopSequence(const std::string& name);
        void PauseSequence(const std::string& name);
        void StopAll();

        void Update(float deltaTime);
        bool IsAnyCutscenePlaying() const;
        Sequence* GetActiveSequence();

        // Console integration
        std::string Console_ListSequences() const;
        std::string Console_GetSequenceInfo(const std::string& name) const;

      private:
        SequencerManager() = default;
        std::unordered_map<std::string, std::unique_ptr<Sequence>> m_sequences;
    };

} // namespace Spark::Cinematic
