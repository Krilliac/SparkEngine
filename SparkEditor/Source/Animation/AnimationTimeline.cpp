/**
 * @file AnimationTimeline.cpp
 * @brief AnimationTimeline construction, initialization, and shutdown
 *
 * Implementation is split across multiple files:
 * - AnimationCurve.cpp         — AnimationCurve class methods
 * - AnimationClipManager.cpp   — AnimationTrack, AnimationClip, and file I/O
 * - AnimationPlayback.cpp      — Playback, recording, keyframe editing, coordinate mapping
 * - AnimationTimelineUI.cpp    — ImGui rendering and input handling
 */

#include "AnimationTimeline.h"
#include "Utils/Validate.h"

using namespace DirectX;
namespace SparkEditor
{

    AnimationTimeline::AnimationTimeline() : EditorPanel("Animation Timeline", "animation_timeline") {}

    AnimationTimeline::~AnimationTimeline() {}

    bool AnimationTimeline::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AnimationTimeline initializing");
        CreateNewClip("Default Animation", 5.0f, 30.0f);
        m_isInitialized = true;
        return true;
    }

    void AnimationTimeline::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AnimationTimeline shutting down");
        m_currentClip.reset();
        m_selection.Clear();
        m_playbackState = PlaybackState::STOPPED;
    }

} // namespace SparkEditor
