/**
 * @file PlayModeManager.h
 * @brief Play-in-editor mode management: scene snapshot, restore, time control
 *
 * Manages the lifecycle of play mode within the editor: saving the current scene
 * state before entering play, running the game loop in the viewport, and restoring
 * the original state when stopping. Includes simulation subsystem control, editor
 * camera switching, live property editing support, and multi-step frame stepping.
 */

#pragma once

#include "PlayModeTypes.h"
#include "SceneSnapshotSerializer.h"

#include "../../Core/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace Spark::Editor
{

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
            m_multiStepRemaining = 0;
            m_wasSimulatingBeforePause = false;
            m_stats.Reset();
            m_liveEdits.clear();
            m_cameraMode = m_config.cameraMode;

            m_state = m_config.startPaused ? PlayModeState::Paused : PlayModeState::Playing;

            if (m_callbacks.onEnterPlay)
                m_callbacks.onEnterPlay();

            return true;
        }

        /// @brief Enter simulation mode without switching to game camera behavior.
        bool EnterSimulationMode()
        {
            if (m_state != PlayModeState::Stopped)
                return false;

            m_state = PlayModeState::Starting;

            if (!SaveSnapshot())
            {
                m_state = PlayModeState::Stopped;
                if (m_callbacks.onError)
                    m_callbacks.onError("Failed to save scene snapshot before entering simulation mode");
                return false;
            }

            m_playTimeSeconds = 0.0f;
            m_frameCount = 0;
            m_playStartTime = Clock::now();
            m_stepRequested = false;
            m_multiStepRemaining = 0;
            m_wasSimulatingBeforePause = true;
            m_stats.Reset();
            m_liveEdits.clear();
            m_cameraMode = EditorCameraMode::EditorFree;

            m_state = m_config.startPaused ? PlayModeState::Paused : PlayModeState::Simulating;

            if (m_callbacks.onEnterSimulate)
                m_callbacks.onEnterSimulate();

            return true;
        }

        bool ExitPlayMode()
        {
            if (m_state == PlayModeState::Stopped)
                return false;

            const bool wasSimulating = (m_state == PlayModeState::Simulating) ||
                                       (m_state == PlayModeState::Paused && m_wasSimulatingBeforePause);
            m_state = PlayModeState::Stopping;

            if (!m_config.keepChangesOnStop)
            {
                if (!RestoreSnapshot())
                {
                    if (m_callbacks.onError)
                        m_callbacks.onError("Failed to restore scene snapshot after exiting play mode");
                }
            }
            else
            {
                // Just clear the snapshot without restoring
                m_snapshot.serializedData.clear();
                m_snapshot.sceneName.clear();
            }

            m_cameraMode = EditorCameraMode::EditorFree;
            m_state = PlayModeState::Stopped;
            m_wasSimulatingBeforePause = false;

            if (m_callbacks.onExitPlay)
                m_callbacks.onExitPlay();
            if (wasSimulating && m_callbacks.onExitSimulate)
                m_callbacks.onExitSimulate();

            return true;
        }

        void TogglePlayMode()
        {
            if (IsInPlayMode())
                ExitPlayMode();
            else
                EnterPlayMode();
        }

        void ToggleSimulationMode()
        {
            if (IsInPlayMode())
                ExitPlayMode();
            else
                EnterSimulationMode();
        }

        // -- Pause / Step --

        void PausePlayMode()
        {
            if (m_state == PlayModeState::Playing || m_state == PlayModeState::Simulating)
            {
                m_wasSimulatingBeforePause = (m_state == PlayModeState::Simulating);
                m_state = PlayModeState::Paused;
                if (m_callbacks.onPause)
                    m_callbacks.onPause();
            }
        }

        void ResumePlayMode()
        {
            if (m_state == PlayModeState::Paused)
            {
                m_state = m_wasSimulatingBeforePause ? PlayModeState::Simulating : PlayModeState::Playing;
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

        /// @brief Step multiple frames then pause again.
        void StepFrames(uint32_t count)
        {
            if (m_state == PlayModeState::Paused && count > 0)
            {
                m_multiStepRemaining = count;
                m_stepRequested = true;
                if (m_callbacks.onFrameStep)
                    m_callbacks.onFrameStep();
            }
        }

        uint32_t GetMultiStepRemaining() const { return m_multiStepRemaining; }

        // -- Time Control --

        void SetTimeScale(float scale) { m_config.timeScale = std::clamp(scale, 0.0f, 10.0f); }
        float GetTimeScale() const { return m_config.timeScale; }

        // -- Simulation Subsystem Control --

        void SetSubsystemEnabled(SimulationSubsystem subsystem, bool enabled)
        {
            if (enabled)
                m_config.activeSubsystems = m_config.activeSubsystems | subsystem;
            else
                m_config.activeSubsystems = m_config.activeSubsystems & ~subsystem;

            if (m_callbacks.onSubsystemToggled)
                m_callbacks.onSubsystemToggled(subsystem, enabled);
        }

        bool IsSubsystemEnabled(SimulationSubsystem subsystem) const
        {
            return HasFlag(m_config.activeSubsystems, subsystem);
        }

        SimulationSubsystem GetActiveSubsystems() const { return m_config.activeSubsystems; }

        // -- Camera Control --

        void SetCameraMode(EditorCameraMode mode)
        {
            m_cameraMode = mode;
            if (m_callbacks.onCameraModeChanged)
                m_callbacks.onCameraModeChanged(mode);
        }

        EditorCameraMode GetCameraMode() const { return m_cameraMode; }

        // -- Live Editing --

        void SetLiveEditingEnabled(bool enabled) { m_config.allowLiveEditing = enabled; }
        bool IsLiveEditingEnabled() const { return m_config.allowLiveEditing; }

        void SetKeepChangesOnStop(bool keep) { m_config.keepChangesOnStop = keep; }
        bool GetKeepChangesOnStop() const { return m_config.keepChangesOnStop; }

        bool RecordLiveEdit(uint32_t entityId, const std::string& componentType, const std::string& propertyName,
                            const std::string& oldValue, const std::string& newValue)
        {
            if (!IsInPlayMode() || !m_config.allowLiveEditing)
                return false;

            LiveEditRecord record;
            record.entityId = entityId;
            record.componentType = componentType;
            record.propertyName = propertyName;
            record.oldValue = oldValue;
            record.newValue = newValue;
            record.timestamp = m_playTimeSeconds;
            m_liveEdits.push_back(record);

            if (m_callbacks.onLiveEdit)
                m_callbacks.onLiveEdit(record);

            return true;
        }

        const std::vector<LiveEditRecord>& GetLiveEdits() const { return m_liveEdits; }
        size_t GetLiveEditCount() const { return m_liveEdits.size(); }

        // -- State --

        PlayModeState GetState() const { return m_state; }
        bool IsPlaying() const { return m_state == PlayModeState::Playing; }
        bool IsSimulating() const { return m_state == PlayModeState::Simulating; }
        bool IsPaused() const { return m_state == PlayModeState::Paused; }
        bool IsStopped() const { return m_state == PlayModeState::Stopped; }
        bool IsInPlayMode() const
        {
            return m_state == PlayModeState::Playing || m_state == PlayModeState::Simulating ||
                   m_state == PlayModeState::Paused;
        }

        // -- Config --

        void SetConfig(const PlayModeConfig& config) { m_config = config; }
        const PlayModeConfig& GetConfig() const { return m_config; }
        void SetCallbacks(const PlayModeCallbacks& callbacks) { m_callbacks = callbacks; }

        // -- Timing --

        float GetPlayTime() const { return m_playTimeSeconds; }
        uint64_t GetFrameCount() const { return m_frameCount; }

        // -- Statistics --

        const PlayModeStats& GetStats() const { return m_stats; }

        float GetCurrentFPS() const
        {
            if (m_frameCount == 0 || m_playTimeSeconds <= 0.0f)
                return 0.0f;
            return static_cast<float>(m_frameCount) / m_playTimeSeconds;
        }

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

                // Handle multi-step: decrement and re-arm if more steps remain
                if (m_multiStepRemaining > 0)
                {
                    m_multiStepRemaining--;
                    if (m_multiStepRemaining > 0)
                        m_stepRequested = true;
                }
            }

            float scaledDelta = deltaTime * m_config.timeScale;
            scaledDelta = std::min(scaledDelta, m_config.maxDeltaTime);

            // Update stats
            if (scaledDelta < m_stats.minFrameTime)
                m_stats.minFrameTime = scaledDelta;
            if (scaledDelta > m_stats.maxFrameTime)
                m_stats.maxFrameTime = scaledDelta;
            if (HasFlag(m_config.activeSubsystems, SimulationSubsystem::Physics))
            {
                m_stats.physicsTimeAccumulated += scaledDelta;
                m_stats.physicsSteps++;
            }
            if (HasFlag(m_config.activeSubsystems, SimulationSubsystem::AI))
                m_stats.aiUpdates++;
            if (HasFlag(m_config.activeSubsystems, SimulationSubsystem::Scripting))
                m_stats.scriptExecutions++;

            m_playTimeSeconds += scaledDelta;
            m_frameCount++;

            if (m_playTimeSeconds > 0.0f)
                m_stats.averageFPS = static_cast<float>(m_frameCount) / m_playTimeSeconds;
        }

        // -- Snapshot --

        bool HasSnapshot() const { return m_snapshot.IsValid(); }
        size_t GetSnapshotSize() const { return m_snapshot.GetSizeBytes(); }

        // -- PIE Dedicated Server --

        /// @brief Configuration for launching a local dedicated server during PIE.
        struct PIEDedicatedServerConfig
        {
            std::string serverName = "PIE Local Server";
            uint16_t port = 27015;
            int maxPlayers = 8;
            float tickRate = 60.0f;
            std::string mapName = "default";
            int gameMode = 0;
            bool autoConnectClient = true; ///< Editor viewport auto-joins as client
            bool lanOnly = true;
        };

        /// @brief Request a dedicated server to be spawned alongside PIE.
        bool RequestDedicatedServer(const PIEDedicatedServerConfig& config)
        {
            if (m_pieServerActive)
                return false;
            m_pieServerConfig = config;
            m_pieServerRequested = true;
            return true;
        }

        /// @brief Check if a PIE dedicated server was requested and consume the flag.
        bool ConsumeDedicatedServerRequest(PIEDedicatedServerConfig& outConfig)
        {
            if (!m_pieServerRequested)
                return false;
            m_pieServerRequested = false;
            outConfig = m_pieServerConfig;
            return true;
        }

        /// @brief Mark the PIE dedicated server as active (called by the server launcher).
        void SetDedicatedServerActive(bool active) { m_pieServerActive = active; }

        /// @brief Check if a PIE dedicated server is currently running.
        bool IsDedicatedServerActive() const { return m_pieServerActive; }

        /// @brief Get the PIE server configuration.
        const PIEDedicatedServerConfig& GetDedicatedServerConfig() const { return m_pieServerConfig; }

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
            case PlayModeState::Simulating:
                oss << "Simulating";
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
                    << " | Scale: " << m_config.timeScale << "x" << " | FPS: " << GetCurrentFPS();
                if (HasSnapshot())
                    oss << " | Snapshot: " << (m_snapshot.GetSizeBytes() / 1024) << " KB";
                if (m_config.allowLiveEditing)
                    oss << " | LiveEdits: " << m_liveEdits.size();
                if (m_pieServerActive)
                    oss << " | DediServer: " << m_pieServerConfig.serverName << " (:" << m_pieServerConfig.port << ")";
            }

            return oss.str();
        }

        /// @brief Get a string describing which subsystems are active.
        std::string GetSubsystemStatusString() const
        {
            std::ostringstream oss;
            auto flags = m_config.activeSubsystems;
            if (HasFlag(flags, SimulationSubsystem::Physics))
                oss << "[Physics] ";
            if (HasFlag(flags, SimulationSubsystem::AI))
                oss << "[AI] ";
            if (HasFlag(flags, SimulationSubsystem::Audio))
                oss << "[Audio] ";
            if (HasFlag(flags, SimulationSubsystem::Animation))
                oss << "[Anim] ";
            if (HasFlag(flags, SimulationSubsystem::Scripting))
                oss << "[Script] ";
            if (HasFlag(flags, SimulationSubsystem::Particles))
                oss << "[Particles] ";
            if (HasFlag(flags, SimulationSubsystem::Networking))
                oss << "[Net] ";
            return oss.str();
        }

        /// @brief Set the ECS registry pointer used for snapshot serialization.
        /// Must be called before EnterPlayMode. Typically set once at editor startup.
        void SetRegistry(void* registry, uint32_t entityCount = 0)
        {
            m_registry = registry;
            m_entityCount = entityCount;
        }

        /// @brief Update the entity count (call before EnterPlayMode if registry changed).
        void SetEntityCount(uint32_t count) { m_entityCount = count; }

      private:
        bool SaveSnapshot()
        {
            m_snapshot.sceneName = "EditorScene";
            m_snapshot.sceneFilePath = "";

            if (m_registry && ComponentSerializerRegistry::Instance().Count() > 0)
            {
                m_snapshot.serializedData = SceneSnapshotSerializer::Serialize(m_registry, m_entityCount);
                return !m_snapshot.serializedData.empty();
            }

            // Fallback: write marker so HasSnapshot() returns true
            const char marker[] = "SPARK_SCENE_SNAPSHOT_V1";
            m_snapshot.serializedData.assign(reinterpret_cast<const uint8_t*>(marker),
                                             reinterpret_cast<const uint8_t*>(marker) + sizeof(marker));
            return true;
        }

        bool RestoreSnapshot()
        {
            if (!m_snapshot.IsValid())
                return false;

            if (m_registry && SceneSnapshotSerializer::Validate(m_snapshot.serializedData))
            {
                bool ok = SceneSnapshotSerializer::Deserialize(m_snapshot.serializedData, m_registry);
                m_snapshot.serializedData.clear();
                m_snapshot.sceneName.clear();
                return ok;
            }

            // Fallback: just clear
            m_snapshot.serializedData.clear();
            m_snapshot.sceneName.clear();
            return true;
        }

        PlayModeState m_state = PlayModeState::Stopped;
        PlayModeConfig m_config;
        PlayModeCallbacks m_callbacks;
        SceneSnapshot m_snapshot;
        PlayModeStats m_stats;

        float m_playTimeSeconds = 0.0f;
        uint64_t m_frameCount = 0;
        bool m_stepRequested = false;
        uint32_t m_multiStepRemaining = 0;
        bool m_wasSimulatingBeforePause = false;

        EditorCameraMode m_cameraMode = EditorCameraMode::EditorFree;
        std::vector<LiveEditRecord> m_liveEdits;

        using Clock = std::chrono::high_resolution_clock;
        Clock::time_point m_playStartTime;

        // PIE Dedicated Server
        PIEDedicatedServerConfig m_pieServerConfig;
        bool m_pieServerRequested = false;
        bool m_pieServerActive = false;

        // ECS registry for snapshot serialization
        void* m_registry = nullptr;
        uint32_t m_entityCount = 0;
    };

} // namespace Spark::Editor
