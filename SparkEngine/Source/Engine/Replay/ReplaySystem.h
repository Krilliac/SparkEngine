/**
 * @file ReplaySystem.h
 * @brief Replay recording and playback system for FPS games
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Records game state snapshots at configurable intervals for replay playback,
 * kill cams, and debugging. Supports both full-state recording (for spectator
 * replays) and input recording (for deterministic replay).
 *
 * ## Features
 * - Frame-by-frame game state recording
 * - Kill cam support with rewind and slow-motion
 * - Replay file save/load
 * - Playback speed control
 * - Camera freedom during playback (free cam, follow cam, first person)
 *
 * ## Usage
 * @code
 *   ReplaySystem replay;
 *   replay.SetRecordInterval(1.0f / 20.0f);  // 20 snapshots/second
 *   replay.StartRecording();
 *
 *   // Each frame:
 *   replay.RecordFrame(world, timestamp);
 *
 *   // Save replay:
 *   replay.StopRecording();
 *   replay.SaveToFile("replays/match_001.replay");
 *
 *   // Kill cam (last 5 seconds):
 *   replay.StartKillCam(5.0f, victimEntity);
 *
 *   // Full playback:
 *   replay.LoadFromFile("replays/match_001.replay");
 *   replay.StartPlayback();
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <mutex>

namespace Spark
{

    // =============================================================================
    // ReplayEntityState
    // =============================================================================

    /**
 * @brief Snapshot of a single entity's state at a point in time.
 */
    struct ReplayEntityState
    {
        uint32_t entityId = 0;
        DirectX::XMFLOAT3 position{0, 0, 0};
        DirectX::XMFLOAT4 rotation{0, 0, 0, 1};
        DirectX::XMFLOAT3 velocity{0, 0, 0};
        float health = 100.0f;
        int animationState = 0;
        uint32_t flags = 0; ///< Bitmask: alive, visible, firing, etc.
    };

    // =============================================================================
    // ReplayFrame
    // =============================================================================

    /**
 * @brief A single frame of replay data — snapshot of all entities.
 */
    struct ReplayFrame
    {
        float timestamp = 0.0f;                  ///< Game time in seconds
        uint32_t frameNumber = 0;                ///< Sequential frame number
        std::vector<ReplayEntityState> entities; ///< All entity states this frame
    };

    // =============================================================================
    // ReplayEvent
    // =============================================================================

    /**
 * @brief A discrete event recorded during gameplay (kill, explosion, etc.).
 */
    struct ReplayEvent
    {
        float timestamp = 0.0f;
        std::string type; ///< Event type ("kill", "explosion", "pickup", etc.)
        uint32_t sourceEntity = 0;
        uint32_t targetEntity = 0;
        DirectX::XMFLOAT3 position{0, 0, 0};
        std::string data; ///< Additional event-specific data
    };

    // =============================================================================
    // ReplayData
    // =============================================================================

    /**
 * @brief Complete replay recording — frames + events.
 */
    struct ReplayData
    {
        std::string mapName;             ///< Map the replay was recorded on
        std::string gameMode;            ///< Game mode name
        float duration = 0.0f;           ///< Total recording duration
        uint32_t version = 1;            ///< Replay format version
        std::vector<ReplayFrame> frames; ///< All recorded frames
        std::vector<ReplayEvent> events; ///< All recorded events
    };

    // =============================================================================
    // PlaybackCamera
    // =============================================================================

    /**
 * @brief Camera mode during replay playback.
 */
    enum class PlaybackCamera
    {
        FreeCam,     ///< Free-flying camera controlled by player
        FollowCam,   ///< Third-person camera following an entity
        FirstPerson, ///< First-person view of an entity
        KillCam      ///< Cinematic kill camera
    };

    // =============================================================================
    // PlaybackState
    // =============================================================================

    /**
 * @brief Current state of replay playback.
 */
    enum class PlaybackState
    {
        Stopped,
        Playing,
        Paused,
        Rewinding,
        FastForward
    };

    // =============================================================================
    // ReplaySystem
    // =============================================================================

    /**
 * @class ReplaySystem
 * @brief Records and plays back game state for replays and kill cams.
 */
    class ReplaySystem
    {
      public:
        ReplaySystem();
        ~ReplaySystem() = default;

        // --- Recording ---

        /**
     * @brief Set the interval between recorded frames.
     * @param seconds Time between snapshots (e.g. 0.05 = 20fps).
     */
        void SetRecordInterval(float seconds) { m_recordInterval = seconds; }

        /** @brief Start recording. */
        void StartRecording();

        /** @brief Stop recording. */
        void StopRecording();

        /** @brief Check if currently recording. */
        bool IsRecording() const { return m_isRecording; }

        /**
     * @brief Record a frame of entity states.
     * @param entities Vector of entity states to record.
     * @param timestamp Current game time.
     */
        void RecordFrame(const std::vector<ReplayEntityState>& entities, float timestamp);

        /**
     * @brief Record a discrete gameplay event.
     * @param event The event to record.
     */
        void RecordEvent(const ReplayEvent& event);

        /** @brief Set map and game mode metadata. */
        void SetMetadata(const std::string& mapName, const std::string& gameMode);

        // --- Playback ---

        /**
     * @brief Start playback from the beginning.
     */
        void StartPlayback();

        /** @brief Pause playback. */
        void PausePlayback();

        /** @brief Resume playback. */
        void ResumePlayback();

        /** @brief Stop playback. */
        void StopPlayback();

        /**
     * @brief Update playback (advance to next frame based on speed).
     * @param deltaTime Frame time.
     */
        void UpdatePlayback(float deltaTime);

        /**
     * @brief Seek to a specific time in the replay.
     * @param timestamp Time in seconds.
     */
        void SeekTo(float timestamp);

        /**
     * @brief Set playback speed multiplier.
     * @param speed 1.0 = normal, 0.25 = quarter speed, 2.0 = double speed.
     */
        void SetPlaybackSpeed(float speed) { m_playbackSpeed = speed; }

        /** @brief Get current playback speed. */
        float GetPlaybackSpeed() const { return m_playbackSpeed; }

        /** @brief Get current playback state. */
        PlaybackState GetPlaybackState() const { return m_playbackState; }

        /** @brief Get current playback time. */
        float GetPlaybackTime() const { return m_playbackTime; }

        /** @brief Get total replay duration. */
        float GetDuration() const { return m_replayData.duration; }

        /**
     * @brief Get the current frame's entity states.
     * @return Entity states for the current playback frame.
     */
        const ReplayFrame* GetCurrentFrame() const;

        /**
     * @brief Get events that occurred at or near the current playback time.
     * @param windowSeconds Time window around current time.
     * @return Events within the window.
     */
        std::vector<ReplayEvent> GetEventsNearTime(float windowSeconds = 0.1f) const;

        // --- Kill Cam ---

        /**
     * @brief Start a kill cam replay.
     * @param rewindSeconds How far back to rewind.
     * @param focusEntity   Entity to focus the camera on.
     */
        void StartKillCam(float rewindSeconds, uint32_t focusEntity);

        /**
     * @brief Stop kill cam and return to normal gameplay.
     */
        void StopKillCam();

        /** @brief Check if kill cam is active. */
        bool IsKillCamActive() const { return m_killCamActive; }

        // --- Camera ---

        /** @brief Set the playback camera mode. */
        void SetCamera(PlaybackCamera mode) { m_cameraMode = mode; }

        /** @brief Get the current camera mode. */
        PlaybackCamera GetCamera() const { return m_cameraMode; }

        /** @brief Set the entity to follow with FollowCam/FirstPerson. */
        void SetFollowEntity(uint32_t entityId) { m_followEntity = entityId; }

        // --- File I/O ---

        /**
     * @brief Save the replay to a file.
     * @param filePath Output file path.
     * @return true if saving succeeded.
     */
        bool SaveToFile(const std::string& filePath) const;

        /**
     * @brief Load a replay from a file.
     * @param filePath Input file path.
     * @return true if loading succeeded.
     */
        bool LoadFromFile(const std::string& filePath);

        // --- Console integration ---

        /** @brief Get replay system status (console integration). */
        std::string Console_GetStatus() const;

      private:
        size_t FindFrameIndex(float timestamp) const;

        ReplayData m_replayData;
        bool m_isRecording = false;
        float m_recordTimer = 0.0f;
        float m_recordInterval = 0.05f; // 20 fps default
        uint32_t m_frameCounter = 0;

        PlaybackState m_playbackState = PlaybackState::Stopped;
        float m_playbackTime = 0.0f;
        float m_playbackSpeed = 1.0f;
        size_t m_currentFrameIndex = 0;
        PlaybackCamera m_cameraMode = PlaybackCamera::FreeCam;
        uint32_t m_followEntity = 0;

        bool m_killCamActive = false;
        float m_killCamStartTime = 0.0f;
        uint32_t m_killCamFocusEntity = 0;

        mutable std::mutex m_mutex;
    };

} // namespace Spark
