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
        const auto contract = [this, graphicalRun](const Spark::Templates::TemplateRuntimeScene& scene)
        {
            if (!graphicalRun)
                return true;

            m_previewGroundEntity = scene.Find("Preview Ground");
            m_previewOriginEntity = scene.Find("Preview Origin");
            m_previewLightEntity = scene.Find("Preview Light");
            m_previewCameraEntity = scene.Find("Preview Camera");
            return HasVisual(scene, m_previewGroundEntity) && HasVisual(scene, m_previewOriginEntity) &&
                   scene.Get<Transform>(m_previewLightEntity) && scene.Get<LightComponent>(m_previewLightEntity) &&
                   scene.Get<Transform>(m_previewCameraEntity) && scene.Get<Camera>(m_previewCameraEntity);
        };

        const bool loaded =
            graphicalRun
                ? m_runtime.Load(
                      context, "EmptyProject",
                      {"Scenes/RuntimePreview.sparkscene", "Startup.sparkscene", "Scenes/Default.sparkscene"}, contract)
                : m_runtime.Load(context, "EmptyProject", {"Startup.sparkscene", "Scenes/Default.sparkscene"},
                                 contract);
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
    static bool HasVisual(const Spark::Templates::TemplateRuntimeScene& scene, uint32_t entity)
    {
        return scene.Get<Transform>(entity) && scene.Get<MeshRenderer>(entity);
    }

    void PlacePreviewHud()
    {
        m_runtime.PlaceHud(m_previewCameraEntity, m_previewHudEntity, -0.13f, 0.085f, 0.32f, 0.06f, 0.06f);
    }

    void ResetPreviewHandles()
    {
        constexpr uint32_t invalid = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_previewGroundEntity = invalid;
        m_previewOriginEntity = invalid;
        m_previewLightEntity = invalid;
        m_previewCameraEntity = invalid;
        m_previewHudEntity = invalid;
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    uint64_t m_updateCount = 0;
    float m_elapsedSeconds = 0.0f;
    uint32_t m_previewGroundEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_previewOriginEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_previewLightEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_previewCameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_previewHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_paused = false;
};
