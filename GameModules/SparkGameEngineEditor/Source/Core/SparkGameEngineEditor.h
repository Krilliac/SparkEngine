/**
 * @file SparkGameEngineEditor.h
 * @brief Engine/editor no-code game creation showcase module
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameEngineEditor is a game module that showcases SparkEngine's no-code
 * game creation pipeline: visual scripting graphs, level design tools,
 * material editor, VFX authoring, animation state machine editor, WYSIWYG
 * UI editor, rapid prototyping templates, and asset pipeline management.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

namespace EngineEditor
{
    class EEVisualScriptingSystem;
    class EELevelDesignSystem;
    class EEMaterialEditorSystem;
    class EEVFXEditorSystem;
    class EEAnimationEditorSystem;
    class EEUIEditorSystem;
    class EEPrototypingSystem;
    class EEAssetPipelineSystem;
    class EEEngineSystems;
} // namespace EngineEditor

/**
 * @brief Game module showcasing no-code game creation tools on SparkEngine
 *
 * Wires up 9 subsystems covering visual scripting, level design, material
 * editing, VFX authoring, animation editing, UI layout, rapid prototyping,
 * asset pipeline, and engine integration.
 */
class SparkGameEngineEditorModule : public Spark::IModule
{
  public:
    SparkGameEngineEditorModule();
    ~SparkGameEngineEditorModule() override;

    // --- Spark::IModule interface ---
    Spark::ModuleInfo GetModuleInfo() const override;
    bool OnLoad(Spark::IEngineContext* context) override;
    void OnUnload() override;
    void OnUpdate(float deltaTime) override;
    void OnFixedUpdate(float fixedDeltaTime) override;
    void OnRender() override;
    void OnResize(int width, int height) override;
    void OnPause() override;
    void OnResume() override;
    void OnImGui() override;

  private:
    void RegisterConsoleCommands();

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};

    std::unique_ptr<EngineEditor::EEVisualScriptingSystem> m_visualScripting;
    std::unique_ptr<EngineEditor::EELevelDesignSystem> m_levelDesign;
    std::unique_ptr<EngineEditor::EEMaterialEditorSystem> m_materialEditor;
    std::unique_ptr<EngineEditor::EEVFXEditorSystem> m_vfxEditor;
    std::unique_ptr<EngineEditor::EEAnimationEditorSystem> m_animationEditor;
    std::unique_ptr<EngineEditor::EEUIEditorSystem> m_uiEditor;
    std::unique_ptr<EngineEditor::EEPrototypingSystem> m_prototyping;
    std::unique_ptr<EngineEditor::EEAssetPipelineSystem> m_assetPipeline;
    std::unique_ptr<EngineEditor::EEEngineSystems> m_engineSystems;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
