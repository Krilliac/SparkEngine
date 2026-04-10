/**
 * @file PlayModeTypes.h
 * @brief Type definitions for play-in-editor mode: enums, structs, callbacks
 *
 * Contains all standalone types used by PlayModeManager and its consumers:
 * play state enum, simulation subsystem flags, camera mode, configuration,
 * scene snapshot, live edit records, statistics, and callback signatures.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Spark::Editor
{

    // ============================================================================
    // Play Mode State
    // ============================================================================

    enum class PlayModeState
    {
        Stopped,    ///< Editor mode — scene is editable
        Starting,   ///< Transitioning to play: saving snapshot
        Playing,    ///< Game loop running in viewport
        Simulating, ///< Simulation running while retaining editor camera/workflow
        Paused,     ///< Game loop paused — can inspect/step
        Stopping    ///< Transitioning back: restoring snapshot
    };

    // ============================================================================
    // Simulation Subsystem Flags
    // ============================================================================

    enum class SimulationSubsystem : uint32_t
    {
        None = 0,
        Physics = 1 << 0,
        AI = 1 << 1,
        Audio = 1 << 2,
        Animation = 1 << 3,
        Scripting = 1 << 4,
        Particles = 1 << 5,
        Networking = 1 << 6,
        All = Physics | AI | Audio | Animation | Scripting | Particles | Networking
    };

    inline SimulationSubsystem operator|(SimulationSubsystem a, SimulationSubsystem b)
    {
        return static_cast<SimulationSubsystem>(std::to_underlying(a) | std::to_underlying(b));
    }

    inline SimulationSubsystem operator&(SimulationSubsystem a, SimulationSubsystem b)
    {
        return static_cast<SimulationSubsystem>(std::to_underlying(a) & std::to_underlying(b));
    }

    inline SimulationSubsystem operator~(SimulationSubsystem a)
    {
        return static_cast<SimulationSubsystem>(~std::to_underlying(a));
    }

    inline bool HasFlag(SimulationSubsystem flags, SimulationSubsystem flag)
    {
        return (std::to_underlying(flags) & std::to_underlying(flag)) != 0;
    }

    // ============================================================================
    // Editor Camera Mode
    // ============================================================================

    enum class EditorCameraMode
    {
        EditorFree,  ///< Free-fly editor camera (default in edit mode)
        GameCamera,  ///< Uses the scene's main camera
        FollowPlayer ///< Follows the player entity during play
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

        SimulationSubsystem activeSubsystems = SimulationSubsystem::All; ///< Which subsystems run
        EditorCameraMode cameraMode = EditorCameraMode::GameCamera;      ///< Camera mode during play
        bool allowLiveEditing = false;                                   ///< Allow property changes during play
        bool keepChangesOnStop = false;                                  ///< Persist play-mode edits after stopping

        /// @brief Build subsystem flags from legacy bool fields.
        SimulationSubsystem GetSubsystemFlags() const
        {
            auto flags = SimulationSubsystem::None;
            if (enablePhysics)
                flags = flags | SimulationSubsystem::Physics;
            if (enableAI)
                flags = flags | SimulationSubsystem::AI;
            if (enableAudio)
                flags = flags | SimulationSubsystem::Audio;
            if (enableNetworking)
                flags = flags | SimulationSubsystem::Networking;
            return flags | (activeSubsystems & (SimulationSubsystem::Animation | SimulationSubsystem::Scripting |
                                                SimulationSubsystem::Particles));
        }
    };

    // ============================================================================
    // Scene Snapshot
    // ============================================================================

    struct SceneSnapshot
    {
        std::string sceneName;
        std::vector<uint8_t> serializedData; ///< Full ECS state (binary)
        std::string sceneFilePath;           ///< Original scene file for reference
        uint32_t entityCount = 0;            ///< Number of entities at snapshot time

        bool IsValid() const { return !serializedData.empty(); }
        size_t GetSizeBytes() const { return serializedData.size(); }
    };

    // ============================================================================
    // Live Edit Record
    // ============================================================================

    /// @brief Tracks a single property change made during play mode.
    struct LiveEditRecord
    {
        uint32_t entityId;
        std::string componentType;
        std::string propertyName;
        std::string oldValue;
        std::string newValue;
        float timestamp; ///< Play time when the edit occurred
    };

    // ============================================================================
    // Play Mode Statistics
    // ============================================================================

    /// @brief Runtime statistics collected during play mode.
    struct PlayModeStats
    {
        float averageFPS = 0.0f;
        float minFrameTime = 999.0f;
        float maxFrameTime = 0.0f;
        uint32_t physicsSteps = 0;
        uint32_t aiUpdates = 0;
        uint32_t scriptExecutions = 0;
        float physicsTimeAccumulated = 0.0f;

        void Reset()
        {
            averageFPS = 0.0f;
            minFrameTime = 999.0f;
            maxFrameTime = 0.0f;
            physicsSteps = 0;
            aiUpdates = 0;
            scriptExecutions = 0;
            physicsTimeAccumulated = 0.0f;
        }
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
        PlayModeCallback onEnterSimulate;
        PlayModeCallback onExitSimulate;
        PlayModeCallback onPause;
        PlayModeCallback onResume;
        PlayModeCallback onFrameStep;
        PlayModeErrorCallback onError;

        std::function<void(EditorCameraMode)> onCameraModeChanged;
        std::function<void(SimulationSubsystem, bool)> onSubsystemToggled;
        std::function<void(const LiveEditRecord&)> onLiveEdit;
    };

} // namespace Spark::Editor
