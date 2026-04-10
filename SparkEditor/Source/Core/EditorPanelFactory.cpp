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
#include "../Panels/ProjectBrowserPanel.h"
#include "../Panels/DebugVisualizerPanel.h"
#include "../Panels/ObjectPlacementPanel.h"
#include "../Panels/BuildCookPanel.h"
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
#include "EditorIcons.h"
#include <imgui.h>

namespace SparkEditor
{

    void EditorUI::CreateCorePanels(
        const std::function<void(const std::string&, std::shared_ptr<EditorPanel>)>& registerPanel)
    {
        registerPanel("SceneView", std::make_shared<SceneViewPanel>());
        registerPanel("Console", std::make_shared<ConsolePanel>());
        registerPanel("Hierarchy", std::make_shared<HierarchyPanel>());
        registerPanel("Inspector", std::make_shared<InspectorPanel>());
        registerPanel("AssetBrowser", std::make_shared<AssetBrowserPanel>());
        registerPanel("GameView", std::make_shared<GameViewPanel>());
        registerPanel("Profiler", std::make_shared<PerformanceProfiler>());

        registerPanel("WeaponEditor", std::make_shared<WeaponEditorPanel>());
        registerPanel("FPSTools", std::make_shared<FPSToolsPanel>());

        registerPanel("SpriteEditor", std::make_shared<SpriteEditorPanel>());
        registerPanel("TilemapEditor", std::make_shared<TilemapEditorPanel>());
        registerPanel("SpriteAnimEditor", std::make_shared<SpriteAnimationEditorPanel>());
        registerPanel("Physics2D", std::make_shared<Physics2DPanel>());
        registerPanel("Physics3D", std::make_shared<Physics3DPanel>());

        registerPanel("UndoHistory", std::make_shared<UndoHistoryPanel>(m_undoRedoManager.get()));
        registerPanel("PrefabEditor", std::make_shared<PrefabEditorPanel>(m_prefabManager.get()));
    }

    void EditorUI::CreateToolAndContentPanels(
        const std::function<void(const std::string&, std::shared_ptr<EditorPanel>)>& registerPanel)
    {
        registerPanel("SceneStats", std::make_shared<SceneStatisticsPanel>());
        registerPanel("Search", std::make_shared<SearchPanel>());
        registerPanel("DedicatedServer", std::make_shared<DedicatedServerPanel>());
        registerPanel("DebugVisualizer", std::make_shared<DebugVisualizerPanel>());
        registerPanel("ObjectPlacement", std::make_shared<ObjectPlacementPanel>());
        registerPanel("BuildCook", std::make_shared<BuildCookPanel>());
        registerPanel("PlayModeToolbar", std::make_shared<PlayModeToolbarPanel>());
        registerPanel("MaterialEditor", std::make_shared<MaterialEditorPanel>());
        registerPanel("TerrainEditor", std::make_shared<TerrainEditor>());

        registerPanel("PostProcessing", std::make_shared<PostProcessingPanel>());
        registerPanel("DialogueEditor", std::make_shared<DialogueEditorPanel>());
        registerPanel("AIEditor", std::make_shared<AIEditorPanel>());
        registerPanel("AIDebug", std::make_shared<AIDebugPanel>());
        registerPanel("SplineEditor", std::make_shared<SplineEditorPanel>());
        registerPanel("ParticleEditor", std::make_shared<ParticleEditorPanel>());
        registerPanel("EventMonitor", std::make_shared<EventMonitorPanel>());
        registerPanel("SaveSystem", std::make_shared<SaveSystemPanel>());
        registerPanel("Localization", std::make_shared<LocalizationPanel>());
        registerPanel("WeatherFog", std::make_shared<WeatherFogPanel>());
        registerPanel("CinematicSequencer", std::make_shared<CinematicSequencerPanel>());
        registerPanel("ProjectSettings", std::make_shared<ProjectSettingsPanel>());

        registerPanel("AudioMixer", std::make_shared<AudioMixerPanel>());
        registerPanel("ScriptEditor", std::make_shared<ScriptEditorPanel>());
        registerPanel("DestructionEditor", std::make_shared<DestructionEditorPanel>());
        registerPanel("Replay", std::make_shared<ReplayPanel>());
        registerPanel("VRConfig", std::make_shared<VRConfigPanel>());
        registerPanel("Streaming", std::make_shared<StreamingPanel>());
        registerPanel("Modding", std::make_shared<ModdingPanel>());
        registerPanel("CoroutineDebug", std::make_shared<CoroutineDebugPanel>());
        registerPanel("GameModuleSelector", std::make_shared<GameModuleSelectorPanel>());
        registerPanel("Collaboration", std::make_shared<CollaborationPanel>(m_collabSession.get()));

        registerPanel("TimeOfDay", std::make_shared<TimeOfDayPanel>());
        registerPanel("AbilityEditor", std::make_shared<AbilityEditorPanel>());
        registerPanel("TriggerEditor", std::make_shared<TriggerEditorPanel>());
        registerPanel("ConditionEditor", std::make_shared<ConditionEditorPanel>());
        registerPanel("DecalEditor", std::make_shared<DecalEditorPanel>());
        registerPanel("EventResponses", std::make_shared<EventResponsePanel>());
        registerPanel("VisualScript", std::make_shared<VisualScriptPanel>());

        registerPanel("Prototyping", std::make_shared<PrototypingPanel>());
        registerPanel("UIDesigner", std::make_shared<UIDesignerPanel>());
        registerPanel("ScriptDebugger", std::make_shared<ScriptDebugPanel>());

        registerPanel("CSGEditor", std::make_shared<CSGEditorPanel>());
        registerPanel("NetworkDebug", std::make_shared<NetworkDebugPanel>());

        auto workflowPanel = std::make_shared<WorkflowPanel>();
        workflowPanel->SetEditorUI(this);
        registerPanel("Workflows", workflowPanel);
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
        };

        for (const auto& [name, icon] : panelIcons)
        {
            if (m_panels.count(name))
                m_panels[name]->SetIcon(icon);
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
        SetDefaultPanelVisibility();

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Panel creation complete: %zu panels registered", m_panels.size());
        console.LogSuccess("Created " + std::to_string(m_panels.size()) + " editor panels");
    }

} // namespace SparkEditor
