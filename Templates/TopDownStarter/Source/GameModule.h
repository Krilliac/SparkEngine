#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>
#include <cmath>

struct TopDownStarterState
{
    float playerX = 0.0f;
    float playerZ = 0.0f;
    float playerHealth = 100.0f;
    float enemyX = 6.0f;
    float enemyZ = 4.0f;
    float enemyHealth = 75.0f;
    float cameraX = 0.0f;
    float cameraZ = 0.0f;
    float cameraHeight = 20.0f;
    bool pickupCollected = false;
    bool enemyDefeated = false;
    bool won = false;
};

class TopDownStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "TopDownStarter";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        Restart();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f || m_state.enemyDefeated || m_state.won)
            return;

        const float dx = m_state.playerX - m_state.enemyX;
        const float dz = m_state.playerZ - m_state.enemyZ;
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance > 0.01f)
        {
            const float step = std::min(distance, 1.5f * deltaTime);
            m_state.enemyX += dx / distance * step;
            m_state.enemyZ += dz / distance * step;
        }
        if (distance < 1.2f)
            m_state.playerHealth = std::max(0.0f, m_state.playerHealth - 12.0f * deltaTime);
    }

    void Move(float xAxis, float zAxis, float deltaTime)
    {
        if (deltaTime <= 0.0f || m_state.playerHealth <= 0.0f)
            return;
        m_state.playerX = std::clamp(m_state.playerX + std::clamp(xAxis, -1.0f, 1.0f) * 6.0f * deltaTime, -9.0f, 9.0f);
        m_state.playerZ = std::clamp(m_state.playerZ + std::clamp(zAxis, -1.0f, 1.0f) * 6.0f * deltaTime, -9.0f, 9.0f);
    }

    void PanCamera(float xDelta, float zDelta)
    {
        m_state.cameraX = std::clamp(m_state.cameraX + xDelta, -10.0f, 10.0f);
        m_state.cameraZ = std::clamp(m_state.cameraZ + zDelta, -10.0f, 10.0f);
    }

    void ZoomCamera(float delta) { m_state.cameraHeight = std::clamp(m_state.cameraHeight + delta, 8.0f, 30.0f); }

    bool TryCollectPickup()
    {
        if (m_state.pickupCollected || PlayerDistanceSquared(-4.0f, 3.0f) > 2.25f)
            return false;
        m_state.pickupCollected = true;
        m_state.playerHealth = std::min(100.0f, m_state.playerHealth + 25.0f);
        return true;
    }

    bool AttackEnemy()
    {
        if (m_state.enemyDefeated || m_state.playerHealth <= 0.0f ||
            PlayerDistanceSquared(m_state.enemyX, m_state.enemyZ) > 6.25f)
            return false;
        m_state.enemyHealth = std::max(0.0f, m_state.enemyHealth - (m_state.pickupCollected ? 30.0f : 20.0f));
        if (m_state.enemyHealth == 0.0f)
        {
            m_state.enemyDefeated = true;
            m_state.won = true;
        }
        return true;
    }

    void Restart() { m_state = {}; }

    [[nodiscard]] const TopDownStarterState& GetState() const { return m_state; }

  private:
    [[nodiscard]] float PlayerDistanceSquared(float x, float z) const
    {
        const float dx = m_state.playerX - x;
        const float dz = m_state.playerZ - z;
        return dx * dx + dz * dz;
    }

    Spark::IEngineContext* m_context = nullptr;
    TopDownStarterState m_state;
};
