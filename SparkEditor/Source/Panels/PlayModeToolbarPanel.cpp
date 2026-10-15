/**
 * @file PlayModeToolbarPanel.cpp
 * @brief Editor state-preview controls; gameplay is launched out of process
 * @author Spark Engine Team
 * @date 2025
 */

#include "PlayModeToolbarPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Engine/Editor/PlayModeManager.h"
#include "Utils/LogMacros.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <cmath>
#include <optional>
#include "SparkUI/Color.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include <algorithm>

namespace SparkEditor
{
    namespace
    {
        /// Alpha of the Paused line's dim blink phase, raised when the palette needs more for 3:1.
        constexpr float kPausedBlinkDimAlpha = 0.4f;
        /// Smallest contrast the dim phase may reach (WCAG non-text minimum; the bright phase is 4.5:1+).
        constexpr float kPausedBlinkMinContrast = 3.0f;
    } // namespace

    // ============================================================================
    // Construction / Lifecycle
    // ============================================================================

    PlayModeToolbarPanel::PlayModeToolbarPanel() : EditorPanel("State Preview Toolbar", "play_mode_toolbar_panel") {}

    bool PlayModeToolbarPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayModeToolbarPanel initialized");

        SetIcon(ICON_FA_PLAY);
        m_timeScaleSlider = 1.0f;
        m_multiStepCount = 10;
        m_showSubsystemToggles = false;
        m_showAdvancedControls = false;
        m_statusBlinkTimer = 0.0f;

