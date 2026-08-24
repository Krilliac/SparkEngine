#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <cmath>

struct Blank3DCameraState
{
    float x = 2.0f;
    float y = 2.5f;
    float z = -6.0f;
    float yawDegrees = -15.0f;
    float pitchDegrees = 15.0f;
    float moveSpeed = 5.0f;
};

class Blank3DModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "Blank3D";
        info.version = "0.2.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetCamera();
        m_elapsedSeconds = 0.0f;
        m_resetHeld = false;
        if (!m_runtime.Load(context, "Blank3D", {"Startup.sparkscene", "Scenes/Default.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_cameraEntity = scene.Find("Main Camera");
                                return scene.Get<Transform>(m_cameraEntity) && scene.Get<Camera>(m_cameraEntity) &&
                                       scene.Get<MeshRenderer>(scene.Find("Starter Cube"));
                            }))
            return false;
        if (m_runtime.IsActive() && m_runtime.GetGraphics())
            m_hudEntity = m_runtime.CreateSprite("Blank3D Transform HUD", "Assets/blank3d_runtime_sheet.png",
                                                 Spark::Templates::TemplateRuntimeScene::SheetCell(0, 0));
        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_context = nullptr;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f)
            return;
        m_elapsedSeconds += deltaTime;
        UpdateRuntimeInput(deltaTime);
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    void Move(float forward, float right, float vertical, float deltaTime)
    {
        if (deltaTime <= 0.0f)
            return;
        m_camera.z += forward * m_camera.moveSpeed * deltaTime;
        m_camera.x += right * m_camera.moveSpeed * deltaTime;
        m_camera.y += vertical * m_camera.moveSpeed * deltaTime;
    }

    void Look(float yawDeltaDegrees, float pitchDeltaDegrees)
    {
        m_camera.yawDegrees += yawDeltaDegrees;
        m_camera.pitchDegrees = std::clamp(m_camera.pitchDegrees + pitchDeltaDegrees, -89.0f, 89.0f);
    }

    void SetMoveSpeed(float speed) { m_camera.moveSpeed = std::clamp(speed, 0.5f, 50.0f); }

    void ResetCamera() { m_camera = {}; }

    [[nodiscard]] const Blank3DCameraState& GetCameraState() const { return m_camera; }
    [[nodiscard]] float GetElapsedSeconds() const { return m_elapsedSeconds; }

  private:
    void UpdateRuntimeInput(float deltaTime)
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        float forward = 0.0f;
        float right = 0.0f;
        float vertical = 0.0f;
        if (input->IsKeyDown('W'))
            forward += 1.0f;
        if (input->IsKeyDown('S'))
            forward -= 1.0f;
        if (input->IsKeyDown('D'))
            right += 1.0f;
        if (input->IsKeyDown('A'))
            right -= 1.0f;
        if (input->IsKeyDown('E'))
            vertical += 1.0f;
        if (input->IsKeyDown('Q'))
            vertical -= 1.0f;

        const float length = std::sqrt(forward * forward + right * right + vertical * vertical);
        if (length > 1.0f)
        {
            forward /= length;
            right /= length;
            vertical /= length;
        }
        const float yaw = DirectX::XMConvertToRadians(m_camera.yawDegrees);
        m_camera.x += (forward * std::sin(yaw) + right * std::cos(yaw)) * m_camera.moveSpeed * deltaTime;
        m_camera.z += (forward * std::cos(yaw) - right * std::sin(yaw)) * m_camera.moveSpeed * deltaTime;
        m_camera.y += vertical * m_camera.moveSpeed * deltaTime;

        if (input->IsMouseButtonDown(1))
        {
            const MousePoint delta = input->GetMouseDelta();
            Look(static_cast<float>(delta.x) * 0.12f, static_cast<float>(delta.y) * 0.12f);
        }
        const bool resetDown = input->IsKeyDown('R');
        if (resetDown && !m_resetHeld)
            ResetCamera();
        m_resetHeld = resetDown;
    }

    void SyncRuntimeState()
    {
        if (Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            camera->position = {m_camera.x, m_camera.y, m_camera.z};
            camera->rotation = {m_camera.pitchDegrees, m_camera.yawDegrees, 0.0f};
        }
        m_runtime.PlaceHud(m_cameraEntity, m_hudEntity, -0.12f, 0.08f, 0.32f, 0.055f, 0.055f);
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    Blank3DCameraState m_camera;
    float m_elapsedSeconds = 0.0f;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_resetHeld = false;
};
