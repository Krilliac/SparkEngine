/**
 * @file CinematicSequencerPanel.h
 * @brief Cinematic sequence editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Keyframe data for a single property at a point in time
     */
    struct CinematicKeyframe
    {
        float time = 0.0f;
        float value[4] = {};    ///< Up to 4 floats (scalar, vec2, vec3, vec4)
        int componentCount = 1; ///< How many components are used
    };

    /**
     * @brief A single track controlling one property of one actor
     */
    struct CinematicTrack
    {
        char name[64] = {};
        char actorName[64] = {};
        char propertyName[64] = {};
        std::vector<CinematicKeyframe> keyframes;
        bool muted = false;
        bool locked = false;
    };

    /**
     * @brief A complete cinematic sequence with tracks and metadata
     */
    struct CinematicSequence
    {
        char name[128] = "Untitled Sequence";
        float duration = 10.0f;
        float fps = 30.0f;
        std::vector<CinematicTrack> tracks;
    };

    /**
     * @brief Panel for authoring cinematic sequences (cutscenes, cameras, events)
     *
     * Provides a timeline scrubber, track editor, keyframe placement, and
     * playback preview. Sequences are saved as JSON for engine consumption.
     */
    class CinematicSequencerPanel : public EditorPanel
    {
      public:
        CinematicSequencerPanel();
        ~CinematicSequencerPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderToolbar();
        void RenderTimeline();
        void RenderTrackList();
        void RenderPropertyEditor();

        std::vector<CinematicSequence> m_sequences;
        int m_selectedSequence = -1;
        int m_selectedTrack = -1;
        int m_selectedKeyframe = -1;
        float m_playheadTime = 0.0f;
        float m_zoom = 1.0f;
        bool m_isPlaying = false;
        bool m_isLooping = false;
    };

} // namespace SparkEditor