        m_isInitialized = true;
        return true;
    }

    void PlayModeToolbarPanel::Update(float deltaTime)
    {
        // Blink timer for paused-state indicator
        if (m_playModeManager && m_playModeManager->IsPaused())
        {
            m_statusBlinkTimer += deltaTime;
            if (m_statusBlinkTimer > 1.0f)
            {
                m_statusBlinkTimer -= 1.0f;
            }
        }
        else
        {
            m_statusBlinkTimer = 0.0f;
        }

        // Sync time-scale slider from the manager when available
        if (m_playModeManager)
        {
            m_timeScaleSlider = m_playModeManager->GetTimeScale();
        }
    }

    void PlayModeToolbarPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        if (!m_playModeManager)
        {
            Spark::Ui::EmptyState(ICON_FA_INFO_CIRCLE, "State preview controls are unavailable",
                                  "Connect a PlayModeManager to enable controls.");
            EndPanel();
            return;
        }

        ImGui::TextWrapped("State preview only: no gameplay systems tick here. "
                           "Use Play Control > Launch Game to run the game.");

        RenderTransportControls();
        SparkEditor::VerticalSeparator();

        RenderTimeScaleControls();
        SparkEditor::VerticalSeparator();

        RenderSubsystemToggles();
        SparkEditor::VerticalSeparator();

        RenderCameraModeSelector();
        SparkEditor::VerticalSeparator();

        RenderLiveEditControls();
        SparkEditor::VerticalSeparator();

        RenderStatusDisplay();

        EndPanel();
    }

    void PlayModeToolbarPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayModeToolbarPanel shutting down");
        m_playModeManager = nullptr;
        m_isInitialized = false;
    }

    bool PlayModeToolbarPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (!m_playModeManager)
        {
            return false;
        }

        if (eventType == "play_mode_toggle")
        {
            m_playModeManager->TogglePlayMode();
            return true;
        }
        else if (eventType == "play_mode_pause")
        {
            m_playModeManager->TogglePause();
            return true;
        }
        else if (eventType == "play_mode_stop")
        {
            m_playModeManager->ExitPlayMode();
            return true;
        }
        else if (eventType == "play_mode_step")
        {
            m_playModeManager->StepFrame();
            return true;
        }
        else if (eventType == "play_mode_time_scale" && eventData)
        {
            float scale = *static_cast<float*>(eventData);
            m_playModeManager->SetTimeScale(scale);
            return true;
        }

        return false;
    }

    // ============================================================================
    // Transport Controls
    // ============================================================================

    void PlayModeToolbarPanel::RenderTransportControls()
    {
        using namespace Spark::Editor;

        bool isPlaying = m_playModeManager->IsPlaying();
        bool isSimulating = m_playModeManager->IsSimulating();
        bool isPaused = m_playModeManager->IsPaused();
        bool isStopped = m_playModeManager->IsStopped();
        bool isInPlayMode = m_playModeManager->IsInPlayMode();

        // --- Play Button --- (shown selected while the preview runs; the status text says so too)
        const char* playTooltip = isStopped ? "Start state preview (no gameplay tick)"
                                            : (isPaused ? "Resume state preview" : "State preview active");
        if (Spark::Ui::IconButton("PlayBtn", ICON_FA_PLAY, playTooltip, isPlaying || isSimulating))
        {
            if (isStopped)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayModeToolbar: entering play mode");
                m_playModeManager->EnterPlayMode();
            }
            else if (isPaused)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayModeToolbar: resuming play mode");
                m_playModeManager->ResumePlayMode();
            }
        }

        ImGui::SameLine();

        // --- Pause Button ---
        const bool pauseDisabled = (!isPlaying && !isSimulating) && !isPaused;
        if (Spark::Ui::IconButton("PauseBtn", ICON_FA_PAUSE, isPaused ? "Resume" : "Pause", isPaused, !pauseDisabled))
        {
            m_playModeManager->TogglePause();
        }

        ImGui::SameLine();

        // --- Stop Button --- (error-tinted glyph while a preview can be stopped)
        {
            std::optional<Spark::Ui::ScopedRoleColor> stopTint;
            if (isInPlayMode)
            {
                stopTint.emplace(ImGuiCol_Text, Spark::Ui::Role::ErrorText);
            }
            if (Spark::Ui::IconButton("StopBtn", ICON_FA_STOP, "Stop state preview", false, !isStopped))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayModeToolbar: exiting play mode");
                m_playModeManager->ExitPlayMode();
            }
        }

        ImGui::SameLine();

        // --- Step Button ---
        bool stepDisabled = !isPaused;
        if (Spark::Ui::IconButton("StepBtn", ICON_FA_STEP_FORWARD, "Step one preview state frame", false,
                                  !stepDisabled))
        {
            m_playModeManager->StepFrame();
        }

        ImGui::SameLine();

        // --- Multi-Step Button ---
        if (stepDisabled)
        {
            ImGui::BeginDisabled();
        }
        char multiStepLabel[64];
        snprintf(multiStepLabel, sizeof(multiStepLabel), ICON_FA_FORWARD " %d##MultiStep", m_multiStepCount);
        if (ImGui::Button(multiStepLabel))
        {
            m_playModeManager->StepFrames(static_cast<uint32_t>(m_multiStepCount));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            char tooltip[64];
            snprintf(tooltip, sizeof(tooltip), "Step %d preview state frames", m_multiStepCount);
            ImGui::SetTooltip("%s", tooltip);
        }
        if (stepDisabled)
        {
            ImGui::EndDisabled();
        }

        // Right-click multi-step to change count
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("MultiStepCountPopup");
        }
        if (ImGui::BeginPopup("MultiStepCountPopup"))
        {
            Spark::Ui::SubHeader("Multi-Step Count");
            ImGui::SliderInt("##MultiStepSlider", &m_multiStepCount, 1, 120);
            ImGui::EndPopup();
        }
    }

    // ============================================================================
    // Time Scale Controls
    // ============================================================================

    void PlayModeToolbarPanel::RenderTimeScaleControls()
    {
        ImGui::Text(ICON_FA_CLOCK);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(Spark::Ui::Em(6.0f));
        if (ImGui::SliderFloat("##TimeScale", &m_timeScaleSlider, 0.0f, 10.0f, "%.2fx"))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "PlayModeToolbar: time scale set to %.2f", m_timeScaleSlider);
            m_playModeManager->SetTimeScale(m_timeScaleSlider);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Time Scale: %.2fx", m_timeScaleSlider);
        }

        // Preset buttons
        for (int i = 0; i < NUM_TIME_SCALE_PRESETS; ++i)
        {
            ImGui::SameLine();

            float preset = TIME_SCALE_PRESETS[i];
            bool isActive = std::fabs(m_timeScaleSlider - preset) < 0.01f;

            // The active preset uses the accent button colours.
            std::optional<Spark::Ui::ScopedPrimaryButtonColors> activeColors;
            if (isActive)
            {
                activeColors.emplace();
            }

            char label[32];
            if (preset == static_cast<int>(preset))
            {
                snprintf(label, sizeof(label), "%dx##TSP%d", static_cast<int>(preset), i);
            }
            else
            {
                snprintf(label, sizeof(label), "%.2fx##TSP%d", preset, i);
            }

            if (ImGui::SmallButton(label))
            {
                m_timeScaleSlider = preset;
                m_playModeManager->SetTimeScale(preset);
            }
        }
    }

    // ============================================================================
    // Subsystem Toggles
    // ============================================================================

    void PlayModeToolbarPanel::RenderSubsystemToggles()
    {
        using namespace Spark::Editor;

        // Compact toggle button to expand/collapse subsystem toggles
        if (ImGui::SmallButton(m_showSubsystemToggles ? (ICON_FA_COGS " " ICON_FA_CHEVRON_DOWN "##SubsysToggle")
                                                      : (ICON_FA_COGS "##SubsysToggle")))
        {
            m_showSubsystemToggles = !m_showSubsystemToggles;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Preview counter flags; no gameplay subsystems tick");
        }

        if (!m_showSubsystemToggles)
        {
            return;
        }

        struct SubsystemInfo
        {
            const char* label;
            const char* icon;
            SimulationSubsystem flag;
        };

        static const SubsystemInfo subsystems[] = {
            {"Physics", ICON_FA_GLOBE, SimulationSubsystem::Physics},
            {"AI", ICON_FA_CROSSHAIRS, SimulationSubsystem::AI},
            {"Audio", ICON_FA_VOLUME_UP, SimulationSubsystem::Audio},
            {"Animation", ICON_FA_RUNNING, SimulationSubsystem::Animation},
            {"Scripting", ICON_FA_CODE, SimulationSubsystem::Scripting},
            {"Particles", ICON_FA_FIRE, SimulationSubsystem::Particles},
        };

        for (const auto& subsys : subsystems)
        {
            ImGui::SameLine();

            bool enabled = m_playModeManager->IsSubsystemEnabled(subsys.flag);

            // Enabled counters show as selected toggles; the tooltip spells the state out.
            char tooltip[128];
            snprintf(tooltip, sizeof(tooltip), "%s preview counter: %s (not a gameplay toggle)", subsys.label,
                     enabled ? "Enabled" : "Disabled");
            char buttonId[64];
            snprintf(buttonId, sizeof(buttonId), "Sub_%s", subsys.label);

            if (Spark::Ui::IconButton(buttonId, subsys.icon, tooltip, enabled))
            {
                m_playModeManager->SetSubsystemEnabled(subsys.flag, !enabled);
            }
        }
    }

    // ============================================================================
    // Camera Mode Selector
    // ============================================================================

    void PlayModeToolbarPanel::RenderCameraModeSelector()
    {
        using namespace Spark::Editor;

        EditorCameraMode currentMode = m_playModeManager->GetCameraMode();

        static const char* cameraModeLabels[] = {"Editor Free", "Game Camera", "Follow Player"};

        int currentIndex = static_cast<int>(currentMode);

        ImGui::Text(ICON_FA_CAMERA);
        ImGui::SameLine();

        // Wide enough for the longest mode label plus the combo arrow.
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Follow Player").x + ImGui::GetFrameHeight() +
                                ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::Combo("##CameraMode", &currentIndex, cameraModeLabels, 3))
        {
            m_playModeManager->SetCameraMode(static_cast<EditorCameraMode>(currentIndex));
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Preview camera mode label: %s (does not start gameplay)",
                              cameraModeLabels[currentIndex]);
        }
    }

    // ============================================================================
    // Live Edit Controls
    // ============================================================================

    void PlayModeToolbarPanel::RenderLiveEditControls()
    {
        bool liveEditing = m_playModeManager->IsLiveEditingEnabled();
        bool keepChanges = m_playModeManager->GetKeepChangesOnStop();

        // Live-edit toggle: warning-filled while on, because edits now touch the previewed scene.
        {
            std::optional<Spark::Ui::ScopedRoleColor> liveFill;
            if (liveEditing)
            {
                liveFill.emplace(ImGuiCol_Button, Spark::Ui::Role::WarningFill);
            }
            if (ImGui::SmallButton(liveEditing ? (ICON_FA_EDIT " Live##LiveEdit") : (ICON_FA_LOCK " Live##LiveEdit")))
            {
                m_playModeManager->SetLiveEditingEnabled(!liveEditing);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Live Editing: %s\nEdit the scene during state preview", liveEditing ? "ON" : "OFF");
        }

        ImGui::SameLine();

        // Keep-changes-on-stop toggle: accent-filled while Stop will keep the edits.
        {
            std::optional<Spark::Ui::ScopedPrimaryButtonColors> keepFill;
            if (keepChanges)
            {
                keepFill.emplace();
            }
            if (ImGui::SmallButton(keepChanges ? (ICON_FA_SAVE " Keep##KeepChanges")
                                               : (ICON_FA_UNDO " Revert##KeepChanges")))
            {
                m_playModeManager->SetKeepChangesOnStop(!keepChanges);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("On Stop: %s", keepChanges ? "Keep Changes" : "Revert to Snapshot");
        }

        // Show live-edit count if any
        if (liveEditing && m_playModeManager->GetLiveEditCount() > 0)
        {
            ImGui::SameLine();
            Spark::Ui::TextRole(Spark::Ui::Role::WarningText, "(%zu edits)", m_playModeManager->GetLiveEditCount());
        }
    }

    // ============================================================================
    // Status Display
    // ============================================================================

    void PlayModeToolbarPanel::RenderStatusDisplay()
    {
        using namespace Spark::Editor;

        PlayModeState state = m_playModeManager->GetState();

        // State indicator with color
        switch (state)
        {
        case PlayModeState::Stopped:
            Spark::Ui::StatusText(Spark::Ui::Status::Neutral, ICON_FA_STOP " Stopped");
            break;

        case PlayModeState::Starting:
            Spark::Ui::StatusText(Spark::Ui::Status::Info, ICON_FA_SPINNER " Starting...");
            break;

        case PlayModeState::Playing:
            Spark::Ui::StatusText(Spark::Ui::Status::Success, ICON_FA_PLAY " State preview");
            break;

        case PlayModeState::Simulating:
            Spark::Ui::StatusText(Spark::Ui::Status::Info, ICON_FA_CUBE " Simulation preview");
            break;

        case PlayModeState::Paused:
        {
            // Blink by fading the whole status line at 1 Hz; Reduce Motion keeps it steady. The dim
            // phase never drops the word below 3:1 on the panel background (alpha measured, not fixed).
            namespace Ui = Spark::Ui;
            const Ui::Tokens& tokens = Ui::ActiveTokens();
            const float dimAlpha =
                std::max(kPausedBlinkDimAlpha,
                         Ui::MinimumAlphaForContrast(Ui::Get(tokens, Ui::Role::WarningText),
                                                     Ui::Get(tokens, Ui::Role::SurfaceBase), kPausedBlinkMinContrast));
            const float blinkAlpha = Ui::ReduceMotion() || m_statusBlinkTimer < 0.5f ? 1.0f : dimAlpha;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * blinkAlpha);
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, ICON_FA_PAUSE " Paused");
            ImGui::PopStyleVar();
            break;
        }

        case PlayModeState::Stopping:
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, ICON_FA_SPINNER " Stopping...");
            break;
        }

        if (m_playModeManager->IsInPlayMode())
        {
            float playTime = m_playModeManager->GetPlayTime();
            uint64_t frameCount = m_playModeManager->GetFrameCount();
            float fps = m_playModeManager->GetCurrentFPS();

            int minutes = static_cast<int>(playTime) / 60;
            int seconds = static_cast<int>(playTime) % 60;
            int millis = static_cast<int>((playTime - std::floor(playTime)) * 100.0f);

            ImGui::SameLine();
            ImGui::Text(ICON_FA_CLOCK " %02d:%02d.%02d", minutes, seconds, millis);

            ImGui::SameLine();
            ImGui::Text(ICON_FA_FILM " %llu", static_cast<unsigned long long>(frameCount));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Preview state frames; no game-system frames are counted");

            ImGui::SameLine();
            ImGui::Text(ICON_FA_BOLT " %.1f/s", fps);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Preview state rate (state frames / scaled preview time); not gameplay FPS");

            // Show multi-step remaining if active
            uint32_t stepsRemaining = m_playModeManager->GetMultiStepRemaining();
            if (stepsRemaining > 0)
            {
                ImGui::SameLine();
                Spark::Ui::TextRole(Spark::Ui::Role::InfoText, ICON_FA_FORWARD " %u steps left", stepsRemaining);
            }
        }
    }

} // namespace SparkEditor
