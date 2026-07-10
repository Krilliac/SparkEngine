/**
 * @file EditorPanelFactory.cpp
 * @brief Panel creation for the SparkEditor UI
 *
 * Contains EditorUI::CreatePanels() which instantiates all editor panels.
 * Split from EditorUI.cpp for maintainability.
 */
#include "EditorUI.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "../Panels/SceneViewPanel.h"
#include "../Panels/ConsolePanel.h"
#include "../Panels/HierarchyPanel.h"
#include "../Panels/InspectorPanel.h"
#include "../Panels/AssetBrowserPanel.h"
#include "../Panels/GameViewPanel.h"
#include "../Panels/WeaponEditorPanel.h"
#include "../Panels/FPSToolsPanel.h"
#include "../Panels/DebugVisualizerPanel.h"
#include "../Panels/ObjectPlacementPanel.h"
#include "../Panels/BuildCookPanel.h"
// Phase AA Theme 3C: ProjectBrowserPanel is instantiated separately in
// EditorUI::Initialize as `m_projectBrowserPanel` (a modal overlay, not
// a dockable panel), so it does not need to be included here. The stale
// include has been removed.
#include "../Panels/SpriteEditorPanel.h"
#include "../Panels/TilemapEditorPanel.h"
#include "../Panels/SpriteAnimationEditorPanel.h"
#include "../Panels/Physics2DPanel.h"
#include "../Panels/Physics3DPanel.h"
#include "../Panels/UndoHistoryPanel.h"
#include "../Panels/SceneStatisticsPanel.h"
#include "../Panels/PrefabEditorPanel.h"
#include "../Panels/SearchPanel.h"
#include "../Panels/DedicatedServerPanel.h"
#include "../Panels/MaterialEditorPanel.h"
#include "../Panels/PlayModeToolbarPanel.h"
#include "../Panels/PostProcessingPanel.h"
#include "../Panels/DialogueEditorPanel.h"
#include "../Panels/AIDebugPanel.h"
#include "../Panels/AIEditorPanel.h"
#include "../Panels/SplineEditorPanel.h"
#include "../Panels/ParticleEditorPanel.h"
#include "../Panels/EventMonitorPanel.h"
#include "../Panels/SaveSystemPanel.h"
#include "../Panels/LocalizationPanel.h"
#include "../Panels/WeatherFogPanel.h"
#include "../Panels/CinematicSequencerPanel.h"
#include "../Panels/ProjectSettingsPanel.h"
#include "../Panels/AudioMixerPanel.h"
#include "../Panels/ScriptEditorPanel.h"
#include "../Panels/DestructionEditorPanel.h"
#include "../Panels/ReplayPanel.h"
#include "../Panels/VRConfigPanel.h"
#include "../Panels/StreamingPanel.h"
#include "../Panels/ModdingPanel.h"
#include "../Panels/CoroutineDebugPanel.h"
#include "../Panels/GameModuleSelectorPanel.h"
#include "../Panels/CollaborationPanel.h"
#include "../Panels/TimeOfDayPanel.h"
#include "../Panels/AbilityEditorPanel.h"
#include "../Panels/TriggerEditorPanel.h"
#include "../Panels/ConditionEditorPanel.h"
#include "../Panels/DecalEditorPanel.h"
#include "../Panels/EventResponsePanel.h"
#include "../Panels/VisualScriptPanel.h"
#include "../Panels/WorkflowPanel.h"
#include "../Panels/PrototypingPanel.h"
#include "../Panels/UIDesignerPanel.h"
#include "../Panels/ScriptDebugPanel.h"
#include "../Panels/CSGEditorPanel.h"
#include "../Panels/NetworkDebugPanel.h"
#include "../Terrain/TerrainEditor.h"
#include "../Profiler/PerformanceProfiler.h"
// Phase AA Theme 3C: activate the two remaining orphan EditorPanel
// subclasses that live outside the Panels/ directory. Both were
// EditorPanel-derived with real Initialize / Update / Render / Shutdown
// overrides but zero production call sites, which made them
// unreachable from the Window menu and the dockspace.
#include "../LevelStreaming/LevelStreamingSystem.h"
#include "../VersionControl/VersionControlSystem.h"
#include "EditorIcons.h"
#include <imgui.h>

