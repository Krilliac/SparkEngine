/**
 * @file GameModule.h
 * @brief {{PROJECT_NAME}} — Platformer game module
 *
 * Side-scrolling / 3D platformer template with character controller,
 * collectibles, checkpoints, and level timer.
 */

#pragma once

#include <Spark/SparkSDK.h>

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Platformer player state
// ============================================================================

struct PlatformerPlayer
{
    float posX = 0.0f;
    float posY = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float moveSpeed = 8.0f;
    float jumpForce = 12.0f;
    float gravity = -30.0f;
    bool isGrounded = true;
    bool canDoubleJump = true;
    uint32_t lives = 3;
    uint32_t coins = 0;
    uint32_t score = 0;
};

// ============================================================================
// Checkpoint
// ============================================================================

struct Checkpoint
{
    float posX = 0.0f;
    float posY = 0.0f;
    bool activated = false;
};

// ============================================================================
// Platformer Game Module
// ============================================================================

class {{PROJECT_NAME}}Module : public Spark::IModule
{
  public:
    {{PROJECT_NAME}}Module() = default;
    ~{{PROJECT_NAME}}Module() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "{{PROJECT_NAME}}";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_player = PlatformerPlayer{};
        InitializeCheckpoints();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        // Apply gravity
        if (!m_player.isGrounded)
        {
            m_player.velocityY += m_player.gravity * deltaTime;
        }

        // Update position
        m_player.posX += m_player.velocityX * deltaTime;
        m_player.posY += m_player.velocityY * deltaTime;

        // Ground check (simplified)
        if (m_player.posY <= 0.0f)
        {
            m_player.posY = 0.0f;
            m_player.velocityY = 0.0f;
            m_player.isGrounded = true;
            m_player.canDoubleJump = true;
        }

        // Update level timer
        m_levelTimer += deltaTime;
    }

    void OnRender() override
    {
        // Render coins, lives, score, timer HUD
    }

  private:
    void InitializeCheckpoints()
    {
        m_checkpoints.push_back({0.0f, 0.0f, true});
        m_checkpoints.push_back({50.0f, 10.0f, false});
        m_checkpoints.push_back({120.0f, 25.0f, false});
    }

    Spark::IEngineContext* m_context = nullptr;
    PlatformerPlayer m_player;
    std::vector<Checkpoint> m_checkpoints;
    float m_levelTimer = 0.0f;
};
