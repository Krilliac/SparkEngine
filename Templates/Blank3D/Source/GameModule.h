#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>

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
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetCamera();
        m_elapsedSeconds = 0.0f;
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime > 0.0f)
            m_elapsedSeconds += deltaTime;
    }

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
    Spark::IEngineContext* m_context = nullptr;
    Blank3DCameraState m_camera;
    float m_elapsedSeconds = 0.0f;
};
