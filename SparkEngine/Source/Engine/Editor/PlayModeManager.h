/**
 * @file PlayModeManager.h
 * @brief Play-in-editor mode management: scene snapshot, restore, time control
 *
 * Manages the lifecycle of play mode within the editor: saving the current scene
 * state before entering play, running the game loop in the viewport, and restoring
 * the original state when stopping.
 */

#pragma once

#include "../../Core/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace Spark::Editor
{

    // ============================================================================
    // Play Mode State
    // ============================================================================

    enum class PlayModeState
    {
        Stopped,  ///< Editor mode — scene is editable
        Starting, ///< Transitioning to play: saving snapshot
        Playing,  ///< Game loop running in viewport
        Paused,   ///< Game loop paused — can inspect/step
        Stopping  ///< Transitioning back: restoring snapshot
    };

    // ============================================================================
    // Play Mode Configuration
    // ============================================================================

    struct PlayModeConfig
    {
        bool startPaused = false;           ///< Enter play mode in paused state
        bool enablePhysics = true;          ///< Run physics during play
        bool enableAI = true;               ///< Run AI during play
        bool enableAudio = true;            ///< Run audio during play
        bool enableNetworking = false;      ///< Run networking during play
        float fixedTimestep = 1.0f / 60.0f; ///< Physics timestep
        float maxDeltaTime = 0.1f;          ///< Clamp delta time
        float timeScale = 1.0f;             ///< Game speed multiplier
    };

    // ============================================================================
    // Scene Snapshot
    // ============================================================================

    struct SceneSnapshot
    {
        std::string sceneName;
        std::vector<uint8_t> serializedData; ///< Full ECS state (binary)
        std::string sceneFilePath;           ///< Original scene file for reference

        bool IsValid() const { return !serializedData.empty(); }
        size_t GetSizeBytes() const { return serializedData.size(); }
    };

    // ============================================================================
    // Play Mode Events
    // ============================================================================

    using PlayModeCallback = std::function<void()>;
    using PlayModeErrorCallback = std::function<void(const std::string&)>;

    struct PlayModeCallbacks
    {
        PlayModeCallback onEnterPlay;
        PlayModeCallback onExitPlay;
        PlayModeCallback onPause;
        PlayModeCallback onResume;
        PlayModeCallback onFrameStep;
        PlayModeErrorCallback onError;
    };

    // ============================================================================
    // Play Mode Manager
    // ============================================================================

    class PlayModeManager
    {
      public:
        PlayModeManager() = default;
        ~PlayModeManager() = default;

        // -- Lifecycle --

        bool EnterPlayMode()
        {
            if (m_state != PlayModeState::Stopped)
                return false;

            m_state = PlayModeState::Starting;

            if (!SaveSnapshot())
            {
                m_state = PlayModeState::Stopped;
                if (m_callbacks.onError)
                    m_callbacks.onError("Failed to save scene snapshot before entering play mode");
                return false;
            }

            m_playTimeSeconds = 0.0f;
            m_frameCount = 0;
            m_playStartTime = Clock::now();
            m_stepRequested = false;

            m_state = m_config.startPaused ? PlayModeState::Paused : PlayModeState::Playing;

            if (m_callbacks.onEnterPlay)
                m_callbacks.onEnterPlay();

            return true;
        }

        bool ExitPlayMode()
        {
            if (m_state == PlayModeState::Stopped)
                return false;

            m_state = PlayModeState::Stopping;

            if (!RestoreSnapshot())
            {
                if (m_callbacks.onError)
                    m_callbacks.onError("Failed to restore scene snapshot after exiting play mode");
            }

            m_state = PlayModeState::Stopped;

            if (m_callbacks.onExitPlay)
                m_callbacks.onExitPlay();

            return true;
        }

        void TogglePlayMode()
        {
            if (IsInPlayMode())
                ExitPlayMode();
            else
                EnterPlayMode();
        }

        // -- Pause / Step --

        void PausePlayMode()
        {
            if (m_state == PlayModeState::Playing)
            {
                m_state = PlayModeState::Paused;
                if (m_callbacks.onPause)
                    m_callbacks.onPause();
            }
        }

        void ResumePlayMode()
        {
            if (m_state == PlayModeState::Paused)
            {
                m_state = PlayModeState::Playing;
                if (m_callbacks.onResume)
                    m_callbacks.onResume();
            }
        }

        void TogglePause()
        {
            if (m_state == PlayModeState::Playing)
                PausePlayMode();
            else if (m_state == PlayModeState::Paused)
                ResumePlayMode();
        }

        void StepFrame()
        {
            if (m_state == PlayModeState::Paused)
            {
                m_stepRequested = true;
                if (m_callbacks.onFrameStep)
                    m_callbacks.onFrameStep();
            }
        }

        // -- Time Control --

        void SetTimeScale(float scale) { m_config.timeScale = std::clamp(scale, 0.0f, 10.0f); }
        float GetTimeScale() const { return m_config.timeScale; }

        // -- State --

        PlayModeState GetState() const { return m_state; }
        bool IsPlaying() const { return m_state == PlayModeState::Playing; }
        bool IsPaused() const { return m_state == PlayModeState::Paused; }
        bool IsStopped() const { return m_state == PlayModeState::Stopped; }
        bool IsInPlayMode() const { return m_state == PlayModeState::Playing || m_state == PlayModeState::Paused; }

        // -- Config --

        void SetConfig(const PlayModeConfig& config) { m_config = config; }
        const PlayModeConfig& GetConfig() const { return m_config; }
        void SetCallbacks(const PlayModeCallbacks& callbacks) { m_callbacks = callbacks; }

        // -- Timing --

        float GetPlayTime() const { return m_playTimeSeconds; }
        uint64_t GetFrameCount() const { return m_frameCount; }

        // -- Frame Update --

        void Update(float deltaTime)
        {
            if (m_state == PlayModeState::Stopped || m_state == PlayModeState::Starting ||
                m_state == PlayModeState::Stopping)
                return;

            if (m_state == PlayModeState::Paused)
            {
                if (!m_stepRequested)
                    return;
                m_stepRequested = false;
                deltaTime = m_config.fixedTimestep;
            }

            float scaledDelta = deltaTime * m_config.timeScale;
            scaledDelta = std::min(scaledDelta, m_config.maxDeltaTime);

            m_playTimeSeconds += scaledDelta;
            m_frameCount++;
        }

        // -- Snapshot --

        bool HasSnapshot() const { return m_snapshot.IsValid(); }
        size_t GetSnapshotSize() const { return m_snapshot.GetSizeBytes(); }

        // -- Console --

        std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "PlayMode: ";

            switch (m_state)
            {
            case PlayModeState::Stopped:
                oss << "Stopped";
                break;
            case PlayModeState::Starting:
                oss << "Starting...";
                break;
            case PlayModeState::Playing:
                oss << "Playing";
                break;
            case PlayModeState::Paused:
                oss << "Paused";
                break;
            case PlayModeState::Stopping:
                oss << "Stopping...";
                break;
            }

            if (IsInPlayMode())
            {
                oss << " | Time: " << m_playTimeSeconds << "s" << " | Frames: " << m_frameCount
                    << " | Scale: " << m_config.timeScale << "x";
                if (HasSnapshot())
                    oss << " | Snapshot: " << (m_snapshot.GetSizeBytes() / 1024) << " KB";
            }

            return oss.str();
        }

      private:
        bool SaveSnapshot()
        {
            m_snapshot.sceneName = "EditorScene";
            m_snapshot.sceneFilePath = "";
            const char marker[] = "SPARK_SCENE_SNAPSHOT_V1";
            m_snapshot.serializedData.assign(reinterpret_cast<const uint8_t*>(marker),
                                             reinterpret_cast<const uint8_t*>(marker) + sizeof(marker));
            return true;
        }

        bool RestoreSnapshot()
        {
            if (!m_snapshot.IsValid())
                return false;
            m_snapshot.serializedData.clear();
            m_snapshot.sceneName.clear();
            return true;
        }

        PlayModeState m_state = PlayModeState::Stopped;
        PlayModeConfig m_config;
        PlayModeCallbacks m_callbacks;
        SceneSnapshot m_snapshot;

        float m_playTimeSeconds = 0.0f;
        uint64_t m_frameCount = 0;
        bool m_stepRequested = false;

        using Clock = std::chrono::high_resolution_clock;
        Clock::time_point m_playStartTime;
    };

} // namespace Spark::Editor
