/**
 * @file ReplayPanel.h
 * @brief Replay system editor panel for recording and playback management
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for managing replay recording, playback, and export
     *
     * Provides transport controls (play/pause/seek), recording toggle,
     * replay file management, and playback speed adjustment.
     */
    class ReplayPanel : public EditorPanel
    {
      public:
        ReplayPanel();
        ~ReplayPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct ReplayFileInfo
        {
            char name[128] = {};
            char sceneName[64] = {};
            float duration = 0.0f;
            int frameCount = 0;
            int fileSizeKB = 0;
        };

        void RenderTransportControls();
        void RenderRecordingControls();
        void RenderReplayBrowser();

        std::vector<ReplayFileInfo> m_replayFiles;
        int m_selectedReplay = -1;
        float m_playbackPosition = 0.0f;
        float m_playbackSpeed = 1.0f;
        float m_replayDuration = 0.0f;
        bool m_isRecording = false;
        bool m_isPlaying = false;
        bool m_isPaused = false;
    };

} // namespace SparkEditor
