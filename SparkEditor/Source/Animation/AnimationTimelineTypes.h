/**
 * @file AnimationTimelineTypes.h
 * @brief Type definitions for the animation timeline system
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all enums, structs, and data types used by AnimationTimeline:
 * AnimationKeyframe, AnimationCurve, AnimationTrack, AnimationClip,
 * PlaybackState, and TimelineSelection.
 */

#pragma once

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

} // namespace SparkEditor