namespace SparkEditor
{

    void EditorUI::CreateCorePanels(
        const std::function<void(const std::string&, std::shared_ptr<EditorPanel>)>& registerPanel)
    {
        // Construct each panel INSIDE the try so a throwing panel constructor is
        // isolated and logged instead of propagating out of this helper and aborting
        // creation of every remaining panel. registerPanel() only ever receives a
        // fully-constructed panel.
        auto tryRegister = [&](const std::string& name, auto&& factory)
        {
            try
            {
                registerPanel(name, factory());
            }
            catch (const std::exception& e)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to construct panel '%s': %s", name.c_str(),
                                e.what());
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to construct panel '%s': unknown exception",
                                name.c_str());
            }
        };

        tryRegister("SceneView", [&] { return std::make_shared<SceneViewPanel>(); });
        tryRegister("Console", [&] { return std::make_shared<ConsolePanel>(); });
        tryRegister("Hierarchy", [&] { return std::make_shared<HierarchyPanel>(); });
        tryRegister("Inspector", [&] { return std::make_shared<InspectorPanel>(); });
        tryRegister("AssetBrowser", [&] { return std::make_shared<AssetBrowserPanel>(); });
        tryRegister("GameView", [&] { return std::make_shared<GameViewPanel>(); });
        tryRegister("Profiler", [&] { return std::make_shared<PerformanceProfiler>(); });

        tryRegister("WeaponEditor", [&] { return std::make_shared<WeaponEditorPanel>(); });
        tryRegister("FPSTools", [&] { return std::make_shared<FPSToolsPanel>(); });

        tryRegister("SpriteEditor", [&] { return std::make_shared<SpriteEditorPanel>(); });
        tryRegister("TilemapEditor", [&] { return std::make_shared<TilemapEditorPanel>(); });
        tryRegister("SpriteAnimEditor", [&] { return std::make_shared<SpriteAnimationEditorPanel>(); });
        tryRegister("Physics2D", [&] { return std::make_shared<Physics2DPanel>(); });
        tryRegister("Physics3D", [&] { return std::make_shared<Physics3DPanel>(); });

        // Display the process-wide undo/redo history (the singleton wrapped by
        // CommandHistory) — the panel would show a permanently empty stack if
        // it were pointed at any other UndoRedoManager instance.
        tryRegister("UndoHistory", [&] { return std::make_shared<UndoHistoryPanel>(&UndoRedoManager::GetInstance()); });
        tryRegister("PrefabEditor", [&] { return std::make_shared<PrefabEditorPanel>(m_prefabManager.get()); });
    }

    void EditorUI::CreateToolAndContentPanels(
        const std::function<void(const std::string&, std::shared_ptr<EditorPanel>)>& registerPanel)
    {
        // Construct each panel INSIDE the try (see CreateCorePanels) so one panel's
        // throwing constructor cannot abort registration of the rest.
        auto tryRegister = [&](const std::string& name, auto&& factory)
        {
            try
            {
                registerPanel(name, factory());
            }
            catch (const std::exception& e)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to construct panel '%s': %s", name.c_str(),
                                e.what());
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to construct panel '%s': unknown exception",
                                name.c_str());
            }
        };

        tryRegister("SceneStats", [&] { return std::make_shared<SceneStatisticsPanel>(); });
        tryRegister("Search", [&] { return std::make_shared<SearchPanel>(); });
        tryRegister("DedicatedServer", [&] { return std::make_shared<DedicatedServerPanel>(); });
        tryRegister("DebugVisualizer", [&] { return std::make_shared<DebugVisualizerPanel>(); });
        tryRegister("ObjectPlacement", [&] { return std::make_shared<ObjectPlacementPanel>(); });
        tryRegister("BuildCook", [&] { return std::make_shared<BuildCookPanel>(); });
        tryRegister("PlayModeToolbar", [&] { return std::make_shared<PlayModeToolbarPanel>(); });
        tryRegister("MaterialEditor", [&] { return std::make_shared<MaterialEditorPanel>(); });
        tryRegister("TerrainEditor", [&] { return std::make_shared<TerrainEditor>(); });

        tryRegister("PostProcessing", [&] { return std::make_shared<PostProcessingPanel>(); });
        tryRegister("DialogueEditor", [&] { return std::make_shared<DialogueEditorPanel>(); });
        tryRegister("AIEditor", [&] { return std::make_shared<AIEditorPanel>(); });
        tryRegister("AIDebug", [&] { return std::make_shared<AIDebugPanel>(); });
        tryRegister("SplineEditor", [&] { return std::make_shared<SplineEditorPanel>(); });
        tryRegister("ParticleEditor", [&] { return std::make_shared<ParticleEditorPanel>(); });
        tryRegister("EventMonitor", [&] { return std::make_shared<EventMonitorPanel>(); });
        tryRegister("SaveSystem", [&] { return std::make_shared<SaveSystemPanel>(); });
        tryRegister("Localization", [&] { return std::make_shared<LocalizationPanel>(); });
        tryRegister("WeatherFog", [&] { return std::make_shared<WeatherFogPanel>(); });
        tryRegister("CinematicSequencer", [&] { return std::make_shared<CinematicSequencerPanel>(); });
        tryRegister("ProjectSettings", [&] { return std::make_shared<ProjectSettingsPanel>(); });

        tryRegister("AudioMixer", [&] { return std::make_shared<AudioMixerPanel>(); });
        tryRegister("ScriptEditor", [&] { return std::make_shared<ScriptEditorPanel>(); });
        tryRegister("DestructionEditor", [&] { return std::make_shared<DestructionEditorPanel>(); });
        tryRegister("Replay", [&] { return std::make_shared<ReplayPanel>(); });
        tryRegister("VRConfig", [&] { return std::make_shared<VRConfigPanel>(); });
        tryRegister("Streaming", [&] { return std::make_shared<StreamingPanel>(); });
        tryRegister("Modding", [&] { return std::make_shared<ModdingPanel>(); });
        tryRegister("CoroutineDebug", [&] { return std::make_shared<CoroutineDebugPanel>(); });
        tryRegister("GameModuleSelector", [&] { return std::make_shared<GameModuleSelectorPanel>(); });
        tryRegister("Collaboration", [&] { return std::make_shared<CollaborationPanel>(m_collabSession.get()); });

        tryRegister("TimeOfDay", [&] { return std::make_shared<TimeOfDayPanel>(); });
        tryRegister("AbilityEditor", [&] { return std::make_shared<AbilityEditorPanel>(); });
        tryRegister("TriggerEditor", [&] { return std::make_shared<TriggerEditorPanel>(); });
        tryRegister("ConditionEditor", [&] { return std::make_shared<ConditionEditorPanel>(); });
        tryRegister("DecalEditor", [&] { return std::make_shared<DecalEditorPanel>(); });
        tryRegister("EventResponses", [&] { return std::make_shared<EventResponsePanel>(); });
        tryRegister("VisualScript", [&] { return std::make_shared<VisualScriptPanel>(); });

        tryRegister("Prototyping", [&] { return std::make_shared<PrototypingPanel>(); });
        tryRegister("UIDesigner", [&] { return std::make_shared<UIDesignerPanel>(); });
        tryRegister("ScriptDebugger", [&] { return std::make_shared<ScriptDebugPanel>(); });

        tryRegister("CSGEditor", [&] { return std::make_shared<CSGEditorPanel>(); });
        tryRegister("NetworkDebug", [&] { return std::make_shared<NetworkDebugPanel>(); });

        // Phase AA Theme 3C: register the two orphaned EditorPanel
        // subclasses that live outside Panels/ so they appear in the
        // Window menu alongside every other panel.
        tryRegister("LevelStreaming", [&] { return std::make_shared<LevelStreamingSystem>(); });
        tryRegister("VersionControl", [&] { return std::make_shared<VersionControlSystem>(); });

        tryRegister("Workflows",
                    [&]
                    {
                        auto workflowPanel = std::make_shared<WorkflowPanel>();
                        workflowPanel->SetEditorUI(this);
                        return workflowPanel;
                    });
    }

    void EditorUI::InitializePanelIcons()
    {
        struct PanelIcon
        {
            const char* name;
            const char* icon;
        };
        constexpr PanelIcon panelIcons[] = {
            {"SceneView", ICON_FA_CAMERA},
            {"Console", ICON_FA_TERMINAL},
            {"Hierarchy", ICON_FA_SITEMAP},
            {"Inspector", ICON_FA_SLIDERS},
            {"AssetBrowser", ICON_FA_FOLDER},
            {"GameView", ICON_FA_GAMEPAD},
            {"Profiler", ICON_FA_CHART_BAR},
            {"WeaponEditor", ICON_FA_CROSSHAIRS},
            {"FPSTools", ICON_FA_ROCKET},
            {"DebugVisualizer", ICON_FA_BUG},
            {"SceneStats", ICON_FA_CHART_BAR},
            {"ObjectPlacement", ICON_FA_CUBE},
            {"BuildCook", ICON_FA_HAMMER},
            {"DedicatedServer", ICON_FA_SERVER},
            {"TerrainEditor", ICON_FA_MOUNTAIN},
            {"CinematicSequencer", ICON_FA_FILM},
            {"ProjectSettings", ICON_FA_COGS},
            {"UndoHistory", ICON_FA_UNDO},
            {"PrefabEditor", ICON_FA_CUBE},
            {"Search", ICON_FA_SEARCH},
            {"PostProcessing", ICON_FA_MAGIC},
            {"DialogueEditor", ICON_FA_COMMENTS},
            {"AIEditor", ICON_FA_BRAIN},
            {"AIDebug", ICON_FA_BUG},
            {"SplineEditor", ICON_FA_BEZIER_CURVE},
            {"ParticleEditor", ICON_FA_FIRE},
            {"EventMonitor", ICON_FA_BOLT},
            {"SaveSystem", ICON_FA_SAVE},
            {"Localization", ICON_FA_GLOBE},
            {"WeatherFog", ICON_FA_CLOUD_SUN},
            {"AudioMixer", ICON_FA_VOLUME_UP},
            {"ScriptEditor", ICON_FA_CODE},
            {"DestructionEditor", ICON_FA_BOMB},
            {"Replay", ICON_FA_FILM},
            {"VRConfig", ICON_FA_GAMEPAD},
            {"Streaming", ICON_FA_MAP},
            {"Modding", ICON_FA_BOXES},
            {"CoroutineDebug", ICON_FA_CLOCK},
            {"GameModuleSelector", ICON_FA_PUZZLE_PIECE},
            {"Collaboration", ICON_FA_USERS},
            {"TimeOfDay", ICON_FA_SUN},
            {"AbilityEditor", ICON_FA_MAGIC},
            {"TriggerEditor", ICON_FA_CROSSHAIRS},
            {"ConditionEditor", ICON_FA_CHECK_CIRCLE},
            {"DecalEditor", ICON_FA_STAMP},
            {"EventResponses", ICON_FA_BOLT},
            {"VisualScript", ICON_FA_PROJECT_DIAGRAM},
            {"Workflows", ICON_FA_COGS},
            {"Prototyping", ICON_FA_CUBES},
            {"UIDesigner", ICON_FA_COLUMNS},
            {"ScriptDebugger", ICON_FA_BUG},
            {"CSGEditor", ICON_FA_CUBES},
            {"NetworkDebug", ICON_FA_NETWORK_WIRED},
            // Phase AA Theme 3C: icons for the two newly-activated panels.
            {"LevelStreaming", ICON_FA_MAP},
            {"VersionControl", ICON_FA_CODE_BRANCH},
        };

        for (const auto& [name, icon] : panelIcons)
        {
            if (m_panels.count(name))
                m_panels[name]->SetIcon(icon);
        }
    }

    void EditorUI::InitializePanelCategories()
    {
        struct PanelCat
        {
            const char* name;
            PanelCategory category;
        };
        constexpr PanelCat panelCategories[] = {
            // Viewports
            {"SceneView", PanelCategory::Viewport},
            {"GameView", PanelCategory::Viewport},
            {"TilemapEditor", PanelCategory::Viewport},
            {"CSGEditor", PanelCategory::Viewport},

            // Inspectors
            {"Inspector", PanelCategory::Inspector},
            {"Hierarchy", PanelCategory::Inspector},
            {"ProjectSettings", PanelCategory::Inspector},
            {"UndoHistory", PanelCategory::Inspector},
            {"ObjectPlacement", PanelCategory::Inspector},
            {"UIDesigner", PanelCategory::Inspector},
            {"Search", PanelCategory::Inspector},

            // Tools
            {"AssetBrowser", PanelCategory::Tool},
            {"MaterialEditor", PanelCategory::Tool},
            {"SpriteEditor", PanelCategory::Tool},
            {"SpriteAnimEditor", PanelCategory::Tool},
            {"ParticleEditor", PanelCategory::Tool},
            {"DialogueEditor", PanelCategory::Tool},
            {"AIEditor", PanelCategory::Tool},
            {"SplineEditor", PanelCategory::Tool},
            {"WeaponEditor", PanelCategory::Tool},
            {"AbilityEditor", PanelCategory::Tool},
            {"TriggerEditor", PanelCategory::Tool},
            {"ConditionEditor", PanelCategory::Tool},
            {"DecalEditor", PanelCategory::Tool},
            {"DestructionEditor", PanelCategory::Tool},
            {"CinematicSequencer", PanelCategory::Tool},
            {"AudioMixer", PanelCategory::Tool},
            {"ScriptEditor", PanelCategory::Tool},
            {"PostProcessing", PanelCategory::Tool},
            {"TerrainEditor", PanelCategory::Tool},
            {"PrefabEditor", PanelCategory::Tool},
            {"VisualScript", PanelCategory::Tool},
            {"FPSTools", PanelCategory::Tool},
            {"PlayModeToolbar", PanelCategory::Tool},
            {"BuildCook", PanelCategory::Tool},
            {"Prototyping", PanelCategory::Tool},
            {"Workflows", PanelCategory::Tool},
            {"EventResponses", PanelCategory::Tool},

            // Config
            {"SaveSystem", PanelCategory::Config},
            {"Localization", PanelCategory::Config},
            {"WeatherFog", PanelCategory::Config},
            {"VRConfig", PanelCategory::Config},
            {"Streaming", PanelCategory::Config},
            {"Modding", PanelCategory::Config},
            {"Collaboration", PanelCategory::Config},
            {"TimeOfDay", PanelCategory::Config},
            {"DedicatedServer", PanelCategory::Config},
            {"GameModuleSelector", PanelCategory::Config},
            {"LevelStreaming", PanelCategory::Config},
            {"VersionControl", PanelCategory::Config},

            // Debug
            {"Console", PanelCategory::Debug},
            {"Profiler", PanelCategory::Debug},
            {"AIDebug", PanelCategory::Debug},
            {"ScriptDebugger", PanelCategory::Debug},
            {"DebugVisualizer", PanelCategory::Debug},
            {"EventMonitor", PanelCategory::Debug},
            {"SceneStats", PanelCategory::Debug},
            {"Physics2D", PanelCategory::Debug},
            {"Physics3D", PanelCategory::Debug},
            {"NetworkDebug", PanelCategory::Debug},
            {"CoroutineDebug", PanelCategory::Debug},
            {"Replay", PanelCategory::Debug},
        };

        for (const auto& [name, cat] : panelCategories)
        {
            if (m_panels.count(name))
                m_panels[name]->SetPanelCategory(cat);
        }
    }

    void EditorUI::SetDefaultPanelVisibility()
    {
        // On first launch, only show the essential panels that form a clean,
        // non-overlapping workspace. All other panels can be enabled from the
        // Window menu. This prevents the overwhelming "everything stacked on
        // everything" experience for new users.
        const char* visibleByDefault[] = {
            "SceneView", "Hierarchy", "Inspector", "Console", "AssetBrowser", "GameView",
        };

        // Start with everything hidden, then enable the defaults
        for (auto& [name, panel] : m_panels)
        {
            panel->SetVisible(false);
        }

        for (const char* name : visibleByDefault)
        {
            if (m_panels.count(name))
                m_panels[name]->SetVisible(true);
        }
    }

    void EditorUI::CreatePanels()
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Creating editor panels...");

        bool isDebuggerPresent = false;
