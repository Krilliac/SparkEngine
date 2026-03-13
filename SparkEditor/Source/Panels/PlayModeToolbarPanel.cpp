/**
 * @file PlayModeToolbarPanel.cpp
 * @brief Implementation of the play-in-editor toolbar with transport controls and simulation options
 * @author Spark Engine Team
 * @date 2025
 */

#include "PlayModeToolbarPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Engine/Editor/PlayModeManager.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <cmath>

namespace SparkEditor
{

    // ============================================================================
    // Construction / Lifecycle
    // ============================================================================

    PlayModeToolbarPanel::PlayModeToolbarPanel() : EditorPanel("Play Mode Toolbar", "play_mode_toolbar_panel") {}

    bool PlayModeToolbarPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Play Mode Toolbar panel\n";

        SetIcon(ICON_FA_PLAY);
        m_timeScaleSlider = 1.0f;
        m_multiStepCount = 10;
        m_showSubsystemToggles = false;
        m_showAdvancedControls = false;
        m_playButtonPulse = 0.0f;
        m_statusBlinkTimer = 0.0f;

        m_isInitialized = true;
        return true;
    }

    void PlayModeToolbarPanel::Update(float deltaTime)
    {
        // Animate the play-button pulse when playing
        if (m_playModeManager && m_playModeManager->IsPlaying())
        {
            m_playButtonPulse += deltaTime * 3.0f;
            if (m_playButtonPulse > 6.2831853f) // 2 * PI
            {
                m_playButtonPulse -= 6.2831853f;
            }
        }
        else
        {
            m_playButtonPulse = 0.0f;
        }

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
            ImGui::TextDisabled(ICON_FA_INFO_CIRCLE " Play Mode Toolbar is available during play mode.");
            ImGui::TextDisabled("Connect a PlayModeManager to enable controls.");
            EndPanel();
            return;
        }

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
        std::cout << "Shutting down Play Mode Toolbar panel\n";
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
        bool isPaused = m_playModeManager->IsPaused();
        bool isStopped = m_playModeManager->IsStopped();
        bool isInPlayMode = m_playModeManager->IsInPlayMode();

        // --- Play Button ---
        if (isPlaying)
        {
            // Pulsing green when playing
            float pulse = 0.5f + 0.5f * std::sin(m_playButtonPulse);
            ImVec4 playColor(0.1f, 0.5f + 0.3f * pulse, 0.1f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, playColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.4f, 0.15f, 1.0f));
        }

        if (ImGui::Button(ICON_FA_PLAY "##PlayBtn", ImVec2(32, 24)))
        {
            if (isStopped)
            {
                m_playModeManager->EnterPlayMode();
            }
            else if (isPaused)
            {
                m_playModeManager->ResumePlayMode();
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(isStopped ? "Play (Enter play mode)" : (isPaused ? "Resume" : "Playing..."));
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // --- Pause Button ---
        if (isPaused)
        {
            float blink = m_statusBlinkTimer < 0.5f ? 1.0f : 0.6f;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f * blink, 0.6f * blink, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.8f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.5f, 0.1f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }

        bool pauseDisabled = !isPlaying && !isPaused;
        if (pauseDisabled)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(ICON_FA_PAUSE "##PauseBtn", ImVec2(32, 24)))
        {
            m_playModeManager->TogglePause();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(isPaused ? "Resume" : "Pause");
        }
        if (pauseDisabled)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // --- Stop Button ---
        if (isInPlayMode)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }

        if (isStopped)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(ICON_FA_STOP "##StopBtn", ImVec2(32, 24)))
        {
            m_playModeManager->ExitPlayMode();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Stop (Exit play mode)");
        }
        if (isStopped)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // --- Step Button ---
        bool stepDisabled = !isPaused;
        if (stepDisabled)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(ICON_FA_STEP_FORWARD "##StepBtn", ImVec2(32, 24)))
        {
            m_playModeManager->StepFrame();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Step one frame");
        }
        if (stepDisabled)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();

        // --- Multi-Step Button ---
        if (stepDisabled)
        {
            ImGui::BeginDisabled();
        }
        char multiStepLabel[64];
        snprintf(multiStepLabel, sizeof(multiStepLabel), ICON_FA_FORWARD " %d##MultiStep", m_multiStepCount);
        if (ImGui::Button(multiStepLabel, ImVec2(56, 24)))
        {
            m_playModeManager->StepFrames(static_cast<uint32_t>(m_multiStepCount));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            char tooltip[64];
            snprintf(tooltip, sizeof(tooltip), "Step %d frames", m_multiStepCount);
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
            ImGui::Text("Multi-Step Count");
            ImGui::Separator();
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

        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::SliderFloat("##TimeScale", &m_timeScaleSlider, 0.0f, 10.0f, "%.2fx"))
        {
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

            if (isActive)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.4f, 0.7f, 1.0f));
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

            if (isActive)
            {
                ImGui::PopStyleColor(3);
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
            ImGui::SetTooltip("Simulation Subsystem Toggles");
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

            if (enabled)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.4f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.4f, 0.25f, 1.0f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.15f, 0.15f, 1.0f));
            }

            char btnLabel[64];
            snprintf(btnLabel, sizeof(btnLabel), "%s##Sub_%s", subsys.icon, subsys.label);

            if (ImGui::SmallButton(btnLabel))
            {
                m_playModeManager->SetSubsystemEnabled(subsys.flag, !enabled);
            }
            if (ImGui::IsItemHovered())
            {
                char tooltip[128];
                snprintf(tooltip, sizeof(tooltip), "%s: %s (click to toggle)", subsys.label,
                         enabled ? "Enabled" : "Disabled");
                ImGui::SetTooltip("%s", tooltip);
            }

            ImGui::PopStyleColor(3);
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

        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##CameraMode", &currentIndex, cameraModeLabels, 3))
        {
            m_playModeManager->SetCameraMode(static_cast<EditorCameraMode>(currentIndex));
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Camera Mode: %s", cameraModeLabels[currentIndex]);
        }
    }

    // ============================================================================
    // Live Edit Controls
    // ============================================================================

    void PlayModeToolbarPanel::RenderLiveEditControls()
    {
        bool liveEditing = m_playModeManager->IsLiveEditingEnabled();
        bool keepChanges = m_playModeManager->GetKeepChangesOnStop();

        // Live-edit toggle
        if (liveEditing)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.35f, 0.1f, 1.0f));
        }

        if (ImGui::SmallButton(liveEditing ? (ICON_FA_EDIT " Live##LiveEdit") : (ICON_FA_LOCK " Live##LiveEdit")))
        {
            m_playModeManager->SetLiveEditingEnabled(!liveEditing);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Live Editing: %s\nEdit properties while playing", liveEditing ? "ON" : "OFF");
        }

        if (liveEditing)
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        // Keep-changes-on-stop toggle
        if (keepChanges)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.5f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.4f, 0.6f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.25f, 0.4f, 1.0f));
        }

        if (ImGui::SmallButton(keepChanges ? (ICON_FA_SAVE " Keep##KeepChanges")
                                           : (ICON_FA_UNDO " Revert##KeepChanges")))
        {
            m_playModeManager->SetKeepChangesOnStop(!keepChanges);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("On Stop: %s", keepChanges ? "Keep Changes" : "Revert to Snapshot");
        }

        if (keepChanges)
        {
            ImGui::PopStyleColor(3);
        }

        // Show live-edit count if any
        if (liveEditing && m_playModeManager->GetLiveEditCount() > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(%zu edits)", m_playModeManager->GetLiveEditCount());
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
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ICON_FA_STOP " Stopped");
            break;

        case PlayModeState::Starting:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), ICON_FA_SPINNER " Starting...");
            break;

        case PlayModeState::Playing:
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), ICON_FA_PLAY " Playing");
            break;

        case PlayModeState::Paused:
        {
            float blinkAlpha = m_statusBlinkTimer < 0.5f ? 1.0f : 0.4f;
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, blinkAlpha), ICON_FA_PAUSE " Paused");
            break;
        }

        case PlayModeState::Stopping:
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), ICON_FA_SPINNER " Stopping...");
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

            ImGui::SameLine();

            // Color-code FPS
            ImVec4 fpsColor;
            if (fps >= 55.0f)
            {
                fpsColor = ImVec4(0.3f, 0.9f, 0.3f, 1.0f); // Green — good
            }
            else if (fps >= 30.0f)
            {
                fpsColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); // Yellow — acceptable
            }
            else
            {
                fpsColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red — poor
            }
            ImGui::TextColored(fpsColor, ICON_FA_BOLT " %.1f FPS", fps);

            // Show multi-step remaining if active
            uint32_t stepsRemaining = m_playModeManager->GetMultiStepRemaining();
            if (stepsRemaining > 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), ICON_FA_FORWARD " %u steps left", stepsRemaining);
            }
        }
    }

} // namespace SparkEditor
