/**
 * @file PlayModeToolbarPanel.h
 * @brief Editor state-preview toolbar; actual gameplay runs out of process
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a compact toolbar for preview-state controls: start/pause/stop/step,
 * time-scale slider, counter flags, and a camera-mode label selector. It does
 * not tick game subsystems.
 * Designed to dock at the top of the editor viewport.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <cstdint>
#include <functional>
#include <string>

namespace Spark::Editor
{
    class PlayModeManager;
    enum class SimulationSubsystem : uint32_t;
    enum class EditorCameraMode;
    enum class PlayModeState;
} // namespace Spark::Editor

namespace SparkEditor
{

    /**
     * @brief Editor state-preview toolbar panel
     *
     * Renders a horizontal toolbar with:
     * - Transport controls: Play, Pause, Stop, Step, Multi-Step
     * - Time-scale slider with preset buttons (0.25x, 0.5x, 1x, 2x, 4x)
     * - Counter flags (Physics, AI, Audio, Animation, Scripting, Particles); no subsystem ticks
     * - Camera-mode switcher (Editor Free, Game Camera, Follow Player)
     * - Live-editing toggle and keep-changes-on-stop toggle
     * - Status display: scaled preview time, state frame count, state rate
     */
    class PlayModeToolbarPanel : public EditorPanel
    {
      public:
        PlayModeToolbarPanel();
        ~PlayModeToolbarPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /// @brief Connect to the engine's PlayModeManager.
        void SetPlayModeManager(Spark::Editor::PlayModeManager* manager) { m_playModeManager = manager; }

      private:
        void RenderTransportControls();
        void RenderTimeScaleControls();
        void RenderSubsystemToggles();
        void RenderCameraModeSelector();
        void RenderLiveEditControls();
        void RenderStatusDisplay();

        Spark::Editor::PlayModeManager* m_playModeManager = nullptr;

        // UI state
        float m_timeScaleSlider = 1.0f;
        int m_multiStepCount = 10;
        bool m_showSubsystemToggles = false;
        bool m_showAdvancedControls = false;

        // Animation
        float m_playButtonPulse = 0.0f;
        float m_statusBlinkTimer = 0.0f;

        // Presets
        static constexpr float TIME_SCALE_PRESETS[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
        static constexpr int NUM_TIME_SCALE_PRESETS = 5;
    };

} // namespace SparkEditor