#ifdef _WIN32
        isDebuggerPresent = IsDebuggerPresent();
#endif

        auto registerPanel = [&](const std::string& name, std::shared_ptr<EditorPanel> panel)
        {
            try
            {
                m_panels[name] = std::move(panel);
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Registered panel: %s", name.c_str());
                console.LogSuccess("Created " + name + " panel");
            }
            catch (const std::exception& e)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create panel '%s': %s", name.c_str(), e.what());
                console.LogError("Failed to create " + name + " panel: " + std::string(e.what()));
            }
        };

        if (isDebuggerPresent)
        {
            console.LogWarning("DEBUGGER DETECTED - Using minimal panel set to avoid deadlocks");
            registerPanel("SceneView", std::make_shared<SceneViewPanel>());
        }
        else
        {
            console.LogInfo("Creating full panel set...");
            CreateCorePanels(registerPanel);
            CreateToolAndContentPanels(registerPanel);
        }

        for (auto& [name, panel] : m_panels)
        {
            try
            {
                console.LogInfo("Initializing " + name + " panel");
                if (panel && panel->Initialize())
                {
                    console.LogSuccess("Initialized " + name + " panel");
                }
                else
                {
                    console.LogError("Failed to initialize " + name + " panel");
                }
            }
            catch (const std::exception& e)
            {
                console.LogError("Exception initializing " + name + " panel: " + std::string(e.what()));
            }
        }

        // Wire play/sim manager into dependent panels after initialization.
        if (auto it = m_panels.find("PlayModeToolbar"); it != m_panels.end())
        {
            if (auto* toolbar = dynamic_cast<PlayModeToolbarPanel*>(it->second.get()))
            {
                toolbar->SetPlayModeManager(&m_playModeManager);
            }
        }
        if (auto it = m_panels.find("DedicatedServer"); it != m_panels.end())
        {
            if (auto* dedicatedServer = dynamic_cast<DedicatedServerPanel*>(it->second.get()))
            {
                dedicatedServer->SetPlayModeManager(&m_playModeManager);
            }
        }

        InitializePanelIcons();
        InitializePanelCategories();
        SetDefaultPanelVisibility();

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Panel creation complete: %zu panels registered", m_panels.size());
        console.LogSuccess("Created " + std::to_string(m_panels.size()) + " editor panels");
    }

} // namespace SparkEditor
