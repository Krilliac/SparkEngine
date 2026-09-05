/**
 * @file EditorPanelFactoryMetadata.cpp
 * @brief Panel metadata setup (icons, categories, default visibility) for the SparkEditor UI
 *
 * Contains EditorUI::InitializePanelIcons(), EditorUI::InitializePanelCategories()
 * and EditorUI::SetDefaultPanelVisibility().
 * Split from EditorPanelFactory.cpp for maintainability.
 */
#include "EditorUI.h"
#include "EditorPanel.h"
#include "EditorIcons.h"

namespace SparkEditor
{

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
            {"PlayControl", ICON_FA_PLAY},
            {"Collaboration", ICON_FA_USERS},
            {"ServiceTopology", ICON_FA_NETWORK_WIRED},
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
            {"SceneImport", ICON_FA_FILE_IMPORT},
            {"RegionMapEditor", ICON_FA_GLOBE},
            {"DecorLayoutEditor", ICON_FA_CUBES},
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
            {"BasicMaterialEditor", PanelCategory::Tool},
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
            {"TerrainEditor", PanelCategory::Tool},
            {"PrefabEditor", PanelCategory::Tool},
            {"VisualScript", PanelCategory::Tool},
            {"FPSTools", PanelCategory::Tool},
            {"PlayModeToolbar", PanelCategory::Tool},
            {"PlayControl", PanelCategory::Tool},
            {"BuildCook", PanelCategory::Tool},
            {"Prototyping", PanelCategory::Tool},
            {"Workflows", PanelCategory::Tool},
            {"EventResponses", PanelCategory::Tool},
            {"SceneImport", PanelCategory::Tool},
            {"RegionMapEditor", PanelCategory::Tool},
            {"DecorLayoutEditor", PanelCategory::Tool},

            // Config
            {"SaveSystem", PanelCategory::Config},
            {"Localization", PanelCategory::Config},
            {"WeatherFog", PanelCategory::Config},
            {"VRConfig", PanelCategory::Config},
            {"Streaming", PanelCategory::Config},
            {"Modding", PanelCategory::Config},
            {"Collaboration", PanelCategory::Config},
            {"ServiceTopology", PanelCategory::Config},
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

} // namespace SparkEditor
