#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>

struct ThirdPersonStarterState
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float verticalVelocity = 0.0f;
    float orbitYawDegrees = 0.0f;
    float orbitPitchDegrees = 25.0f;
    float orbitDistance = 8.0f;
    bool grounded = true;
    bool pickupCollected = false;
    bool goalReached = false;
};

class ThirdPersonStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "ThirdPersonStarter";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetAdventure();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f || m_state.grounded)
            return;
        m_state.verticalVelocity -= 18.0f * deltaTime;
        m_state.y += m_state.verticalVelocity * deltaTime;
        if (m_state.y <= 0.0f)
        {
            m_state.y = 0.0f;
            m_state.verticalVelocity = 0.0f;
            m_state.grounded = true;
        }
    }

    void Move(float xAxis, float zAxis, float deltaTime)
    {
        if (deltaTime <= 0.0f)
            return;
        m_state.x = std::clamp(m_state.x + std::clamp(xAxis, -1.0f, 1.0f) * 5.0f * deltaTime, -20.0f, 20.0f);
        m_state.z = std::clamp(m_state.z + std::clamp(zAxis, -1.0f, 1.0f) * 5.0f * deltaTime, -20.0f, 20.0f);
    }

    bool Jump()
    {
        if (!m_state.grounded)
            return false;
        m_state.grounded = false;
        m_state.verticalVelocity = 7.0f;
        return true;
    }

    void Orbit(float yawDeltaDegrees, float pitchDeltaDegrees, float zoomDelta)
    {
        m_state.orbitYawDegrees += yawDeltaDegrees;
        m_state.orbitPitchDegrees = std::clamp(m_state.orbitPitchDegrees + pitchDeltaDegrees, -10.0f, 75.0f);
        m_state.orbitDistance = std::clamp(m_state.orbitDistance + zoomDelta, 3.0f, 14.0f);
    }

    bool TryCollectPickup()
    {
        if (m_state.pickupCollected || DistanceSquared(3.0f, 2.0f) > 2.25f)
            return false;
        m_state.pickupCollected = true;
        return true;
    }

    bool TryReachGoal()
    {
        if (!m_state.pickupCollected || DistanceSquared(7.0f, 5.0f) > 4.0f)
            return false;
        m_state.goalReached = true;
        return true;
    }

    void ResetAdventure() { m_state = {}; }

    [[nodiscard]] const ThirdPersonStarterState& GetState() const { return m_state; }

  private:
    [[nodiscard]] float DistanceSquared(float x, float z) const
    {
        const float dx = m_state.x - x;
        const float dz = m_state.z - z;
        return dx * dx + dz * dz;
    }

    Spark::IEngineContext* m_context = nullptr;
    ThirdPersonStarterState m_state;
};
