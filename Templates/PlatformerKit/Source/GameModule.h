#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

struct PlatformerKitState
{
    float x = 0.0f;
    float y = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float checkpointX = 0.0f;
    float checkpointY = 0.0f;
    float elapsedSeconds = 0.0f;
    uint32_t lives = 3;
    uint32_t coins = 0;
    uint32_t deaths = 0;
    uint32_t jumpsUsed = 0;
    bool grounded = true;
    bool checkpointActive = false;
    bool finished = false;
};

class PlatformerKitModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "PlatformerKit";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        RestartLevel();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f || m_state.finished || m_state.lives == 0)
            return;

        m_state.elapsedSeconds += deltaTime;
        m_state.velocityX = m_moveInput * 8.0f;
        if (!m_state.grounded)
            m_state.velocityY -= 24.0f * deltaTime;
        m_state.x += m_state.velocityX * deltaTime;
        m_state.y += m_state.velocityY * deltaTime;

        if (m_state.y <= 0.0f)
        {
            m_state.y = 0.0f;
            m_state.velocityY = 0.0f;
            m_state.grounded = true;
            m_state.jumpsUsed = 0;
        }
    }

    void SetMoveInput(float input) { m_moveInput = std::clamp(input, -1.0f, 1.0f); }

    bool Jump()
    {
        if (m_state.finished || m_state.lives == 0 || m_state.jumpsUsed >= 2)
            return false;
        ++m_state.jumpsUsed;
        m_state.grounded = false;
        m_state.velocityY = 10.0f;
        return true;
    }

    bool CollectCoin(std::size_t index)
    {
        if (index >= m_collected.size() || m_collected[index])
            return false;
        m_collected[index] = true;
        ++m_state.coins;
        return true;
    }

    void ActivateCheckpoint(float x = 12.0f, float y = 4.0f)
    {
        m_state.checkpointX = x;
        m_state.checkpointY = y;
        m_state.checkpointActive = true;
    }

    void HitHazard()
    {
        if (m_state.lives == 0 || m_state.finished)
            return;
        --m_state.lives;
        ++m_state.deaths;
        if (m_state.lives > 0)
            Respawn();
    }

    bool ReachFinish()
    {
        if (m_state.coins != m_collected.size() || m_state.lives == 0)
            return false;
        m_state.finished = true;
        m_moveInput = 0.0f;
        return true;
    }

    void RestartLevel()
    {
        m_state = {};
        m_collected.fill(false);
        m_moveInput = 0.0f;
    }

    [[nodiscard]] const PlatformerKitState& GetState() const { return m_state; }
    [[nodiscard]] bool IsCoinCollected(std::size_t index) const
    {
        return index < m_collected.size() && m_collected[index];
    }

  private:
    void Respawn()
    {
        m_state.x = m_state.checkpointX;
        m_state.y = m_state.checkpointY;
        m_state.velocityX = 0.0f;
        m_state.velocityY = 0.0f;
        m_state.grounded = m_state.checkpointY == 0.0f;
        m_state.jumpsUsed = 0;
    }

    Spark::IEngineContext* m_context = nullptr;
    PlatformerKitState m_state;
    std::array<bool, 3> m_collected{};
    float m_moveInput = 0.0f;
};
