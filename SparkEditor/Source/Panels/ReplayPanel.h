/**
 * @file ReplayPanel.h
 * @brief Replay system editor panel for recording and playback management
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Spark
{
    class ReplaySystem;
}

namespace SparkEditor
{

    /**
     * @brief Panel for managing replay recording, playback, and export
     *
     * Transport, recording and file actions all run against the engine
     * Spark::ReplaySystem; the panel keeps no private playback state.
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

        /// @brief Start engine recording.
        void StartRecording();
        /// @brief Stop engine recording.
        void StopRecording();
        /// @brief Whether the engine ReplaySystem is recording.
        bool IsRecording() const;

        /// @brief Number of frames the engine ReplaySystem currently holds.
        size_t GetCapturedFrameCount() const;
        /// @brief Duration of the loaded/recorded replay in seconds.
        float GetDuration() const;

        /// @brief Load a replay file through the engine ReplaySystem.
        bool LoadReplayFile(const std::string& filePath);

        /// @brief Rescan @p directory for `*.replay` files; returns the number found.
        size_t RefreshReplayFiles(const std::string& directory);

        /**
         * @brief Whether this panel advances playback itself.
         *
         * True in the standalone editor, where no engine lifecycle calls
         * UpdatePlayback(). False once an EngineContext exists, because the
         * gameplay lifecycle already advances the same system.
         */
        bool IsDrivingPlayback() const;

      private:
        struct ReplayFileInfo
        {
            std::string path;
            std::string name;
            uintmax_t fileSizeBytes = 0;
        };

        Spark::ReplaySystem& System() const;

        void RenderTransportControls();
        void RenderRecordingControls();
        void RenderReplayBrowser();

        std::vector<ReplayFileInfo> m_replayFiles;
        std::string m_replayDirectory = "Replays";
        int m_selectedReplay = -1;
        /// ImGui scrubber scratch; re-synced from the engine playback time each Update().
        float m_scrubPosition = 0.0f;
        float m_playbackSpeed = 1.0f;
    };

} // namespace SparkEditor
