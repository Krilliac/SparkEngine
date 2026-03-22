/**
 * @file EditorPanelFactory.cpp
 * @brief Panel creation for the SparkEditor UI
 *
 * Contains EditorUI::CreatePanels() which instantiates all editor panels.
 * Split from EditorUI.cpp for maintainability.
 */
#include "EditorUI.h"
#include "../Utils/SparkConsole.h"
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
#include "../Panels/UndoHistoryPanel.h"
#include "../Panels/SceneStatisticsPanel.h"
#include "../Panels/PrefabEditorPanel.h"
#include "../Panels/SearchPanel.h"
#include "../Panels/DedicatedServerPanel.h"
#include "../Panels/MaterialEditorPanel.h"
#include "../Panels/PlayModeToolbarPanel.h"
#include "../Panels/PostProcessingPanel.h"
#include "../Panels/DialogueEditorPanel.h"
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
#include "../Terrain/TerrainEditor.h"
#include "../Profiler/PerformanceProfiler.h"
#include "EditorIcons.h"
#include <imgui.h>

namespace SparkEditor
{

    void EditorUI::CreatePanels()
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Creating editor panels...");

        // Skip complex panels that may cause deadlocks in debugger environment
        bool isDebuggerPresent = false;
#ifdef _WIN32
        isDebuggerPresent = IsDebuggerPresent();
#endif

        // Helper: create a panel, register it, and log the result
        auto registerPanel = [&](const std::string& name, std::shared_ptr<EditorPanel> panel)
        {
            try
            {
                m_panels[name] = std::move(panel);
                console.LogSuccess("Created " + name + " panel");
            }
            catch (const std::exception& e)
            {
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

            // Core panels
            registerPanel("SceneView", std::make_shared<SceneViewPanel>());
            registerPanel("Console", std::make_shared<ConsolePanel>());
            registerPanel("Hierarchy", std::make_shared<HierarchyPanel>());
            registerPanel("Inspector", std::make_shared<InspectorPanel>());
            registerPanel("AssetBrowser", std::make_shared<AssetBrowserPanel>());
            registerPanel("GameView", std::make_shared<GameViewPanel>());
            registerPanel("Profiler", std::make_shared<PerformanceProfiler>());

            // FPS panels
            registerPanel("WeaponEditor", std::make_shared<WeaponEditorPanel>());
            registerPanel("FPSTools", std::make_shared<FPSToolsPanel>());

            // 2D/2.5D panels
            registerPanel("SpriteEditor", std::make_shared<SpriteEditorPanel>());
            registerPanel("TilemapEditor", std::make_shared<TilemapEditorPanel>());
            registerPanel("SpriteAnimEditor", std::make_shared<SpriteAnimationEditorPanel>());
            registerPanel("Physics2D", std::make_shared<Physics2DPanel>());

            // Tool panels (require special constructor args)
            registerPanel("UndoHistory", std::make_shared<UndoHistoryPanel>(m_undoRedoManager.get()));
            registerPanel("PrefabEditor", std::make_shared<PrefabEditorPanel>(m_prefabManager.get()));

            // Tool panels (default constructors)
            registerPanel("SceneStats", std::make_shared<SceneStatisticsPanel>());
            registerPanel("Search", std::make_shared<SearchPanel>());
            registerPanel("DedicatedServer", std::make_shared<DedicatedServerPanel>());
            registerPanel("DebugVisualizer", std::make_shared<DebugVisualizerPanel>());
            registerPanel("ObjectPlacement", std::make_shared<ObjectPlacementPanel>());
            registerPanel("BuildCook", std::make_shared<BuildCookPanel>());
            registerPanel("PlayModeToolbar", std::make_shared<PlayModeToolbarPanel>());
            registerPanel("MaterialEditor", std::make_shared<MaterialEditorPanel>());
            registerPanel("TerrainEditor", std::make_shared<TerrainEditor>());

            // Content/system panels
            registerPanel("PostProcessing", std::make_shared<PostProcessingPanel>());
            registerPanel("DialogueEditor", std::make_shared<DialogueEditorPanel>());
            registerPanel("AIEditor", std::make_shared<AIEditorPanel>());
            registerPanel("SplineEditor", std::make_shared<SplineEditorPanel>());
            registerPanel("ParticleEditor", std::make_shared<ParticleEditorPanel>());
            registerPanel("EventMonitor", std::make_shared<EventMonitorPanel>());
            registerPanel("SaveSystem", std::make_shared<SaveSystemPanel>());
            registerPanel("Localization", std::make_shared<LocalizationPanel>());
            registerPanel("WeatherFog", std::make_shared<WeatherFogPanel>());
            registerPanel("CinematicSequencer", std::make_shared<CinematicSequencerPanel>());
            registerPanel("ProjectSettings", std::make_shared<ProjectSettingsPanel>());

            // Engine system panels
            registerPanel("AudioMixer", std::make_shared<AudioMixerPanel>());
            registerPanel("ScriptEditor", std::make_shared<ScriptEditorPanel>());
            registerPanel("DestructionEditor", std::make_shared<DestructionEditorPanel>());
            registerPanel("Replay", std::make_shared<ReplayPanel>());
            registerPanel("VRConfig", std::make_shared<VRConfigPanel>());
            registerPanel("Streaming", std::make_shared<StreamingPanel>());
            registerPanel("Modding", std::make_shared<ModdingPanel>());
            registerPanel("CoroutineDebug", std::make_shared<CoroutineDebugPanel>());

            // Multi-game module management
            registerPanel("GameModuleSelector", std::make_shared<GameModuleSelectorPanel>());
        }

        // Initialize all panels
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

        // Panel icons — map panel name to FontAwesome icon
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
        };

        for (const auto& [name, icon] : panelIcons)
        {
            if (m_panels.count(name))
                m_panels[name]->SetIcon(icon);
        }

        // Panels hidden by default (accessible via menus)
        const char* hiddenPanels[] = {
            "WeaponEditor",       "FPSTools",        "DebugVisualizer",
            "SceneStats",         "ObjectPlacement", "BuildCook",
            "UndoHistory",        "PrefabEditor",    "Search",
            "DedicatedServer",    "TerrainEditor",   "PostProcessing",
            "DialogueEditor",     "AIEditor",        "SplineEditor",
            "ParticleEditor",     "EventMonitor",    "SaveSystem",
            "Localization",       "WeatherFog",      "CinematicSequencer",
            "ProjectSettings",    "AudioMixer",      "ScriptEditor",
            "DestructionEditor",  "Replay",          "VRConfig",
            "Streaming",          "Modding",         "CoroutineDebug",
            "GameModuleSelector",
        };

        for (const char* name : hiddenPanels)
        {
            if (m_panels.count(name))
                m_panels[name]->SetVisible(false);
        }

        console.LogSuccess("Created " + std::to_string(m_panels.size()) + " editor panels");
    }

} // namespace SparkEditor
