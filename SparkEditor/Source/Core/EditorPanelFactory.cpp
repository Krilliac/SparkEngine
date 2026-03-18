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
#include "../Terrain/TerrainEditor.h"
#include "../Profiler/PerformanceProfiler.h"
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

        if (isDebuggerPresent)
        {
            console.LogWarning("DEBUGGER DETECTED - Using minimal panel set to avoid deadlocks");

            // Only create the most essential panels
            try
            {
                console.LogInfo("Creating Scene View panel...");
                auto sceneViewPanel = std::shared_ptr<SceneViewPanel>(new SceneViewPanel());
                m_panels["SceneView"] = sceneViewPanel;
                console.LogSuccess("Created Scene View panel");
            }
            catch (const std::exception& e)
            {
                console.LogError("Failed to create Scene View panel: " + std::string(e.what()));
            }

            // Initialize essential panels only
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

            console.LogInfo("Created " + std::to_string(m_panels.size()) + " editor panels (minimal set for debugger)");
            return;
        }

        // Full panel creation for release mode
        console.LogInfo("Creating full panel set...");

        // Create Scene View Panel (working)
        try
        {
            console.LogInfo("Creating Scene View panel...");
            auto sceneViewPanel = std::shared_ptr<SceneViewPanel>(new SceneViewPanel());
            m_panels["SceneView"] = sceneViewPanel;
            console.LogSuccess("Created Scene View panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Scene View panel: " + std::string(e.what()));
        }

        // Create Console Panel (advanced logging, filtering, command execution)
        try
        {
            console.LogInfo("Creating Console panel...");
            auto consolePanel = std::shared_ptr<ConsolePanel>(new ConsolePanel());
            m_panels["Console"] = consolePanel;
            console.LogSuccess("Created Console panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Console panel: " + std::string(e.what()));
        }

        // Create Hierarchy Panel (full tree with drag-drop, undo, multi-select)
        try
        {
            console.LogInfo("Creating Hierarchy panel...");
            auto hierarchyPanel = std::shared_ptr<HierarchyPanel>(new HierarchyPanel());
            m_panels["Hierarchy"] = hierarchyPanel;
            console.LogSuccess("Created Hierarchy panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Hierarchy panel: " + std::string(e.what()));
        }

        // Create Inspector Panel
        try
        {
            console.LogInfo("Creating Inspector panel...");
            auto inspectorPanel = std::shared_ptr<InspectorPanel>(new InspectorPanel());
            m_panels["Inspector"] = inspectorPanel;
            console.LogSuccess("Created Inspector panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Inspector panel: " + std::string(e.what()));
        }

        // Create Asset Browser Panel
        try
        {
            console.LogInfo("Creating Asset Browser panel...");
            auto assetBrowserPanel = std::shared_ptr<AssetBrowserPanel>(new AssetBrowserPanel());
            m_panels["AssetBrowser"] = assetBrowserPanel;
            console.LogSuccess("Created Asset Browser panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Asset Browser panel: " + std::string(e.what()));
        }

        // Create Game View Panel (FPS player camera)
        try
        {
            console.LogInfo("Creating Game View panel...");
            auto gameViewPanel = std::shared_ptr<GameViewPanel>(new GameViewPanel());
            m_panels["GameView"] = gameViewPanel;
            console.LogSuccess("Created Game View panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Game View panel: " + std::string(e.what()));
        }

        // Create Performance Profiler Panel
        try
        {
            console.LogInfo("Creating Performance Profiler panel...");
            auto profilerPanel = std::shared_ptr<PerformanceProfiler>(new PerformanceProfiler());
            m_panels["Profiler"] = profilerPanel;
            console.LogSuccess("Created Performance Profiler panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Profiler panel: " + std::string(e.what()));
        }

        // Create Weapon Editor Panel
        try
        {
            console.LogInfo("Creating Weapon Editor panel...");
            auto weaponEditorPanel = std::shared_ptr<WeaponEditorPanel>(new WeaponEditorPanel());
            m_panels["WeaponEditor"] = weaponEditorPanel;
            console.LogSuccess("Created Weapon Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Weapon Editor panel: " + std::string(e.what()));
        }

        // Create FPS Tools Panel
        try
        {
            console.LogInfo("Creating FPS Tools panel...");
            auto fpsToolsPanel = std::shared_ptr<FPSToolsPanel>(new FPSToolsPanel());
            m_panels["FPSTools"] = fpsToolsPanel;
            console.LogSuccess("Created FPS Tools panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create FPS Tools panel: " + std::string(e.what()));
        }

        // Create 2D/2.5D Editor Panels
        try
        {
            console.LogInfo("Creating Sprite Editor panel...");
            auto spriteEditorPanel = std::shared_ptr<SpriteEditorPanel>(new SpriteEditorPanel());
            m_panels["SpriteEditor"] = spriteEditorPanel;
            console.LogSuccess("Created Sprite Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Sprite Editor panel: " + std::string(e.what()));
        }

        try
        {
            console.LogInfo("Creating Tilemap Editor panel...");
            auto tilemapEditorPanel = std::shared_ptr<TilemapEditorPanel>(new TilemapEditorPanel());
            m_panels["TilemapEditor"] = tilemapEditorPanel;
            console.LogSuccess("Created Tilemap Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Tilemap Editor panel: " + std::string(e.what()));
        }

        try
        {
            console.LogInfo("Creating Sprite Animation Editor panel...");
            auto spriteAnimEditorPanel = std::shared_ptr<SpriteAnimationEditorPanel>(new SpriteAnimationEditorPanel());
            m_panels["SpriteAnimEditor"] = spriteAnimEditorPanel;
            console.LogSuccess("Created Sprite Animation Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Sprite Animation Editor panel: " + std::string(e.what()));
        }

        try
        {
            console.LogInfo("Creating Physics 2D panel...");
            auto physics2DPanel = std::shared_ptr<Physics2DPanel>(new Physics2DPanel());
            m_panels["Physics2D"] = physics2DPanel;
            console.LogSuccess("Created Physics 2D panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Physics 2D panel: " + std::string(e.what()));
        }

        // Create Undo History Panel
        try
        {
            console.LogInfo("Creating Undo History panel...");
            auto undoHistoryPanel = std::shared_ptr<UndoHistoryPanel>(new UndoHistoryPanel(m_undoRedoManager.get()));
            m_panels["UndoHistory"] = undoHistoryPanel;
            console.LogSuccess("Created Undo History panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Undo History panel: " + std::string(e.what()));
        }

        // Create Scene Statistics Panel
        try
        {
            console.LogInfo("Creating Scene Statistics panel...");
            auto sceneStatsPanel = std::shared_ptr<SceneStatisticsPanel>(new SceneStatisticsPanel());
            m_panels["SceneStats"] = sceneStatsPanel;
            console.LogSuccess("Created Scene Statistics panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Scene Statistics panel: " + std::string(e.what()));
        }

        // Create Prefab Editor Panel
        try
        {
            console.LogInfo("Creating Prefab Editor panel...");
            auto prefabEditorPanel = std::shared_ptr<PrefabEditorPanel>(new PrefabEditorPanel(m_prefabManager.get()));
            m_panels["PrefabEditor"] = prefabEditorPanel;
            console.LogSuccess("Created Prefab Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Prefab Editor panel: " + std::string(e.what()));
        }

        // Create Search Panel
        try
        {
            console.LogInfo("Creating Search panel...");
            auto searchPanel = std::shared_ptr<SearchPanel>(new SearchPanel());
            m_panels["Search"] = searchPanel;
            console.LogSuccess("Created Search panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Search panel: " + std::string(e.what()));
        }

        // Create Dedicated Server Panel
        try
        {
            console.LogInfo("Creating Dedicated Server panel...");
            auto dediServerPanel = std::shared_ptr<DedicatedServerPanel>(new DedicatedServerPanel());
            m_panels["DedicatedServer"] = dediServerPanel;
            console.LogSuccess("Created Dedicated Server panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Dedicated Server panel: " + std::string(e.what()));
        }

        // Create Debug Visualizer Panel
        try
        {
            console.LogInfo("Creating Debug Visualizer panel...");
            auto debugVisualizerPanel = std::shared_ptr<DebugVisualizerPanel>(new DebugVisualizerPanel());
            m_panels["DebugVisualizer"] = debugVisualizerPanel;
            console.LogSuccess("Created Debug Visualizer panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Debug Visualizer panel: " + std::string(e.what()));
        }

        // Create Object Placement Panel
        try
        {
            console.LogInfo("Creating Object Placement panel...");
            auto objectPlacementPanel = std::shared_ptr<ObjectPlacementPanel>(new ObjectPlacementPanel());
            m_panels["ObjectPlacement"] = objectPlacementPanel;
            console.LogSuccess("Created Object Placement panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Object Placement panel: " + std::string(e.what()));
        }

        // Create Build & Cook Panel
        try
        {
            console.LogInfo("Creating Build & Cook panel...");
            auto buildCookPanel = std::shared_ptr<BuildCookPanel>(new BuildCookPanel());
            m_panels["BuildCook"] = buildCookPanel;
            console.LogSuccess("Created Build & Cook panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Build & Cook panel: " + std::string(e.what()));
        }

        // Create Play Mode Toolbar Panel (Play/Stop/Pause transport controls)
        try
        {
            console.LogInfo("Creating Play Mode Toolbar panel...");
            auto playModeToolbar = std::shared_ptr<PlayModeToolbarPanel>(new PlayModeToolbarPanel());
            m_panels["PlayModeToolbar"] = playModeToolbar;
            console.LogSuccess("Created Play Mode Toolbar panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Play Mode Toolbar panel: " + std::string(e.what()));
        }

        // Create Material Editor Panel (PBR material and shader property editor)
        try
        {
            console.LogInfo("Creating Material Editor panel...");
            auto materialEditor = std::shared_ptr<MaterialEditorPanel>(new MaterialEditorPanel());
            m_panels["MaterialEditor"] = materialEditor;
            console.LogSuccess("Created Material Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Material Editor panel: " + std::string(e.what()));
        }

        // Create Terrain Editor Panel (height sculpting, texture painting, vegetation)
        try
        {
            console.LogInfo("Creating Terrain Editor panel...");
            auto terrainEditor = std::shared_ptr<TerrainEditor>(new TerrainEditor());
            m_panels["TerrainEditor"] = terrainEditor;
            console.LogSuccess("Created Terrain Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Terrain Editor panel: " + std::string(e.what()));
        }

        // Create Post Processing Panel
        try
        {
            auto postProcessPanel = std::shared_ptr<PostProcessingPanel>(new PostProcessingPanel());
            m_panels["PostProcessing"] = postProcessPanel;
            console.LogSuccess("Created Post Processing panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Post Processing panel: " + std::string(e.what()));
        }

        // Create Dialogue Editor Panel
        try
        {
            auto dialoguePanel = std::shared_ptr<DialogueEditorPanel>(new DialogueEditorPanel());
            m_panels["DialogueEditor"] = dialoguePanel;
            console.LogSuccess("Created Dialogue Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Dialogue Editor panel: " + std::string(e.what()));
        }

        // Create AI Editor Panel
        try
        {
            auto aiPanel = std::shared_ptr<AIEditorPanel>(new AIEditorPanel());
            m_panels["AIEditor"] = aiPanel;
            console.LogSuccess("Created AI Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create AI Editor panel: " + std::string(e.what()));
        }

        // Create Spline Editor Panel
        try
        {
            auto splinePanel = std::shared_ptr<SplineEditorPanel>(new SplineEditorPanel());
            m_panels["SplineEditor"] = splinePanel;
            console.LogSuccess("Created Spline Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Spline Editor panel: " + std::string(e.what()));
        }

        // Create Particle Editor Panel
        try
        {
            auto particlePanel = std::shared_ptr<ParticleEditorPanel>(new ParticleEditorPanel());
            m_panels["ParticleEditor"] = particlePanel;
            console.LogSuccess("Created Particle Editor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Particle Editor panel: " + std::string(e.what()));
        }

        // Create Event Monitor Panel
        try
        {
            auto eventPanel = std::shared_ptr<EventMonitorPanel>(new EventMonitorPanel());
            m_panels["EventMonitor"] = eventPanel;
            console.LogSuccess("Created Event Monitor panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Event Monitor panel: " + std::string(e.what()));
        }

        // Create Save System Panel
        try
        {
            auto savePanel = std::shared_ptr<SaveSystemPanel>(new SaveSystemPanel());
            m_panels["SaveSystem"] = savePanel;
            console.LogSuccess("Created Save System panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Save System panel: " + std::string(e.what()));
        }

        // Create Localization Panel
        try
        {
            auto locPanel = std::shared_ptr<LocalizationPanel>(new LocalizationPanel());
            m_panels["Localization"] = locPanel;
            console.LogSuccess("Created Localization panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Localization panel: " + std::string(e.what()));
        }

        // Create Weather & Fog Panel
        try
        {
            auto weatherPanel = std::shared_ptr<WeatherFogPanel>(new WeatherFogPanel());
            m_panels["WeatherFog"] = weatherPanel;
            console.LogSuccess("Created Weather & Fog panel");
        }
        catch (const std::exception& e)
        {
            console.LogError("Failed to create Weather & Fog panel: " + std::string(e.what()));
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

        // Assign panel icons (FontAwesome)
        if (m_panels.count("SceneView"))
            m_panels["SceneView"]->SetIcon(ICON_FA_CAMERA);
        if (m_panels.count("Console"))
            m_panels["Console"]->SetIcon(ICON_FA_TERMINAL);
        if (m_panels.count("Hierarchy"))
            m_panels["Hierarchy"]->SetIcon(ICON_FA_SITEMAP);
        if (m_panels.count("Inspector"))
            m_panels["Inspector"]->SetIcon(ICON_FA_SLIDERS);
        if (m_panels.count("AssetBrowser"))
            m_panels["AssetBrowser"]->SetIcon(ICON_FA_FOLDER);
        if (m_panels.count("GameView"))
            m_panels["GameView"]->SetIcon(ICON_FA_GAMEPAD);
        if (m_panels.count("Profiler"))
            m_panels["Profiler"]->SetIcon(ICON_FA_CHART_BAR);
        if (m_panels.count("WeaponEditor"))
            m_panels["WeaponEditor"]->SetIcon(ICON_FA_CROSSHAIRS);
        if (m_panels.count("FPSTools"))
            m_panels["FPSTools"]->SetIcon(ICON_FA_ROCKET);
        if (m_panels.count("DebugVisualizer"))
            m_panels["DebugVisualizer"]->SetIcon(ICON_FA_BUG);
        if (m_panels.count("SceneStats"))
            m_panels["SceneStats"]->SetIcon(ICON_FA_CHART_BAR);
        if (m_panels.count("ObjectPlacement"))
            m_panels["ObjectPlacement"]->SetIcon(ICON_FA_CUBE);
        if (m_panels.count("BuildCook"))
            m_panels["BuildCook"]->SetIcon(ICON_FA_HAMMER);
        if (m_panels.count("DedicatedServer"))
            m_panels["DedicatedServer"]->SetIcon(ICON_FA_SERVER);
        if (m_panels.count("TerrainEditor"))
            m_panels["TerrainEditor"]->SetIcon(ICON_FA_MOUNTAIN);

        // Hide secondary panels by default (accessible via menus)
        if (m_panels.count("UndoHistory"))
            m_panels["UndoHistory"]->SetIcon(ICON_FA_UNDO);
        if (m_panels.count("SceneStats"))
            m_panels["SceneStats"]->SetIcon(ICON_FA_CHART_BAR);
        if (m_panels.count("PrefabEditor"))
            m_panels["PrefabEditor"]->SetIcon(ICON_FA_CUBE);
        if (m_panels.count("Search"))
            m_panels["Search"]->SetIcon(ICON_FA_SEARCH);

        // Hide new panels by default (accessible via FPS Tools menu)
        if (m_panels.count("WeaponEditor"))
            m_panels["WeaponEditor"]->SetVisible(false);
        if (m_panels.count("FPSTools"))
            m_panels["FPSTools"]->SetVisible(false);
        if (m_panels.count("DebugVisualizer"))
            m_panels["DebugVisualizer"]->SetVisible(false);
        if (m_panels.count("SceneStats"))
            m_panels["SceneStats"]->SetVisible(false);
        if (m_panels.count("ObjectPlacement"))
            m_panels["ObjectPlacement"]->SetVisible(false);
        if (m_panels.count("BuildCook"))
            m_panels["BuildCook"]->SetVisible(false);
        if (m_panels.count("UndoHistory"))
            m_panels["UndoHistory"]->SetVisible(false);
        if (m_panels.count("PrefabEditor"))
            m_panels["PrefabEditor"]->SetVisible(false);
        if (m_panels.count("Search"))
            m_panels["Search"]->SetVisible(false);
        if (m_panels.count("DedicatedServer"))
            m_panels["DedicatedServer"]->SetVisible(false);
        if (m_panels.count("TerrainEditor"))
            m_panels["TerrainEditor"]->SetVisible(false);

        // New content/system panels — hidden by default
        if (m_panels.count("PostProcessing"))
        {
            m_panels["PostProcessing"]->SetIcon(ICON_FA_MAGIC);
            m_panels["PostProcessing"]->SetVisible(false);
        }
        if (m_panels.count("DialogueEditor"))
        {
            m_panels["DialogueEditor"]->SetIcon(ICON_FA_COMMENTS);
            m_panels["DialogueEditor"]->SetVisible(false);
        }
        if (m_panels.count("AIEditor"))
        {
            m_panels["AIEditor"]->SetIcon(ICON_FA_BRAIN);
            m_panels["AIEditor"]->SetVisible(false);
        }
        if (m_panels.count("SplineEditor"))
        {
            m_panels["SplineEditor"]->SetIcon(ICON_FA_BEZIER_CURVE);
            m_panels["SplineEditor"]->SetVisible(false);
        }
        if (m_panels.count("ParticleEditor"))
        {
            m_panels["ParticleEditor"]->SetIcon(ICON_FA_FIRE);
            m_panels["ParticleEditor"]->SetVisible(false);
        }
        if (m_panels.count("EventMonitor"))
        {
            m_panels["EventMonitor"]->SetIcon(ICON_FA_BOLT);
            m_panels["EventMonitor"]->SetVisible(false);
        }
        if (m_panels.count("SaveSystem"))
        {
            m_panels["SaveSystem"]->SetIcon(ICON_FA_SAVE);
            m_panels["SaveSystem"]->SetVisible(false);
        }
        if (m_panels.count("Localization"))
        {
            m_panels["Localization"]->SetIcon(ICON_FA_GLOBE);
            m_panels["Localization"]->SetVisible(false);
        }
        if (m_panels.count("WeatherFog"))
        {
            m_panels["WeatherFog"]->SetIcon(ICON_FA_CLOUD_SUN);
            m_panels["WeatherFog"]->SetVisible(false);
        }

        console.LogSuccess("Created " + std::to_string(m_panels.size()) + " editor panels");
    }

} // namespace SparkEditor
