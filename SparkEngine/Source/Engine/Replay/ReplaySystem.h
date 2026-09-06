/**
 * @file ReplaySystem.h
 * @brief Replay recording and playback system for FPS games
 *
 * Captures entity state snapshots at configurable intervals, supporting
 * full match replays, kill cams, slow-motion, and multiple camera modes.
 * Uses a compact binary file format with safety-bounded deserialization.
 */

#pragma once

#include "Core/Platform.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Spark
{

    // ============================================================================
    // Enums
    // ============================================================================

    /// Camera modes available during replay playback
    enum class PlaybackCamera
    {
        FreeCam,
        FollowCam,
        FirstPerson,
        KillCam
    };

    /// Current state of replay playback
    enum class PlaybackState
    {
        Stopped,
        Playing,
        Paused,
        Rewinding,
        FastForward
    };

    // ============================================================================
    // Data Structures
    // ============================================================================

    /// Snapshot of a single entity's state at a point in time
    struct ReplayEntityState
    {
        uint32_t entityId = 0;
        XMFLOAT3 position{0, 0, 0};
        XMFLOAT4 rotation{0, 0, 0, 1};
        XMFLOAT3 velocity{0, 0, 0};
        float health = 100.0f;
        int animationState = 0;
        uint32_t flags = 0;
    };

    /// Entity flag bitmask constants
    namespace ReplayFlags
    {
        constexpr uint32_t Alive = 0x01;
        constexpr uint32_t Visible = 0x02;
        constexpr uint32_t Firing = 0x04;
        constexpr uint32_t Crouching = 0x08;
        constexpr uint32_t Sprinting = 0x10;
        constexpr uint32_t Reloading = 0x20;
        constexpr uint32_t InVehicle = 0x40;
        constexpr uint32_t Scoped = 0x80;
    } // namespace ReplayFlags

    /// A single frame of replay data
    struct ReplayFrame
    {
        float timestamp = 0.0f;
        uint32_t frameNumber = 0;
        std::vector<ReplayEntityState> entities;
    };

    /// A discrete event recorded during gameplay
    struct ReplayEvent
    {
        float timestamp = 0.0f;
        std::string type;
        uint32_t sourceEntity = 0;
        uint32_t targetEntity = 0;
        XMFLOAT3 position{0, 0, 0};
        std::string data;
    };

    /// Complete replay recording
    struct ReplayData
    {
        std::string mapName;
        std::string gameMode;
        float duration = 0.0f;
        uint32_t version = 1;
        std::vector<ReplayFrame> frames;
        std::vector<ReplayEvent> events;
    };

    // ============================================================================
    // Deserialization Safety Limits
    // ============================================================================

    constexpr uint32_t kReplayMagic = 0x52504C59; // "RPLY"
    constexpr uint32_t kMaxStringLength = 4096;
    constexpr uint32_t kMaxFrameCount = 1'000'000;
    constexpr uint32_t kMaxEntityCount = 100'000;
    constexpr uint32_t kMaxEventCount = 1'000'000;

    // ============================================================================
    // ReplaySystem
    // ============================================================================

    class ReplaySystem
    {
      public:
        static ReplaySystem& GetInstance();

        ReplaySystem();
        ~ReplaySystem() = default;

        // --- Recording ---
        void SetRecordInterval(float seconds);
        void StartRecording();
        void StopRecording();
        bool IsRecording() const;
        void RecordFrame(const std::vector<ReplayEntityState>& entities, float timestamp);

        /**
         * @brief Record a frame `deltaSeconds` after the previous one.
         *
         * The per-tick producer's entry point. The capture clock lives here and
         * StartRecording resets it, so no caller keeps a second one: the engine
         * lifecycle used to accumulate its own `EngineRuntime::replayCaptureTime`
         * and re-derive "a new recording just started" from
         * `GetFrameCount() == 0`, a second clock that could disagree with this
         * system's own notion of when recording began.
         */
        void RecordFrameTick(const std::vector<ReplayEntityState>& entities, float deltaSeconds);

        void RecordEvent(const ReplayEvent& event);
        void SetMetadata(const std::string& mapName, const std::string& gameMode);

        // --- Playback ---
        void StartPlayback();
        void PausePlayback();
        void ResumePlayback();
        void StopPlayback();
        void UpdatePlayback(float deltaTime);
        void SeekTo(float timestamp);
        void SetPlaybackSpeed(float speed);
        float GetPlaybackSpeed() const;
        PlaybackState GetPlaybackState() const;
        float GetPlaybackTime() const;
        float GetDuration() const;

        /**
         * @brief Number of frames currently held by the system.
         *
         * Reports what recording actually captured (or what LoadFromFile read), so a
         * UI can distinguish "recording armed" from "frames captured".
         */
        size_t GetFrameCount() const;

        const ReplayFrame* GetCurrentFrame() const;
        std::vector<ReplayEvent> GetEventsNearTime(float windowSeconds = 0.1f) const;

        // --- Kill Cam ---
        void StartKillCam(float rewindSeconds, uint32_t focusEntity);
        void StopKillCam();
        bool IsKillCamActive() const;

        // --- Camera ---
        void SetCamera(PlaybackCamera mode);
        PlaybackCamera GetCamera() const;
        void SetFollowEntity(uint32_t entityId);

        // --- File I/O ---
        bool SaveToFile(const std::string& filePath) const;
        bool LoadFromFile(const std::string& filePath);

        // --- Console ---
        std::string Console_GetStatus() const;

      private:
        /// Find the frame index nearest to the given timestamp via binary search
        size_t FindFrameIndex(float timestamp) const;

        mutable std::mutex m_mutex;
        ReplayData m_data;

        // Recording state
        bool m_recording = false;
        float m_recordInterval = 0.05f; // 20 fps default
        float m_lastRecordTime = -1.0f; ///< Timestamp of the last stored frame; negative until the first.
        float m_captureTime = 0.0f;     ///< RecordFrameTick's clock: seconds of gameplay since StartRecording.
        uint32_t m_frameCounter = 0;

        // Playback state
        PlaybackState m_playbackState = PlaybackState::Stopped;
        float m_playbackTime = 0.0f;
        float m_playbackSpeed = 1.0f;
        size_t m_currentFrameIndex = 0;

        // Camera state
        PlaybackCamera m_camera = PlaybackCamera::FreeCam;
        uint32_t m_followEntity = 0;

        // Kill cam state
        bool m_killCamActive = false;
    };

} // namespace Spark
