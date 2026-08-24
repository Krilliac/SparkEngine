/**
 * @file EditorCommandRegistry.cpp
 * @brief Command palette registration for the SparkEditor UI.
 *
 * Contains the EditorUI command-registration methods that populate the
 * CommandPalette with panel toggles, edit operations, scene commands,
 * and tool shortcuts. Split from EditorUI.cpp for maintainability —
 * these methods are data-driven registration and have no rendering
 * responsibilities.
 *
 * Follows the same sibling-file pattern as EditorMenuBar.cpp.
 */
#include "EditorUI.h"

#include "../Search/CommandPalette.h"
#include "../UndoRedo/UndoRedoManager.h"
#include "../Prefabs/PrefabManager.h"

namespace SparkEditor
{

    void EditorUI::InitializeCommandPalette()
    {
        if (!m_commandPalette)
        {
            return;
        }

        RegisterPanelToggleCommands();
        RegisterEditCommands();
        RegisterSceneCommands();
        RegisterToolCommands();
    }

    void EditorUI::RegisterPanelToggleCommands()
    {
        auto RegisterPanelToggle = [this](const std::string& panelKey, const std::string& displayName)
        {
            m_commandPalette->RegisterAction("Toggle " + displayName, "Panel", [this, panelKey]()
                                             { SetPanelVisible(panelKey, !IsPanelVisible(panelKey)); });
        };

        // Core panels
        RegisterPanelToggle("SceneView", "Scene View");
        RegisterPanelToggle("Console", "Console");
        RegisterPanelToggle("Hierarchy", "Hierarchy");
        RegisterPanelToggle("Inspector", "Inspector");
        RegisterPanelToggle("AssetBrowser", "Asset Browser");
        RegisterPanelToggle("GameView", "Game View");
        RegisterPanelToggle("Profiler", "Profiler");

        // FPS / gameplay panels
        RegisterPanelToggle("WeaponEditor", "Weapon Editor");
        RegisterPanelToggle("FPSTools", "FPS Tools");

        // 2D panels
        RegisterPanelToggle("SpriteEditor", "Sprite Editor");
        RegisterPanelToggle("TilemapEditor", "Tilemap Editor");
        RegisterPanelToggle("SpriteAnimEditor", "Sprite Animation Editor");
        RegisterPanelToggle("Physics2D", "Physics 2D");
        RegisterPanelToggle("Physics3D", "Physics 3D");

        // Editor utility panels
        RegisterPanelToggle("UndoHistory", "Undo History");
        RegisterPanelToggle("SceneStats", "Scene Statistics");
        RegisterPanelToggle("PrefabEditor", "Prefab Editor");
        RegisterPanelToggle("Search", "Search");
        RegisterPanelToggle("PostProcessing", "Post Processing");

        // Domain-specific panels
        RegisterPanelToggle("DialogueEditor", "Dialogue Editor");
        RegisterPanelToggle("AIEditor", "AI Editor");
        RegisterPanelToggle("SplineEditor", "Spline Editor");
        RegisterPanelToggle("ParticleEditor", "Particle Editor");
        RegisterPanelToggle("EventMonitor", "Event Monitor");
        RegisterPanelToggle("SaveSystem", "Save System");
        RegisterPanelToggle("Localization", "Localization");
        RegisterPanelToggle("WeatherFog", "Weather & Fog");
        RegisterPanelToggle("CinematicSequencer", "Cinematic Sequencer");
        RegisterPanelToggle("ProjectSettings", "Project Settings");
    }

    void EditorUI::RegisterEditCommands()
    {
        // Undo / redo — route through the process-wide CommandHistory, the
        // same history every edit surface executes into and the one
        // SwapWorld() clears on scene replacement.
        m_commandPalette->RegisterAction(
            "Undo", "Command",
            []()
            {
                auto& history = Spark::Editor::CommandHistory::GetInstance();
                if (history.CanUndo())
                {
                    history.Undo();
                }
            },
            "Ctrl+Z");

        m_commandPalette->RegisterAction(
            "Redo", "Command",
            []()
            {
                auto& history = Spark::Editor::CommandHistory::GetInstance();
                if (history.CanRedo())
                {
                    history.Redo();
                }
            },
            "Ctrl+Y");

        // Layout commands
        m_commandPalette->RegisterAction("Reset Layout", "Layout", [this]() { ResetToDefaultLayout(); });
        m_commandPalette->RegisterAction("Save Layout", "Layout", [this]() { SaveLayout("Quick Save"); });

        // Prefab commands
        m_commandPalette->RegisterAction("Create Empty Prefab", "Command",
                                         [this]()
                                         {
                                             if (m_prefabManager)
                                             {
                                                 m_prefabManager->CreateEmptyPrefab("New Prefab");
                                                 SetPanelVisible("PrefabEditor", true);
                                                 ShowNotification("Created new prefab", "success");
                                             }
                                         });
    }

    void EditorUI::RegisterSceneCommands()
    {
        m_commandPalette->RegisterAction("New Scene", "Scene", [this]() { NewScene(); });

        m_commandPalette->RegisterAction("Save Scene", "Scene", [this]() { SaveScene(); });

        // Play mode
        m_commandPalette->RegisterAction(
            "Play", "Command",
            [this]()
            {
                if (m_playModeManager.IsPaused())
                    m_playModeManager.ResumePlayMode();
                else if (m_playModeManager.IsStopped())
                    m_playModeManager.EnterPlayMode();

                m_playMode = m_playModeManager.IsPlaying()
                                 ? PlayMode::Playing
                                 : (m_playModeManager.IsSimulating() ? PlayMode::Simulating
                                                                    : (m_playModeManager.IsPaused() ? PlayMode::Paused
                                                                                                    : PlayMode::Stopped));
                const bool running = m_playMode != PlayMode::Stopped;
                ShowNotification(running ? "Running..." : "Unable to enter play mode", running ? "success" : "error");
            },
            "F5");

        m_commandPalette->RegisterAction(
            "Stop", "Command",
            [this]()
            {
                m_playModeManager.ExitPlayMode();
                m_playMode = PlayMode::Stopped;
                ShowNotification("Stopped", "info");
            },
            "Shift+F5");
    }

    void EditorUI::RegisterToolCommands()
    {
        // Theme commands
        m_commandPalette->RegisterAction("Theme: Spark Fusion", "Command", [this]() { ApplyTheme("Spark Fusion"); });
        m_commandPalette->RegisterAction("Theme: Spark Professional", "Command",
                                         [this]() { ApplyTheme("Spark Professional"); });
        m_commandPalette->RegisterAction("Theme: Spark Ember", "Command", [this]() { ApplyTheme("Spark Ember"); });
        m_commandPalette->RegisterAction("Theme: Professional Light", "Command",
                                         [this]() { ApplyTheme("Professional Light"); });

        // Transform tool commands
        m_commandPalette->RegisterAction(
            "Tool: Move", "Command", [this]() { m_currentTool = TransformTool::Move; }, "W");
        m_commandPalette->RegisterAction(
            "Tool: Rotate", "Command", [this]() { m_currentTool = TransformTool::Rotate; }, "E");
        m_commandPalette->RegisterAction(
            "Tool: Scale", "Command", [this]() { m_currentTool = TransformTool::Scale; }, "R");

        m_commandPalette->RegisterAction("Toggle Snap", "Command", [this]() { m_snapEnabled = !m_snapEnabled; });
    }

} // namespace SparkEditor
