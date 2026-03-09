/**
 * @file AnimationTimeline.h
 * @brief Professional animation system and timeline editor for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive animation system with keyframe editing,
 * curve manipulation, and timeline-based animation creation similar to
 * Maya, 3ds Max, and Blender animation systems.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <cfloat>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>


namespace SparkEditor
{

    /**
 * @brief Animation keyframe
 */
    struct AnimationKeyframe
    {
        float time;                   ///< Keyframe time in seconds
        XMFLOAT4 value;               ///< Keyframe value (up to 4 components)
        XMFLOAT2 inTangent = {0, 0};  ///< In tangent for curve interpolation
        XMFLOAT2 outTangent = {0, 0}; ///< Out tangent for curve interpolation

        enum InterpolationType
        {
            LINEAR = 0,
            BEZIER = 1,
            STEP = 2,
            EASE_IN = 3,
            EASE_OUT = 4,
            EASE_IN_OUT = 5,
            CUSTOM = 6
        } interpolation = LINEAR; ///< Interpolation type

        bool isSelected = false; ///< Whether keyframe is selected
        bool isLocked = false;   ///< Whether keyframe is locked
        std::string note;        ///< Optional note/comment
    };

    /**
 * @brief Animation curve (track)
 */
    struct AnimationCurve
    {
        std::string propertyPath;                 ///< Property path (e.g., "transform.position.x")
        std::string displayName;                  ///< Display name for UI
        XMFLOAT4 color = {1, 1, 1, 1};            ///< Curve display color
        std::vector<AnimationKeyframe> keyframes; ///< Keyframes in this curve
        bool isVisible = true;                    ///< Whether curve is visible
        bool isMuted = false;                     ///< Whether curve is muted
        bool isLocked = false;                    ///< Whether curve is locked

        // Value range for visualization
        float minValue = -FLT_MAX; ///< Minimum value for display
        float maxValue = FLT_MAX;  ///< Maximum value for display
        bool autoFitRange = true;  ///< Auto-fit value range

        /**
     * @brief Evaluate curve at given time
     * @param time Time to evaluate
     * @return Interpolated value
     */
        XMFLOAT4 Evaluate(float time) const;

        /**
     * @brief Add keyframe to curve
     * @param keyframe Keyframe to add
     */
        void AddKeyframe(const AnimationKeyframe& keyframe);

        /**
     * @brief Remove keyframe at index
     * @param index Keyframe index to remove
     */
        void RemoveKeyframe(size_t index);

        /**
     * @brief Find keyframe at time
     * @param time Time to search
     * @param tolerance Time tolerance
     * @return Index of keyframe, or -1 if not found
     */
        int FindKeyframe(float time, float tolerance = 0.01f) const;
    };

    /**
 * @brief Animation track for an object/component
 */
    struct AnimationTrack
    {
        ObjectID objectID;                                      ///< Target object ID
        std::string objectName;                                 ///< Object display name
        ComponentType componentType = ComponentType::TRANSFORM; ///< Target component type
        std::string componentName;                              ///< Component display name
        std::vector<std::unique_ptr<AnimationCurve>> curves;    ///< Animation curves

        bool isExpanded = true; ///< Whether track is expanded in UI
        bool isVisible = true;  ///< Whether track is visible
        bool isMuted = false;   ///< Whether track is muted
        bool isLocked = false;  ///< Whether track is locked
        bool isSolo = false;    ///< Whether track is solo

        XMFLOAT4 trackColor = {0.5f, 0.5f, 0.5f, 1.0f}; ///< Track header color

        /**
     * @brief Add curve to track
     * @param propertyPath Property path for the curve
     * @param displayName Display name for the curve
     * @return Pointer to created curve
     */
        AnimationCurve* AddCurve(const std::string& propertyPath, const std::string& displayName);

        /**
     * @brief Remove curve from track
     * @param propertyPath Property path of curve to remove
     */
        void RemoveCurve(const std::string& propertyPath);

        /**
     * @brief Find curve by property path
     * @param propertyPath Property path to search
     * @return Pointer to curve, or nullptr if not found
     */
        AnimationCurve* FindCurve(const std::string& propertyPath);
    };

    /**
 * @brief Animation clip
 */
    struct AnimationClip
    {
        std::string name = "New Animation"; ///< Animation clip name
        std::string description;            ///< Animation description
        float duration = 5.0f;              ///< Animation duration in seconds
        float frameRate = 30.0f;            ///< Animation frame rate
        bool isLooping = false;             ///< Whether animation loops

        std::vector<std::unique_ptr<AnimationTrack>> tracks; ///< Animation tracks

        // Playback state
        float currentTime = 0.0f; ///< Current playback time
        bool isPlaying = false;   ///< Whether animation is playing
        bool isPaused = false;    ///< Whether animation is paused

        // Timeline markers
        std::vector<std::pair<float, std::string>> markers; ///< Timeline markers

        /**
     * @brief Add track for object
     * @param objectID Target object ID
     * @param objectName Object display name
     * @return Pointer to created track
     */
        AnimationTrack* AddTrack(ObjectID objectID, const std::string& objectName);

        /**
     * @brief Remove track
     * @param objectID Object ID of track to remove
     */
        void RemoveTrack(ObjectID objectID);

        /**
     * @brief Find track by object ID
     * @param objectID Object ID to search
     * @return Pointer to track, or nullptr if not found
     */
        AnimationTrack* FindTrack(ObjectID objectID);

        /**
     * @brief Evaluate animation at current time
     * @param outValues Output map of property paths to values
     */
        void Evaluate(std::unordered_map<std::string, XMFLOAT4>& outValues) const;

        /**
     * @brief Set animation time
     * @param time New animation time
     */
        void SetTime(float time);

        /**
     * @brief Get total frame count
     * @return Number of frames in animation
     */
        int GetFrameCount() const { return static_cast<int>(duration * frameRate); }

        /**
     * @brief Convert time to frame number
     * @param time Time in seconds
     * @return Frame number
     */
        int TimeToFrame(float time) const { return static_cast<int>(time * frameRate); }

        /**
     * @brief Convert frame number to time
     * @param frame Frame number
     * @return Time in seconds
     */
        float FrameToTime(int frame) const { return frame / frameRate; }
    };

    /**
 * @brief Animation playback state
 */
    enum class PlaybackState
    {
        STOPPED = 0,
        PLAYING = 1,
        PAUSED = 2,
        RECORDING = 3
    };

    /**
 * @brief Timeline selection
 */
    struct TimelineSelection
    {
        std::vector<AnimationKeyframe*> selectedKeyframes; ///< Selected keyframes
        std::vector<AnimationCurve*> selectedCurves;       ///< Selected curves
        std::vector<AnimationTrack*> selectedTracks;       ///< Selected tracks
        float timeRangeStart = 0.0f;                       ///< Selection time range start
        float timeRangeEnd = 0.0f;                         ///< Selection time range end
        bool hasTimeRange = false;                         ///< Whether time range is selected

        /**
     * @brief Clear all selection
     */
        void Clear()
        {
            selectedKeyframes.clear();
            selectedCurves.clear();
            selectedTracks.clear();
            hasTimeRange = false;
        }

        /**
     * @brief Check if anything is selected
     * @return true if any items are selected
     */
        bool HasSelection() const
        {
            return !selectedKeyframes.empty() || !selectedCurves.empty() || !selectedTracks.empty() || hasTimeRange;
        }
    };

    /**
 * @brief Professional animation system and timeline editor
 * 
 * Provides comprehensive animation tools including:
 * - Multi-track keyframe animation
 * - Bezier curve interpolation
 * - Real-time animation preview
 * - Animation recording
 * - Curve editor with tangent manipulation
 * - Timeline scrubbing and playback
 * - Animation blending and layering
 * - Performance optimization
 * - Import/export functionality
 * 
 * Inspired by Maya's Graph Editor, 3ds Max's Curve Editor, and Blender's Dope Sheet.
 */
    class AnimationTimeline : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        AnimationTimeline();

        /**
     * @brief Destructor
     */
        ~AnimationTimeline() override;

        /**
     * @brief Initialize the animation timeline
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update animation timeline
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render animation timeline UI
     */
        void Render() override;

        /**
     * @brief Shutdown the animation timeline
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Create new animation clip
     * @param name Animation clip name
     * @param duration Animation duration in seconds
     * @param frameRate Animation frame rate
     */
        void CreateNewClip(const std::string& name = "New Animation", float duration = 5.0f, float frameRate = 30.0f);

        /**
     * @brief Load animation clip from file
     * @param filePath Path to animation file
     * @return true if animation was loaded successfully
     */
        bool LoadAnimationClip(const std::string& filePath);

        /**
     * @brief Save current animation clip to file
     * @param filePath Path to save animation
     * @return true if animation was saved successfully
     */
        bool SaveAnimationClip(const std::string& filePath);

        /**
     * @brief Set current animation clip
     * @param clip Animation clip to set as current
     */
        void SetCurrentClip(std::unique_ptr<AnimationClip> clip);

        /**
     * @brief Get current animation clip
     * @return Pointer to current clip, or nullptr if none
     */
        AnimationClip* GetCurrentClip() const { return m_currentClip.get(); }

        /**
     * @brief Start animation playback
     */
        void Play();

        /**
     * @brief Pause animation playback
     */
        void Pause();

        /**
     * @brief Stop animation playback
     */
        void Stop();

        /**
     * @brief Step to next frame
     */
        void StepForward();

        /**
     * @brief Step to previous frame
     */
        void StepBackward();

        /**
     * @brief Go to first frame
     */
        void GoToStart();

        /**
     * @brief Go to last frame
     */
        void GoToEnd();

        /**
     * @brief Set playback time
     * @param time New playback time in seconds
     */
        void SetPlaybackTime(float time);

        /**
     * @brief Get current playback time
     * @return Current playback time in seconds
     */
        float GetPlaybackTime() const;

        /**
     * @brief Get current playback state
     * @return Current playback state
     */
        PlaybackState GetPlaybackState() const { return m_playbackState; }

        /**
     * @brief Enable/disable animation recording
     * @param recording Whether to enable recording
     */
        void SetRecording(bool recording);

        /**
     * @brief Check if animation recording is enabled
     * @return true if recording is enabled
     */
        bool IsRecording() const { return m_playbackState == PlaybackState::RECORDING; }

        /**
     * @brief Add keyframe for object property
     * @param objectID Target object ID
     * @param propertyPath Property path (e.g., "transform.position.x")
     * @param value Property value
     * @param time Keyframe time (use current time if negative)
     */
        void AddKeyframe(ObjectID objectID, const std::string& propertyPath, const XMFLOAT4& value, float time = -1.0f);

        /**
     * @brief Remove selected keyframes
     */
        void RemoveSelectedKeyframes();

        /**
     * @brief Set keyframe interpolation type
     * @param interpolationType New interpolation type
     */
        void SetKeyframeInterpolation(AnimationKeyframe::InterpolationType interpolationType);

        /**
     * @brief Frame selected keyframes in view
     */
        void FrameSelected();

        /**
     * @brief Frame all keyframes in view
     */
        void FrameAll();

        /**
     * @brief Get timeline selection
     * @return Reference to current selection
     */
        const TimelineSelection& GetSelection() const { return m_selection; }

        /**
     * @brief Set timeline view range
     * @param startTime Start time in seconds
     * @param endTime End time in seconds
     */
        void SetViewRange(float startTime, float endTime);

        /**
     * @brief Auto-fit view to animation duration
     */
        void AutoFitView();

        /**
     * @brief Set timeline zoom
     * @param zoom Zoom factor (1.0 = normal)
     */
        void SetTimelineZoom(float zoom);

        /**
     * @brief Get timeline zoom
     * @return Current zoom factor
     */
        float GetTimelineZoom() const { return m_timelineZoom; }

      private:
        /**
     * @brief Render timeline header
     */
        void RenderTimelineHeader();

        /**
     * @brief Render track list
     */
        void RenderTrackList();

        /**
     * @brief Render timeline editor
     */
        void RenderTimelineEditor();

        /**
     * @brief Render curve editor
     */
        void RenderCurveEditor();

        /**
     * @brief Render playback controls
     */
        void RenderPlaybackControls();

        /**
     * @brief Render animation properties
     */
        void RenderAnimationProperties();

        /**
     * @brief Render a single track
     * @param track Track to render
     * @param trackIndex Track index
     */
        void RenderTrack(AnimationTrack* track, int trackIndex);

        /**
     * @brief Render keyframes for a curve
     * @param curve Curve containing keyframes
     * @param trackRect Track rectangle area
     */
        void RenderKeyframes(AnimationCurve* curve, const XMFLOAT4& trackRect);

        /**
     * @brief Render curve in curve editor
     * @param curve Curve to render
     */
        void RenderCurve(AnimationCurve* curve);

        /**
     * @brief Render time ruler
     * @param timelineRect Timeline rectangle area
     */
        void RenderTimeRuler(const XMFLOAT4& timelineRect);

        /**
     * @brief Render playhead
     * @param timelineRect Timeline rectangle area
     */
        void RenderPlayhead(const XMFLOAT4& timelineRect);

        /**
     * @brief Handle timeline mouse input
     * @param timelineRect Timeline rectangle area
     */
        void HandleTimelineInput(const XMFLOAT4& timelineRect);

        /**
     * @brief Handle curve editor mouse input
     */
        void HandleCurveEditorInput();

        /**
     * @brief Handle keyframe selection
     * @param mousePos Mouse position
     * @param isAdditive Whether to add to current selection
     */
        void HandleKeyframeSelection(const XMFLOAT2& mousePos, bool isAdditive);

        /**
     * @brief Handle keyframe dragging
     */
        void HandleKeyframeDragging();

        /**
     * @brief Convert time to screen X coordinate
     * @param time Time in seconds
     * @param timelineRect Timeline rectangle
     * @return Screen X coordinate
     */
        float TimeToScreen(float time, const XMFLOAT4& timelineRect) const;

        /**
     * @brief Convert screen X coordinate to time
     * @param screenX Screen X coordinate
     * @param timelineRect Timeline rectangle
     * @return Time in seconds
     */
        float ScreenToTime(float screenX, const XMFLOAT4& timelineRect) const;

        /**
     * @brief Convert value to screen Y coordinate (curve editor)
     * @param value Value
     * @param curveRect Curve editor rectangle
     * @return Screen Y coordinate
     */
        float ValueToScreen(float value, const XMFLOAT4& curveRect) const;

        /**
     * @brief Convert screen Y coordinate to value (curve editor)
     * @param screenY Screen Y coordinate
     * @param curveRect Curve editor rectangle
     * @return Value
     */
        float ScreenToValue(float screenY, const XMFLOAT4& curveRect) const;

        /**
     * @brief Find keyframe at screen position
     * @param screenPos Screen position
     * @param tolerance Hit test tolerance
     * @return Pointer to keyframe, or nullptr if none found
     */
        AnimationKeyframe* FindKeyframeAtPosition(const XMFLOAT2& screenPos, float tolerance = 5.0f);

        /**
     * @brief Update animation preview
     */
        void UpdateAnimationPreview();

        /**
     * @brief Apply animation to scene objects
     */
        void ApplyAnimationToScene();

        /**
     * @brief Record current object states as keyframes
     */
        void RecordKeyframes();

        /**
     * @brief Snap time to frame boundary
     * @param time Input time
     * @return Snapped time
     */
        float SnapToFrame(float time) const;

        /**
     * @brief Calculate curve tangents automatically
     * @param curve Curve to calculate tangents for
     */
        void CalculateAutoTangents(AnimationCurve* curve);

      private:
        // Current animation clip
        std::unique_ptr<AnimationClip> m_currentClip; ///< Current animation clip

        // Playback state
        PlaybackState m_playbackState = PlaybackState::STOPPED; ///< Current playback state
        float m_playbackSpeed = 1.0f;                           ///< Playback speed multiplier
        bool m_loopPlayback = false;                            ///< Whether to loop playback

        // Timeline view
        float m_viewStartTime = 0.0f;    ///< Timeline view start time
        float m_viewEndTime = 5.0f;      ///< Timeline view end time
        float m_timelineZoom = 1.0f;     ///< Timeline zoom factor
        float m_trackHeight = 40.0f;     ///< Height of each track
        float m_trackListWidth = 200.0f; ///< Width of track list

        // Curve editor view
        float m_curveViewMinValue = -10.0f; ///< Curve editor minimum value
        float m_curveViewMaxValue = 10.0f;  ///< Curve editor maximum value
        bool m_showCurveEditor = false;     ///< Whether curve editor is visible

        // Selection and interaction
        TimelineSelection m_selection;      ///< Current selection
        bool m_isDraggingKeyframes = false; ///< Currently dragging keyframes
        bool m_isDraggingPlayhead = false;  ///< Currently dragging playhead
        bool m_isDraggingTimeRange = false; ///< Currently dragging time range
        XMFLOAT2 m_dragStartPos = {0, 0};   ///< Drag start position
        XMFLOAT2 m_dragOffset = {0, 0};     ///< Drag offset

        // Snapping
        bool m_snapToFrames = true;     ///< Snap to frame boundaries
        bool m_snapToKeyframes = false; ///< Snap to existing keyframes
        bool m_snapToMarkers = false;   ///< Snap to timeline markers

        // Display options
        bool m_showFrameNumbers = true; ///< Show frame numbers
        bool m_showValueLabels = true;  ///< Show value labels on keyframes
        bool m_showCurveHandles = true; ///< Show curve tangent handles
        bool m_autoFitCurves = true;    ///< Auto-fit curve display range

        // Colors
        XMFLOAT4 m_timelineBackgroundColor = {0.15f, 0.15f, 0.15f, 1.0f}; ///< Timeline background
        XMFLOAT4 m_trackBackgroundColor = {0.2f, 0.2f, 0.2f, 1.0f};       ///< Track background
        XMFLOAT4 m_playheadColor = {1.0f, 0.3f, 0.3f, 1.0f};              ///< Playhead color
        XMFLOAT4 m_keyframeColor = {1.0f, 1.0f, 1.0f, 1.0f};              ///< Keyframe color
        XMFLOAT4 m_selectedKeyframeColor = {1.0f, 1.0f, 0.0f, 1.0f};      ///< Selected keyframe color
        XMFLOAT4 m_curveColor = {0.7f, 0.7f, 1.0f, 1.0f};                 ///< Curve color

        // Performance
        bool m_usePreviewQuality = false; ///< Use preview quality during playback
        int m_maxPreviewFrameRate = 60;   ///< Maximum preview frame rate
    };

} // namespace SparkEditor