#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <cmath>
#include <cstdint>

class EmptyProjectModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "EmptyProject";
        info.version = "0.2.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_updateCount = 0;
        m_elapsedSeconds = 0.0f;
        m_paused = false;
        ResetPreviewHandles();

        const bool graphicalRun = context && context->GetGraphics();
        const auto contract = [this](const Spark::Templates::TemplateRuntimeScene& scene)
        {
            // Required: the camera this module renders through. Everything else is
            // decorative and only degrades the preview when an author removes it.
            m_previewCameraEntity = scene.Find("Preview Camera");
            if (!scene.Get<Transform>(m_previewCameraEntity) || !scene.Get<Camera>(m_previewCameraEntity))
                return false;

            // Decoration: nothing outside this contract drives the props or the
            // light, so a missing one only warns instead of being tracked.
            WarnOnMissingVisual(scene, "Preview Ground");
            WarnOnMissingVisual(scene, "Preview Origin");
            const uint32_t light = scene.Find("Preview Light");
            if (!scene.Get<Transform>(light) || !scene.Get<LightComponent>(light))
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "EmptyProject scene has no usable 'Preview Light'; the preview stays unlit");
            return true;
        };

        // The runtime preview is the scene this module drives whether or not a
        // graphics device exists; a cooked Startup.sparkscene is the fallback.
        // Scenes/Default.sparkscene is deliberately NOT a candidate: it is the
        // empty authoring scene the editor opens, and an empty scene can never
        // satisfy the 'Preview Camera' contract above, so listing it only
        // pretended to offer a fallback that could never be taken.
        const bool loaded = m_runtime.Load(context, "EmptyProject",
                                           {"Scenes/RuntimePreview.sparkscene", "Startup.sparkscene"}, contract);
        if (!loaded)
        {
            ResetPreviewHandles();
            m_context = nullptr;
            return false;
        }

        if (graphicalRun && m_runtime.IsActive())
        {
            m_previewHudEntity = m_runtime.CreateSprite(
                "EmptyProject Starter HUD", "Assets/empty_project_runtime_sheet.png",
                Spark::Templates::TemplateRuntimeScene::SheetCell(0, 0), {0.75f, 0.9f, 1.0f, 0.92f});
            PlacePreviewHud();
        }
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetPreviewHandles();
        m_context = nullptr;
        m_paused = false;
    }

    void OnUpdate(float deltaTime) override
    {
        if (m_paused || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;
        ++m_updateCount;
        m_elapsedSeconds += deltaTime;
    }

    void OnPause() override { m_paused = true; }
    void OnResume() override { m_paused = false; }
    void OnRender() override { m_runtime.Render(m_previewCameraEntity); }
    void OnResize(int width, int height) override
    {
        m_runtime.Resize(width, height);
        PlacePreviewHud();
    }
    bool SupportsHotReload() const override { return false; }

    [[nodiscard]] uint64_t GetUpdateCount() const { return m_updateCount; }
    [[nodiscard]] float GetElapsedSeconds() const { return m_elapsedSeconds; }
    [[nodiscard]] bool IsPaused() const { return m_paused; }
    [[nodiscard]] bool HasEngineContext() const { return m_context != nullptr; }
    [[nodiscard]] bool IsPreviewActive() const
    {
        return m_previewCameraEntity != Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    }

  private:
    static void WarnOnMissingVisual(const Spark::Templates::TemplateRuntimeScene& scene, const char* name)
    {
        const uint32_t entity = scene.Find(name);
        if (scene.Get<Transform>(entity) && scene.Get<MeshRenderer>(entity))
            return;
        SPARK_LOG_WARN(Spark::LogCategory::Game, "EmptyProject scene has no usable '%s' preview prop", name);
    }

    void PlacePreviewHud()
    {
        m_runtime.PlaceHud(m_previewCameraEntity, m_previewHudEntity, -0.13f, 0.085f, 0.32f, 0.06f, 0.06f);
    }

    void ResetPreviewHandles()
    {
        constexpr uint32_t invalid = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_previewCameraEntity = invalid;
        m_previewHudEntity = invalid;
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    uint64_t m_updateCount = 0;
    float m_elapsedSeconds = 0.0f;
    uint32_t m_previewCameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_previewHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_paused = false;
};
