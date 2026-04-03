/**
 * @file Main.cpp
 * @brief SparkGameEngineEditor DLL - IModule implementation and exports
 *
 * Implements the SparkGameEngineEditorModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameEngineEditor.h"
#include "EEEngineSystems.h"
#include "VisualScripting/EEVisualScriptingSystem.h"
#include "LevelDesign/EELevelDesignSystem.h"
#include "MaterialEditor/EEMaterialEditorSystem.h"
#include "VFXEditor/EEVFXEditorSystem.h"
#include "AnimationEditor/EEAnimationEditorSystem.h"
#include "UIEditor/EEUIEditorSystem.h"
#include "Prototyping/EEPrototypingSystem.h"
#include "AssetPipeline/EEAssetPipelineSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif

// =============================================================================
// Module exports
// =============================================================================

SPARK_IMPLEMENT_MODULE(SparkGameEngineEditorModule)

// =============================================================================
// SparkGameEngineEditorModule implementation
// =============================================================================

SparkGameEngineEditorModule::SparkGameEngineEditorModule() = default;

SparkGameEngineEditorModule::~SparkGameEngineEditorModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameEngineEditorModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Engine Editor - No-Code Game Creation";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1009;
    return info;
}

bool SparkGameEngineEditorModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[EngineEditor] Loading Spark Engine Editor module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine Editor module loading - initializing 9 subsystems");

    // 1. Visual scripting (node graph, pin types, compilation)
    m_visualScripting = std::make_unique<EngineEditor::EEVisualScriptingSystem>();
    if (!m_visualScripting->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize visual scripting system");
        return false;
    }

    // 2. Level design (prefabs, terrain, foliage, splines)
    m_levelDesign = std::make_unique<EngineEditor::EELevelDesignSystem>();
    if (!m_levelDesign->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize level design system");
        return false;
    }

    // 3. Material editor (node graph, PBR, shading models)
    m_materialEditor = std::make_unique<EngineEditor::EEMaterialEditorSystem>();
    if (!m_materialEditor->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize material editor system");
        return false;
    }

    // 4. VFX editor (emitters, modules, presets)
    m_vfxEditor = std::make_unique<EngineEditor::EEVFXEditorSystem>();
    if (!m_vfxEditor->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize VFX editor system");
        return false;
    }

    // 5. Animation editor (state machines, IK, layers)
    m_animationEditor = std::make_unique<EngineEditor::EEAnimationEditorSystem>();
    if (!m_animationEditor->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize animation editor system");
        return false;
    }

    // 6. UI editor (WYSIWYG, widgets, anchors, data binding)
    m_uiEditor = std::make_unique<EngineEditor::EEUIEditorSystem>();
    if (!m_uiEditor->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize UI editor system");
        return false;
    }

    // 7. Prototyping (blockout, templates, gameplay rules, play-test)
    m_prototyping = std::make_unique<EngineEditor::EEPrototypingSystem>();
    if (!m_prototyping->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize prototyping system");
        return false;
    }

    // 8. Asset pipeline (import, LOD, compression, packaging)
    m_assetPipeline = std::make_unique<EngineEditor::EEAssetPipelineSystem>();
    if (!m_assetPipeline->Initialize(context))
    {
        console.LogError("[EngineEditor] Failed to initialize asset pipeline system");
        return false;
    }

    // 9. Engine systems integration (save, events, undo/redo)
    m_engineSystems = std::make_unique<EngineEditor::EEEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogWarning("[EngineEditor] Engine systems integration partially failed (non-fatal)");
    }

    RegisterConsoleCommands();

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine Editor module loaded successfully - 9 subsystems active");
    console.LogInfo("[EngineEditor] Module loaded successfully (9 subsystems)");
    console.LogInfo("[EngineEditor] Scripts: " + std::to_string(m_visualScripting->GetGraphCount()) +
                    " | Prefabs: " + std::to_string(m_levelDesign->GetPrefabCount()) +
                    " | Materials: " + std::to_string(m_materialEditor->GetMaterialCount()) +
                    " | VFX: " + std::to_string(m_vfxEditor->GetVFXCount()) +
                    " | AnimCtrl: " + std::to_string(m_animationEditor->GetControllerCount()) +
                    " | Screens: " + std::to_string(m_uiEditor->GetScreenCount()) +
                    " | Templates: " + std::to_string(m_prototyping->GetTemplateCount()) +
                    " | Assets: " + std::to_string(m_assetPipeline->GetAssetCount()));
    return true;
}

void SparkGameEngineEditorModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[EngineEditor] Unloading Spark Engine Editor module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine Editor module shutting down");

    // Shutdown in reverse initialization order
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_assetPipeline)
    {
        m_assetPipeline->Shutdown();
        m_assetPipeline.reset();
    }
    if (m_prototyping)
    {
        m_prototyping->Shutdown();
        m_prototyping.reset();
    }
    if (m_uiEditor)
    {
        m_uiEditor->Shutdown();
        m_uiEditor.reset();
    }
    if (m_animationEditor)
    {
        m_animationEditor->Shutdown();
        m_animationEditor.reset();
    }
    if (m_vfxEditor)
    {
        m_vfxEditor->Shutdown();
        m_vfxEditor.reset();
    }
    if (m_materialEditor)
    {
        m_materialEditor->Shutdown();
        m_materialEditor.reset();
    }
    if (m_levelDesign)
    {
        m_levelDesign->Shutdown();
        m_levelDesign.reset();
    }
    if (m_visualScripting)
    {
        m_visualScripting->Shutdown();
        m_visualScripting.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine Editor module unloaded");
    console.LogInfo("[EngineEditor] Module unloaded");
}

void SparkGameEngineEditorModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_visualScripting->Update(deltaTime);
    m_levelDesign->Update(deltaTime);
    m_materialEditor->Update(deltaTime);
    m_vfxEditor->Update(deltaTime);
    m_animationEditor->Update(deltaTime);
    m_uiEditor->Update(deltaTime);
    m_prototyping->Update(deltaTime);
    m_assetPipeline->Update(deltaTime);
    m_engineSystems->Update(deltaTime);
}

void SparkGameEngineEditorModule::OnFixedUpdate([[maybe_unused]] float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;
}

void SparkGameEngineEditorModule::OnRender()
{
    if (!m_initialized)
        return;
}

void SparkGameEngineEditorModule::OnResize([[maybe_unused]] int width, [[maybe_unused]] int height) {}

void SparkGameEngineEditorModule::OnPause()
{
    m_paused = true;
}

void SparkGameEngineEditorModule::OnResume()
{
    m_paused = false;
}

void SparkGameEngineEditorModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_visualScripting->RenderDebugUI();
    m_levelDesign->RenderDebugUI();
    m_materialEditor->RenderDebugUI();
    m_vfxEditor->RenderDebugUI();
    m_animationEditor->RenderDebugUI();
    m_uiEditor->RenderDebugUI();
    m_prototyping->RenderDebugUI();
    m_assetPipeline->RenderDebugUI();
    m_engineSystems->RenderDebugUI();
}

void SparkGameEngineEditorModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // --- Status ---
    console.RegisterCommand(
        "ee_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            std::string s = "=== Spark Engine Editor Status ===\n";
            s += "Scripts: " + std::to_string(m_visualScripting->GetGraphCount()) + " graphs (" +
                 std::to_string(m_visualScripting->GetNodeTemplateCount()) + " node types)\n";
            s += "Level: " + std::to_string(m_levelDesign->GetInstanceCount()) + " instances, " +
                 std::to_string(m_levelDesign->GetPrefabCount()) + " prefabs\n";
            s += "Materials: " + std::to_string(m_materialEditor->GetMaterialCount()) + "\n";
            s += "VFX: " + std::to_string(m_vfxEditor->GetVFXCount()) + "\n";
            s += "AnimCtrl: " + std::to_string(m_animationEditor->GetControllerCount()) + "\n";
            s += "UI Screens: " + std::to_string(m_uiEditor->GetScreenCount()) + "\n";
            s += "Blockouts: " + std::to_string(m_prototyping->GetBlockoutCount()) +
                 " | Templates: " + std::to_string(m_prototyping->GetTemplateCount()) + "\n";
            s += "Assets: " + std::to_string(m_assetPipeline->GetAssetCount()) + " (" +
                 std::to_string(m_assetPipeline->GetProcessedCount()) + " processed)\n";
            s += "Undo: " + std::to_string(m_engineSystems->GetUndoStackSize()) +
                 " | Redo: " + std::to_string(m_engineSystems->GetRedoStackSize()) + "\n";
            return s;
        },
        "Show engine editor module status", "EngineEditor");

    // --- Visual Scripting ---
    console.RegisterCommand(
        "ee_scripts", [this](const std::vector<std::string>&) -> std::string
        { return m_visualScripting->GetGraphListString(); }, "List visual script graphs", "EngineEditor");

    console.RegisterCommand(
        "ee_nodes", [this](const std::vector<std::string>&) -> std::string
        { return m_visualScripting->GetNodeCatalogString(); }, "Show visual scripting node catalog", "EngineEditor");

    console.RegisterCommand(
        "ee_new_script",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string name = args.empty() ? "NewScript" : args[0];
            uint32_t id = m_visualScripting->CreateGraph(name);
            m_engineSystems->RecordAction("Created script '" + name + "'");
            return "Script '" + name + "' created (id=" + std::to_string(id) + ")";
        },
        "Create a new visual script", "EngineEditor");

    console.RegisterCommand(
        "ee_compile_scripts", [this](const std::vector<std::string>&) -> std::string
        { return m_visualScripting->CompileAll() ? "All scripts compiled" : "Compilation errors"; },
        "Compile all visual scripts", "EngineEditor");

    // --- Level Design ---
    console.RegisterCommand(
        "ee_prefabs", [this](const std::vector<std::string>&) -> std::string
        { return m_levelDesign->GetPrefabCatalogString(); }, "Show prefab catalog", "EngineEditor");

    console.RegisterCommand(
        "ee_instances", [this](const std::vector<std::string>&) -> std::string
        { return m_levelDesign->GetInstanceListString(); }, "List placed prefab instances", "EngineEditor");

    console.RegisterCommand(
        "ee_place",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 4)
                return "Usage: ee_place <prefab_id> <x> <y> <z>";
            try
            {
                uint32_t pid = static_cast<uint32_t>(std::stoi(args[0]));
                float x = std::stof(args[1]);
                float y = std::stof(args[2]);
                float z = std::stof(args[3]);
                uint32_t id = m_levelDesign->PlacePrefab(pid, x, y, z);
                if (id > 0)
                {
                    m_engineSystems->RecordAction("Placed prefab " + args[0]);
                    return "Placed (id=" + std::to_string(id) + ")";
                }
                return "Invalid prefab ID";
            }
            catch (...)
            {
                return "Invalid arguments";
            }
        },
        "Place a prefab", "EngineEditor", "ee_place <prefab_id> <x> <y> <z>");

    console.RegisterCommand(
        "ee_splines", [this](const std::vector<std::string>&) -> std::string
        { return m_levelDesign->GetSplineListString(); }, "List spline paths", "EngineEditor");

    console.RegisterCommand(
        "ee_tool_status", [this](const std::vector<std::string>&) -> std::string
        { return m_levelDesign->GetToolStatusString(); }, "Show level design tool status", "EngineEditor");

    // --- Material Editor ---
    console.RegisterCommand(
        "ee_materials", [this](const std::vector<std::string>&) -> std::string
        { return m_materialEditor->GetMaterialListString(); }, "List materials", "EngineEditor");

    console.RegisterCommand(
        "ee_new_material",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string name = args.empty() ? "NewMaterial" : args[0];
            uint32_t id = m_materialEditor->CreateMaterial(name, EngineEditor::ShadingModel::DefaultLit);
            m_engineSystems->RecordAction("Created material '" + name + "'");
            return "Material '" + name + "' created (id=" + std::to_string(id) + ")";
        },
        "Create a new material", "EngineEditor");

    console.RegisterCommand(
        "ee_mat_nodes", [this](const std::vector<std::string>&) -> std::string
        { return m_materialEditor->GetNodeCatalogString(); }, "Show material node types", "EngineEditor");

    // --- VFX Editor ---
    console.RegisterCommand(
        "ee_vfx", [this](const std::vector<std::string>&) -> std::string { return m_vfxEditor->GetVFXListString(); },
        "List VFX assets", "EngineEditor");

    console.RegisterCommand(
        "ee_new_vfx",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: ee_new_vfx <name> <category>";
            uint32_t id = m_vfxEditor->CreateVFX(args[0], args[1]);
            m_engineSystems->RecordAction("Created VFX '" + args[0] + "'");
            return "VFX '" + args[0] + "' created (id=" + std::to_string(id) + ")";
        },
        "Create a new VFX asset", "EngineEditor", "ee_new_vfx <name> <category>");

    console.RegisterCommand(
        "ee_vfx_play",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ee_vfx_play <asset_id>";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                return m_vfxEditor->PlayVFX(id) ? "Playing VFX" : "Invalid VFX ID";
            }
            catch (...)
            {
                return "Invalid ID";
            }
        },
        "Play a VFX preview", "EngineEditor");

    // --- Animation Editor ---
    console.RegisterCommand(
        "ee_anims", [this](const std::vector<std::string>&) -> std::string
        { return m_animationEditor->GetControllerListString(); }, "List animation controllers", "EngineEditor");

    console.RegisterCommand(
        "ee_new_anim",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string name = args.empty() ? "NewController" : args[0];
            uint32_t id = m_animationEditor->CreateController(name);
            m_engineSystems->RecordAction("Created anim controller '" + name + "'");
            return "Controller '" + name + "' created (id=" + std::to_string(id) + ")";
        },
        "Create a new animation controller", "EngineEditor");

    console.RegisterCommand(
        "ee_ik_solvers", [this](const std::vector<std::string>&) -> std::string
        { return m_animationEditor->GetIKSolverCatalog(); }, "Show IK solver types", "EngineEditor");

    // --- UI Editor ---
    console.RegisterCommand(
        "ee_screens", [this](const std::vector<std::string>&) -> std::string
        { return m_uiEditor->GetScreenListString(); }, "List UI screens", "EngineEditor");

    console.RegisterCommand(
        "ee_new_screen",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: ee_new_screen <name> <category>";
            uint32_t id = m_uiEditor->CreateScreen(args[0], args[1]);
            m_engineSystems->RecordAction("Created UI screen '" + args[0] + "'");
            return "Screen '" + args[0] + "' created (id=" + std::to_string(id) + ")";
        },
        "Create a new UI screen", "EngineEditor", "ee_new_screen <name> <category>");

    console.RegisterCommand(
        "ee_widgets", [this](const std::vector<std::string>&) -> std::string
        { return m_uiEditor->GetWidgetCatalogString(); }, "Show widget types", "EngineEditor");

    // --- Prototyping ---
    console.RegisterCommand(
        "ee_templates", [this](const std::vector<std::string>&) -> std::string
        { return m_prototyping->GetTemplateListString(); }, "List game templates", "EngineEditor");

    console.RegisterCommand(
        "ee_blockouts", [this](const std::vector<std::string>&) -> std::string
        { return m_prototyping->GetBlockoutListString(); }, "List blockout primitives", "EngineEditor");

    console.RegisterCommand(
        "ee_blockout",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 4)
                return "Usage: ee_blockout <shape_id> <x> <y> <z>";
            try
            {
                auto shape = static_cast<EngineEditor::BlockoutShape>(std::stoi(args[0]));
                float x = std::stof(args[1]);
                float y = std::stof(args[2]);
                float z = std::stof(args[3]);
                uint32_t id = m_prototyping->PlaceBlockout(shape, x, y, z);
                m_engineSystems->RecordAction("Placed blockout");
                return "Blockout placed (id=" + std::to_string(id) + ")";
            }
            catch (...)
            {
                return "Invalid arguments";
            }
        },
        "Place a blockout primitive", "EngineEditor", "ee_blockout <shape_id> <x> <y> <z>");

    console.RegisterCommand(
        "ee_rules", [this](const std::vector<std::string>&) -> std::string
        { return m_prototyping->GetRuleListString(); }, "List gameplay rules", "EngineEditor");

    console.RegisterCommand(
        "ee_play",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (m_prototyping->IsPlaying())
            {
                m_prototyping->StopPlayTest();
                return "Play-test stopped";
            }
            m_prototyping->StartPlayTest();
            return "Play-test started";
        },
        "Toggle play-test mode", "EngineEditor");

    // --- Asset Pipeline ---
    console.RegisterCommand(
        "ee_assets", [this](const std::vector<std::string>&) -> std::string
        { return m_assetPipeline->GetAssetListString(); }, "List pipeline assets", "EngineEditor");

    console.RegisterCommand(
        "ee_import",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: ee_import <path> <format_id>";
            try
            {
                auto fmt = static_cast<EngineEditor::AssetFormat>(std::stoi(args[1]));
                uint32_t id = m_assetPipeline->ImportAsset(args[0], fmt);
                m_engineSystems->RecordAction("Imported asset '" + args[0] + "'");
                return "Imported (id=" + std::to_string(id) + ")";
            }
            catch (...)
            {
                return "Invalid arguments";
            }
        },
        "Import an asset", "EngineEditor", "ee_import <path> <format_id>");

    console.RegisterCommand(
        "ee_process_all", [this](const std::vector<std::string>&) -> std::string
        { return m_assetPipeline->ProcessAll() ? "All assets processed" : "Processing errors"; },
        "Process all unprocessed assets", "EngineEditor");

    console.RegisterCommand(
        "ee_pipeline", [this](const std::vector<std::string>&) -> std::string
        { return m_assetPipeline->GetPipelineStatusString(); }, "Show asset pipeline status", "EngineEditor");

    console.RegisterCommand(
        "ee_import_rules", [this](const std::vector<std::string>&) -> std::string
        { return m_assetPipeline->GetRuleListString(); }, "List import rules", "EngineEditor");

    // --- Project / Undo ---
    console.RegisterCommand(
        "ee_project", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems->GetProjectStatus(); }, "Show project status", "EngineEditor");

    console.RegisterCommand(
        "ee_new_project",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string name = args.empty() ? "NewProject" : args[0];
            return m_engineSystems->NewProject(name);
        },
        "Create a new project", "EngineEditor");

    console.RegisterCommand(
        "ee_save",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string slot = args.empty() ? "editor_slot1" : args[0];
            return m_engineSystems->SaveProject(slot);
        },
        "Save project", "EngineEditor");

    console.RegisterCommand(
        "ee_load",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string slot = args.empty() ? "editor_slot1" : args[0];
            return m_engineSystems->LoadProject(slot);
        },
        "Load project", "EngineEditor");

    console.RegisterCommand(
        "ee_undo", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems->Undo() ? "Undone" : "Nothing to undo"; }, "Undo last action", "EngineEditor");

    console.RegisterCommand(
        "ee_redo", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems->Redo() ? "Redone" : "Nothing to redo"; }, "Redo last undone action", "EngineEditor");

    console.RegisterCommand(
        "ee_history", [this](const std::vector<std::string>&) -> std::string
        { return m_engineSystems->GetUndoHistoryString(); }, "Show undo history", "EngineEditor");
}
