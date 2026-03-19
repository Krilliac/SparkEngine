/**
 * @file ProjectSettingsPanel.cpp
 * @brief Implementation of the project settings editor panel
 */

#include "ProjectSettingsPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineSettings.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    ProjectSettingsPanel::ProjectSettingsPanel() : EditorPanel("Project Settings", "project_settings_panel") {}

    bool ProjectSettingsPanel::Initialize()
    {
        std::cout << "Initializing Project Settings panel\n";
        return true;
    }

    void ProjectSettingsPanel::Update(float /*deltaTime*/) {}

    void ProjectSettingsPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            // Tab bar for categories
            if (ImGui::BeginTabBar("SettingsTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_DESKTOP " Graphics"))
                {
                    RenderGraphicsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_VOLUME_UP " Audio"))
                {
                    RenderAudioTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_ATOM " Physics"))
                {
                    RenderPhysicsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_BRAIN " AI"))
                {
                    RenderAITab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GAMEPAD " Gameplay"))
                {
                    RenderGameplayTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_VIDEO " Camera"))
                {
                    RenderCameraTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_PENCIL_ALT " Editor"))
                {
                    RenderEditorTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_INFO_CIRCLE " Project"))
                {
                    RenderProjectInfoTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void ProjectSettingsPanel::Shutdown()
    {
        std::cout << "Shutting down Project Settings panel\n";
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void ProjectSettingsPanel::RenderToolbar()
    {
        auto& settings = EngineSettings::GetInstance();

        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            settings.Save();
            m_modified = false;
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO " Reset to Defaults"))
        {
            settings.ResetToDefaults();
            m_modified = true;
        }

        if (m_modified)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), ICON_FA_EXCLAMATION_TRIANGLE " Unsaved changes");
        }
    }

    // =========================================================================
    // Graphics Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderGraphicsTab()
    {
        auto& gfx = EngineSettings::GetInstance().Graphics();

        if (ImGui::DragInt("Window Width", &gfx.windowWidth, 1, 640, 7680))
            m_modified = true;
        if (ImGui::DragInt("Window Height", &gfx.windowHeight, 1, 480, 4320))
            m_modified = true;
        if (ImGui::Checkbox("Fullscreen", &gfx.fullscreen))
            m_modified = true;
        if (ImGui::Checkbox("VSync", &gfx.vsync))
            m_modified = true;

        const char* aaItems[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        int aaIndex = 0;
        if (gfx.antiAliasing == 2)
            aaIndex = 1;
        else if (gfx.antiAliasing == 4)
            aaIndex = 2;
        else if (gfx.antiAliasing >= 8)
            aaIndex = 3;
        if (ImGui::Combo("Anti-Aliasing", &aaIndex, aaItems, 4))
        {
            const int aaMap[] = {1, 2, 4, 8};
            gfx.antiAliasing = aaMap[aaIndex];
            m_modified = true;
        }

        const char* shadowItems[] = {"Off", "Low", "Medium", "High"};
        if (ImGui::Combo("Shadow Quality", &gfx.shadowQuality, shadowItems, 4))
            m_modified = true;

        if (ImGui::SliderFloat("Render Scale", &gfx.renderScale, 0.25f, 2.0f, "%.2f"))
            m_modified = true;
        if (ImGui::Checkbox("HDR", &gfx.hdr))
            m_modified = true;
    }

    // =========================================================================
    // Audio Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAudioTab()
    {
        auto& audio = EngineSettings::GetInstance().Audio();

        if (ImGui::SliderFloat("Master Volume", &audio.masterVolume, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::SliderFloat("SFX Volume", &audio.sfxVolume, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::SliderFloat("Music Volume", &audio.musicVolume, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::SliderFloat("Voice Volume", &audio.voiceVolume, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::Checkbox("Mute on Focus Loss", &audio.muteOnFocusLoss))
            m_modified = true;
    }

    // =========================================================================
    // Physics Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderPhysicsTab()
    {
        auto& phys = EngineSettings::GetInstance().Physics();

        ImGui::Text("Gravity");
        if (ImGui::DragFloat("Gravity X", &phys.gravityX, 0.1f, -100.0f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Gravity Y", &phys.gravityY, 0.1f, -100.0f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Gravity Z", &phys.gravityZ, 0.1f, -100.0f, 100.0f))
            m_modified = true;

        ImGui::Separator();
        if (ImGui::DragFloat("Fixed Timestep", &phys.fixedTimestep, 0.001f, 0.001f, 0.1f, "%.4f"))
            m_modified = true;
        if (ImGui::DragInt("Max Sub-steps", &phys.maxSubSteps, 1, 1, 16))
            m_modified = true;
        if (ImGui::DragFloat("Default Friction", &phys.defaultFriction, 0.01f, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::DragFloat("Default Restitution", &phys.defaultRestitution, 0.01f, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::Checkbox("Debug Draw", &phys.debugDraw))
            m_modified = true;
    }

    // =========================================================================
    // AI Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAITab()
    {
        auto& ai = EngineSettings::GetInstance().AI();

        if (ImGui::DragFloat("Detection Range", &ai.detectionRange, 0.5f, 1.0f, 200.0f))
            m_modified = true;
        if (ImGui::DragFloat("Attack Range", &ai.attackRange, 0.5f, 1.0f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Melee Range", &ai.meleeRange, 0.1f, 0.5f, 10.0f))
            m_modified = true;
        if (ImGui::DragFloat("Move Speed", &ai.moveSpeed, 0.1f, 0.1f, 50.0f))
            m_modified = true;
        if (ImGui::DragFloat("Turn Speed", &ai.turnSpeed, 1.0f, 10.0f, 720.0f, "%.0f deg/s"))
            m_modified = true;
        if (ImGui::SliderFloat("Accuracy", &ai.accuracy, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::DragFloat("Reaction Time", &ai.reactionTime, 0.01f, 0.0f, 5.0f, "%.2f s"))
            m_modified = true;
        if (ImGui::DragFloat("Cover Search Radius", &ai.coverSearchRadius, 0.5f, 1.0f, 100.0f))
            m_modified = true;
        if (ImGui::Checkbox("Can Strafe", &ai.canStrafe))
            m_modified = true;
        if (ImGui::Checkbox("Can Sprint", &ai.canSprint))
            m_modified = true;
        if (ImGui::Checkbox("Can Use Cover", &ai.canUseCover))
            m_modified = true;
    }

    // =========================================================================
    // Gameplay Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderGameplayTab()
    {
        auto& gm = EngineSettings::GetInstance().GameMode();

        if (ImGui::CollapsingHeader("Scoring", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragInt("Score Limit", &gm.scoreLimit, 1, 0, 1000))
                m_modified = true;
            if (ImGui::DragInt("Kill Points", &gm.killPoints, 1, 0, 500))
                m_modified = true;
            if (ImGui::DragInt("Assist Points", &gm.assistPoints, 1, 0, 500))
                m_modified = true;
            if (ImGui::DragInt("Objective Points", &gm.objectivePoints, 1, 0, 1000))
                m_modified = true;
            if (ImGui::DragInt("Headshot Bonus", &gm.headshotBonus, 1, 0, 200))
                m_modified = true;
            if (ImGui::DragInt("Death Penalty", &gm.deathPenalty, 1, 0, 200))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Rules", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragInt("Round Limit", &gm.roundLimit, 1, 0, 100))
                m_modified = true;
            if (ImGui::DragFloat("Time Limit", &gm.timeLimit, 1.0f, 0.0f, 3600.0f, "%.0f s"))
                m_modified = true;
            if (ImGui::DragFloat("Respawn Delay", &gm.respawnDelay, 0.1f, 0.0f, 30.0f, "%.1f s"))
                m_modified = true;
            if (ImGui::Checkbox("Auto Respawn", &gm.autoRespawn))
                m_modified = true;
            if (ImGui::Checkbox("Friendly Fire", &gm.friendlyFire))
                m_modified = true;
            if (ImGui::Checkbox("Headshots", &gm.headshots))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Multipliers"))
        {
            if (ImGui::DragFloat("Damage Multiplier", &gm.damageMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Health Multiplier", &gm.healthMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Speed Multiplier", &gm.speedMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Headshot Multiplier", &gm.headshotMultiplier, 0.1f, 1.0f, 10.0f))
                m_modified = true;
        }
    }

    // =========================================================================
    // Camera Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderCameraTab()
    {
        auto& cam = EngineSettings::GetInstance().Camera();

        if (ImGui::DragFloat("Move Speed", &cam.moveSpeed, 0.1f, 0.1f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Rotation Speed", &cam.rotationSpeed, 0.1f, 0.1f, 20.0f))
            m_modified = true;
        if (ImGui::DragFloat("Default FOV", &cam.defaultFov, 1.0f, 30.0f, 150.0f, "%.0f"))
            m_modified = true;
        if (ImGui::DragFloat("Zoomed FOV", &cam.zoomedFov, 1.0f, 10.0f, 90.0f, "%.0f"))
            m_modified = true;
        if (ImGui::Checkbox("Smooth Movement", &cam.smoothMovement))
            m_modified = true;
        if (ImGui::DragFloat("Near Plane", &cam.nearPlane, 0.01f, 0.001f, 10.0f, "%.3f"))
            m_modified = true;
        if (ImGui::DragFloat("Far Plane", &cam.farPlane, 10.0f, 10.0f, 100000.0f, "%.0f"))
            m_modified = true;
    }

    // =========================================================================
    // Editor Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderEditorTab()
    {
        auto& ed = EngineSettings::GetInstance().Editor();

        if (ImGui::DragFloat("Grid Size", &ed.gridSize, 0.1f, 0.1f, 100.0f))
            m_modified = true;
        if (ImGui::Checkbox("Snap to Grid", &ed.snapToGrid))
            m_modified = true;
        if (ImGui::Checkbox("Show Grid", &ed.showGrid))
            m_modified = true;
        if (ImGui::DragFloat("Gizmo Scale", &ed.gizmoScale, 0.1f, 0.1f, 10.0f))
            m_modified = true;
        if (ImGui::Checkbox("Autosave Enabled", &ed.autosaveEnabled))
            m_modified = true;
        if (ImGui::DragFloat("Autosave Interval", &ed.autosaveIntervalSeconds, 10.0f, 30.0f, 3600.0f, "%.0f s"))
            m_modified = true;
        if (ImGui::DragInt("Undo History Size", &ed.undoHistorySize, 1, 10, 1000))
            m_modified = true;
    }

    // =========================================================================
    // Project Info Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderProjectInfoTab()
    {
        ImGui::Text("Engine: SparkEngine");
        ImGui::Text("Settings File: %s", EngineSettings::GetInstance().GetFilePath().c_str());
        ImGui::Separator();

        ImGui::TextWrapped("Project settings are loaded from settings.ini at startup and saved on demand. "
                           "Changes made here take effect immediately for most subsystems.");
    }

} // namespace SparkEditor
